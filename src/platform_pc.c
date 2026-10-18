/* PC test harness: headless EGL (Mesa llvmpipe), scripted input, PNG screenshots.
 * Used to check rendering and movement before anything goes to the Vita. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <sys/stat.h>
#include <time.h>
#include "common.h"

#ifndef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24_OES 0x81A6
#endif

static EGLDisplay g_dpy;
static EGLContext g_ctx;
static GLuint g_fbo, g_color, g_depth;
static int g_w = 960, g_h = 544;
static const char *g_data = "data";
static const char *g_out = "out";
static const char *g_pak = NULL;
static double g_t0;
static long g_frame;
static const char *g_audio_path;
static FILE *g_audio_file;
static AudioMixFn g_mix;
static double g_audio_acc;
static long g_audio_frames;
static double g_audio_cpu, g_audio_load;

/* current scripted state */


int plat_init_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)
            g_data = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            g_out = argv[++i];
        else if (!strcmp(argv[i], "--pak") && i + 1 < argc)
            g_pak = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc)
            g_audio_path = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            if (!script_open(argv[++i])) {
                fprintf(stderr, "cannot open script %s\n", argv[i]);
                return 0;
            }
        }
    }
    mkdir(g_out, 0755);
    return 1;
}

double plat_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (g_t0 == 0) g_t0 = t;
    return t - g_t0;
}

const char *plat_data_dir(void) { return g_data; }
const char *plat_pak_path(void) { return g_pak; }
GLuint plat_backbuffer(void) { return g_fbo; }
const char *plat_write_dir(void) { return g_out; }

void plat_present_message(const char *msg) { fprintf(stderr, "FATAL: %s\n", msg); }
void plat_set_vsync(int on) { (void)on; }
void plat_log_memory(const char *when) { (void)when; }
void plat_set_gpu_clock(int mhz) { (void)mhz; }
const char *plat_shot_dir(void) { return g_out; }

void plat_log_async(int on) { (void)on; }
void plat_cpu_boost(int on) { (void)on; }
void plat_cpu_bench(void) {}
void plat_log_lock(void) {}
void plat_log_unlock(void) {}

int plat_init(int *width, int *height)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC getdpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getdpy) {
        lsd_log("no eglGetPlatformDisplayEXT");
        return 0;
    }
    g_dpy = getdpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint maj, min;
    if (!eglInitialize(g_dpy, &maj, &min)) {
        lsd_log("eglInitialize failed");
        return 0;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        lsd_log("no EGL config");
        return 0;
    }
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!g_ctx || !eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_ctx)) {
        lsd_log("EGL context failed");
        return 0;
    }
    lsd_log("GL: %s | %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_w, g_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24_OES, g_w, g_h);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        lsd_log("FBO incomplete");
        return 0;
    }
    *width = g_w;
    *height = g_h;
    return 1;
}

void plat_shutdown(void)
{
    if (g_dpy) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_dpy, g_ctx);
        eglTerminate(g_dpy);
    }
}

/* test: LSD_JITTER=1 gives the frames uneven times, as the Vita's display queue does (the display still
 * shows each one for a refresh), and every 600 frames a stall of 12 refreshes (the sound plays on) */
static double g_interval = -1;
double plat_frame_interval(void) { return g_interval >= 0 ? g_interval : 1.0 / plat_refresh_hz(); }

/* test: LSD_SHOTFREEZE=1.3 makes each screenshot stand the game still that long, as the Vita's PNG
 * writing does (the sound plays on; nothing else runs) */
static double g_freeze;
void plat_after_shot(void)
{
    const char *e = getenv("LSD_SHOTFREEZE");
    if (e && atof(e) > 0) {
        g_freeze = atof(e);
        plat_audio_pump((float)g_freeze);
    }
}

