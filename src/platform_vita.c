/* PS Vita platform layer (vitaGL) */
#include <psp2/ime_dialog.h>
#include <psp2/apputil.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <stdarg.h>
#include "common.h"

/* plenty for us; vitaGL takes its GPU pools from the rest */
int _newlib_heap_size_user = 96 * 1024 * 1024;

static SceCtrlData g_prev;
static uint32_t g_prev_held;
static double g_last;
static double g_quit_hold;
static int g_have_dialog;

int plat_init_args(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 1;
}

double plat_seconds(void)
{
    return (double)sceKernelGetProcessTimeWide() / 1000000.0;
}

const char *plat_data_dir(void) { return "ux0:data/lsdr"; }
const char *plat_pak_path(void) { return "ux0:data/lsdr/dreams.pak"; }
GLuint plat_backbuffer(void) { return 0; }
const char *plat_write_dir(void) { return "ux0:data/lsdr"; }
const char *plat_shot_dir(void) { return "ux0:data/lsdr/shots"; }
static int g_vsync = 1;
void plat_set_vsync(int on)
{
    g_vsync = on;
    vglWaitVblankStart(on ? GL_TRUE : GL_FALSE);
}
int plat_vsync_on(void) { return g_vsync; }
void plat_after_shot(void) {}
static double g_interval = 1.0 / 60.0, g_begin_prev;
double plat_frame_interval(void) { return g_interval; }

void plat_set_gpu_clock(int mhz)
{
    if (mhz > 0) {
        int r = scePowerSetGpuClockFrequency(mhz);
        if (r < 0)
            lsd_log("GPU clock request %d MHz refused (0x%08x)", mhz, (unsigned)r);
    }
    lsd_log("clocks: CPU %d MHz, GPU %d MHz, GPU crossbar %d MHz, bus %d MHz", scePowerGetArmClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency(), scePowerGetBusClockFrequency());
}

static int g_clk_arm, g_clk_gpu, g_clk_bus, g_clk_xbar;

/* the system may lower the clocks (power saving while idle): put back what we started with */
void plat_check_clocks(void)
{
    int arm = scePowerGetArmClockFrequency(), gpu = scePowerGetGpuClockFrequency();
    int bus = scePowerGetBusClockFrequency(), xbar = scePowerGetGpuXbarClockFrequency();
    if (!g_clk_arm) {
        g_clk_arm = arm;
        g_clk_gpu = gpu;
        g_clk_bus = bus;
        g_clk_xbar = xbar;
        return;
    }
    if (arm == g_clk_arm && gpu == g_clk_gpu && bus == g_clk_bus && xbar == g_clk_xbar)
        return;
    lsd_log("clocks changed by the system: CPU %d MHz, GPU %d MHz, crossbar %d MHz, bus %d MHz; restoring %d/%d/%d/%d",
            arm, gpu, xbar, bus, g_clk_arm, g_clk_gpu, g_clk_xbar, g_clk_bus);
    if (arm != g_clk_arm) scePowerSetArmClockFrequency(g_clk_arm);
    if (bus != g_clk_bus) scePowerSetBusClockFrequency(g_clk_bus);
    if (gpu != g_clk_gpu) scePowerSetGpuClockFrequency(g_clk_gpu);
    if (xbar != g_clk_xbar) scePowerSetGpuXbarClockFrequency(g_clk_xbar);
}

void plat_cpu_boost(int on) { (void)on; } /* (the CPU runs at 444 MHz all along) */

/* test: how fast the CPU really is at some clock settings (a fixed piece of work, timed), then back */
static double cpu_work(void)
{
    static uint8_t a[1 << 20], b[1 << 20];
    double t0 = plat_seconds();
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3000000; i++)
        h = (h ^ (uint32_t)i) * 16777619u;
    for (int k = 0; k < 8; k++) {
        memcpy(b, a, sizeof(a));
        a[k] = (uint8_t)(h >> k);
        memcpy(a, b, sizeof(b));
    }
    volatile uint32_t sink = h + a[5];
    (void)sink;
    return (plat_seconds() - t0) * 1000.0;
}

