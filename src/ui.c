/* LSD Revamped's Unity UI screens (the title screen, the graph, the credits, the options, the pause
 * menu), laid out once by tools/uibake.py the way Unity's layout system does it at 960x544, and
 * drawn here as coloured, textured quads: Image / RawImage (Graphic colour x the Selectable's
 * ColorTint), Text (glyph atlases from the game's fonts, Shadow effect), UIGraph's squares, and the
 * moving parts of controls (a toggle's checkmark, a slider's fill and handle). On the Vita the options
 * are baked larger, with a highlight behind the selected control's row, and a bar of button prompts
 * (icons drawn here) under them; the touch screen finds controls with ui_control_at. */
#include "ui.h"
#include "render.h"

typedef struct {
    int kind, view, tex, font;
    float x0, y0, x1, y1;
    float col[4];
    int tag, text;
    unsigned align;
    float sdx, sdy, sa;
    int group;            /* tab panel / scrolling content it belongs to (-1 none) */
    int vis;              /* toggle whose checkmark this is (-1 none) */
    int follow, follow_mode; /* slider: 1 its fill, 2 its handle (and what is on it); 3: the scrollbar
                              * handle of scrolling content (follow: its group) */
    float follow_base;    /* the handle's anchor as laid out */
    int ctl;              /* the control whose text this is (slider value, dropdown caption; -1 none) */
    float rot, px, py;    /* rotation about the pivot (degrees, Unity's counter-clockwise) */
} UiElem;

typedef struct {
    UiControlInfo i;
    float col[4][4]; /* normal, highlighted (= selected), pressed, disabled */
    float mult;
    int check, fill, handle, text, caption;
    float farea[2], foff[2], harea[2], hoff[2];
} UiButton;

typedef struct {
    int view, kind, tab, visible; /* kind: 0 a tab's panel, 1 scrolling content, 2 a game-shown panel */
    float height;
    int parent;                   /* shown only while this group is too (-1 none) */
    float clip[4];                /* scrolling content: its viewport (x0 y0 x1 y1; all 0 = not clipped) */
    float bar[4];                 /* its scrollbar: sliding area bottom, height, handle offsets (min, max) */
} UiGroup;

typedef struct {
    uint32_t cp;
    float adv;
    int16_t x, y, w, h, bx, by;
} UiGlyph;

typedef struct {
    float px, asc, lh;
    int n, aw, ah;
    UiGlyph *g;
    GLuint tex;
} UiFont;

#define UI_MAX_VIEWS 16
static struct {
    int nview;
    char vname[UI_MAX_VIEWS][24];
    int vfirst[UI_MAX_VIEWS], vcount[UI_MAX_VIEWS];
    int nelem;
    UiElem *e;
    int nbtn;
    UiButton b[UI_MAX_CTL];
    int ngroup;
    UiGroup g[UI_MAX_GROUPS];
    int nstr;
    char **str;
    int nfont;
    UiFont *f;
    int ntex;
    GLuint *tex;
    GLuint white;
    GLuint icon[UI_ICONS]; /* button prompt icons (made at load) */
    int prompt_font;       /* a font with all of printable ASCII, about prompt size (-1 none) */
    int year_icon; /* element (kind 4) */
    int ok;
} U;

/* ---------------------------------------------------------------- drawing: batched quads */

typedef struct {
    float x, y, u, v;
    uint32_t rgba;
} UiVert;

#define UI_MAX_QUADS 4096
static UiVert g_v[UI_MAX_QUADS * 6];
static int g_nv;
static GLuint g_prog, g_cur_tex;
static GLint g_scale_loc, g_tex_loc;
static int g_sw, g_sh;

#ifdef __vita__
static const char *CG_UI_VS =
    "uniform float2 uScale;\n"
    "void main(float2 aPos, float2 aUV, float4 aCol, float4 out gl_Position : POSITION,\n"
    "  float2 out vUV : TEXCOORD0, float4 out vCol : TEXCOORD1)\n"
    "{\n"
    "  gl_Position = float4(aPos.x * uScale.x - 1.0f, 1.0f - aPos.y * uScale.y, 0.0f, 1.0f);\n"
    "  vUV = aUV;\n"
    "  vCol = aCol;\n"
    "}\n";
static const char *CG_UI_FS =
    "uniform sampler2D uTex;\n"
    "float4 main(float2 vUV : TEXCOORD0, float4 vCol : TEXCOORD1) : COLOR\n"
    "{\n"
    "  return tex2D(uTex, vUV) * vCol;\n"
    "}\n";
#endif
static const char *GL_UI_VS =
    "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aCol; uniform vec2 uScale;\n"
    "varying vec2 vUV; varying vec4 vCol;\n"
    "void main() { gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);\n"
    "  vUV = aUV; vCol = aCol; }\n";
static const char *GL_UI_FS =
    "precision mediump float; uniform sampler2D uTex; varying vec2 vUV; varying vec4 vCol;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV) * vCol; }\n";

static void flush(void)
{
    if (!g_nv)
        return;
    glBindTexture(GL_TEXTURE_2D, g_cur_tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVert), &g_v[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVert), &g_v[0].u);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UiVert), &g_v[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, g_nv);
    g_nv = 0;
}

static uint32_t rgba32(const float c[4])
{
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        out |= (uint32_t)(v * 255.0f + 0.5f) << (i * 8);
    }
    return out;
}

/* the element being drawn is rotated (a RectTransform's Z rotation about its pivot) */
static int g_rotated;
static float g_rc, g_rs, g_rpx, g_rpy;

/* (u, v) with v = 0 at the bottom row, as the textures are stored */
static float g_shear, g_shear_base; /* italic text: x moved right by this much per pixel above the baseline */
static int g_clipping;
static float g_clip[4]; /* (a Mask: what is drawn outside it is cut away) */

