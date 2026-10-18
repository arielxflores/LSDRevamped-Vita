/* Dream renderer.
 *
 * Reproduces LSD Revamped's look:
 *   - ClassicDiffuseSet / RevampedDiffuseSet (LSDR.cginc, LSDRFog.cginc):
 *     vertex snapping, affine texture warp, tanh vertex-colour curve, grid fog
 *   - LSDR/GradientSky
 *   - PixelateImageEffect (render at 224 lines) + Jazz "PSX Dither" (16 levels, 4x4 ordered)
 *
 * Classic pipeline: sky + world into a 396x224 target, PSX dither into a second
 * 396x224 target (the dither is constant across each low-res pixel, so doing it at
 * low resolution gives the same picture for 1/6 of the work), then a plain nearest
 * upscale to the screen.
 *
 * Opaque geometry is split at bake time into "solid" (no transparent texels, drawn
 * with a shader that never discards, which keeps the Vita GPU's hidden-surface
 * removal working) and "cutout" (needs the alpha <= 0.1 discard).
 */
#include "render.h"
#include "skyfx.h"
#include "scene.h"

#ifdef __vita__
#define VS_TYPE GL_CG_VERTEX_SHADER_EXT
#define FS_TYPE GL_CG_FRAGMENT_SHADER_EXT
#else
#define VS_TYPE GL_VERTEX_SHADER
#define FS_TYPE GL_FRAGMENT_SHADER
#endif

#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif

#define LOWRES_H 224 /* PixelateImageEffect.Height on LSDR's Player prefab */

RenderDebug g_rdbg = {1.0f, 0, 0, 0, 0, 0, 0};

static int g_w, g_h;
static int g_low_w, g_low_h;
/* the Revamped graphics (no pixelation): drawn at the screen's full size; should the GPU fall behind
 * the display in a dream (frames missed while it draws), at 80%, then two thirds of it, filtered up,
 * until the next dream. (At the GPU's 111 MHz the full size took 13-19 ms a frame in the busier
 * dreams, two thirds 8-12 ms; the game runs it at 222 MHz.) */
#define RV_LEVELS 3
static const int RV_PERCENT[RV_LEVELS] = {100, 80, 67};
static struct {
    int w, h;
    GLuint fbo, tex, depth;
} g_rv[RV_LEVELS];
static int g_rv_level, g_rv_fixed = -1; /* (test: a size held, not lowered) */
static GLuint g_fbo[2], g_fbo_tex[2], g_fbo_depth;
static GLuint g_dither_tex;
static GLuint g_fullscreen_vbo;

typedef struct {
    GLuint prog;
    int tried;
    GLint mvp, view, model, affine, classic, fog_end;
    GLint tex, fog, fog_range, cam_i, vci, bright, tint;
    GLint pal, time, walpha; /* water */
} WorldProg;

/* index: bit0 cutout, bit1 half precision colour maths, bit2 projective texture lookup, bit3 water */
static WorldProg g_world[16];
static GLuint g_sky_prog, g_flat_prog, g_dither_prog, g_dither_old_prog, g_copy_prog;
static int g_dither_is_old; /* the new dither shader did not compile */
static struct {
    GLint sky, sky_fog, height, gradient, offset;
} S;
static GLint F_color;
static struct {
    GLint scene, dither, res;
} D, DO;
static GLint C_scene;
static GLuint g_sun_prog, g_sky_tex[SKY_NTEX], g_quad_vbo, g_quad_ibo;
static int g_sky_tex_tried;
static struct {
    GLint proj, center, xform, tex, tint;
} SU;
static SkyDraw g_skydraw[96];

#ifdef __vita__
/* ---------------------------------------------------------------- Cg (vitaGL) */
static const char *CG_WORLD_VS =
    "uniform float4x4 uMVP;\n"
    "uniform float4x4 uView;\n"
    "uniform float4x4 uModel;\n"
    "uniform float uAffine;\n"
    "uniform float uClassic;\n"
    "uniform float uFogEnd;\n"
    "void main(float3 aPos, float2 aUV, float4 aCol,\n"
    "  float4 out gl_Position : POSITION,\n"
    "  float3 out vUVW : TEXCOORD0,\n"
    "  float4 out vWD : TEXCOORD1,\n"
    "  float4 out vCol : TEXCOORD2)\n"
    "{\n"
    "  float4 cpos = mul(float4(aPos, 1.0f), uMVP);\n"
    "  float4 vp = mul(float4(aPos, 1.0f), uView);\n"
    "  float dist = length(vp.xyz);\n"
    "  float4 pos = cpos;\n"
    "  float w2 = 1.0f;\n"
    "  if (uClassic > 0.5f) {\n"
    "    float3 ndc = cpos.xyz / cpos.w;\n"
    "    ndc.x = floor(160.0f * ndc.x) / 160.0f;\n"
    "    ndc.y = floor(120.0f * ndc.y) / 120.0f;\n"
    "    pos = float4(ndc * cpos.w, cpos.w);\n"
    "    w2 = lerp(1.0f, cpos.w + 0.25f, uAffine);\n"
    "  }\n"
    /* (LSDR pushes z to -1000: past the far plane in Direct3D's reversed depth; here +1000 is, where
     * -1000 would be in front of the near plane, drawing the cut triangles over closer ones) */
    "  if (dist > uFogEnd) pos.z = 1000.0f;\n"
    "  gl_Position = pos;\n"
    "  vUVW = float3(aUV * w2, w2);\n"
    "  vWD = float4(mul(float4(aPos, 1.0f), uModel).xyz, dist);\n"
    "  vCol = aCol;\n"
    "}\n";

static const char *CG_WORLD_FS =
    "#ifdef HALFP\n"
    "#define CT half\n"
    "#define CT3 half3\n"
    "#define CT4 half4\n"
    "#else\n"
    "#define CT float\n"
    "#define CT3 float3\n"
    "#define CT4 float4\n"
    "#endif\n"
    "uniform sampler2D uTex;\n"
    "uniform float4 uFog;\n"
    "uniform float2 uFogRange;\n"
    "uniform float3 uCamI;\n"
    "uniform float uBright;\n"
    "uniform float uClassic;\n"
    "uniform float4 uTint;\n"
    "#ifdef WATER\n"
    "uniform sampler2D uPal;\n"
    "uniform float uTime;\n"
    "uniform float uWAlpha;\n"
    "#else\n"
    "uniform float uVCI;\n"
    "#endif\n"
    "float4 main(float3 vUVW : TEXCOORD0, float4 vWD : TEXCOORD1, float4 vCol : TEXCOORD2) : COLOR\n"
    "{\n"
    "#ifdef PROJ\n"
    "  float4 t0 = tex2Dproj(uTex, vUVW);\n"
    "#else\n"
    "  float4 t0 = tex2D(uTex, vUVW.xy / vUVW.z);\n"
    "#endif\n"
    "#ifdef WATER\n"
    /* LSDR/ClassicWater: the water map's red channel, scrolled by time, picks the palette colour */
    "  CT4 c = (CT4)tex2D(uPal, float2(t0.r - uTime, 0.5f));\n"
    "  c.rgb *= (CT3)(vCol.rgb * 1.3f);\n"
    "#else\n"
    "  CT4 c = (CT4)t0;\n"
    "#ifdef CUTOUT\n"
    "  if (c.a <= 0.1f) discard;\n"
    "#endif\n"
    /* (tanh((x - 0.5) * k) + 1) / 2 == 1 / (1 + exp(-2k (x - 0.5))) */
    "  CT3 vc = (CT3)vCol.rgb;\n"
    "  CT3 ex = exp((vc - CT3(0.5f, 0.5f, 0.5f)) * (CT)(-2.0f * uVCI));\n"
    "  c.rgb *= CT3(1.0f, 1.0f, 1.0f) / (CT3(1.0f, 1.0f, 1.0f) + ex);\n"
    "  c.a *= (CT)vCol.a;\n"
    "#endif\n"
    "  c *= (CT4)uTint;\n"
    "  float d = vWD.w;\n"
    "  if (uClassic > 0.5f) d = length(sign(vWD.xyz) * floor(abs(vWD.xyz)) - uCamI);\n"
    "  CT f = (CT)saturate((uFogRange.y - d) / (uFogRange.y - uFogRange.x));\n"
    "  CT g = (CT)1.0f - f;\n"
    "  CT3 fc = (CT3)uFog.rgb * g;\n"
    "  if (uFog.a < 0.5f) c.rgb += fc; else c.rgb = lerp(c.rgb, fc, CT3(g, g, g));\n"
    "  c.rgb *= (CT)uBright;\n"
    "#ifdef WATER\n"
    "  c.a = (CT)min(uWAlpha, t0.a);\n"
    "#endif\n"
    "  return (float4)c;\n"
    "}\n";