int plat_frame_begin(Input *in, float *dt)
{
    if (!script_input(in))
        return 0;
    if (g_freeze > 0) { /* (the frame after a screenshot came that much later) */
        g_interval = 1.0 / plat_refresh_hz() + g_freeze;
        g_freeze = 0;
        *dt = (float)(1.0 / plat_refresh_hz());
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
        return 1;
    }
    *dt = (float)(1.0 / plat_refresh_hz());
    g_interval = -1;
    static int jitter = -1;
    static long fno;
    if (jitter < 0) {
        const char *e = getenv("LSD_JITTER");
        jitter = e ? atoi(e) : 0;
    }
    if (jitter) {
        static const double PAT[6] = {0.6, 1.4, 1.0, 1.0, 0.3, 1.7};
        double f = 1.0 / plat_refresh_hz();
        fno++;
        g_interval = PAT[fno % 6] * f;
        if (fno % 600 == 0) {
            g_interval = 12 * f;
            plat_audio_pump((float)(11 * f)); /* (the sound played on through the stall) */
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    return 1;
}


void plat_frame_end(void)
{
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        lsd_log("GL error 0x%x at frame %ld", err, g_frame);
    glFinish();
    g_frame++;
}

/* ---------------------------------------------------------------- audio: mixed per frame, optionally recorded */

static void wav_header(FILE *f, long frames)
{
    uint32_t data = (uint32_t)(frames * 4);
    uint32_t v;
    uint16_t h;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    v = 36 + data;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    h = 1;
    fwrite(&h, 2, 1, f);
    h = 2;
    fwrite(&h, 2, 1, f);
    v = 44100;
    fwrite(&v, 4, 1, f);
    v = 44100 * 4;
    fwrite(&v, 4, 1, f);
    h = 4;
    fwrite(&h, 2, 1, f);
    h = 16;
    fwrite(&h, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fseek(f, 0, SEEK_END);
}

void plat_audio_start(AudioMixFn mix)
{
    g_mix = mix;
    if (g_audio_path && !g_audio_file) {
        g_audio_file = fopen(g_audio_path, "wb");
        if (g_audio_file) wav_header(g_audio_file, 0);
        else lsd_log("cannot write %s", g_audio_path);
    }
}

void plat_audio_stop(void)
{
    if (g_audio_file) { /* (kept open: the output may be started again) */
        wav_header(g_audio_file, g_audio_frames);
        fflush(g_audio_file);
        lsd_log("audio: %.1f s recorded to %s", g_audio_frames / 44100.0, g_audio_path);
    }
    g_mix = NULL;
}

void plat_audio_pump(float dt)
{
    if (!g_mix) return;
    g_audio_acc += dt * 44100.0;
    int frames = (int)g_audio_acc;
    static int grain = -1; /* (test: LSD_AUDIO_GRAIN=1024 mixes in blocks, as the Vita's output does) */
    if (grain < 0) {
        const char *e = getenv("LSD_AUDIO_GRAIN");
        grain = e ? atoi(e) : 0;
    }
    if (grain > 0) frames = frames / grain * grain;
    g_audio_acc -= frames;
    static int16_t buf[44100 * 2];
    while (frames > 0) {
        int n = frames > 44100 ? 44100 : frames;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        g_mix(buf, n);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double spent = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
        g_audio_cpu += spent;
        g_audio_load = g_audio_load * 0.9 + 0.1 * (spent / (n / 44100.0));
        if (g_audio_file) fwrite(buf, 4, (size_t)n, g_audio_file);
        g_audio_frames += n;
        frames -= n;
    }
}

double plat_audio_latency(void) /* (with LSD_AUDIO_GRAIN, like the Vita: a block waits while one plays) */
{
    const char *e = getenv("LSD_AUDIO_GRAIN");
    return e && atoi(e) > 0 ? atoi(e) / 44100.0 : 0.0;
}
double plat_refresh_hz(void) /* (test: LSD_REFRESH=59.94 runs the game at the Vita's rate) */
{
    static double hz;
    if (hz <= 0) {
        const char *e = getenv("LSD_REFRESH");
        hz = e && atof(e) > 10 ? atof(e) : 60.0;
    }
    return hz;
}
int plat_vsync_on(void) { return 1; }
void plat_audio_lock(void) {}
double plat_audio_lock_wait_ms(void) { return 0; }
void plat_audio_unlock(void) {}
double plat_audio_load(void) { return g_audio_load; }

/* ---------------------------------------------------------------- worker: runs at once (repeatable tests) */

static WorkFn g_work_fn;
static void *g_work_arg;

int plat_worker_start(WorkFn fn, void *arg)
{
    g_work_fn = fn;
    g_work_arg = arg;
    return 1;
}

void plat_worker_kick(void)
{
    if (g_work_fn) g_work_fn(g_work_arg);
}

void plat_worker_stop(void) { g_work_fn = NULL; }

/* (one-off jobs run at once here, so test runs repeat exactly) */
int plat_task_start(WorkFn fn, void *arg, int core)
{
    (void)core;
    fn(arg);
    return -1;
}
void plat_task_join(int task) { (void)task; }
void plat_task_yield(void) {}
void plat_wait_until(double t) { (void)t; }
void plat_open_url(const char *url) { lsd_log("open url: %s", url); }

static char g_ime_next[64];
static int g_ime_on, g_ime_have;
static char g_ime_initial[64];

void plat_text_input_abort(void) { lsd_log("keyboard: abort"); }

void plat_text_input_set(const char *text)
{
    snprintf(g_ime_next, sizeof(g_ime_next), "%s", text);
    g_ime_have = 1;
}

int plat_text_input_begin(const char *title, const char *initial, int max_len)
{
    lsd_log("keyboard: %s (up to %d characters, now '%s')", title, max_len, initial);
    snprintf(g_ime_initial, sizeof(g_ime_initial), "%s", initial);
    g_ime_on = 1;
    return 1;
}

int plat_text_input_poll(char *out, size_t n)
{
    if (!g_ime_on)
        return -1;
    g_ime_on = 0;
    if (!g_ime_have) { /* (nothing given: as if cancelled) */
        lsd_log("keyboard: cancelled");
        return -1;
    }
    g_ime_have = 0;
    snprintf(out, n, "%s", g_ime_next);
    lsd_log("keyboard: entered '%s'", out);
    return 1;
}
void plat_worker_lock(void) {}
void plat_worker_unlock(void) {}
void plat_check_clocks(void) {}
void plat_keep_awake(void) {}
void plat_test_keep_awake(int on) { (void)on; }