void plat_cpu_bench(void)
{
    static const int CFG[4][4] = {{333, 222, 111, 111}, {333, 222, 222, 166}, {444, 222, 111, 111}, {444, 222, 222, 166}};
    for (int c = 0; c < 4; c++) {
        scePowerSetArmClockFrequency(CFG[c][0]);
        scePowerSetBusClockFrequency(CFG[c][1]);
        scePowerSetGpuClockFrequency(CFG[c][2]);
        scePowerSetGpuXbarClockFrequency(CFG[c][3]);
        sceKernelDelayThread(20000);
        double best = 1e9;
        for (int r = 0; r < 3; r++) {
            double t = cpu_work();
            if (t < best) best = t;
        }
        lsd_log("cpu bench: CPU %d / GPU %d (reported %d / %d MHz): %.1f ms", CFG[c][0], CFG[c][2],
                scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), best);
    }
    scePowerSetArmClockFrequency(g_clk_arm);
    scePowerSetBusClockFrequency(g_clk_bus);
    scePowerSetGpuClockFrequency(g_clk_gpu);
    scePowerSetGpuXbarClockFrequency(g_clk_xbar);
}

void plat_keep_awake(void) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }

static int g_test_awake = 1;
void plat_test_keep_awake(int on) { g_test_awake = on; }