/* LSDR/Sun: a quad turned to face the camera in view space, scaled and rotated */
static const char *CG_SUN_VS =
    "uniform float4x4 uProj;\n"
    "uniform float3 uCenter;\n"
    "uniform float4 uXform;\n"
    "void main(float3 aPos, float2 aUV, float4 out gl_Position : POSITION, float2 out vUV : TEXCOORD0)\n"
    "{\n"
    "  float2 p = aPos.xy * uXform.xy;\n"
    "  float2 r = float2(uXform.z * p.x - uXform.w * p.y, uXform.w * p.x + uXform.z * p.y);\n"
    "  gl_Position = mul(float4(uCenter + float3(r, 0.0f), 1.0f), uProj);\n"
    "  vUV = aUV;\n"
    "}\n";

static const char *CG_SUN_FS =
    "uniform sampler2D uTex;\n"
    "uniform float4 uTint;\n"
    "float4 main(float2 vUV : TEXCOORD0) : COLOR\n"
    "{\n"
    "  float4 c = tex2D(uTex, vUV);\n"
    "  c.rgb *= uTint.rgb * 2.0f;\n"
    "  c.a *= uTint.a;\n"
    "  if (c.a < 0.5f) discard;\n"
    "  return c;\n"
    "}\n";

static const char *CG_SKY_VS =
    "void main(float2 aPos, float4 out gl_Position : POSITION, float2 out vY : TEXCOORD0)\n"
    "{\n"
    "  gl_Position = float4(aPos, 0.999f, 1.0f);\n"
    "  vY = float2(aPos.y * 0.5f + 0.5f, 0.0f);\n"
    "}\n";

static const char *CG_SKY_FS =
    "uniform float3 uSky;\n"
    "uniform float3 uSkyFog;\n"
    "uniform float uHeight;\n"
    "uniform float uGradient;\n"
    "uniform float uOffset;\n"
    "float4 main(float2 vY : TEXCOORD0) : COLOR\n"
    "{\n"
    "  float y = vY.x + uOffset;\n"
    "  float upper = saturate((y - (uHeight / 2.0f) - 0.5f) / uGradient);\n"
    "  float lower = saturate((0.5f - (y + (uHeight / 2.0f))) / uGradient);\n"
    "  float k = upper + lower;\n"
    "  return float4(uSky * k + uSkyFog * (1.0f - k), 1.0f);\n"
    "}\n";

static const char *CG_FLAT_VS =
    "void main(float2 aPos, float4 out gl_Position : POSITION)\n"
    "{\n"
    "  gl_Position = float4(aPos, 0.0f, 1.0f);\n"
    "}\n";

static const char *CG_FLAT_FS =
    "uniform float4 uColor;\n"
    "float4 main() : COLOR { return uColor; }\n";

static const char *CG_POST_VS =
    "void main(float2 aPos, float4 out gl_Position : POSITION, float2 out vUV : TEXCOORD0)\n"
    "{\n"
    "  gl_Position = float4(aPos, 0.0f, 1.0f);\n"
    "  vUV = aPos * 0.5f + 0.5f;\n"
    "}\n";

static const char *CG_COPY_FS =
    "uniform sampler2D uScene;\n"
    "float4 main(float2 vUV : TEXCOORD0) : COLOR\n"
    "{\n"
    "  return tex2D(uScene, vUV);\n"
    "}\n";

static const char *CG_DITHER_OLD_FS =
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uDither;\n"
    "uniform float2 uRes;\n"
    "float4 main(float2 vUV : TEXCOORD0) : COLOR\n"
    "{\n"
    "  float3 rgb = tex2D(uScene, vUV).rgb;\n"
    "  float y = dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));\n"
    "  float3 yuv = float3(y, (rgb.b - y) / 1.8556f + 0.5f, (rgb.r - y) / 1.5748f + 0.5f);\n"
    "  float3 c1 = floor(yuv * 16.0f) / 16.0f;\n"
    "  float3 c2 = ceil(yuv * 16.0f) / 16.0f;\n"
    "  float3 range = max(abs(c2 - c1), float3(1e-6f, 1e-6f, 1e-6f));\n"
    "  float3 level = floor(abs(yuv - c1) / range * 9.0f);\n"
    "  float thr = tex2D(uDither, (floor(vUV * uRes) + 0.5f) * 0.25f).r * 255.0f / 28.0f;\n"
    "  float3 t = step(float3(thr - 0.5f, thr - 0.5f, thr - 0.5f), level);\n"
    "  yuv = lerp(c1, c2, t) - float3(0.0f, 0.5f, 0.5f);\n"
    "  return float4(yuv.x + yuv.z * 1.5748f, yuv.x - yuv.y * 0.187324f - yuv.z * 0.468124f,\n"
    "                yuv.x + yuv.y * 1.8556f, 1.0f);\n"
    "}\n";

/* Same picture as CG_DITHER_OLD_FS with the algebra folded:
 *   level = floor(frac(yuv * 16) * 9) >= thr  <=>  frac(yuv * 16) >= thr / 9
 * and the YUV scale/offset folded into the conversion rows. */
static const char *CG_DITHER_VS =
    "uniform float2 uRes4;\n"
    "void main(float2 aPos, float4 out gl_Position : POSITION, float2 out vUV : TEXCOORD0,\n"
    "          float2 out vD : TEXCOORD1)\n"
    "{\n"
    "  gl_Position = float4(aPos, 0.0f, 1.0f);\n"
    "  vUV = aPos * 0.5f + 0.5f;\n"
    "  vD = vUV * uRes4;\n"
    "}\n";

static const char *CG_DITHER_FS =
    "uniform sampler2D uScene;\n"
    "uniform sampler2D uDither;\n"
    "float4 main(float2 vUV : TEXCOORD0, float2 vD : TEXCOORD1) : COLOR\n"
    "{\n"
    "  float3 rgb = tex2D(uScene, vUV).rgb;\n"
    "  float3 q = float3(dot(rgb, float3(3.4016f, 11.4432f, 1.1552f)),\n"
    "                    dot(rgb, float3(-1.833153697f, -6.166846303f, 8.0f)),\n"
    "                    dot(rgb, float3(8.0f, -7.266446533f, -0.733553467f))) + float3(0.0f, 8.0f, 8.0f);\n"
    "  float3 fl = floor(q);\n"
    "  float th = tex2D(uDither, vD).r * 1.011904762f;\n"
    "  float3 k = (fl + step(float3(th, th, th), q - fl)) * 0.0625f - float3(0.0f, 0.5f, 0.5f);\n"
    "  return float4(k.x + k.z * 1.5748f, k.x - k.y * 0.187324f - k.z * 0.468124f, k.x + k.y * 1.8556f, 1.0f);\n"
    "}\n";
#endif

/* ---------------------------------------------------------------- GLSL ES (PC test build) */
static const char *GL_WORLD_VS =
    "attribute vec3 aPos; attribute vec2 aUV; attribute vec4 aCol;\n"
    "uniform mat4 uMVP; uniform mat4 uView; uniform mat4 uModel;\n"
    "uniform float uAffine; uniform float uClassic; uniform float uFogEnd;\n"
    "varying vec3 vUVW; varying vec4 vWD; varying vec4 vCol;\n"
    "void main() {\n"
    "  vec4 cpos = uMVP * vec4(aPos, 1.0);\n"
    "  float dist = length((uView * vec4(aPos, 1.0)).xyz);\n"
    "  vec4 pos = cpos; float w2 = 1.0;\n"
    "  if (uClassic > 0.5) {\n"
    "    vec3 ndc = cpos.xyz / cpos.w;\n"
    "    ndc.x = floor(160.0 * ndc.x) / 160.0;\n"
    "    ndc.y = floor(120.0 * ndc.y) / 120.0;\n"
    "    pos = vec4(ndc * cpos.w, cpos.w);\n"
    "    w2 = mix(1.0, cpos.w + 0.25, uAffine);\n"
    "  }\n"
    "  if (dist > uFogEnd) pos.z = 1000.0;\n"
    "  gl_Position = pos;\n"
    "  vUVW = vec3(aUV * w2, w2); vWD = vec4((uModel * vec4(aPos, 1.0)).xyz, dist); vCol = aCol;\n"
    "}\n";

