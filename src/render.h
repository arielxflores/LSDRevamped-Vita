#ifndef LSD_RENDER_H
#define LSD_RENDER_H

#include "dream.h"

typedef struct {
    vec3 eye;
    vec3 right, up, fwd;
    float fovy;
    int classic;      /* LSDR "Classic" shaders: vertex snap, affine UVs, grid fog */
    int pixelate;     /* PixelateImageEffect: render at 224 lines */
    int dither;       /* PSX Dither post effect (needs pixelate) */
    float affine;     /* LSDR AffineIntensity (0.4 default) */
    int long_draw;    /* fog end x3 */
    int flashback;    /* FlashbackImageEffect: +0.4 on every channel */
    int texset;       /* 0..3 = Normal, Kanji, Downer, Upper */
    double time;      /* game time in seconds (the water shader's _Time) */
    const DreamEnv *env;
    float fade;       /* 0..1 fade overlay (ToriiFader) */
    float fade_rgb[3];
} View;

/* runtime switches for A/B performance tests (bench script "cmd r ...") */
typedef struct {
    float fog_mul;    /* scales fog distances */
    int no_sky, no_world;
    int dither_hires; /* dither at full screen resolution instead of the pixelated size */
    int half;         /* world shader colour maths in half precision */
    int proj;         /* world shader uses tex2Dproj */
    int old_dither;   /* the reference dither shader (CG_DITHER_OLD_FS) */
} RenderDebug;
extern RenderDebug g_rdbg;

typedef struct {
    int drawn_batches;
    int drawn_tris;
} RenderStats;

int render_init(int width, int height);
void render_frame(const Dream *d, const View *v, RenderStats *st); /* d NULL: black + fade only */
void render_picture(GLuint tex, int w, int h, const View *v); /* a video frame or a text day's screen */
void render_set_rv_scale(int percent); /* test: the Revamped graphics held at a size (% of the screen; 0: automatic) */
int render_rv_lower(void);  /* the GPU fell behind with the Revamped graphics: a step smaller (1 if so) */
void render_rv_reset(void); /* a new dream: full size again */
void render_clear(float r, float g, float b);              /* the backbuffer, for 2D screens */
void render_fade(const View *v);                            /* the fade overlay alone */
void render_shutdown(void);

void render_shader_cache(int on); /* Vita: keep compiled shaders in ux0:data/lsdr/shaders (default on) */
GLuint render_compile2(const char *cg_vs, const char *cg_fs, const char *gl_vs, const char *gl_fs,
                       const char *fs_prefix, const char *const *attribs, int nattr);

#endif