void plat_log_memory(const char *when)
{
    lsd_log("memory %s: vitaGL free %u KB (VRAM %u KB, RAM %u KB)", when,
            (unsigned)(vglMemFree(VGL_MEM_ALL) / 1024), (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
            (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024));
}

static int file_exists(const char *p)
{
    SceIoStat st;
    return sceIoGetstat(p, &st) >= 0;
}

/* ---------------------------------------------------------------- the log
 * A write to the memory card takes the game thread 40-60 ms (a line flushed every time): the lines
 * are queued instead, and a thread on the second core writes them out four times a second. */
#define LOG_QUEUE (64 * 1024)
static SceKernelLwMutexWork g_log_mutex;
static int g_log_mutex_ok;
static char g_logq[LOG_QUEUE];
static unsigned g_logq_n, g_log_dropped;
static SceUID g_log_thread = -1, g_log_sema = -1, g_log_fd = -1;
static volatile int g_log_run;

void plat_log_lock(void)
{
    if (g_log_mutex_ok) sceKernelLockLwMutex(&g_log_mutex, 1, NULL);
}

void plat_log_unlock(void)
{
    if (g_log_mutex_ok) sceKernelUnlockLwMutex(&g_log_mutex, 1);
}

static void log_out(const char *s, unsigned n)
{
    if (g_log_fd < 0)
        g_log_fd = sceIoOpen("ux0:data/lsdr/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (g_log_fd >= 0)
        sceIoWrite(g_log_fd, s, n);
}

/* take what is queued (with a note of lines that did not fit) */
static unsigned log_take(char *out, unsigned cap)
{
    plat_log_lock();
    unsigned n = g_logq_n < cap ? g_logq_n : cap;
    memcpy(out, g_logq, n);
    g_logq_n = 0;
    unsigned dropped = g_log_dropped;
    g_log_dropped = 0;
    plat_log_unlock();
    if (dropped && n + 64 < cap)
        n += (unsigned)snprintf(out + n, cap - n, "[log: %u lines did not fit in the queue]\n", dropped);
    return n;
}

static int log_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static char out[LOG_QUEUE + 128];
    while (g_log_run) {
        SceUInt wait_us = 250000;
        sceKernelWaitSema(g_log_sema, 1, &wait_us);
        unsigned n = log_take(out, sizeof(out));
        if (n) log_out(out, n);
    }
    return sceKernelExitThread(0);
}

void plat_log_write(const char *s, size_t n)
{
    if (g_log_thread < 0) { /* (before the thread, or without one: at once) */
        plat_log_lock();
        log_out(s, (unsigned)n);
        plat_log_unlock();
        return;
    }
    plat_log_lock();
    if (g_logq_n + n <= LOG_QUEUE) {
        memcpy(g_logq + g_logq_n, s, n);
        g_logq_n += (unsigned)n;
    } else {
        g_log_dropped++;
    }
    int half = g_logq_n > LOG_QUEUE / 2;
    plat_log_unlock();
    if (half)
        sceKernelSignalSema(g_log_sema, 1);
}

static void log_stop_thread(void)
{
    if (g_log_thread >= 0) {
        g_log_run = 0;
        sceKernelSignalSema(g_log_sema, 1);
        sceKernelWaitThreadEnd(g_log_thread, NULL, NULL);
        sceKernelDeleteThread(g_log_thread);
        g_log_thread = -1;
    }
    static char out[LOG_QUEUE + 128];
    unsigned n = log_take(out, sizeof(out));
    if (n) log_out(out, n);
}

/* everything queued written out, the writer stopped (quitting) */
void plat_log_flush(void)
{
    log_stop_thread();
    if (g_log_fd >= 0) {
        sceIoClose(g_log_fd);
        g_log_fd = -1;
    }
}

static void log_start_thread(void)
{
    if (g_log_thread >= 0 || !g_log_mutex_ok || g_log_sema < 0)
        return;
    g_log_run = 1;
    SceUID th = sceKernelCreateThread("lsd_logw", log_thread, 0x10000100 + 20, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_1,
                                      NULL);
    if (th >= 0 && sceKernelStartThread(th, 0, NULL) >= 0)
        g_log_thread = th;
    else if (th >= 0)
        sceKernelDeleteThread(th);
}

static void log_start(void)
{
    g_log_mutex_ok = sceKernelCreateLwMutex(&g_log_mutex, "lsd_log", 0, 0, NULL) >= 0;
    g_log_sema = sceKernelCreateSema("lsd_log", 0, 0, 1, NULL);
    log_start_thread();
}

/* test: the log written by its thread (1), or by whoever logs, at once (0) */
void plat_log_async(int on)
{
    if (on) log_start_thread();
    else log_stop_thread();
}

int plat_init(int *width, int *height)
{
    sceIoMkdir("ux0:data/lsdr", 0777);
    log_start();
    lsd_log("LSD Revamped Vita %s (vitaGL)", LSD_VERSION);
    /* a scripted test run (docs/testing.md): ux0:data/lsdr/bench.txt is read once, then removed */
    if (script_open("ux0:data/lsdr/bench.txt")) {
        sceIoRemove("ux0:data/lsdr/bench.txt");
        sceIoMkdir("ux0:data/lsdr/shots", 0777);
    }
    /* the game on the first core; the sound mixer gets the third (plat_audio_start) */
    SceUID me = sceKernelGetThreadId();
    int was = sceKernelGetThreadCpuAffinityMask(me);
    int r = sceKernelChangeThreadCpuAffinityMask(me, SCE_KERNEL_CPU_MASK_USER_0);
    lsd_log("game thread core mask 0x%x -> 0x%x (%s)", (unsigned)was, (unsigned)sceKernelGetThreadCpuAffinityMask(me),
            r < 0 ? "refused" : "ok");
    int shacccg = file_exists("ur0:data/libshacccg.suprx") || file_exists("ur0:data/external/libshacccg.suprx");
    lsd_log("libshacccg.suprx %s", shacccg ? "found" : "NOT FOUND");

    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    g_have_dialog = sceAppUtilInit(&init, &boot) >= 0;
    if (g_have_dialog) {
        SceCommonDialogConfigParam cfg;
        sceCommonDialogConfigParamInit(&cfg);
        sceCommonDialogSetConfigParam(&cfg);
    }

    /* returns GL_TRUE only when it had to lower the resolution; it is not a success flag */
    if (vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE))
        lsd_log("vitaGL: display resolution fallback");
    vglWaitVblankStart(GL_TRUE);
    /* CPU 444 MHz, bus 222, graphics chip 222 (its crossbar 166). (With the graphics chip at 222 the CPU
     * does about a third less at a given setting - unpacking a dream 350 -> 515 ms at "333" - so 444
     * keeps it where 333 was with the chip at 111) */
    int ra = scePowerSetArmClockFrequency(444), rb = scePowerSetBusClockFrequency(222);
    int rg = scePowerSetGpuClockFrequency(222), rx = scePowerSetGpuXbarClockFrequency(166);
    if (ra < 0 || rb < 0 || rg < 0 || rx < 0)
        lsd_log("clock requests refused: CPU 0x%08x bus 0x%08x GPU 0x%08x crossbar 0x%08x", (unsigned)ra, (unsigned)rb,
                (unsigned)rg, (unsigned)rx);
    plat_set_gpu_clock(0); /* log the clocks */
    plat_check_clocks();   /* (and remember them: put back if the system lowers them) */
    lsd_log("display: %.4f Hz", plat_refresh_hz());
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START); /* (the menus take taps) */
    *width = 960;
    *height = 544;
    g_last = plat_seconds();
    lsd_log("GL_RENDERER: %s", (const char *)glGetString(GL_RENDERER));
    return 1;
}

