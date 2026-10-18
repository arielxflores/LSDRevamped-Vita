/* Minimal bitmap text overlay (font8x8 by Daniel Hepper, public domain) */
#include "common.h"
#include "render.h"
#include "font8x8_basic.h"

#define MAX_CHARS 2048

typedef struct {
    float x, y, u, v;
    uint32_t rgba;
} TextVert;

static GLuint g_prog, g_tex;
static GLint g_scale_loc, g_tex_loc;
static TextVert g_verts[MAX_CHARS * 6];
static int g_count;
static int g_sw, g_sh;

#ifdef __vita__
static const char *CG_TEXT_VS =
    "uniform float2 uScale;\n"
    "void main(float2 aPos, float2 aUV, float4 aCol, float4 out gl_Position : POSITION,\n"
    "  float2 out vUV : TEXCOORD0, float4 out vCol : TEXCOORD1)\n"
    "{\n"
    "  gl_Position = float4(aPos.x * uScale.x - 1.0f, 1.0f - aPos.y * uScale.y, 0.0f, 1.0f);\n"
    "  vUV = aUV;\n"
    "  vCol = aCol;\n"
    "}\n";
static const char *CG_TEXT_FS =
    "uniform sampler2D uTex;\n"
    "float4 main(float2 vUV : TEXCOORD0, float4 vCol : TEXCOORD1) : COLOR\n"
    "{\n"
    "  float4 t = tex2D(uTex, vUV);\n"
    "  return float4(vCol.rgb, vCol.a * t.a);\n"
    "}\n";
#endif
static const char *GL_TEXT_VS =
    "attribute vec2 aPos; attribute vec2 aUV; attribute vec4 aCol; uniform vec2 uScale;\n"
    "varying vec2 vUV; varying vec4 vCol;\n"
    "void main() { gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);\n"
    "  vUV = aUV; vCol = aCol; }\n";
static const char *GL_TEXT_FS =
    "precision mediump float; uniform sampler2D uTex; varying vec2 vUV; varying vec4 vCol;\n"
    "void main() { vec4 t = texture2D(uTex, vUV); gl_FragColor = vec4(vCol.rgb, vCol.a * t.a); }\n";

int text_init(void)
{
    static const char *const attr[] = {"aPos", "aUV", "aCol"};
#ifdef __vita__
    g_prog = render_compile2(CG_TEXT_VS, CG_TEXT_FS, GL_TEXT_VS, GL_TEXT_FS, NULL, attr, 3);
#else
    g_prog = render_compile2(NULL, NULL, GL_TEXT_VS, GL_TEXT_FS, NULL, attr, 3);
#endif
    if (!g_prog)
        return 0;
    g_scale_loc = glGetUniformLocation(g_prog, "uScale");
    g_tex_loc = glGetUniformLocation(g_prog, "uTex");
    /* 16x8 glyph grid, 128x64 RGBA (white, alpha = glyph) */
    static uint8_t px[128 * 64 * 4];
    for (int ch = 0; ch < 128; ch++) {
        int gx = (ch % 16) * 8, gy = (ch / 16) * 8;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                int on = (font8x8_basic[ch][row] >> col) & 1;
                uint8_t *p = &px[((gy + row) * 128 + gx + col) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = on ? 255 : 0;
            }
    }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return 1;
}

void text_begin(int screen_w, int screen_h)
{
    g_sw = screen_w;
    g_sh = screen_h;
    g_count = 0;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c)
{
    if (g_count + 6 > MAX_CHARS * 6)
        return;
    TextVert q[4] = {{x0, y0, u0, v0, c}, {x1, y0, u1, v0, c}, {x1, y1, u1, v1, c}, {x0, y1, u0, v1, c}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
        g_verts[g_count++] = q[idx[i]];
}

void text_draw(float x, float y, float scale, uint32_t rgba, const char *s)
{
    float cx = x;
    float cw = 8 * scale;
    /* drop shadow first, then the text */
    for (int pass = 0; pass < 2; pass++) {
        float ox = pass ? 0 : scale, oy = pass ? 0 : scale;
        uint32_t col = pass ? rgba : 0xC0000000u;
        cx = x;
        float cy = y;
        for (const char *p = s; *p; p++) {
            unsigned ch = (unsigned char)*p;
            if (ch == '\n') {
                cx = x;
                cy += cw + 2 * scale;
                continue;
            }
            if (ch >= 128) ch = '?';
            if (ch != ' ') {
                float u0 = (ch % 16) * 8 / 128.0f, v0 = (ch / 16) * 8 / 64.0f;
                quad(cx + ox, cy + oy, cx + ox + cw, cy + oy + cw, u0, v0, u0 + 8 / 128.0f, v0 + 8 / 64.0f, col);
            }
            cx += cw;
        }
    }
}

void text_end(void)
{
    if (!g_count)
        return;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform2f(g_scale_loc, 2.0f / g_sw, 2.0f / g_sh);
    glUniform1i(g_tex_loc, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TextVert), &g_verts[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TextVert), &g_verts[0].u);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TextVert), &g_verts[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, g_count);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisable(GL_BLEND);
    g_count = 0;
}
