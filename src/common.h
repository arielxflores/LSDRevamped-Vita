/* LSD Revamped Vita port - shared declarations */
#ifndef LSD_COMMON_H
#define LSD_COMMON_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
#include <vitaGL.h>
#else
#include <GLES2/gl2.h>
#endif

#define LSD_VERSION "1.0"

/* ---------------------------------------------------------------- logging */
void lsd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plat_log_lock(void); /* (lsd_log may be called from loading threads) */
void plat_log_unlock(void);
void plat_log_write(const char *s, size_t n); /* Vita: a log line, queued for the writer thread */
void plat_log_flush(void);                    /* Vita: everything queued written out (quitting) */
void plat_log_async(int on);                  /* test: 0 = each line written at once */

/* ---------------------------------------------------------------- frame profile (the hitch reports) */
enum { PROF_AUDIO, PROF_DS, PROF_LUA, PROF_WORLD, PROF_UI, PROF_IO, PROF_TEX, PROF_LOG, PROF_N };
void prof_add(int part, double seconds);
double prof_get(int part);
void prof_reset(void);
void prof_mark_test(void); /* this frame did test-only work (writing a recording...) */
int prof_test(void);

/* ---------------------------------------------------------------- platform */
enum {
    BTN_UP = 1 << 0,
    BTN_DOWN = 1 << 1,
    BTN_LEFT = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_CROSS = 1 << 4,
    BTN_CIRCLE = 1 << 5,
    BTN_SQUARE = 1 << 6,
    BTN_TRIANGLE = 1 << 7,
    BTN_L = 1 << 8,
    BTN_R = 1 << 9,
    BTN_START = 1 << 10,
    BTN_SELECT = 1 << 11,
};

typedef struct {
    float lx, ly, rx, ry;  /* sticks, -1..1, y positive = down (pad convention) */
    uint32_t held;         /* BTN_* currently held */
    uint32_t pressed;      /* BTN_* pressed this frame */
    int touch;             /* a finger on the front touch screen (the menus use it) */
    float tx, ty;          /* where, in screen pixels */
} Input;

int plat_init(int *width, int *height);
void plat_shutdown(void);
int plat_frame_begin(Input *in, float *dt); /* returns 0 to quit */
void plat_frame_end(void);
void plat_cpu_boost(int on);      /* (unused: the CPU runs at 444 MHz all along) */
void plat_cpu_bench(void);        /* test: the CPU timed at some clock settings */
double plat_refresh_hz(void);      /* the display (Vita: 59.94) */
double plat_frame_interval(void);  /* real seconds between the last two frames */
int plat_vsync_on(void);
void plat_after_shot(void);       /* test (PC): LSD_SHOTFREEZE stands the game still after a screenshot */
double plat_audio_latency(void); /* seconds between the mixer taking a sample and it being heard */
double plat_seconds(void);
const char *plat_data_dir(void);  /* read-only game data */
GLuint plat_backbuffer(void);      /* framebuffer object the frame ends up in */
const char *plat_write_dir(void); /* logs */
void plat_present_message(const char *msg); /* fatal error screen */
void plat_set_vsync(int on);
void plat_log_memory(const char *when);
void plat_set_gpu_clock(int mhz); /* 0 = leave as is; logs the clocks */
void plat_check_clocks(void);      /* log (and undo) any change the system made to the clocks since start */
void plat_keep_awake(void);        /* reset the system's idle timers (no dimming while a video plays) */
void plat_test_keep_awake(int on); /* test runs keep the system awake (default); 0 = let it idle */
/* a background worker: Vita, a thread on the second core; PC, the job runs at once (repeatable tests) */
typedef void (*WorkFn)(void *arg);
int plat_worker_start(WorkFn fn, void *arg); /* 0 if no thread could be made */
void plat_worker_kick(void);                 /* run fn (again) */
void plat_worker_stop(void);                 /* wait for the current run, end the thread */
void plat_worker_lock(void);                 /* guards state shared with the worker */
void plat_worker_unlock(void);
/* one-off jobs: Vita, a thread on the given core (1 or 2) until it returns; PC, fn runs at once */
int plat_task_start(WorkFn fn, void *arg, int core); /* handle for plat_task_join (-1: ran here) */
void plat_task_join(int task);
void plat_task_yield(void); /* let other threads run while waiting on a task */
void plat_wait_until(double t);  /* sleep until plat_seconds() reaches t (PC: no wait) */
const char *plat_shot_dir(void);
void plat_open_url(const char *url); /* Vita: the web browser (the game is suspended); PC: logged */
/* the system keyboard (Vita: the IME dialog over the game; PC: tests give the text with "cmd imetext") */
int plat_text_input_begin(const char *title, const char *initial, int max_len); /* 0 if it cannot */
int plat_text_input_poll(char *out, size_t n); /* 0 still typing, 1 entered (out), -1 cancelled */
void plat_text_input_set(const char *text);    /* tests: what the next entry will be (no keyboard shown) */
void plat_text_input_abort(void);              /* test: close the keyboard */