void plat_shutdown(void)
{
    plat_log_flush();
    sceKernelExitProcess(0);
}

void plat_present_message(const char *msg)
{
    lsd_log("message: %s", msg);
    if (!g_have_dialog)
        return;
    SceMsgDialogUserMessageParam up;
    memset(&up, 0, sizeof(up));
    up.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    up.msg = (const SceChar8 *)msg;
    SceMsgDialogParam p;
    sceMsgDialogParamInit(&p);
    p.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    p.userMsgParam = &up;
    if (sceMsgDialogInit(&p) < 0)
        return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

static float stick(unsigned char v)
{
    float f = ((float)v - 128.0f) / 127.5f;
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    return f;
}

int plat_frame_begin(Input *in, float *dt)
{
    double begin = plat_seconds();
    if (g_begin_prev > 0) g_interval = begin - g_begin_prev;
    g_begin_prev = begin;
    if (script_active()) {
        /* nobody touches the pad during a test run: keep the system from idling (dimming, power saving) */
        if (g_test_awake)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        /* fixed step, like the PC harness, so test runs are repeatable */
        g_last = plat_seconds();
        *dt = 1.0f / 60.0f;
        return script_input(in);
    }
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    static const struct { unsigned sce; uint32_t b; } map[] = {
        {SCE_CTRL_UP, BTN_UP}, {SCE_CTRL_DOWN, BTN_DOWN}, {SCE_CTRL_LEFT, BTN_LEFT},
        {SCE_CTRL_RIGHT, BTN_RIGHT}, {SCE_CTRL_CROSS, BTN_CROSS}, {SCE_CTRL_CIRCLE, BTN_CIRCLE},
        {SCE_CTRL_SQUARE, BTN_SQUARE}, {SCE_CTRL_TRIANGLE, BTN_TRIANGLE}, {SCE_CTRL_LTRIGGER, BTN_L},
        {SCE_CTRL_RTRIGGER, BTN_R}, {SCE_CTRL_START, BTN_START}, {SCE_CTRL_SELECT, BTN_SELECT}};
    memset(in, 0, sizeof(*in));
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (pad.buttons & map[i].sce)
            in->held |= map[i].b;
    in->pressed = in->held & ~g_prev_held;
    g_prev_held = in->held;
    in->lx = stick(pad.lx);
    in->ly = stick(pad.ly);
    in->rx = stick(pad.rx);
    in->ry = stick(pad.ry);
    /* the front touch screen: 1920x1088 for the 960x544 screen */
    SceTouchData td;
    memset(&td, 0, sizeof(td));
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) > 0 && td.reportNum > 0) {
        in->touch = 1;
        in->tx = td.report[0].x * 0.5f;
        in->ty = td.report[0].y * 0.5f;
    }
    g_prev = pad;

    double now = plat_seconds();
    double d = now - g_last;
    g_last = now;
    if (d < 0) d = 0;
    if (d > 0.1) d = 0.1;
    /* a long frame under a full fade (a dream loading) is not played catch-up for: the fade that
     * follows starts from its beginning instead of a tenth of a second in */
    if (d > 1.0 / 40.0 && game_screen_covered()) d = 1.0 / 60.0;
    *dt = (float)d;

    /* hold START+SELECT for 2 s to quit */
    if ((in->held & (BTN_START | BTN_SELECT)) == (BTN_START | BTN_SELECT)) {
        g_quit_hold += d;
        if (g_quit_hold > 2.0)
            return 0;
    } else {
        g_quit_hold = 0;
    }
    return 1;
}