static void quad(GLuint tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 const float c[4])
{
    if (c[3] <= 0.002f)
        return;
    if (g_clipping && !g_rotated && g_shear == 0) { /* (cut to the mask, the texture with it) */
        if (x1 <= g_clip[0] || x0 >= g_clip[2] || y1 <= g_clip[1] || y0 >= g_clip[3])
            return;
        float w = x1 - x0, h = y1 - y0;
        if (x0 < g_clip[0]) { u0 += (u1 - u0) * (g_clip[0] - x0) / w; x0 = g_clip[0]; w = x1 - x0; }
        if (x1 > g_clip[2]) { u1 -= (u1 - u0) * (x1 - g_clip[2]) / w; x1 = g_clip[2]; }
        /* (v1 is the top's v, v0 the bottom's) */
        if (y0 < g_clip[1]) { v1 -= (v1 - v0) * (g_clip[1] - y0) / h; y0 = g_clip[1]; h = y1 - y0; }
        if (y1 > g_clip[3]) { v0 += (v1 - v0) * (y1 - g_clip[3]) / h; y1 = g_clip[3]; }
    } else if (g_clipping && (y1 <= g_clip[1] || y0 >= g_clip[3])) {
        return;
    }
    if (tex != g_cur_tex || g_nv + 6 > UI_MAX_QUADS * 6) {
        flush();
        g_cur_tex = tex;
    }
    uint32_t k = rgba32(c);
    UiVert q[4] = {{x0, y0, u0, v1, k}, {x1, y0, u1, v1, k}, {x1, y1, u1, v0, k}, {x0, y1, u0, v0, k}};
    if (g_shear != 0)
        for (int i = 0; i < 4; i++)
            q[i].x += (g_shear_base - q[i].y) * g_shear;
    if (g_rotated)
        for (int i = 0; i < 4; i++) {
            float dx = q[i].x - g_rpx, dy = q[i].y - g_rpy;
            q[i].x = g_rpx + dx * g_rc - dy * g_rs;
            q[i].y = g_rpy + dx * g_rs + dy * g_rc;
        }
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
        g_v[g_nv++] = q[idx[i]];
}

/* ---------------------------------------------------------------- loading */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Rd;

static const void *take(Rd *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return NULL;
    }
    const void *q = r->p;
    r->p += n;
    return q;
}

static uint32_t u32(Rd *r)
{
    uint32_t v = 0;
    const void *q = take(r, 4);
    if (q) memcpy(&v, q, 4);
    return v;
}

static float f32(Rd *r)
{
    float v = 0;
    const void *q = take(r, 4);
    if (q) memcpy(&v, q, 4);
    return v;
}

static GLuint make_tex(int w, int h, const void *rgba, int linear)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

/* ---------------------------------------------------------------- button prompt icons */

#define ICON_PX 48
/* coverage of a shape at (x, y) in -1..1 (y down) */
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay, t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return sqrtf(ex * ex + ey * ey);
}

static int icon_inside(int which, float x, float y, int *sym)
{
    float r = sqrtf(x * x + y * y);
    *sym = 0;
    switch (which) {
    case UI_ICON_CROSS:
        if (seg_dist(x, y, -0.42f, -0.42f, 0.42f, 0.42f) < 0.11f || seg_dist(x, y, -0.42f, 0.42f, 0.42f, -0.42f) < 0.11f) {
            *sym = 1;
            return 1;
        }
        return r < 0.98f;
    case UI_ICON_CIRCLE:
        if (fabsf(r - 0.44f) < 0.1f) {
            *sym = 1;
            return 1;
        }
        return r < 0.98f;
    case UI_ICON_TRIANGLE: {
        float ax = 0, ay = -0.5f, bx = 0.47f, by = 0.33f, cx = -0.47f, cy = 0.33f;
        if (seg_dist(x, y, ax, ay, bx, by) < 0.09f || seg_dist(x, y, bx, by, cx, cy) < 0.09f ||
            seg_dist(x, y, cx, cy, ax, ay) < 0.09f) {
            *sym = 1;
            return 1;
        }
        return r < 0.98f;
    }
    case UI_ICON_SQUARE:
        if (fabsf(fmaxf(fabsf(x), fabsf(y)) - 0.38f) < 0.09f) {
            *sym = 1;
            return 1;
        }
        return r < 0.98f;
    case UI_ICON_SHOULDER: /* a rounded tab (the letter is drawn over it) */
        return fabsf(x) < 0.95f && fabsf(y) < 0.72f &&
               (fabsf(x) < 0.7f || fabsf(y) < 0.47f || (fabsf(x) - 0.7f) * (fabsf(x) - 0.7f) + (fabsf(y) - 0.47f) * (fabsf(y) - 0.47f) < 0.0625f);
    case UI_ICON_DPAD_LR: case UI_ICON_DPAD_UD: { /* two arrowheads, left and right (or up and down) */
        float a = which == UI_ICON_DPAD_LR ? fabsf(x) : fabsf(y), b = which == UI_ICON_DPAD_LR ? y : x;
        /* (a triangle with its point at a = 0.95, its base at a = 0.2, half as wide as it is long) */
        if (a > 0.2f && a < 0.95f && fabsf(b) < (0.95f - a) * 0.75f) {
            *sym = 1;
            return 1;
        }
        return 0;
    }
    case UI_ICON_TOUCH: /* a fingertip ring */
        if (fabsf(r - 0.55f) < 0.12f) {
            *sym = 1;
            return 1;
        }
        return r < 0.3f ? (*sym = 1) : 0;
    default:
        return 0;
    }
}

static void make_icons(void)
{
    /* the face buttons' own colours (green triangle, red circle, blue cross, pink square) on dark grey */
    static const uint8_t SYM[UI_ICONS][3] = {{124, 178, 232}, {255, 102, 102}, {64, 226, 160}, {255, 105, 248},
                                             {230, 230, 230}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
    static uint8_t px[ICON_PX * ICON_PX * 4];
    for (int which = 0; which < UI_ICONS; which++) {
        for (int y = 0; y < ICON_PX; y++)
            for (int x = 0; x < ICON_PX; x++) {
                float cov = 0, sym = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        float fx = ((float)x + (sx + 0.5f) / 4.0f) / ICON_PX * 2.0f - 1.0f;
                        float fy = ((float)y + (sy + 0.5f) / 4.0f) / ICON_PX * 2.0f - 1.0f;
                        int s2;
                        if (icon_inside(which, fx, fy, &s2)) {
                            cov += 1.0f / 16.0f;
                            sym += s2 ? 1.0f / 16.0f : 0;
                        }
                    }
                /* (stored bottom row first, like the other textures) */
                uint8_t *p = px + ((size_t)(ICON_PX - 1 - y) * ICON_PX + x) * 4;
                float k = cov > 0 ? sym / cov : 0;
                float bg = which == UI_ICON_SHOULDER ? 70.0f : which >= UI_ICON_DPAD_LR ? 120.0f : 38.0f;
                for (int c = 0; c < 3; c++)
                    p[c] = (uint8_t)(bg + (SYM[which][c] - bg) * k + 0.5f);
                p[3] = (uint8_t)(cov * 255.0f + 0.5f);
            }
        U.icon[which] = make_tex(ICON_PX, ICON_PX, px, 1);
    }
}