static const char *GL_WORLD_FS =
    "precision highp float;\n"
    "uniform sampler2D uTex; uniform vec4 uFog; uniform vec2 uFogRange; uniform vec3 uCamI;\n"
    "uniform float uBright; uniform float uClassic; uniform vec4 uTint;\n"
    "#ifdef WATER\n"
    "uniform sampler2D uPal; uniform float uTime; uniform float uWAlpha;\n"
    "#else\n"
    "uniform float uVCI;\n"
    "#endif\n"
    "varying vec3 vUVW; varying vec4 vWD; varying vec4 vCol;\n"
    "void main() {\n"
    "#ifdef PROJ\n"
    "  vec4 t0 = texture2DProj(uTex, vUVW);\n"
    "#else\n"
    "  vec4 t0 = texture2D(uTex, vUVW.xy / vUVW.z);\n"
    "#endif\n"
    "#ifdef WATER\n"
    "  vec4 c = texture2D(uPal, vec2(t0.r - uTime, 0.5));\n"
    "  c.rgb *= vCol.rgb * 1.3;\n"
    "#else\n"
    "  vec4 c = t0;\n"
    "#ifdef CUTOUT\n"
    "  if (c.a <= 0.1) discard;\n"
    "#endif\n"
    "  c.rgb *= 1.0 / (1.0 + exp((vCol.rgb - 0.5) * (-2.0 * uVCI)));\n"
    "  c.a *= vCol.a;\n"
    "#endif\n"
    "  c *= uTint;\n"
    "  float d = vWD.w;\n"
    "  if (uClassic > 0.5) d = length(sign(vWD.xyz) * floor(abs(vWD.xyz)) - uCamI);\n"
    "  float f = clamp((uFogRange.y - d) / (uFogRange.y - uFogRange.x), 0.0, 1.0);\n"
    "  vec3 fc = uFog.rgb * (1.0 - f);\n"
    "  if (uFog.a < 0.5) c.rgb += fc; else c.rgb = mix(c.rgb, fc, 1.0 - f);\n"
    "  c.rgb *= uBright;\n"
    "#ifdef WATER\n"
    "  c.a = min(uWAlpha, t0.a);\n"
    "#endif\n"
    "  gl_FragColor = c;\n"
    "}\n";

static const char *GL_SUN_VS =
    "attribute vec3 aPos; attribute vec2 aUV;\n"
    "uniform mat4 uProj; uniform vec3 uCenter; uniform vec4 uXform; varying vec2 vUV;\n"
    "void main() {\n"
    "  vec2 p = aPos.xy * uXform.xy;\n"
    "  vec2 r = vec2(uXform.z * p.x - uXform.w * p.y, uXform.w * p.x + uXform.z * p.y);\n"
    "  gl_Position = uProj * vec4(uCenter + vec3(r, 0.0), 1.0); vUV = aUV;\n"
    "}\n";

static const char *GL_SUN_FS =
    "precision mediump float; uniform sampler2D uTex; uniform vec4 uTint; varying vec2 vUV;\n"
    "void main() {\n"
    "  vec4 c = texture2D(uTex, vUV); c.rgb *= uTint.rgb * 2.0; c.a *= uTint.a;\n"
    "  if (c.a < 0.5) discard;\n"
    "  gl_FragColor = c;\n"
    "}\n";

static const char *GL_SKY_VS =
    "attribute vec2 aPos; varying vec2 vY;\n"
    "void main() { gl_Position = vec4(aPos, 0.999, 1.0); vY = vec2(aPos.y * 0.5 + 0.5, 0.0); }\n";

static const char *GL_SKY_FS =
    "precision highp float;\n"
    "uniform vec3 uSky; uniform vec3 uSkyFog; uniform float uHeight; uniform float uGradient; uniform float uOffset;\n"
    "varying vec2 vY;\n"
    "void main() {\n"
    "  float y = vY.x + uOffset;\n"
    "  float upper = clamp((y - (uHeight / 2.0) - 0.5) / uGradient, 0.0, 1.0);\n"
    "  float lower = clamp((0.5 - (y + (uHeight / 2.0))) / uGradient, 0.0, 1.0);\n"
    "  float k = upper + lower;\n"
    "  gl_FragColor = vec4(uSky * k + uSkyFog * (1.0 - k), 1.0);\n"
    "}\n";

static const char *GL_FLAT_VS =
    "attribute vec2 aPos; void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *GL_FLAT_FS =
    "precision mediump float; uniform vec4 uColor; void main() { gl_FragColor = uColor; }\n";

static const char *GL_POST_VS =
    "attribute vec2 aPos; varying vec2 vUV;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aPos * 0.5 + 0.5; }\n";

static const char *GL_COPY_FS =
    "precision highp float; uniform sampler2D uScene; varying vec2 vUV;\n"
    "void main() { gl_FragColor = texture2D(uScene, vUV); }\n";

static const char *GL_DITHER_OLD_FS =
    "precision highp float;\n"
    "uniform sampler2D uScene; uniform sampler2D uDither; uniform vec2 uRes;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vec3 rgb = texture2D(uScene, vUV).rgb;\n"
    "  float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "  vec3 yuv = vec3(y, (rgb.b - y) / 1.8556 + 0.5, (rgb.r - y) / 1.5748 + 0.5);\n"
    "  vec3 c1 = floor(yuv * 16.0) / 16.0;\n"
    "  vec3 c2 = ceil(yuv * 16.0) / 16.0;\n"
    "  vec3 range = max(abs(c2 - c1), vec3(1e-6));\n"
    "  vec3 level = floor(abs(yuv - c1) / range * 9.0);\n"
    "  float thr = texture2D(uDither, (floor(vUV * uRes) + 0.5) * 0.25).r * 255.0 / 28.0;\n"
    "  vec3 lit = step(vec3(thr - 0.5), level);\n"
    "  yuv = mix(c1, c2, lit) - vec3(0.0, 0.5, 0.5);\n"
    "  gl_FragColor = vec4(yuv.x + yuv.z * 1.5748, yuv.x - yuv.y * 0.187324 - yuv.z * 0.468124,\n"
    "                      yuv.x + yuv.y * 1.8556, 1.0);\n"
    "}\n";

static const char *GL_DITHER_VS =
    "attribute vec2 aPos; uniform vec2 uRes4; varying vec2 vUV; varying vec2 vD;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); vUV = aPos * 0.5 + 0.5; vD = vUV * uRes4; }\n";

static const char *GL_DITHER_FS =
    "precision highp float;\n"
    "uniform sampler2D uScene; uniform sampler2D uDither;\n"
    "varying vec2 vUV; varying vec2 vD;\n"
    "void main() {\n"
    "  vec3 rgb = texture2D(uScene, vUV).rgb;\n"
    "  vec3 q = vec3(dot(rgb, vec3(3.4016, 11.4432, 1.1552)),\n"
    "                dot(rgb, vec3(-1.833153697, -6.166846303, 8.0)),\n"
    "                dot(rgb, vec3(8.0, -7.266446533, -0.733553467))) + vec3(0.0, 8.0, 8.0);\n"
    "  vec3 fl = floor(q);\n"
    "  float th = texture2D(uDither, vD).r * 1.011904762;\n"
    "  vec3 k = (fl + step(vec3(th), q - fl)) * 0.0625 - vec3(0.0, 0.5, 0.5);\n"
    "  gl_FragColor = vec4(k.x + k.z * 1.5748, k.x - k.y * 0.187324 - k.z * 0.468124, k.x + k.y * 1.8556, 1.0);\n"
    "}\n";

/* Jazz PSX Dither pattern (LSDR psx_dither.png, 36x4): the 4x4 cell lights up once
 * the quantisation error level (0..8) reaches its threshold. Rows bottom-up. */
static const uint8_t DITHER_THRESHOLD[4][4] = {{8, 4, 7, 3}, {2, 6, 1, 5}, {7, 3, 8, 4}, {1, 5, 2, 6}};