static int g_ime_on; /* 1 the dialog is up, 2 a test's text stands in for it */
static char g_ime_preset[64];
static int g_ime_have;

void plat_frame_end(void)
{
    vglSwapBuffers(g_ime_on == 1 ? GL_TRUE : GL_FALSE); /* (the IME dialog is drawn over the game) */
}

/* the system keyboard: sceImeDialog, Basic Latin */
static SceWChar16 g_ime_title[64], g_ime_initial[64], g_ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static void to_utf16(SceWChar16 *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++)
        dst[i] = (unsigned char)src[i];
    dst[i] = 0;
}

int plat_text_input_begin(const char *title, const char *initial, int max_len)
{
    if (g_ime_on)
        return 0;
    if (g_ime_have) { /* (test runs: no keyboard to wait on) */
        lsd_log("keyboard: %s (test text '%s')", title, g_ime_preset);
        g_ime_on = 2;
        return 1;
    }
    if (!g_have_dialog)
        return 0;
    to_utf16(g_ime_title, 64, title);
    to_utf16(g_ime_initial, 64, initial);
    memset(g_ime_buf, 0, sizeof(g_ime_buf));
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_BASIC_LATIN;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    p.title = g_ime_title;
    p.maxTextLength = (SceUInt32)(max_len > 0 && max_len < SCE_IME_DIALOG_MAX_TEXT_LENGTH ? max_len : 31);
    p.initialText = g_ime_initial;
    p.inputTextBuffer = g_ime_buf;
    int r = sceImeDialogInit(&p);
    if (r < 0) {
        lsd_log("keyboard: sceImeDialogInit failed (0x%08x)", (unsigned)r);
        return 0;
    }
    g_ime_on = 1;
    lsd_log("keyboard: open (%s)", title);
    return 1;
}