/* ---------------------------------------------------------------- audio output (44100 Hz stereo) */
typedef void (*AudioMixFn)(int16_t *out, int frames);
void plat_audio_start(AudioMixFn mix); /* Vita: an output thread calls mix; PC: plat_audio_pump does */
void plat_audio_stop(void);
void plat_audio_pump(float dt);        /* PC: mix dt worth (and record it with --audio) */
void plat_audio_lock(void);            /* guards state shared with the mixer */
void plat_audio_unlock(void);
double plat_audio_load(void);          /* share of real time the mixer took recently (0..1) */
double plat_audio_lock_wait_ms(void);  /* time any thread spent waiting for that lock since last asked */

/* ---------------------------------------------------------------- game data */
int vfs_init(const char *pak_path, const char *dir); /* pak_path NULL = plain files in dir */
uint8_t *vfs_read(const char *name, size_t *size);  /* malloc'd; NULL on failure */
uint8_t *vfs_read_quiet(const char *name, size_t *size); /* the same, without logging a missing file */
uint8_t *vfs_read_bg(const char *name, size_t *size);    /* vfs_read for a loading thread */
int vfs_add_pak(const char *pak_path);              /* another pack (moviesN.pak); 0 if missing */
int vfs_add_pak_quiet(const char *pak_path);        /* the same, without logging a missing file */
void vfs_check_crc(int on);                         /* test: skip the data checks */
/* streamed reading: a reader thread loads the entry while vfs_stream_need unpacks what has arrived */
typedef struct VfsStream VfsStream;
VfsStream *vfs_stream_open(const char *name);      /* NULL if missing */
size_t vfs_stream_size(const VfsStream *s);
const uint8_t *vfs_stream_need(VfsStream *s, size_t upto); /* the whole buffer, with [0, upto) ready; NULL on error */
int vfs_stream_check(VfsStream *s);                        /* waits for all of it, checks it; 0 if damaged */
uint8_t *vfs_stream_take(VfsStream *s, size_t *size);      /* ends the stream; its buffer (caller frees) */
uint8_t *vfs_stream_finish(VfsStream *s, size_t *size);    /* check + take; NULL (and freed) if damaged */
void vfs_stream_abort(VfsStream *s);
const char *plat_pak_path(void);                    /* where dreams.pak lives (NULL = use plain files) */

/* small deterministic RNG (xorshift32) shared by the game */
void rng_seed(uint32_t s);
uint32_t rng_u32(void);
int rng_int(int n);                 /* 0..n-1 */
float rng_float(float lo, float hi); /* [lo, hi) */

/* ---------------------------------------------------------------- scripted input */
int script_open(const char *path);
int script_active(void);
int script_input(Input *in);          /* 0 = script finished */
const char *script_take_shot(void);   /* name of a screenshot due this frame, or NULL */
void shot_write(const char *name, int w, int h);

/* ---------------------------------------------------------------- maths */
typedef struct { float x, y, z; } vec3;

static inline vec3 v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec3 v3add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3cross(vec3 a, vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3len(vec3 a) { return sqrtf(v3dot(a, a)); }

#define DEG2RAD 0.017453292519943295f

/* column-major 4x4, as GL expects */
typedef struct { float m[16]; } mat4;
mat4 mat4_mul(mat4 a, mat4 b);
mat4 mat4_perspective(float fovy_deg, float aspect, float znear, float zfar);
mat4 mat4_view(vec3 eye, vec3 right, vec3 up, vec3 fwd);

/* ---------------------------------------------------------------- game */
int game_init(int width, int height);
void game_update(const Input *in, float dt);
void game_render(void);
void game_command(const char *cmd);
void game_frame_stats(double frame_s, double cpu_s);
/* a frame that missed the display's beat: what it spent its time on, and whether it could be seen */
void game_hitch(double frame_s, double update_s, double render_s, double swap_s);
int game_screen_covered(void); /* a fade covers the whole screen (a long frame cannot be seen) */
int game_frame_limit(void); /* frames a second to hold to (0 = as fast as vsync) */
void game_query(float *x, float *y, float *z, int *grounded, int *wall);
void game_shutdown(void);
void game_request_quit(void);
int game_quit_requested(void);

/* debug text overlay */
int text_init(void);
void text_begin(int screen_w, int screen_h);
void text_draw(float x, float y, float scale, uint32_t rgba, const char *s);
void text_end(void);

#endif