static GLuint compile_one(GLenum type, const char *prefix, const char *src)
{
    GLuint sh = glCreateShader(type);
    size_t n = (prefix ? strlen(prefix) : 0) + strlen(src) + 1;
    char *buf = (char *)malloc(n);
    snprintf(buf, n, "%s%s", prefix ? prefix : "", src);
    const char *use = buf;
    GLint len = (GLint)strlen(use);
    glShaderSource(sh, 1, &use, &len);
    glCompileShader(sh);
    free(buf);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
        lsd_log("%s shader compile failed (%s): %s", type == VS_TYPE ? "vertex" : "fragment",
                prefix ? prefix : "-", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint link_prog(GLuint v, GLuint f, const char *const *attribs, int nattr)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (int i = 0; i < nattr; i++)
        glBindAttribLocation(p, (GLuint)i, attribs[i]);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, NULL, log);
        lsd_log("program link failed: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

#ifdef __vita__
/* Compiled programs are kept in ux0:data/lsdr/shaders (vitaGL's glGetProgramBinary: the GXP
 * binaries and attribute bindings), keyed by a hash of the sources: compiling a Cg shader on the
 * Vita takes up to half a second, loading one a few milliseconds. */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#define SHADER_CACHE_MAGIC 0x3153534Cu /* "LSS1" */
#define SHADER_BINARY_MAX (512 * 1024)

static uint32_t fnv1a(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t fnv_str(uint32_t h, const char *s) { return fnv1a(h, s ? s : "", (s ? strlen(s) : 0) + 1); }

static void cache_path(char *out, size_t n, uint32_t key)
{
    snprintf(out, n, "%s/shaders/%08x.bin", plat_write_dir(), (unsigned)key);
}

static GLuint cache_load(uint32_t key)
{
    char path[128];
    cache_path(path, sizeof(path), key);
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint32_t hdr[4];
    GLuint p = 0;
    if (fread(hdr, 4, 4, f) == 4 && hdr[0] == SHADER_CACHE_MAGIC && hdr[1] == key && hdr[2] > 0 &&
        hdr[2] <= SHADER_BINARY_MAX) {
        uint8_t *b = (uint8_t *)malloc(hdr[2]);
        if (b && fread(b, 1, hdr[2], f) == hdr[2] && fnv1a(2166136261u, b, hdr[2]) == hdr[3]) {
            p = glCreateProgram();
            glProgramBinary(p, 0, b, (GLsizei)hdr[2]);
            GLint ok = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &ok);
            if (!ok) {
                glDeleteProgram(p);
                p = 0;
            }
        }
        free(b);
    }
    fclose(f);
    return p;
}

static void cache_store(uint32_t key, GLuint prog)
{
    uint8_t *b = (uint8_t *)malloc(SHADER_BINARY_MAX);
    if (!b)
        return;
    GLsizei len = 0;
    GLenum fmt = 0;
    glGetProgramBinary(prog, SHADER_BINARY_MAX, &len, &fmt, b);
    if (len > 0 && len <= SHADER_BINARY_MAX) {
        char dir[128], path[128];
        snprintf(dir, sizeof(dir), "%s/shaders", plat_write_dir());
        sceIoMkdir(dir, 0777);
        cache_path(path, sizeof(path), key);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t hdr[4] = {SHADER_CACHE_MAGIC, key, (uint32_t)len, fnv1a(2166136261u, b, (size_t)len)};
            int ok = fwrite(hdr, 4, 4, f) == 4 && fwrite(b, 1, (size_t)len, f) == (size_t)len;
            ok = fclose(f) == 0 && ok;
            if (!ok)
                sceIoRemove(path);
        }
    }
    free(b);
}

static int g_shader_cache = 1;
void render_shader_cache(int on) { g_shader_cache = on; }
#else
void render_shader_cache(int on) { (void)on; }
#endif

/* cg_vs/cg_fs are used on the Vita (native Cg compiler; vitaGL's GLSL path compiles
 * lazily inside glLinkProgram and crashes there on failure); the GLSL pair is the PC
 * test build's version. */
GLuint render_compile2(const char *cg_vs, const char *cg_fs, const char *gl_vs, const char *gl_fs,
                       const char *fs_prefix, const char *const *attribs, int nattr)
{
#ifdef __vita__
    (void)gl_vs;
    (void)gl_fs;
    uint32_t key = fnv_str(2166136261u, "lsdr shader cache v1");
    key = fnv_str(key, cg_vs);
    key = fnv_str(key, fs_prefix);
    key = fnv_str(key, cg_fs);
    for (int i = 0; i < nattr; i++)
        key = fnv_str(key, attribs[i]);
    if (g_shader_cache) {
        GLuint p = cache_load(key);
        if (p)
            return p;
    }
    double t0 = plat_seconds();
    GLuint v = compile_one(VS_TYPE, NULL, cg_vs);
    GLuint f = v ? compile_one(FS_TYPE, fs_prefix, cg_fs) : 0;
    GLuint p = (v && f) ? link_prog(v, f, attribs, nattr) : 0;
    if (p && g_shader_cache) {
        /* stored, then used the way the next launch will: from the cache */
        double t1 = plat_seconds();
        cache_store(key, p);
        double t2 = plat_seconds();
        GLuint c = cache_load(key);
        lsd_log("shader %08x: compiled in %.0f ms, cached in %.0f ms, %s (%.0f ms)", (unsigned)key, (t1 - t0) * 1000.0,
                (t2 - t1) * 1000.0, c ? "reloaded from the cache" : "NOT reloaded from the cache",
                (plat_seconds() - t2) * 1000.0);
        if (c) {
            glDeleteProgram(p);
            p = c;
        }
    }
    return p;
#else
    (void)cg_vs;
    (void)cg_fs;
    GLuint v = compile_one(VS_TYPE, NULL, gl_vs);
    GLuint f = v ? compile_one(FS_TYPE, fs_prefix, gl_fs) : 0;
    return (v && f) ? link_prog(v, f, attribs, nattr) : 0;
#endif
}

#ifdef __vita__
#define PROG(vs, fs, prefix, attr, n) render_compile2(CG_##vs, CG_##fs, GL_##vs, GL_##fs, prefix, attr, n)
#else
#define PROG(vs, fs, prefix, attr, n) render_compile2(NULL, NULL, GL_##vs, GL_##fs, prefix, attr, n)
#endif

static WorldProg *world_prog(int idx)
{
    WorldProg *w = &g_world[idx];
    if (w->tried)
        return w->prog ? w : NULL;
    w->tried = 1;
    char prefix[96] = "";
    if (idx & 1) strcat(prefix, "#define CUTOUT 1\n");
    if (idx & 2) strcat(prefix, "#define HALFP 1\n");
    if (idx & 4) strcat(prefix, "#define PROJ 1\n");
    if (idx & 8) strcat(prefix, "#define WATER 1\n");
    static const char *const attr[] = {"aPos", "aUV", "aCol"};
    double t0 = plat_seconds();
    w->prog = PROG(WORLD_VS, WORLD_FS, prefix, attr, 3);
    if (!w->prog) {
        lsd_log("world shader variant %d unavailable", idx);
        return NULL;
    }
    if (idx)
        lsd_log("world shader variant %d ready (%.0f ms)", idx, (plat_seconds() - t0) * 1000.0);
    GLuint p = w->prog;
    w->mvp = glGetUniformLocation(p, "uMVP");
    w->view = glGetUniformLocation(p, "uView");
    w->affine = glGetUniformLocation(p, "uAffine");
    w->classic = glGetUniformLocation(p, "uClassic");
    w->fog_end = glGetUniformLocation(p, "uFogEnd");
    w->model = glGetUniformLocation(p, "uModel");
    w->tex = glGetUniformLocation(p, "uTex");
    w->fog = glGetUniformLocation(p, "uFog");
    w->fog_range = glGetUniformLocation(p, "uFogRange");
    w->cam_i = glGetUniformLocation(p, "uCamI");
    w->vci = glGetUniformLocation(p, "uVCI");
    w->bright = glGetUniformLocation(p, "uBright");
    w->tint = glGetUniformLocation(p, "uTint");
    w->pal = glGetUniformLocation(p, "uPal");
    w->time = glGetUniformLocation(p, "uTime");
    w->walpha = glGetUniformLocation(p, "uWAlpha");
    return w;
}

static GLuint color_target(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    return t;
}

static int make_lowres_targets(void)
{
    g_low_h = LOWRES_H;
    g_low_w = (int)floorf((float)LOWRES_H * (float)g_w / (float)g_h + 0.5f);
    g_low_w = (g_low_w + 1) & ~1; /* even: vitaGL halves the viewport width */
    glGenRenderbuffers(1, &g_fbo_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_fbo_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_low_w, g_low_h);
    glGenFramebuffers(2, g_fbo);
    for (int i = 0; i < 2; i++) {
        g_fbo_tex[i] = color_target(g_low_w, g_low_h);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbo_tex[i], 0);
        if (i == 0)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbo_depth);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            lsd_log("low-res framebuffer %d incomplete (0x%x)", i, st);
            glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
            return 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());

    uint8_t px[4 * 4 * 4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t v = (uint8_t)(DITHER_THRESHOLD[y][x] * 28);
            uint8_t *p = &px[(y * 4 + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    glGenTextures(1, &g_dither_tex);
    glBindTexture(GL_TEXTURE_2D, g_dither_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    lsd_log("pixelation targets %dx%d", g_low_w, g_low_h);
    return 1;
}

static void free_rv_targets(void)
{
    for (int i = 1; i < RV_LEVELS; i++) {
        if (g_rv[i].fbo) glDeleteFramebuffers(1, &g_rv[i].fbo);
        if (g_rv[i].tex) glDeleteTextures(1, &g_rv[i].tex);
        if (g_rv[i].depth) glDeleteRenderbuffers(1, &g_rv[i].depth);
        g_rv[i].fbo = g_rv[i].tex = g_rv[i].depth = 0;
    }
}

static void make_rv_targets(void)
{
    g_rv[0].w = g_w;
    g_rv[0].h = g_h;
    for (int i = 1; i < RV_LEVELS; i++) {
        int w = ((g_w * RV_PERCENT[i] / 100) + 1) & ~1, h = ((g_h * RV_PERCENT[i] / 100) + 1) & ~1;
        g_rv[i].w = w;
        g_rv[i].h = h;
        glGenRenderbuffers(1, &g_rv[i].depth);
        glBindRenderbuffer(GL_RENDERBUFFER, g_rv[i].depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        g_rv[i].tex = color_target(w, h);
        glBindTexture(GL_TEXTURE_2D, g_rv[i].tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); /* (filtered up: smooth, not pixelated) */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &g_rv[i].fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rv[i].fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rv[i].tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rv[i].depth);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            lsd_log("Revamped graphics target %dx%d incomplete (0x%x)", w, h, st);
            if (g_rv[i].fbo) glDeleteFramebuffers(1, &g_rv[i].fbo);
            g_rv[i].fbo = 0;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
}

/* test: hold the Revamped graphics at a size (100, 80 or 67%), or 0 to let the game choose again */
void render_set_rv_scale(int percent)
{
    if (percent <= 0) {
        g_rv_fixed = -1;
        g_rv_level = 0;
        lsd_log("Revamped graphics: size chosen by the game");
        return;
    }
    int best = 0;
    for (int i = 1; i < RV_LEVELS; i++)
        if (abs(RV_PERCENT[i] - percent) < abs(RV_PERCENT[best] - percent)) best = i;
    if (best && !g_rv[best].fbo) best = 0;
    g_rv_fixed = g_rv_level = best;
    lsd_log("Revamped graphics held at %dx%d (%d%%)", g_rv[best].w, g_rv[best].h, RV_PERCENT[best]);
}

/* the GPU has fallen behind with the Revamped graphics: a step smaller, until the next dream (1 if so) */
int render_rv_lower(void)
{
    if (g_rv_fixed >= 0 || g_rv_level + 1 >= RV_LEVELS || !g_rv[g_rv_level + 1].fbo)
        return 0;
    g_rv_level++;
    lsd_log("Revamped graphics: the GPU fell behind; drawn at %dx%d (%d%%) for the rest of this dream",
            g_rv[g_rv_level].w, g_rv[g_rv_level].h, RV_PERCENT[g_rv_level]);
    return 1;
}

void render_rv_reset(void)
{
    if (g_rv_fixed < 0)
        g_rv_level = 0;
}

int render_init(int width, int height)
{
    g_w = width;
    g_h = height;
    static const char *const pos_attr[] = {"aPos"};
    if (!world_prog(0) || !world_prog(1))
        return 0;
    world_prog(8); /* (the water and see-through variant too: compiled on first use, it would stall a frame) */
    g_sky_prog = PROG(SKY_VS, SKY_FS, NULL, pos_attr, 1);
    g_flat_prog = PROG(FLAT_VS, FLAT_FS, NULL, pos_attr, 1);
    g_dither_prog = PROG(DITHER_VS, DITHER_FS, NULL, pos_attr, 1);
    g_dither_old_prog = PROG(POST_VS, DITHER_OLD_FS, NULL, pos_attr, 1);
    g_copy_prog = PROG(POST_VS, COPY_FS, NULL, pos_attr, 1);
    if (!g_dither_prog && g_dither_old_prog) {
        lsd_log("using the previous dither shader");
        g_dither_prog = g_dither_old_prog;
        g_dither_old_prog = 0;
        g_dither_is_old = 1;
    }
    if (!g_sky_prog || !g_flat_prog || !g_dither_prog || !g_copy_prog)
        return 0;
    GLuint p = g_sky_prog;
    S.sky = glGetUniformLocation(p, "uSky");
    S.sky_fog = glGetUniformLocation(p, "uSkyFog");
    S.height = glGetUniformLocation(p, "uHeight");
    S.gradient = glGetUniformLocation(p, "uGradient");
    S.offset = glGetUniformLocation(p, "uOffset");
    F_color = glGetUniformLocation(g_flat_prog, "uColor");
    D.scene = glGetUniformLocation(g_dither_prog, "uScene");
    D.dither = glGetUniformLocation(g_dither_prog, "uDither");
    D.res = glGetUniformLocation(g_dither_prog, g_dither_is_old ? "uRes" : "uRes4");
    if (g_dither_old_prog) {
        DO.scene = glGetUniformLocation(g_dither_old_prog, "uScene");
        DO.dither = glGetUniformLocation(g_dither_old_prog, "uDither");
        DO.res = glGetUniformLocation(g_dither_old_prog, "uRes");
    }
    C_scene = glGetUniformLocation(g_copy_prog, "uScene");

    static const char *const sun_attr[] = {"aPos", "aUV"};
    g_sun_prog = PROG(SUN_VS, SUN_FS, NULL, sun_attr, 2);
    if (g_sun_prog) {
        SU.proj = glGetUniformLocation(g_sun_prog, "uProj");
        SU.center = glGetUniformLocation(g_sun_prog, "uCenter");
        SU.xform = glGetUniformLocation(g_sun_prog, "uXform");
        SU.tex = glGetUniformLocation(g_sun_prog, "uTex");
        SU.tint = glGetUniformLocation(g_sun_prog, "uTint");
    } else {
        lsd_log("sun shader unavailable: no suns");
    }
    /* Unity's built-in Quad */
    static const DreamVertex quad[4] = {
        {-0.5f, -0.5f, 0, 0, 0, 255, 255, 255, 255},
        {0.5f, -0.5f, 0, 1, 0, 255, 255, 255, 255},
        {-0.5f, 0.5f, 0, 0, 1, 255, 255, 255, 255},
        {0.5f, 0.5f, 0, 1, 1, 255, 255, 255, 255},
    };
    static const uint16_t quad_idx[6] = {0, 3, 1, 3, 0, 2};
    glGenBuffers(1, &g_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenBuffers(1, &g_quad_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_idx), quad_idx, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    static const float tri[6] = {-1, -1, 3, -1, -1, 3};
    glGenBuffers(1, &g_fullscreen_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_fullscreen_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (!make_lowres_targets())
        return 0;
    make_rv_targets();
    lsd_log("renderer ready (%dx%d)", width, height);
    return 1;
}

static void draw_fullscreen(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, g_fullscreen_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (const void *)0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* frustum planes from a column-major clip matrix (Gribb/Hartmann) */
static void frustum_planes(const mat4 *m, float pl[6][4])
{
    const float *a = m->m;
#define R(i, j) a[(j)*4 + (i)]
    for (int k = 0; k < 4; k++) {
        pl[0][k] = R(3, k) + R(0, k);
        pl[1][k] = R(3, k) - R(0, k);
        pl[2][k] = R(3, k) + R(1, k);
        pl[3][k] = R(3, k) - R(1, k);
        pl[4][k] = R(3, k) + R(2, k);
        pl[5][k] = R(3, k) - R(2, k);
    }
#undef R
}

static int box_visible(float pl[6][4], const float lo[3], const float hi[3])
{
    for (int i = 0; i < 6; i++) {
        float x = pl[i][0] >= 0 ? hi[0] : lo[0];
        float y = pl[i][1] >= 0 ? hi[1] : lo[1];
        float z = pl[i][2] >= 0 ? hi[2] : lo[2];
        if (pl[i][0] * x + pl[i][1] * y + pl[i][2] * z + pl[i][3] < 0)
            return 0;
    }
    return 1;
}

static float box_distance(vec3 p, const float lo[3], const float hi[3])
{
    float dx = fmaxf(fmaxf(lo[0] - p.x, 0), p.x - hi[0]);
    float dy = fmaxf(fmaxf(lo[1] - p.y, 0), p.y - hi[1]);
    float dz = fmaxf(fmaxf(lo[2] - p.z, 0), p.z - hi[2]);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static mat4 mat4_from_aff(const aff *a)
{
    mat4 r;
    const float *m = a->m;
    r.m[0] = m[0]; r.m[1] = m[4]; r.m[2] = m[8]; r.m[3] = 0;
    r.m[4] = m[1]; r.m[5] = m[5]; r.m[6] = m[9]; r.m[7] = 0;
    r.m[8] = m[2]; r.m[9] = m[6]; r.m[10] = m[10]; r.m[11] = 0;
    r.m[12] = m[3]; r.m[13] = m[7]; r.m[14] = m[11]; r.m[15] = 1;
    return r;
}

static void set_model(const WorldProg *w, const mat4 *vp, const mat4 *view, const mat4 *model)
{
    mat4 mvp = mat4_mul(*vp, *model);
    mat4 mv = mat4_mul(*view, *model);
    glUniformMatrix4fv(w->mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(w->view, 1, GL_FALSE, mv.m);
    glUniformMatrix4fv(w->model, 1, GL_FALSE, model->m);
}

static void bind_batch(const DreamBatch *b)
{
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DreamVertex), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DreamVertex), (const void *)12);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DreamVertex), (const void *)20);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
}

static void world_box(const aff *w, const float lo[3], const float hi[3], float olo[3], float ohi[3])
{
    for (int k = 0; k < 3; k++) {
        olo[k] = ohi[k] = w->m[k * 4 + 3];
        for (int j = 0; j < 3; j++) {
            float a = w->m[k * 4 + j] * lo[j], b = w->m[k * 4 + j] * hi[j];
            olo[k] += fminf(a, b);
            ohi[k] += fmaxf(a, b);
        }
    }
}

static void set_world_uniforms(const WorldProg *w, const Dream *d, const View *v, const mat4 *mvp, const mat4 *view,
                               float fog_start, float fog_end)
{
    const DreamEnv *e = v->env;
    glUseProgram(w->prog);
    glUniformMatrix4fv(w->mvp, 1, GL_FALSE, mvp->m);
    glUniformMatrix4fv(w->view, 1, GL_FALSE, view->m);
    glUniform1f(w->affine, v->affine);
    glUniform1f(w->classic, v->classic ? 1.0f : 0.0f);
    glUniform1f(w->fog_end, fog_end);
    glUniform1i(w->tex, 0);
    glUniform4f(w->fog, e->fog[0], e->fog[1], e->fog[2], e->subtractive ? 1.0f : 0.0f);
    glUniform2f(w->fog_range, fog_start, fog_end);
    glUniform3f(w->cam_i, truncf(v->eye.x), truncf(v->eye.y), truncf(v->eye.z));
    if (w->vci >= 0) glUniform1f(w->vci, d->vci);
    glUniform1f(w->bright, d->brightness);
    glUniform4f(w->tint, 1, 1, 1, 1);
}

static void post_pass(GLuint prog, GLuint src_tex, GLuint dst_fbo, int dw, int dh)
{
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(0, 0, dw, dh);
    glUseProgram(prog);
    if (prog == g_dither_prog) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_dither_tex);
        glUniform1i(D.scene, 0);
        glUniform1i(D.dither, 1);
        float k = g_dither_is_old ? 1.0f : 0.25f;
        glUniform2f(D.res, (float)g_low_w * k, (float)g_low_h * k);
    } else if (prog == g_dither_old_prog) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_dither_tex);
        glUniform1i(DO.scene, 0);
        glUniform1i(DO.dither, 1);
        glUniform2f(DO.res, (float)g_low_w, (float)g_low_h);
    } else {
        glUniform1i(C_scene, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    draw_fullscreen();
}

static void draw_fade(const View *v);

void render_clear(float r, float g, float b)
{
    glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void render_fade(const View *v) { draw_fade(v); }

static void draw_fade(const View *v)
{
    if (v->fade <= 0.001f)
        return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_flat_prog);
    glUniform4f(F_color, v->fade_rgb[0], v->fade_rgb[1], v->fade_rgb[2], v->fade > 1.0f ? 1.0f : v->fade);
    draw_fullscreen();
    glDisable(GL_BLEND);
}

/* sky.bin (tools/skydata.py): the sun, cloud and star textures, loaded when first needed */
static int sky_textures(void)
{
    if (g_sky_tex_tried)
        return g_sky_tex[0] != 0;
    g_sky_tex_tried = 1;
    size_t size = 0;
    uint8_t *data = vfs_read("sky.bin", &size);
    if (!data)
        return 0;
    uint32_t hdr[3] = {0, 0, 0};
    if (size >= 12) memcpy(hdr, data, 12);
    size_t off = 12;
    int ok = !memcmp(hdr, "LSDK", 4) && hdr[1] == 1 && hdr[2] == SKY_NTEX;
    GLuint tex[SKY_NTEX] = {0};
    for (int i = 0; ok && i < SKY_NTEX; i++) {
        uint32_t wh[2];
        if (off + 24 > size) { ok = 0; break; }
        memcpy(wh, data + off + 16, 8);
        off += 24;
        size_t bytes = (size_t)wh[0] * wh[1] * 4;
        if (wh[0] == 0 || wh[0] > 1024 || wh[1] == 0 || wh[1] > 1024 || off + bytes > size) { ok = 0; break; }
        glGenTextures(1, &tex[i]);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     data + off);
        off += bytes;
    }
    free(data);
    if (!ok) {
        lsd_log("sky.bin is damaged: no sky effects");
        for (int i = 0; i < SKY_NTEX; i++)
            if (tex[i]) glDeleteTextures(1, &tex[i]);
        return 0;
    }
    memcpy(g_sky_tex, tex, sizeof(tex));
    return 1;
}

/* the effects camera (DreamEnvironmentEffects): suns, rays, clouds and stars behind the world */
static void draw_sky_effects(const Dream *d, const View *v, const mat4 *proj, const mat4 *view, const mat4 *vp,
                             RenderStats *st)
{
    int n = skyfx_draws(v->eye, g_skydraw, (int)(sizeof(g_skydraw) / sizeof(g_skydraw[0])));
    if (n <= 0 || !sky_textures())
        return;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, g_quad_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_quad_ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DreamVertex), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DreamVertex), (const void *)12);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DreamVertex), (const void *)20);
    glActiveTexture(GL_TEXTURE0);
    const WorldProg *w = NULL;
    int cur = -1; /* 0 quads, 1 suns */
    for (int i = 0; i < n; i++) {
        const SkyDraw *k = &g_skydraw[i];
        glBindTexture(GL_TEXTURE_2D, g_sky_tex[k->tex]);
        if (k->sun) {
            if (!g_sun_prog)
                continue;
            if (cur != 1) {
                /* LSDR/Sun: opaque with an alpha test */
                glUseProgram(g_sun_prog);
                glUniformMatrix4fv(SU.proj, 1, GL_FALSE, proj->m);
                glUniform1i(SU.tex, 0);
                glDisableVertexAttribArray(2);
                glDisable(GL_BLEND);
                cur = 1;
            }
            const float *m = view->m;
            vec3 p = k->pos;
            glUniform3f(SU.center, m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]);
            float a = k->angle * DEG2RAD;
            glUniform4f(SU.xform, k->sx, k->sy, cosf(a), sinf(a));
            glUniform4f(SU.tint, k->tint[0], k->tint[1], k->tint[2], k->tint[3]);
        } else {
            if (cur != 0) {
                /* LSDR/Background: the LSDR shader, alpha blended, without fog */
                w = world_prog(0);
                if (!w)
                    return;
                set_world_uniforms(w, d, v, vp, view, 1e6f, 1e6f + 1.0f);
                glUniform1f(w->fog_end, 1e7f);
                glUniform1f(w->vci, 2.0f);
                glUniform1f(w->bright, 1.5f);
                glEnableVertexAttribArray(2);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                cur = 0;
            }
            mat4 model = mat4_from_aff(&k->world);
            set_model(w, vp, view, &model);
            glUniform4f(w->tint, k->tint[0], k->tint[1], k->tint[2], k->tint[3]);
        }
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);
        st->drawn_batches++;
        st->drawn_tris += 2;
    }
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
}

