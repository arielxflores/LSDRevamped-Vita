#include <stdarg.h>
#include <string.h>
#include "common.h"

/* ---------------------------------------------------------------- logging */
#ifndef __vita__
static FILE *g_logf;
#endif

/* ---------------------------------------------------------------- frame profile */
static double g_prof[PROF_N];

void prof_add(int part, double seconds)
{
    if (part >= 0 && part < PROF_N)
        g_prof[part] += seconds;
}

double prof_get(int part) { return part >= 0 && part < PROF_N ? g_prof[part] : 0.0; }

static int g_prof_test;

void prof_reset(void)
{
    memset(g_prof, 0, sizeof(g_prof));
    g_prof_test = 0;
}

void prof_mark_test(void) { g_prof_test = 1; }

int prof_test(void) { return g_prof_test; }

void lsd_log(const char *fmt, ...)
{
    double t_in = plat_seconds();
    char buf[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "[%8.3f] %s\n", t_in, buf);
    if (n < 0) n = 0;
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
#ifdef __vita__
    plat_log_write(line, (size_t)n); /* (queued: a thread writes it to the card) */
#else
    plat_log_lock();
    if (!g_logf) {
        char path[512];
        snprintf(path, sizeof(path), "%s/log.txt", plat_write_dir());
        g_logf = fopen(path, "w");
    }
    if (g_logf) {
        fwrite(line, 1, (size_t)n, g_logf);
        fflush(g_logf);
    }
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    plat_log_unlock();
#endif
    prof_add(PROF_LOG, plat_seconds() - t_in);
}

/* ---------------------------------------------------------------- random numbers */
static uint32_t g_rng = 0x12345678u;

void rng_seed(uint32_t s)
{
    g_rng = s ? s : 0x12345678u;
    for (int i = 0; i < 8; i++)
        rng_u32();
}

uint32_t rng_u32(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

int rng_int(int n)
{
    return n > 0 ? (int)(rng_u32() % (uint32_t)n) : 0;
}

float rng_float(float lo, float hi)
{
    return lo + (hi - lo) * (float)((rng_u32() >> 8) * (1.0 / 16777216.0));
}

/* ---------------------------------------------------------------- matrices */
mat4 mat4_mul(mat4 a, mat4 b)
{
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

mat4 mat4_perspective(float fovy_deg, float aspect, float zn, float zf)
{
    mat4 r;
    memset(&r, 0, sizeof(r));
    float f = 1.0f / tanf(fovy_deg * DEG2RAD * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = 2.0f * zf * zn / (zn - zf);
    return r;
}

/* world (Unity, left-handed) -> GL eye space: rows right, up, -forward */
mat4 mat4_view(vec3 eye, vec3 right, vec3 up, vec3 fwd)
{
    mat4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = right.x; r.m[4] = right.y; r.m[8] = right.z;
    r.m[1] = up.x;    r.m[5] = up.y;    r.m[9] = up.z;
    r.m[2] = -fwd.x;  r.m[6] = -fwd.y;  r.m[10] = -fwd.z;
    r.m[12] = -v3dot(right, eye);
    r.m[13] = -v3dot(up, eye);
    r.m[14] = v3dot(fwd, eye);
    r.m[15] = 1.0f;
    return r;
}