int ui_load(void)
{
    if (U.ok)
        return 1;
    static const char *const attr[] = {"aPos", "aUV", "aCol"};
#ifdef __vita__
    g_prog = render_compile2(CG_UI_VS, CG_UI_FS, GL_UI_VS, GL_UI_FS, NULL, attr, 3);
#else
    g_prog = render_compile2(NULL, NULL, GL_UI_VS, GL_UI_FS, NULL, attr, 3);
#endif
    if (!g_prog)
        return 0;
    g_scale_loc = glGetUniformLocation(g_prog, "uScale");
    g_tex_loc = glGetUniformLocation(g_prog, "uTex");
    static const uint8_t WHITE[4] = {255, 255, 255, 255};
    U.white = make_tex(1, 1, WHITE, 0);

    size_t size = 0;
    uint8_t *buf = vfs_read("ui.bin", &size);
    if (!buf)
        return 0;
    Rd r = {buf, buf + size, 0};
    const uint8_t *magic = (const uint8_t *)take(&r, 4);
    if (!magic || memcmp(magic, "LSDU", 4) != 0 || u32(&r) != 3)
        r.bad = 1;
    U.nview = (int)u32(&r);
    if (U.nview > UI_MAX_VIEWS) r.bad = 1;
    for (int i = 0; i < U.nview && !r.bad; i++) {
        const char *n = (const char *)take(&r, 24);
        if (n) {
            memcpy(U.vname[i], n, 23);
            U.vname[i][23] = 0;
        }
        U.vfirst[i] = (int)u32(&r);
        U.vcount[i] = (int)u32(&r);
    }
    U.nelem = (int)u32(&r);
    if (U.nelem < 0 || U.nelem > 16384) r.bad = 1;
    if (!r.bad) U.e = (UiElem *)calloc(U.nelem ? U.nelem : 1, sizeof(UiElem));
    U.year_icon = -1;
    for (int i = 0; i < U.nelem && !r.bad && U.e; i++) {
        UiElem *e = &U.e[i];
        e->kind = (int)u32(&r);
        e->view = (int)u32(&r);
        e->tex = (int)u32(&r);
        e->font = (int)u32(&r);
        e->x0 = f32(&r);
        e->y0 = f32(&r);
        e->x1 = f32(&r);
        e->y1 = f32(&r);
        for (int k = 0; k < 4; k++) e->col[k] = f32(&r);
        e->tag = (int)u32(&r);
        e->text = (int)u32(&r);
        e->align = u32(&r);
        e->sdx = f32(&r);
        e->sdy = f32(&r);
        e->sa = f32(&r);
        e->group = (int)u32(&r);
        e->vis = (int)u32(&r);
        e->follow = (int)u32(&r);
        e->follow_mode = (int)u32(&r);
        e->follow_base = f32(&r);
        e->rot = f32(&r);
        e->px = f32(&r);
        e->py = f32(&r);
        e->ctl = -1;
        if (e->kind == 4) U.year_icon = i;
    }
    U.nbtn = (int)u32(&r);
    if (U.nbtn > UI_MAX_CTL) r.bad = 1;
    for (int i = 0; i < U.nbtn && !r.bad; i++) {
        UiButton *b = &U.b[i];
        UiControlInfo *c = &b->i;
        c->kind = (int)u32(&r);
        c->graphic = (int)u32(&r);
        for (int k = 0; k < 4; k++) c->nav[k] = (int)u32(&r);
        c->nav_mode = (int)u32(&r);
        c->action = (int)u32(&r);
        c->view = (int)u32(&r);
        c->group = (int)u32(&r);
        c->interactable = (int)u32(&r);
        c->setting = (int)u32(&r);
        for (int st = 0; st < 4; st++)
            for (int k = 0; k < 4; k++) b->col[st][k] = f32(&r);
        b->mult = f32(&r);
        for (int k = 0; k < 4; k++) c->rect[k] = f32(&r);
        b->check = (int)u32(&r);
        b->fill = (int)u32(&r);
        b->handle = (int)u32(&r);
        for (int k = 0; k < 2; k++) b->farea[k] = f32(&r);
        for (int k = 0; k < 2; k++) b->foff[k] = f32(&r);
        for (int k = 0; k < 2; k++) b->harea[k] = f32(&r);
        for (int k = 0; k < 2; k++) b->hoff[k] = f32(&r);
        c->vmin = f32(&r);
        c->vmax = f32(&r);
        c->whole = (int)u32(&r);
        b->text = (int)u32(&r);
        c->decimals = (int)u32(&r);
        int postfix = (int)u32(&r);
        c->tscale = f32(&r);
        b->caption = (int)u32(&r);
        c->selectme = (int)u32(&r);
        u32(&r);
        b->i.postfix = (const char *)(intptr_t)postfix; /* (a string index until the strings are in) */
    }
    U.ngroup = (int)u32(&r);
    if (U.ngroup > UI_MAX_GROUPS) r.bad = 1;
    for (int i = 0; i < U.ngroup && !r.bad; i++) {
        UiGroup *g = &U.g[i];
        g->view = (int)u32(&r);
        g->kind = (int)u32(&r);
        g->tab = (int)u32(&r);
        g->visible = (int)u32(&r);
        g->height = f32(&r);
        g->parent = (int)u32(&r);
        for (int k = 0; k < 4; k++) g->clip[k] = f32(&r);
        for (int k = 0; k < 4; k++) g->bar[k] = f32(&r);
        if (g->parent >= i) r.bad = 1; /* (a group's parent comes before it) */
    }
    U.nstr = (int)u32(&r);
    if (U.nstr < 0 || U.nstr > 4096) r.bad = 1;
    if (!r.bad) U.str = (char **)calloc(U.nstr ? U.nstr : 1, sizeof(char *));
    for (int i = 0; i < U.nstr && !r.bad && U.str; i++) {
        uint32_t n = u32(&r);
        const char *s = (const char *)take(&r, n);
        take(&r, (4 - n % 4) % 4);
        if (!s) break;
        U.str[i] = (char *)malloc(n + 1);
        memcpy(U.str[i], s, n);
        U.str[i][n] = 0;
    }
    U.nfont = (int)u32(&r);
    if (U.nfont < 0 || U.nfont > 64) r.bad = 1;
    if (!r.bad) U.f = (UiFont *)calloc(U.nfont ? U.nfont : 1, sizeof(UiFont));
    for (int i = 0; i < U.nfont && !r.bad && U.f; i++) {
        UiFont *f = &U.f[i];
        f->px = f32(&r);
        f->asc = f32(&r);
        f->lh = f32(&r);
        f->n = (int)u32(&r);
        f->aw = (int)u32(&r);
        f->ah = (int)u32(&r);
        if (f->n < 0 || f->n > 4096 || f->aw <= 0 || f->ah <= 0 || f->aw > 4096 || f->ah > 4096) {
            r.bad = 1;
            break;
        }
        f->g = (UiGlyph *)calloc(f->n ? f->n : 1, sizeof(UiGlyph));
        for (int k = 0; k < f->n && !r.bad; k++) {
            f->g[k].cp = u32(&r);
            f->g[k].adv = f32(&r);
            const int16_t *h = (const int16_t *)take(&r, 12);
            if (!h) break;
            int16_t v[6];
            memcpy(v, h, 12);
            f->g[k].x = v[0];
            f->g[k].y = v[1];
            f->g[k].w = v[2];
            f->g[k].h = v[3];
            f->g[k].bx = v[4];
            f->g[k].by = v[5];
        }
        size_t an = (size_t)f->aw * f->ah;
        const uint8_t *a = (const uint8_t *)take(&r, an);
        take(&r, (4 - an % 4) % 4);
        if (!a) break;
        /* white, coverage in alpha; rows flipped to bottom-first like the other textures */
        uint8_t *rgba = (uint8_t *)malloc(an * 4);
        if (!rgba) {
            r.bad = 1;
            break;
        }
        for (int y = 0; y < f->ah; y++)
            for (int x = 0; x < f->aw; x++) {
                uint8_t *p = rgba + ((size_t)(f->ah - 1 - y) * f->aw + x) * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = a[(size_t)y * f->aw + x];
            }
        f->tex = make_tex(f->aw, f->ah, rgba, 0);
        free(rgba);
    }
    U.ntex = (int)u32(&r);
    if (U.ntex < 0 || U.ntex > 256) r.bad = 1;
    if (!r.bad) U.tex = (GLuint *)calloc(U.ntex ? U.ntex : 1, sizeof(GLuint));
    for (int i = 0; i < U.ntex && !r.bad && U.tex; i++) {
        uint32_t w = u32(&r), h = u32(&r);
        if (!w || !h || w > 4096 || h > 4096) {
            r.bad = 1;
            break;
        }
        const void *px = take(&r, (size_t)w * h * 4);
        if (!px) break;
        U.tex[i] = make_tex((int)w, (int)h, px, 1);
    }
    free(buf);
    for (int i = 0; i < U.nelem && !r.bad; i++) {
        UiElem *e = &U.e[i];
        if (e->tex >= U.ntex || e->font >= U.nfont || e->text >= U.nstr || e->view >= U.nview)
            r.bad = 1;
    }
    for (int i = 0; i < U.nelem && !r.bad; i++) {
        UiElem *e = &U.e[i];
        if (e->group >= U.ngroup || e->vis >= U.nbtn || (e->follow_mode == 3 ? e->follow >= U.ngroup : e->follow >= U.nbtn))
            r.bad = 1;
    }
    for (int i = 0; i < U.nbtn && !r.bad; i++) {
        UiButton *b = &U.b[i];
        intptr_t pf = (intptr_t)b->i.postfix;
        b->i.postfix = pf >= 0 && pf < U.nstr ? U.str[pf] : "";
        if (b->i.graphic >= U.nelem || b->check >= U.nelem || b->fill >= U.nelem || b->handle >= U.nelem ||
            b->text >= U.nelem || b->caption >= U.nelem || b->i.group >= U.ngroup || b->i.view >= U.nview)
            r.bad = 1;
        for (int k = 0; k < 4; k++)
            if (b->i.nav[k] >= U.nbtn) r.bad = 1;
        if (r.bad) break;
        if (b->text >= 0) U.e[b->text].ctl = i;
        if (b->caption >= 0) U.e[b->caption].ctl = i;
    }
    if (r.bad) {
        lsd_log("ui.bin is damaged");
        return 0;
    }
    make_icons();
    U.prompt_font = -1; /* (the nearest to 19 px of the fonts with all of printable ASCII) */
    for (int i = 0; i < U.nfont; i++) {
        int all = 1;
        for (uint32_t cp = 32; cp < 127 && all; cp++) {
            int found = 0;
            for (int k = 0; k < U.f[i].n && !found; k++)
                found = U.f[i].g[k].cp == cp;
            all = found;
        }
        if (all && (U.prompt_font < 0 || fabsf(U.f[i].px - 19.0f) < fabsf(U.f[U.prompt_font].px - 19.0f)))
            U.prompt_font = i;
    }
    lsd_log("ui: %d views, %d elements, %d controls, %d groups, %d fonts, %d textures", U.nview, U.nelem, U.nbtn,
            U.ngroup, U.nfont, U.ntex);
    U.ok = 1;
    return 1;
}