/* Unity CullMode (0 off, 1 front, 2 back) with Unity's clockwise front faces, expressed as
 * "cull back" with the matching winding (vitaGL only maps GL_BACK culling for offscreen targets) */
static void set_cull(int cull)
{
    if (!cull) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glFrontFace(cull == 1 ? GL_CCW : GL_CW);
    glCullFace(GL_BACK);
}

typedef struct {
    float dist;
    int ent, idx;
} TItem;

static int titem_cmp(const void *a, const void *b)
{
    const TItem *x = (const TItem *)a, *y = (const TItem *)b;
    if (x->dist != y->dist) return x->dist < y->dist ? 1 : -1; /* far first */
    if (x->ent != y->ent) return x->ent - y->ent;
    return x->idx - y->idx;
}

/* Unity's transparent queue: each LBD renderer's alpha-blended submesh and then its water submeshes
 * (LSDR/ClassicWater, which writes depth), and the entities' alpha-blended meshes, back to front by
 * the distance from the camera to their bounds' centres. */
static void draw_transparent(const Dream *d, const View *v, const mat4 *vp, const mat4 *view, const mat4 *mvp,
                             const aff *root, const mat4 *root_m, const unsigned char *vis, int nb,
                             const EntityDraw *ed, const unsigned char *evis, int ned, int variant,
                             float fog_start, float fog_end, RenderStats *st)
{
    static TItem *items;
    static int cap;
    if (d->ngroup + ned > cap) {
        cap = (d->ngroup + ned) * 2 + 16;
        items = (TItem *)realloc(items, cap * sizeof(TItem));
        if (!items) {
            cap = 0;
            return;
        }
    }
    int n = 0;
    for (int g = 0; g < d->ngroup; g++) {
        const DreamGroup *gr = &d->group[g];
        int any = 0;
        for (int k = 0; k < gr->tcount && !any; k++) {
            int b = d->tlist[gr->tfirst + k];
            any = b < nb && vis[b];
        }
        if (!any)
            continue;
        vec3 c = aff_point(root, v3((gr->lo[0] + gr->hi[0]) * 0.5f, (gr->lo[1] + gr->hi[1]) * 0.5f,
                                    (gr->lo[2] + gr->hi[2]) * 0.5f));
        vec3 dv = v3sub(c, v->eye);
        items[n].dist = v3dot(dv, dv);
        items[n].ent = 0;
        items[n].idx = g;
        n++;
    }
    for (int i = 0; i < ned; i++) {
        if (!evis[i] || ed[i].batch->kind != KIND_ALPHA)
            continue;
        float lo[3], hi[3];
        world_box(&ed[i].world, ed[i].batch->lo, ed[i].batch->hi, lo, hi);
        vec3 dv = v3sub(v3((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f), v->eye);
        items[n].dist = v3dot(dv, dv);
        items[n].ent = 1;
        items[n].idx = i;
        n++;
    }
    if (!n)
        return;
    qsort(items, n, sizeof(TItem), titem_cmp);

    const WorldProg *wa = world_prog(variant);
    if (!wa) wa = world_prog(0);
    const WorldProg *ww = world_prog(variant | 8);
    if (!ww) ww = world_prog(8);
    const WorldProg *cur = NULL;
    int ts = v->texset & 3;
    int cur_water = -1, cur_cull = -1;
    GLuint bound0 = d->tex[ts];
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int it = 0; it < n; it++) {
        const TItem *t = &items[it];
        if (t->ent) {
            const DreamBatch *b = ed[t->idx].batch;
            if (cur != wa) {
                set_world_uniforms(wa, d, v, mvp, view, fog_start, fog_end);
                cur = wa;
            }
            if (bound0 != d->tex[ts]) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, d->tex[ts]);
                bound0 = d->tex[ts];
            }
            glDepthMask(GL_FALSE);
            int cull = ed[t->idx].cull == 0 ? 0 : d->cull;
            if (cull != cur_cull) set_cull(cur_cull = cull);
            mat4 model = mat4_from_aff(&ed[t->idx].world);
            set_model(wa, vp, view, &model);
            if (ed[t->idx].vci > 0) glUniform1f(wa->vci, ed[t->idx].vci);
            bind_batch(b);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->nidx, GL_UNSIGNED_SHORT, (const void *)0);
            if (ed[t->idx].vci > 0) glUniform1f(wa->vci, d->vci);
            st->drawn_batches++;
            st->drawn_tris += (int)(b->nidx / 3);
            continue;
        }
        const DreamGroup *gr = &d->group[t->idx];
        for (int k = 0; k < gr->tcount; k++) {
            int bi = d->tlist[gr->tfirst + k];
            if (bi >= nb || !vis[bi])
                continue;
            const DreamBatch *b = &d->batch[bi];
            if (b->kind == KIND_ALPHA) {
                if (cur != wa) {
                    set_world_uniforms(wa, d, v, mvp, view, fog_start, fog_end);
                    cur = wa;
                }
                set_model(wa, vp, view, root_m);
                if (bound0 != d->tex[ts]) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, d->tex[ts]);
                    bound0 = d->tex[ts];
                }
                glDepthMask(GL_FALSE);
                if (d->cull != cur_cull) set_cull(cur_cull = d->cull);
            } else {
                if (!ww)
                    continue;
                int wi = b->kind - KIND_WATER;
                const WaterMat *m = &d->water[wi];
                if (cur != ww) {
                    set_world_uniforms(ww, d, v, mvp, view, fog_start, fog_end);
                    glUniform1i(ww->pal, 1);
                    cur = ww;
                    cur_water = -1;
                }
                set_model(ww, vp, view, root_m);
                if (cur_water != wi) {
                    float ph = (float)fmod(v->time / 20.0 * m->speed, 1.0); /* _Time.x * _AnimationSpeed */
                    glUniform1f(ww->time, ph);
                    glUniform1f(ww->walpha, m->alpha);
                    glUniform1f(ww->bright, m->brightness);
                    glUniform4f(ww->tint, m->tint[0], m->tint[1], m->tint[2], m->tint[3]);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, d->wtex[m->pal[ts]]);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, d->wtex[m->map[ts]]);
                    bound0 = d->wtex[m->map[ts]];
                    cur_water = wi;
                }
                glDepthMask(GL_TRUE);
                if (m->cull != cur_cull) set_cull(cur_cull = m->cull);
            }
            bind_batch(b);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->nidx, GL_UNSIGNED_SHORT, (const void *)0);
            st->drawn_batches++;
            st->drawn_tris += (int)(b->nidx / 3);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex[ts]);
}