int plat_text_input_poll(char *out, size_t n)
{
    if (!g_ime_on)
        return -1;
    if (g_ime_on == 2) {
        g_ime_on = 0;
        g_ime_have = 0;
        snprintf(out, n, "%s", g_ime_preset);
        return 1;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;
    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    int ok = res.button == SCE_IME_DIALOG_BUTTON_ENTER;
    lsd_log("keyboard: %s (result 0x%x, button %d)", ok ? "entered" : "closed", (unsigned)res.result, (int)res.button);
    if (ok) {
        size_t i = 0;
        for (; g_ime_buf[i] && i + 1 < n; i++)
            out[i] = g_ime_buf[i] < 128 ? (char)g_ime_buf[i] : '?';
        out[i] = 0;
    }
    sceImeDialogTerm();
    g_ime_on = 0;
    return ok ? 1 : -1;
}

void plat_text_input_abort(void)
{
    if (g_ime_on == 1)
        lsd_log("keyboard: abort -> 0x%08x", (unsigned)sceImeDialogAbort());
}

void plat_text_input_set(const char *text)
{
    snprintf(g_ime_preset, sizeof(g_ime_preset), "%s", text);
    g_ime_have = 1;
}

/* vitaGL diagnostics (LOG_ERRORS build with the log-hook patch) */
void vgl_log_hook(const char *fmt, ...)
{
    static int count;
    if (++count > 300) {
        if (count == 301)
            lsd_log("vitaGL: further messages suppressed");
        return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    lsd_log("vitaGL: %s", buf);
}

/* ---------------------------------------------------------------- audio: an output thread on its own core */

#define AUDIO_GRAIN 1024 /* frames per output buffer (23 ms at 44.1 kHz) */

static SceKernelLwMutexWork g_audio_mutex;
static int g_audio_mutex_ok;
static AudioMixFn g_mix;
static volatile int g_audio_run;
static SceUID g_audio_thread = -1;
static volatile double g_audio_load;

static int audio_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, 44100, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        lsd_log("audio: no output port (0x%08x); running silent", (unsigned)port);
        return 0;
    }
    int vol[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    static int16_t buf[2][AUDIO_GRAIN * 2] __attribute__((aligned(64)));
    int cur = 0;
    const double grain_us = AUDIO_GRAIN * 1000000.0 / 44100.0;
    while (g_audio_run) {
        SceUInt64 t0 = sceKernelGetProcessTimeWide();
        g_mix(buf[cur], AUDIO_GRAIN);
        SceUInt64 t1 = sceKernelGetProcessTimeWide();
        g_audio_load = g_audio_load * 0.95 + 0.05 * ((double)(t1 - t0) / grain_us);
        sceAudioOutOutput(port, buf[cur]);
        cur ^= 1;
    }
    sceAudioOutOutput(port, NULL); /* wait for the last buffer */
    sceAudioOutReleasePort(port);
    return 0;
}

void plat_audio_start(AudioMixFn mix)
{
    if (!g_audio_mutex_ok) {
        g_audio_mutex_ok = sceKernelCreateLwMutex(&g_audio_mutex, "lsd_audio", 0, 0, NULL) >= 0;
        if (!g_audio_mutex_ok) {
            lsd_log("audio: no mutex; running silent");
            return;
        }
    }
    g_mix = mix;
    g_audio_run = 1;
    /* on the third core, away from the game thread */
    g_audio_thread = sceKernelCreateThread("lsd_audio", audio_thread, 0x10000100, 0x10000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (g_audio_thread < 0 || sceKernelStartThread(g_audio_thread, 0, NULL) < 0) {
        lsd_log("audio: thread failed (0x%08x); running silent", (unsigned)g_audio_thread);
        g_audio_run = 0;
        g_audio_thread = -1;
    }
}

void plat_audio_stop(void)
{
    if (g_audio_thread >= 0) {
        g_audio_run = 0;
        sceKernelWaitThreadEnd(g_audio_thread, NULL, NULL);
        sceKernelDeleteThread(g_audio_thread);
        g_audio_thread = -1;
    }
}

void plat_audio_pump(float dt) { (void)dt; }
double plat_audio_latency(void) { return AUDIO_GRAIN / 44100.0; } /* (double-buffered blocks) */
double plat_refresh_hz(void)
{
    static double hz;
    if (hz <= 0) {
        float f = 0;
        hz = (sceDisplayGetRefreshRate(&f) >= 0 && f > 50.0f && f < 70.0f) ? f : 60000.0 / 1001.0;
    }
    return hz;
}

static volatile double g_lock_wait_us; /* (the game thread's waits, for the stats) */

void plat_audio_lock(void)
{
    if (!g_audio_mutex_ok) return;
    if (sceKernelTryLockLwMutex(&g_audio_mutex, 1) >= 0) return;
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    sceKernelLockLwMutex(&g_audio_mutex, 1, NULL);
    g_lock_wait_us += (double)(sceKernelGetProcessTimeWide() - t0);
}

double plat_audio_lock_wait_ms(void)
{
    double ms = g_lock_wait_us / 1000.0;
    g_lock_wait_us = 0;
    return ms;
}

void plat_audio_unlock(void)
{
    if (g_audio_mutex_ok) sceKernelUnlockLwMutex(&g_audio_mutex, 1);
}

double plat_audio_load(void) { return g_audio_load; }

/* ---------------------------------------------------------------- one-off tasks */

typedef struct {
    WorkFn fn;
    void *arg;
} TaskStart;

static int task_thread(SceSize args, void *argp)
{
    TaskStart t;
    memcpy(&t, argp, sizeof(t));
    t.fn(t.arg);
    return sceKernelExitThread(0);
}

int plat_task_start(WorkFn fn, void *arg, int core)
{
    TaskStart t = {fn, arg};
    SceUID th = sceKernelCreateThread("lsd_task", task_thread, 0x10000100, 0x10000, 0,
                                      core == 2 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (th >= 0 && sceKernelStartThread(th, sizeof(t), &t) >= 0)
        return th;
    if (th >= 0)
        sceKernelDeleteThread(th);
    lsd_log("task thread failed (0x%08x): running it here", (unsigned)th);
    fn(arg);
    return -1;
}

void plat_task_join(int task)
{
    if (task < 0)
        return;
    sceKernelWaitThreadEnd(task, NULL, NULL);
    sceKernelDeleteThread(task);
}

void plat_task_yield(void) { sceKernelDelayThread(100); }

void plat_wait_until(double t)
{
    double now = plat_seconds();
    if (t > now + 0.0005)
        sceKernelDelayThread((SceUInt)((t - now) * 1e6));
}

/* the system's web browser, over the game (UIKofiButton / UISDKButton: Application.OpenURL) */
void plat_open_url(const char *url)
{
    SceAppUtilWebBrowserParam p;
    memset(&p, 0, sizeof(p));
    p.str = url;
    p.strlen = strlen(url);
    p.launchMode = 1;
    int r = sceAppUtilLaunchWebBrowser(&p);
    lsd_log("opening %s in the web browser (0x%08x)", url, (unsigned)r);
}

/* ---------------------------------------------------------------- worker: a thread on the second core */

static SceUID g_work_thread = -1, g_work_sema = -1;
static SceKernelLwMutexWork g_work_mutex;
static int g_work_mutex_ok;
static volatile int g_work_run;
static WorkFn g_work_fn;
static void *g_work_arg;

static int work_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    for (;;) {
        sceKernelWaitSema(g_work_sema, 1, NULL);
        if (!g_work_run)
            break;
        g_work_fn(g_work_arg);
    }
    return 0;
}

int plat_worker_start(WorkFn fn, void *arg)
{
    plat_worker_stop();
    if (!g_work_mutex_ok)
        g_work_mutex_ok = sceKernelCreateLwMutex(&g_work_mutex, "lsd_work", 0, 0, NULL) >= 0;
    g_work_sema = sceKernelCreateSema("lsd_work", 0, 0, 1, NULL);
    if (!g_work_mutex_ok || g_work_sema < 0) {
        lsd_log("worker: no semaphore/mutex (0x%08x)", (unsigned)g_work_sema);
        if (g_work_sema >= 0) sceKernelDeleteSema(g_work_sema);
        g_work_sema = -1;
        return 0;
    }
    g_work_fn = fn;
    g_work_arg = arg;
    g_work_run = 1;
    g_work_thread = sceKernelCreateThread("lsd_work", work_thread, 0x10000100, 0x40000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (g_work_thread < 0 || sceKernelStartThread(g_work_thread, 0, NULL) < 0) {
        lsd_log("worker: thread failed (0x%08x)", (unsigned)g_work_thread);
        if (g_work_thread >= 0) sceKernelDeleteThread(g_work_thread);
        g_work_thread = -1;
        sceKernelDeleteSema(g_work_sema);
        g_work_sema = -1;
        g_work_run = 0;
        return 0;
    }
    return 1;
}

void plat_worker_kick(void)
{
    if (g_work_sema >= 0)
        sceKernelSignalSema(g_work_sema, 1); /* (already signalled: the next run picks this up too) */
}

void plat_worker_stop(void)
{
    if (g_work_thread >= 0) {
        g_work_run = 0;
        sceKernelSignalSema(g_work_sema, 1);
        sceKernelWaitThreadEnd(g_work_thread, NULL, NULL);
        sceKernelDeleteThread(g_work_thread);
        g_work_thread = -1;
    }
    if (g_work_sema >= 0) {
        sceKernelDeleteSema(g_work_sema);
        g_work_sema = -1;
    }
}

void plat_worker_lock(void)
{
    if (g_work_mutex_ok) sceKernelLockLwMutex(&g_work_mutex, 1, NULL);
}

void plat_worker_unlock(void)
{
    if (g_work_mutex_ok) sceKernelUnlockLwMutex(&g_work_mutex, 1);
}