int ui_view(const char *name)
{
    for (int i = 0; i < U.nview; i++)
        if (!strcmp(U.vname[i], name))
            return i;
    return -1;
}

int ui_button_count(void) { return U.nbtn; }
int ui_button_action(int b) { return b >= 0 && b < U.nbtn ? U.b[b].i.action : 0; }
int ui_button_view(int b) { return b >= 0 && b < U.nbtn ? U.b[b].i.view : -1; }
int ui_button_nav(int b, int dir) { return b >= 0 && b < U.nbtn && dir >= 0 && dir < 4 ? U.b[b].i.nav[dir] : -1; }
const UiControlInfo *ui_control(int b) { return b >= 0 && b < U.nbtn ? &U.b[b].i : NULL; }
int ui_group_count(void) { return U.ngroup; }
int ui_group_tab(int g) { return g >= 0 && g < U.ngroup && U.g[g].kind == 0 ? U.g[g].tab : -1; }
int ui_group_panel(int g) { return g >= 0 && g < U.ngroup && U.g[g].kind == 2 ? U.g[g].tab : 0; }
float ui_group_clip_height(int g) { return g >= 0 && g < U.ngroup ? U.g[g].clip[3] - U.g[g].clip[1] : 0; }

int ui_control_group_in(int b, int g)
{
    if (b < 0 || b >= U.nbtn)
        return 0;
    for (int h = U.b[b].i.group; h >= 0; h = U.g[h].parent)
        if (h == g)
            return 1;
    return 0;
}

/* how far a group's content has scrolled up (its own and the groups it is in) */
static float scrolled(const UiState *st, int g)
{
    float dy = 0;
    for (; g >= 0; g = U.g[g].parent)
        if (U.g[g].kind == 1)
            dy += floorf(st->scroll[g] + 0.5f);
    return dy;
}

/* the mask over a group's elements (the nearest clipped group it is in); 0 if none */
static int clip_of(int g, float out[4])
{
    for (; g >= 0; g = U.g[g].parent)
        if (U.g[g].kind == 1 && U.g[g].clip[2] > U.g[g].clip[0]) {
            memcpy(out, U.g[g].clip, sizeof(float) * 4);
            return 1;
        }
    return 0;
}

/* a control where it is now (scrolled) */
static void ctl_rect(const UiState *st, int b, float r[4])
{
    memcpy(r, U.b[b].i.rect, sizeof(float) * 4);
    float dy = scrolled(st, U.b[b].i.group);
    r[1] -= dy;
    r[3] -= dy;
}