void render_frame(const Dream *d, const View *v, RenderStats *st)
{
    if (!d) {
        /* between dreams: black (the title screen's place for now) under the fade */
        glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
        glViewport(0, 0, g_w, g_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_fade(v);
        st->drawn_batches = st->drawn_tris = 0;
        return;
    }
    const DreamEnv *e = v->env;
    float fog_end = (v->long_draw ? e->fog_end * 3.0f : e->fog_end) * g_rdbg.fog_mul;
    float fog_start = e->fog_start * g_rdbg.fog_mul;
    int lowres = v->pixelate;
    int scaled = !lowres && g_rv_level > 0 && g_rv[g_rv_level].fbo;
    int vw = lowres ? g_low_w : scaled ? g_rv[g_rv_level].w : g_w, vh = lowres ? g_low_h : scaled ? g_rv[g_rv_level].h : g_h;

    glBindFramebuffer(GL_FRAMEBUFFER, lowres ? g_fbo[0] : scaled ? g_rv[g_rv_level].fbo : plat_backbuffer());
    glViewport(0, 0, vw, vh);
    glDepthMask(GL_TRUE);
    glClearColor(e->sky_fog[0], e->sky_fog[1], e->sky_fog[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* sky (LSDR/GradientSky) */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    if (!g_rdbg.no_sky) {
        glUseProgram(g_sky_prog);
        glUniform3f(S.sky, e->sky[0], e->sky[1], e->sky[2]);
        glUniform3f(S.sky_fog, e->sky_fog[0], e->sky_fog[1], e->sky_fog[2]);
        glUniform1f(S.height, e->fog_height);
        glUniform1f(S.gradient, e->fog_gradient > 1e-4f ? e->fog_gradient : 1e-4f);
        glUniform1f(S.offset, v->fwd.y * (v->up.y >= 0 ? 1.0f : -1.0f));
        draw_fullscreen();
    }

    /* world */
    mat4 proj = mat4_perspective(v->fovy, (float)g_w / (float)g_h, 0.02f, 200.0f);
    mat4 view = mat4_view(v->eye, v->right, v->up, v->fwd);
    mat4 vp = mat4_mul(proj, view);
    /* the LBD geometry lives in the dream instance's space (StretchDream scales it) */
    aff root = g_scene.n > 0 ? *scene_world(0) : aff_ident();
    mat4 root_m = mat4_from_aff(&root);
    aff root_inv = aff_inverse(&root);
    mat4 mvp = mat4_mul(vp, root_m);
    float pl[6][4], wpl[6][4];
    frustum_planes(&mvp, pl);
    frustum_planes(&vp, wpl);
    vec3 eye_local = aff_point(&root_inv, v->eye);

    st->drawn_batches = st->drawn_tris = 0;
    if (!g_rdbg.no_sky)
        draw_sky_effects(d, v, &proj, &view, &vp, st);

    static unsigned char vis[4096];
    int nb = d->nbatch < 4096 ? d->nbatch : 4096;
    for (int i = 0; i < nb; i++) {
        const DreamBatch *b = &d->batch[i];
        vis[i] = !g_rdbg.no_world && b->nidx && box_visible(pl, b->lo, b->hi) &&
                 box_distance(eye_local, b->lo, b->hi) <= fog_end;
    }
    const EntityDraw *ed;
    int ned = g_rdbg.no_world ? 0 : scene_draw_list(&ed);
    static unsigned char *evis;
    static int evis_cap;
    if (ned > evis_cap) {
        evis_cap = ned * 2;
        evis = (unsigned char *)realloc(evis, evis_cap);
    }
    for (int i = 0; i < ned; i++) {
        float lo[3], hi[3];
        world_box(&ed[i].world, ed[i].batch->lo, ed[i].batch->hi, lo, hi);
        evis[i] = box_visible(wpl, lo, hi) && box_distance(v->eye, lo, hi) <= fog_end;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->tex[v->texset & 3]);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (d->cull) {
        /* Unity: clockwise = front. Express "cull front" as "cull back with the other
         * winding" (vitaGL only maps GL_BACK culling for offscreen targets). */
        glEnable(GL_CULL_FACE);
        glFrontFace(d->cull == 1 ? GL_CCW : GL_CW);
        glCullFace(GL_BACK);
    }
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    int variant = (g_rdbg.half ? 2 : 0) | (g_rdbg.proj ? 4 : 0);
    for (int kind = KIND_SOLID; kind <= KIND_CUTOUT; kind++) {
        int any = 0;
        for (int i = 0; i < nb && !any; i++)
            any = vis[i] && d->batch[i].kind == kind;
        for (int i = 0; i < ned && !any; i++)
            any = evis[i] && ed[i].batch->kind == kind;
        if (!any)
            continue;
        const WorldProg *w = world_prog(variant | (kind == KIND_CUTOUT ? 1 : 0));
        if (!w)
            w = world_prog(kind == KIND_CUTOUT ? 1 : 0);
        set_world_uniforms(w, d, v, &mvp, &view, fog_start, fog_end);
        set_model(w, &vp, &view, &root_m);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        for (int i = 0; i < nb; i++) {
            const DreamBatch *b = &d->batch[i];
            if (b->kind != kind || !vis[i])
                continue;
            bind_batch(b);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->nidx, GL_UNSIGNED_SHORT, (const void *)0);
            st->drawn_batches++;
            st->drawn_tris += (int)(b->nidx / 3);
        }
        /* the dream's entities (MOM models) with the same material */
        for (int i = 0; i < ned; i++) {
            const DreamBatch *b = ed[i].batch;
            if (b->kind != kind || !evis[i])
                continue;
            mat4 model = mat4_from_aff(&ed[i].world);
            set_model(w, &vp, &view, &model);
            if (ed[i].vci > 0) glUniform1f(w->vci, ed[i].vci);
            if (ed[i].cull == 0) glDisable(GL_CULL_FACE);
            bind_batch(b);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->nidx, GL_UNSIGNED_SHORT, (const void *)0);
            if (ed[i].vci > 0) glUniform1f(w->vci, d->vci);
            if (ed[i].cull == 0 && d->cull) glEnable(GL_CULL_FACE);
            st->drawn_batches++;
            st->drawn_tris += (int)(b->nidx / 3);
        }
    }
    draw_transparent(d, v, &vp, &view, &mvp, &root, &root_m, vis, nb, ed, evis, ned, variant, fog_start, fog_end, st);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    if (scaled) /* (the Revamped graphics, filtered up to the screen) */
        post_pass(g_copy_prog, g_rv[g_rv_level].tex, plat_backbuffer(), g_w, g_h);
    /* PixelateImageEffect + PSX Dither */
    if (lowres) {
        GLuint dp = (g_rdbg.old_dither && g_dither_old_prog) ? g_dither_old_prog : g_dither_prog;
        if (v->dither && !g_rdbg.dither_hires) {
            post_pass(dp, g_fbo_tex[0], g_fbo[1], g_low_w, g_low_h);
            post_pass(g_copy_prog, g_fbo_tex[1], plat_backbuffer(), g_w, g_h);
        } else if (v->dither) {
            post_pass(dp, g_fbo_tex[0], plat_backbuffer(), g_w, g_h); /* (test switch: full resolution) */
        } else {
            post_pass(g_copy_prog, g_fbo_tex[0], plat_backbuffer(), g_w, g_h);
        }
    }
    if (v->flashback) {
        /* FlashbackImageEffect (Hidden/FlashbackImageEffect): col.rgb + 0.4 */
        glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
        glViewport(0, 0, g_w, g_h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(g_flat_prog);
        glUniform4f(F_color, 0.4f, 0.4f, 0.4f, 1.0f);
        draw_fullscreen();
        glDisable(GL_BLEND);
    }

    draw_fade(v);
}

/* the video_dream and text_dream scenes: the picture over the whole screen, the fade on top */
void render_picture(GLuint tex, int w, int h, const View *v)
{
    /* (the 4:3 videos are stretched to the whole 16:9 screen; the text screens are made at its size) */
    (void)w;
    (void)h;
    glBindFramebuffer(GL_FRAMEBUFFER, plat_backbuffer());
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUseProgram(g_copy_prog);
        glUniform1i(C_scene, 0);
        draw_fullscreen();
    }
    draw_fade(v);
}

void render_shutdown(void)
{
    for (int i = 0; i < 8; i++)
        if (g_world[i].prog) glDeleteProgram(g_world[i].prog);
    if (g_sky_prog) glDeleteProgram(g_sky_prog);
    if (g_sun_prog) glDeleteProgram(g_sun_prog);
    for (int i = 0; i < SKY_NTEX; i++)
        if (g_sky_tex[i]) glDeleteTextures(1, &g_sky_tex[i]);
    if (g_quad_vbo) glDeleteBuffers(1, &g_quad_vbo);
    if (g_quad_ibo) glDeleteBuffers(1, &g_quad_ibo);
    if (g_flat_prog) glDeleteProgram(g_flat_prog);
    if (g_dither_prog) glDeleteProgram(g_dither_prog);
    if (g_dither_old_prog) glDeleteProgram(g_dither_old_prog);
    if (g_copy_prog) glDeleteProgram(g_copy_prog);
    if (g_fullscreen_vbo) glDeleteBuffers(1, &g_fullscreen_vbo);
    free_rv_targets();
    glDeleteFramebuffers(2, g_fbo);
    glDeleteTextures(2, g_fbo_tex);
    if (g_fbo_depth) glDeleteRenderbuffers(1, &g_fbo_depth);
    if (g_dither_tex) glDeleteTextures(1, &g_dither_tex);
}