int ui_scroll_to_show(int b, const UiState *st, float *scroll)
{
    if (b < 0 || b >= U.nbtn)
        return -1;
    for (int g = U.b[b].i.group; g >= 0; g = U.g[g].parent) {
        if (U.g[g].kind != 1 || U.g[g].clip[2] <= U.g[g].clip[0])
            continue;
        const float *c = U.g[g].clip;
        float r[4];
        ctl_rect(st, b, r);
        float s = st->scroll[g];
        if (r[1] < c[1]) s -= c[1] - r[1];
        else if (r[3] > c[3]) s += r[3] - c[3];
        float max = U.g[g].height - (c[3] - c[1]);
        if (s > max) s = max;
        if (s < 0) s = 0;
        *scroll = s;
        return g;
    }
    return -1;
}
int ui_group_view(int g) { return g >= 0 && g < U.ngroup ? U.g[g].view : -1; }
float ui_group_height(int g) { return g >= 0 && g < U.ngroup ? U.g[g].height : 0; }

int ui_button_by_action(int view, int action)
{
    for (int i = 0; i < U.nbtn; i++)
        if (U.b[i].i.action == action && U.b[i].i.view == view)
            return i;
    return -1;
}

void ui_state_init(UiState *st, int view)
{
    memset(st, 0, sizeof(*st));
    st->alpha = 1.0f;
    for (int g = 0; g < U.ngroup; g++)
        st->group_on[g] = (uint8_t)U.g[g].visible;
    for (int b = 0; b < U.nbtn; b++)
        st->state[b] = U.b[b].i.interactable ? UI_BTN_NORMAL : UI_BTN_DISABLED;
    (void)view;
}

static int shown(const UiState *st, int group)
{
    for (; group >= 0; group = U.g[group].parent)
        if (!st->group_on[group])
            return 0;
    return 1;
}

/* a control that can be gone to: in a shown group, and (inside scrolling content other than
 * from's own) at least partly in view */
static int reachable(const UiState *st, int i, int from_group)
{
    const UiControlInfo *o = &U.b[i].i;
    if (!shown(st, o->group))
        return 0;
    float c[4];
    if (o->group != from_group && clip_of(o->group, c)) {
        float r[4];
        ctl_rect(st, i, r);
        if (r[3] <= c[1] || r[1] >= c[3])
            return 0;
    }
    return 1;
}

/* Selectable.FindSelectableOnUp/Down/Left/Right */
int ui_navigate(int view, const UiState *st, int from, int dir)
{
    if (from < 0 || from >= U.nbtn || dir < 0 || dir > 3)
        return -1;
    const UiControlInfo *c = &U.b[from].i;
    float cr[4], orr[4], pr[4];
    ctl_rect(st, from, cr);
    int mode = c->nav_mode;
    if (mode == 4) {
        int t = c->nav[dir];
        if (t >= 0)
            return shown(st, U.b[t].i.group) ? t : -1; /* (Navigate: only to an active object) */
        /* no link that way: LSDR's options would need the mouse here (the tab bar has no way down).
         * Down from outside the panels goes to the first control below (the top of the tab shown);
         * otherwise, the nearest control as automatic navigation finds it */
        if (dir == 1 && c->group < 0) {
            int pick = -1;
            for (int i = 0; i < U.nbtn; i++) {
                const UiControlInfo *o = &U.b[i].i;
                if (i == from || o->view != view || o->nav_mode == 0 || !reachable(st, i, c->group) ||
                    st->state[i] == UI_BTN_DISABLED)
                    continue;
                ctl_rect(st, i, orr);
                if (orr[1] <= cr[3] - 1)
                    continue;
                if (pick >= 0)
                    ctl_rect(st, pick, pr);
                if (pick < 0 || orr[1] < pr[1] - 0.5f || (fabsf(orr[1] - pr[1]) <= 0.5f && orr[0] < pr[0]))
                    pick = i;
            }
            return pick;
        }
        mode = 3;
    }
    if (mode == 0 || (mode == 1 && dir < 2) || (mode == 2 && dir >= 2))
        return -1;
    /* Automatic navigation. Unity scores dot / distance^2 from the edge, which in LSDR's options
     * (toggles to the right of their rows) skips rows; the pad here takes the nearest control that
     * way instead: the gap along the way plus the gap across it. Inside a tab, the tab's own controls
     * first; up out of a tab, its tab button. */
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        if (pass == 0 && c->group < 0)
            continue;
        if (pass == 1 && dir == 0 && c->group >= 0) {
            int tab = -1;
            for (int g = c->group; g >= 0 && tab < 0; g = U.g[g].parent)
                tab = ui_group_tab(g);
            for (int i = 0; i < U.nbtn && tab >= 0; i++) /* (the button of the tab shown) */
                if (U.b[i].i.view == view && U.b[i].i.action == UI_ACT_TAB + tab && st->state[i] != UI_BTN_DISABLED)
                    return i;
        }
        float best = 1e30f;
        for (int i = 0; i < U.nbtn; i++) {
            const UiControlInfo *o = &U.b[i].i;
            if (i == from || o->view != view || o->nav_mode == 0 || !reachable(st, i, c->group) ||
                st->state[i] == UI_BTN_DISABLED || (pass == 0 && !ui_control_group_in(i, c->group)))
                continue;
            ctl_rect(st, i, orr);
            const float *r = cr, *q = orr;
            float along, across, beyond;
            if (dir < 2) { /* up / down */
                beyond = dir == 1 ? (q[1] + q[3]) - (r[1] + r[3]) : (r[1] + r[3]) - (q[1] + q[3]);
                along = dir == 1 ? q[1] - r[3] : r[1] - q[3];
                across = fmaxf(q[0], r[0]) - fminf(q[2], r[2]);
            } else {
                beyond = dir == 3 ? (q[0] + q[2]) - (r[0] + r[2]) : (r[0] + r[2]) - (q[0] + q[2]);
                along = dir == 3 ? q[0] - r[2] : r[0] - q[2];
                across = fmaxf(q[1], r[1]) - fminf(q[3], r[3]);
            }
            if (beyond <= 1.0f)
                continue; /* (its centre is not that way) */
            float score = fmaxf(along, 0) + fmaxf(across, 0);
            if (score < best) {
                best = score;
                pick = i;
            }
        }
    }
    return pick;
}

/* ---------------------------------------------------------------- text */

static const UiGlyph *glyph(const UiFont *f, uint32_t cp)
{
    for (int i = 0; i < f->n; i++)
        if (f->g[i].cp == cp)
            return &f->g[i];
    return NULL;
}

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) {
        c = ((c & 7) << 18) | ((uint32_t)(p[0] & 63) << 12) | ((uint32_t)(p[1] & 63) << 6) | (p[2] & 63);
        p += 3;
    } else if (c >= 0xE0 && p[0] && p[1]) {
        c = ((c & 15) << 12) | ((uint32_t)(p[0] & 63) << 6) | (p[1] & 63);
        p += 2;
    } else if (c >= 0xC0 && p[0]) {
        c = ((c & 31) << 6) | (p[0] & 63);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

/* Unity Text's rich text: <b> and <i> (taking no room); returns the tag's length (0 if not one) */
static int rich_tag(const char *s, const char *end, int *bold, int *italic)
{
    static const char *const TAGS[4] = {"<b>", "</b>", "<i>", "</i>"};
    for (int t = 0; t < 4; t++) {
        size_t n = strlen(TAGS[t]);
        if ((size_t)(end - s) >= n && !memcmp(s, TAGS[t], n)) {
            if (t < 2) *bold += t == 0 ? 1 : -1;
            else *italic += t == 2 ? 1 : -1;
            return (int)n;
        }
    }
    return 0;
}

static float line_width(const UiFont *f, const char *s, const char *end)
{
    float w = 0;
    int b = 0, it = 0, n;
    while (s < end) {
        if (*s == '<' && (n = rich_tag(s, end, &b, &it)) > 0) {
            s += n;
            continue;
        }
        const UiGlyph *g = glyph(f, next_cp(&s));
        if (g) w += g->adv;
    }
    return w;
}

/* Unity Text with a TextAnchor alignment, one line per '\n'; bold drawn twice a pixel apart and
 * italic slanted, as Unity makes them for a font with no bold or italic face */
static void draw_text(const UiFont *f, const char *text, const UiElem *e, const float c[4], float dx, float dy)
{
    int nlines = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') nlines++;
    float total = f->lh * (float)nlines;
    int va = (int)e->align / 3, ha = (int)e->align % 3;
    float top = va == 0 ? e->y0 : va == 1 ? (e->y0 + e->y1 - total) * 0.5f : e->y1 - total;
    const char *ln = text;
    int bold = 0, italic = 0, n;
    for (int li = 0; li < nlines; li++) {
        const char *end = strchr(ln, '\n');
        if (!end) end = ln + strlen(ln);
        float w = line_width(f, ln, end);
        float x = ha == 0 ? e->x0 : ha == 1 ? (e->x0 + e->x1 - w) * 0.5f : e->x1 - w;
        float base = top + (float)li * f->lh + f->asc;
        const char *s = ln;
        while (s < end) {
            if (*s == '<' && (n = rich_tag(s, end, &bold, &italic)) > 0) {
                s += n;
                continue;
            }
            const UiGlyph *g = glyph(f, next_cp(&s));
            if (!g) continue;
            if (g->w > 0 && g->h > 0) {
                float gx = floorf(x + g->bx + dx + 0.5f), gy = floorf(base + g->by + dy + 0.5f);
                float u0 = (float)g->x / f->aw, u1 = (float)(g->x + g->w) / f->aw;
                float v1 = 1.0f - (float)g->y / f->ah, v0 = 1.0f - (float)(g->y + g->h) / f->ah;
                g_shear = italic > 0 ? 0.2f : 0.0f;
                g_shear_base = floorf(base + dy + 0.5f);
                quad(f->tex, gx, gy, gx + g->w, gy + g->h, u0, v0, u1, v1, c);
                if (bold > 0)
                    quad(f->tex, gx + 1, gy, gx + 1 + g->w, gy + g->h, u0, v0, u1, v1, c);
                g_shear = 0;
            }
            x += g->adv;
        }
        ln = *end ? end + 1 : end;
    }
}

/* ---------------------------------------------------------------- views */

static void tinted(const UiElem *e, const UiState *st, int idx, float out[4])
{
    memcpy(out, e->col, sizeof(float) * 4);
    if (e->tag > 0 && e->tag < UI_NTAGS && st->tag_color_set[e->tag])
        memcpy(out, st->tag_color[e->tag], sizeof(float) * 4);
    /* Selectable ColorTint: the Graphic's colour times the state's colour times the multiplier */
    for (int b = 0; b < U.nbtn; b++) {
        if (U.b[b].i.graphic != idx)
            continue;
        int s = st->state[b];
        const float *t = U.b[b].col[s == UI_BTN_SELECTED ? 1 : s == UI_BTN_PRESSED ? 2 : s == UI_BTN_DISABLED ? 3 : 0];
        for (int k = 0; k < 4; k++) {
            float v = t[k] * U.b[b].mult;
            out[k] *= v > 1 ? 1 : v;
        }
    }
    out[3] *= st->alpha;
}

static void draw_begin(int screen_w, int screen_h)
{
    g_sw = screen_w;
    g_sh = screen_h;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform2f(g_scale_loc, 2.0f / screen_w, 2.0f / screen_h);
    glUniform1i(g_tex_loc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    g_cur_tex = U.white;
    g_nv = 0;
}

static void draw_end(void)
{
    g_rotated = 0;
    g_clipping = 0;
    flush();
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisable(GL_BLEND);
}

/* the Vita's highlights (kind 5, baked behind each control's row): while the control is selected, the
 * row lit (flag 1) and a frame round the control; a tab's button marked while its tab is shown (flag 2) */
static const float HL_ROW[4] = {0.55f, 0.52f, 0.12f, 0.55f};
static const float HL_FRAME[4] = {1.0f, 0.86f, 0.25f, 1.0f};

static void frame(float x0, float y0, float x1, float y1, float t, const float c[4])
{
    quad(U.white, x0, y0, x1, y0 + t, 0, 0, 1, 1, c);
    quad(U.white, x0, y1 - t, x1, y1, 0, 0, 1, 1, c);
    quad(U.white, x0, y0 + t, x0 + t, y1 - t, 0, 0, 1, 1, c);
    quad(U.white, x1 - t, y0 + t, x1, y1 - t, 0, 0, 1, 1, c);
}

static void highlight(const UiElem *e, const UiState *st, int b)
{
    int s = st->state[b];
    float cr[4];
    ctl_rect(st, b, cr);
    if ((e->align & 2) && st->on[b]) { /* (the tab shown: a bar under its button) */
        float c[4] = {HL_FRAME[0], HL_FRAME[1], HL_FRAME[2], HL_FRAME[3] * st->alpha};
        quad(U.white, cr[0] + 3, cr[3] + 3, cr[2] - 3, cr[3] + 7, 0, 0, 1, 1, c);
    }
    if (s != UI_BTN_SELECTED && s != UI_BTN_PRESSED)
        return;
    if (e->align & 1) {
        float c[4] = {HL_ROW[0], HL_ROW[1], HL_ROW[2], HL_ROW[3] * st->alpha};
        quad(U.white, e->x0 - 6, e->y0, e->x1 + 6, e->y1, 0, 0, 1, 1, c);
    }
    float c[4] = {HL_FRAME[0], HL_FRAME[1], HL_FRAME[2], HL_FRAME[3] * st->alpha};
    frame(cr[0] - 4, cr[1] - 4, cr[2] + 4, cr[3] + 4, 2.5f, c);
}

void ui_draw(int view, const UiState *st, int screen_w, int screen_h)
{
    if (!U.ok || view < 0 || view >= U.nview)
        return;
    draw_begin(screen_w, screen_h);
    for (int i = U.vfirst[view]; i < U.vfirst[view] + U.vcount[view]; i++) {
        const UiElem *e0 = &U.e[i];
        if (!shown(st, e0->group) || (e0->vis >= 0 && !st->on[e0->vis]))
            continue;
        UiElem moved = *e0; /* (where the slider's value and any scrolling put it) */
        const UiElem *e = &moved;
        if (e0->follow >= 0 && (e0->follow_mode == 1 || e0->follow_mode == 2)) {
            const UiButton *b = &U.b[e0->follow];
            float v = st->value[e0->follow];
            if (e0->follow_mode == 1) { /* fill: anchorMax.x = value */
                moved.x0 = b->farea[0] + b->foff[0];
                moved.x1 = b->farea[0] + v * (b->farea[1] - b->farea[0]) + b->foff[1];
            } else { /* handle: anchors at value */
                float dx = b->harea[0] + v * (b->harea[1] - b->harea[0]) - e0->follow_base;
                moved.x0 += dx;
                moved.x1 += dx;
            }
        }
        if (e0->follow_mode == 3 && e0->follow >= 0) {
            /* Scrollbar.UpdateVisuals (BottomToTop): the handle's anchors at value * (1 - size) */
            const UiGroup *g = &U.g[e0->follow];
            float view = g->clip[3] - g->clip[1], max = g->height - view;
            float size = g->height > 0 ? view / g->height : 1, v = max > 0 ? 1 - st->scroll[e0->follow] / max : 1;
            if (size > 1) size = 1;
            float a0 = v * (1 - size), a1 = a0 + size;
            moved.y0 = floorf(g->bar[0] - a1 * g->bar[1] - g->bar[3] + 0.5f);
            moved.y1 = floorf(g->bar[0] - a0 * g->bar[1] - g->bar[2] + 0.5f);
        }
        if (e0->group >= 0) {
            float dy = scrolled(st, e0->group);
            moved.y0 -= dy;
            moved.y1 -= dy;
            if (dy != 0 && (moved.y1 < 0 || moved.y0 > screen_h))
                continue;
        }
        g_clipping = e0->group >= 0 && clip_of(e0->group, g_clip);
        float c[4];
        tinted(e0, st, i, c);
        g_rotated = e0->rot != 0;
        if (g_rotated) { /* (y is down here: Unity's counter-clockwise is a negative angle) */
            float a = -e0->rot * (float)M_PI / 180.0f;
            g_rc = cosf(a);
            g_rs = sinf(a);
            g_rpx = e0->px;
            g_rpy = e0->py;
        }
        if (e->kind == 0) {
            GLuint t = e->tex >= 0 ? U.tex[e->tex] : U.white;
            quad(t, e->x0, e->y0, e->x1, e->y1, 0, 0, 1, 1, c);
        } else if (e->kind == 1 && e->font >= 0) {
            const char *s = (e->tag > 0 && e->tag < UI_NTAGS && st->tag_text[e->tag]) ? st->tag_text[e->tag]
                            : (e->ctl >= 0 && st->ctl_text[e->ctl]) ? st->ctl_text[e->ctl]
                            : (e->text >= 0 ? U.str[e->text] : "");
            if (e->sa > 0) {
                float sc[4] = {0, 0, 0, e->sa * c[3]};
                draw_text(&U.f[e->font], s, e, sc, e->sdx, e->sdy);
            }
            draw_text(&U.f[e->font], s, e, c, 0, 0);
        } else if (e->kind == 2) {
            /* UIGraphSquare: 16 units square, anchored top-left at ((x + 9) * 16, (y - 9) * 16) */
            float k = e->sdx;
            for (int q = 0; q < st->nsquares; q++) {
                int mod = 5;
                for (int p = 0; p < q; p++)
                    if (st->sq_x[p] == st->sq_x[q] && st->sq_y[p] == st->sq_y[q]) mod--;
                if (mod < 1) mod = 1;
                float g = 0.2f * (float)mod;
                float sc[4] = {g, g, g, st->alpha};
                if (q == st->nsquares - 1 && fmodf(st->time, 0.25f) > 0.125f) {
                    sc[0] = 1;
                    sc[1] = sc[2] = 0;
                }
                float x0 = e->x0 + (float)(st->sq_x[q] + 9) * k, y0 = e->y0 - (float)(st->sq_y[q] - 9) * k;
                quad(U.white, floorf(x0 + 0.5f), floorf(y0 + 0.5f), floorf(x0 + k + 0.5f), floorf(y0 + k + 0.5f), 0, 0,
                     1, 1, sc);
            }
        } else if (e->kind == 5 && e0->follow >= 0) {
            highlight(e, st, e0->follow);
        } else if (e->kind == 3 && e->tag == UI_TAG_YEARS && U.year_icon >= 0 && st->years > 0) {
            /* UIYearsDisplay: one YearIcon per year, in an HLayout centred in the slot */
            const UiElem *ic = &U.e[U.year_icon];
            int n = st->years > 10 ? 1 : st->years;
            float w = ic->x1 - ic->x0, h = ic->y1 - ic->y0, sp = 2.0f * (w / 9.0f);
            float tw = n * w + (n - 1) * sp;
            float x = floorf((e->x0 + e->x1 - tw) * 0.5f + 0.5f), y = floorf((e->y0 + e->y1 - h) * 0.5f + 0.5f);
            float ic_c[4] = {ic->col[0], ic->col[1], ic->col[2], ic->col[3] * st->alpha};
            for (int k = 0; k < n; k++, x += w + sp) {
                if (ic->sa > 0) {
                    float sc[4] = {0.23f, 0.23f, 1.0f, ic->sa * st->alpha};
                    quad(U.tex[ic->tex], x + ic->sdx, y + ic->sdy, x + ic->sdx + w, y + ic->sdy + h, 0, 0, 1, 1, sc);
                }
                quad(U.tex[ic->tex], x, y, x + w, y + h, 0, 0, 1, 1, ic_c);
            }
        }
    }
    draw_end();
}

/* ---------------------------------------------------------------- the Vita's button prompts */

static int prompt_layout(const UiPrompt *p, int n, float y, float rects[][4], float *total)
{
    if (U.prompt_font < 0)
        return 0;
    const UiFont *f = &U.f[U.prompt_font];
    float x = 0, ih = 26.0f;
    for (int i = 0; i < n; i++) {
        float x0 = x;
        for (int k = 0; k < 2; k++)
            if (p[i].icon[k] >= 0)
                x += (p[i].icon[k] == UI_ICON_SHOULDER ? ih * 1.3f : ih) + 3.0f;
        x += 4.0f + line_width(f, p[i].label, p[i].label + strlen(p[i].label));
        rects[i][0] = x0;
        rects[i][1] = y;
        rects[i][2] = x;
        rects[i][3] = y + ih;
        x += 26.0f;
    }
    *total = x - 26.0f;
    return 1;
}

void ui_prompt_rects(const UiPrompt *p, int n, float cx, float y, float rects[][4])
{
    float total = 0;
    if (!prompt_layout(p, n, y, rects, &total))
        return;
    float x0 = floorf(cx - total * 0.5f + 0.5f);
    for (int i = 0; i < n; i++) {
        rects[i][0] += x0;
        rects[i][2] += x0;
    }
}

void ui_draw_prompts(const UiPrompt *p, int n, float cx, float y, float alpha, int screen_w, int screen_h)
{
    float r[UI_MAX_PROMPTS][4];
    if (n > UI_MAX_PROMPTS) n = UI_MAX_PROMPTS;
    if (!U.ok || n <= 0 || U.prompt_font < 0)
        return;
    ui_prompt_rects(p, n, cx, y, r);
    draw_begin(screen_w, screen_h);
    const UiFont *f = &U.f[U.prompt_font];
    float ih = 26.0f;
    float bg[4] = {0.05f, 0.03f, 0.12f, 0.6f * alpha};
    quad(U.white, r[0][0] - 12, y - 4, r[n - 1][2] + 12, y + ih + 4, 0, 0, 1, 1, bg);
    for (int i = 0; i < n; i++) {
        float x = r[i][0];
        for (int k = 0; k < 2; k++) {
            int ic = p[i].icon[k];
            if (ic < 0)
                continue;
            float w = ic == UI_ICON_SHOULDER ? ih * 1.3f : ih;
            float c[4] = {1, 1, 1, alpha};
            quad(U.icon[ic], x, y, x + w, y + ih, 0, 0, 1, 1, c);
            if (ic == UI_ICON_SHOULDER && p[i].letter[k]) {
                char t[2] = {p[i].letter[k], 0};
                UiElem e;
                memset(&e, 0, sizeof(e));
                e.x0 = x;
                e.x1 = x + w;
                e.y0 = y;
                e.y1 = y + ih;
                e.align = 4; /* (middle centre) */
                float tc[4] = {1, 1, 1, alpha};
                draw_text(f, t, &e, tc, 0, 0);
            }
            x += w + 3.0f;
        }
        UiElem e;
        memset(&e, 0, sizeof(e));
        e.x0 = x + 4.0f;
        e.x1 = r[i][2];
        e.y0 = y;
        e.y1 = y + ih;
        e.align = 3; /* (middle left) */
        float sc[4] = {0, 0, 0, 0.6f * alpha}, tc[4] = {1, 1, 1, alpha};
        draw_text(f, p[i].label, &e, sc, 1.5f, 1.5f);
        draw_text(f, p[i].label, &e, tc, 0, 0);
    }
    draw_end();
}

void ui_draw_label(const char *text, float cx, float cy, float alpha, int screen_w, int screen_h)
{
    if (!U.ok || U.prompt_font < 0 || !text || !text[0])
        return;
    const UiFont *f = &U.f[U.prompt_font];
    float w = line_width(f, text, text + strlen(text));
    draw_begin(screen_w, screen_h);
    float bg[4] = {0.05f, 0.03f, 0.12f, 0.75f * alpha};
    quad(U.white, cx - w * 0.5f - 14, cy - f->lh * 0.5f - 6, cx + w * 0.5f + 14, cy + f->lh * 0.5f + 6, 0, 0, 1, 1, bg);
    UiElem e;
    memset(&e, 0, sizeof(e));
    e.x0 = cx - w * 0.5f - 2;
    e.x1 = cx + w * 0.5f + 2;
    e.y0 = cy - f->lh;
    e.y1 = cy + f->lh;
    e.align = 4;
    float tc[4] = {HL_FRAME[0], HL_FRAME[1], HL_FRAME[2], alpha};
    draw_text(f, text, &e, tc, 0, 0);
    draw_end();
}

/* ---------------------------------------------------------------- the touch screen */

static int inside(const float r[4], float x, float y) { return x >= r[0] && x < r[2] && y >= r[1] && y < r[3]; }

int ui_control_at(int view, const UiState *st, float x, float y)
{
    if (!U.ok || view < 0 || view >= U.nview)
        return -1;
    int pick = -1;
    float best = 1e30f;
    for (int b = 0; b < U.nbtn; b++) { /* the control itself (the smallest one there) */
        const UiControlInfo *c = &U.b[b].i;
        if (c->view != view || c->nav_mode == 0 || st->state[b] == UI_BTN_DISABLED || !shown(st, c->group))
            continue;
        float r[4], cl[4];
        ctl_rect(st, b, r);
        if (clip_of(c->group, cl) && !inside(cl, x, y))
            continue;
        /* (a little larger than drawn: fingers are wider than the toggles) */
        float pad = fminf(10.0f, fmaxf(0.0f, (36.0f - (r[3] - r[1])) * 0.5f));
        float rr[4] = {r[0] - pad, r[1] - pad, r[2] + pad, r[3] + pad};
        float area = (r[2] - r[0]) * (r[3] - r[1]);
        if (inside(rr, x, y) && area < best) {
            best = area;
            pick = b;
        }
    }
    if (pick >= 0)
        return pick;
    for (int i = U.vfirst[view]; i < U.vfirst[view] + U.vcount[view]; i++) { /* or the row it heads */
        const UiElem *e = &U.e[i];
        if (e->kind != 5 || !(e->align & 1) || e->follow < 0 || !shown(st, e->group) ||
            st->state[e->follow] == UI_BTN_DISABLED)
            continue;
        float dy = e->group >= 0 ? scrolled(st, e->group) : 0, cl[4];
        float r[4] = {e->x0, e->y0 - dy, e->x1, e->y1 - dy};
        if (e->group >= 0 && clip_of(e->group, cl) && !inside(cl, x, y))
            continue;
        if (inside(r, x, y))
            return e->follow;
    }
    return -1;
}

float ui_slider_value_at(int b, float x)
{
    if (b < 0 || b >= U.nbtn || U.b[b].harea[1] <= U.b[b].harea[0])
        return -1;
    float t = (x - U.b[b].harea[0]) / (U.b[b].harea[1] - U.b[b].harea[0]);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

int ui_scroll_group_at(int view, const UiState *st, float x, float y)
{
    for (int g = 0; g < U.ngroup; g++)
        if (U.g[g].view == view && U.g[g].kind == 1 && U.g[g].clip[2] > U.g[g].clip[0] && shown(st, g) &&
            inside(U.g[g].clip, x, y))
            return g;
    return -1;
}
