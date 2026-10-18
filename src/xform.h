/* Unity-style transform maths: quaternions (x, y, z, w; Hamilton product) and
 * 3x4 row-major affine matrices (column vectors: p' = M p). */
#ifndef LSD_XFORM_H
#define LSD_XFORM_H

#include "common.h"

typedef struct { float x, y, z, w; } quat;

static inline quat q4(float x, float y, float z, float w) { quat q = {x, y, z, w}; return q; }
static inline quat qident(void) { return q4(0, 0, 0, 1); }

static inline quat qmul(quat a, quat b)
{
    return q4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

static inline float qdot(quat a, quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

static inline quat qnorm(quat q)
{
    float l = sqrtf(qdot(q, q));
    if (l < 1e-12f) return qident();
    return q4(q.x / l, q.y / l, q.z / l, q.w / l);
}

static inline quat qconj(quat q) { return q4(-q.x, -q.y, -q.z, q.w); }

static inline quat qaxis(float deg, vec3 axis)
{
    float h = deg * DEG2RAD * 0.5f, s = sinf(h);
    return q4(axis.x * s, axis.y * s, axis.z * s, cosf(h));
}

static inline vec3 qrot(quat q, vec3 v)
{
    /* v' = q v q* */
    vec3 u = v3(q.x, q.y, q.z);
    vec3 t = v3scale(v3cross(u, v), 2.0f);
    return v3add(v3add(v, v3scale(t, q.w)), v3cross(u, t));
}

/* Unity Quaternion.Euler: rotation order Z, then X, then Y (q = qy * qx * qz) */
static inline quat qeuler(vec3 e)
{
    return qmul(qmul(qaxis(e.y, v3(0, 1, 0)), qaxis(e.x, v3(1, 0, 0))), qaxis(e.z, v3(0, 0, 1)));
}

/* Unity Quaternion.eulerAngles (degrees, 0..360) */
static inline vec3 qto_euler(quat q)
{
    /* from the rotation matrix of q, Unity's ZXY convention */
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float m12 = 2 * (yz - wx); /* row 1, col 2 */
    float sx = -m12;
    if (sx > 1) sx = 1;
    if (sx < -1) sx = -1;
    float x = asinf(sx), y, z;
    if (fabsf(sx) < 0.9999f) {
        float m02 = 2 * (xz + wy), m22 = 1 - 2 * (xx + yy);
        float m10 = 2 * (xy + wz), m11 = 1 - 2 * (xx + zz);
        y = atan2f(m02, m22);
        z = atan2f(m10, m11);
    } else {
        float m01 = 2 * (xy - wz), m00 = 1 - 2 * (yy + zz);
        y = 0;
        z = atan2f(-m01, m00);
    }
    vec3 e = v3(x / DEG2RAD, y / DEG2RAD, z / DEG2RAD);
    if (e.x < 0) e.x += 360;
    if (e.y < 0) e.y += 360;
    if (e.z < 0) e.z += 360;
    return e;
}

/* Quaternion.LookRotation(forward, up) */
static inline quat qlook(vec3 f, vec3 up)
{
    float fl = v3len(f);
    if (fl < 1e-9f) return qident();
    f = v3scale(f, 1.0f / fl);
    vec3 r = v3cross(up, f);
    float rl = v3len(r);
    if (rl < 1e-9f) {
        /* forward parallel to up: pick any right */
        r = fabsf(f.x) < 0.9f ? v3cross(v3(1, 0, 0), f) : v3cross(v3(0, 0, 1), f);
        rl = v3len(r);
    }
    r = v3scale(r, 1.0f / rl);
    vec3 u = v3cross(f, r);
    /* matrix columns r, u, f -> quaternion */
    float m00 = r.x, m01 = u.x, m02 = f.x, m10 = r.y, m11 = u.y, m12 = f.y, m20 = r.z, m21 = u.z, m22 = f.z;
    float tr = m00 + m11 + m22;
    quat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q = q4((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q = q4(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q = q4((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q = q4((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
    }
    return qnorm(q);
}

/* Quaternion.RotateTowards(from, to, maxDegrees) */
static inline quat qrotate_towards(quat a, quat b, float max_deg)
{
    float d = qdot(a, b);
    if (d > 1) d = 1;
    if (d < -1) d = -1;
    float angle = acosf(fminf(fabsf(d), 1.0f)) * 2.0f / DEG2RAD;
    if (angle == 0.0f) return b;
    float t = fminf(1.0f, max_deg / angle);
    /* slerp unclamped along the shortest path */
    if (d < 0) { b = q4(-b.x, -b.y, -b.z, -b.w); d = -d; }
    if (d > 0.9995f)
        return qnorm(q4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
    float th = acosf(d), s = sinf(th);
    float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return q4(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb);
}

typedef struct { float m[12]; } aff; /* rows: [r00 r01 r02 tx][r10 r11 r12 ty][r20 r21 r22 tz] */

static inline aff aff_trs(vec3 t, quat q, vec3 s)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    aff a;
    a.m[0] = (1 - 2 * (y * y + z * z)) * s.x; a.m[1] = 2 * (x * y - z * w) * s.y; a.m[2] = 2 * (x * z + y * w) * s.z;
    a.m[3] = t.x;
    a.m[4] = 2 * (x * y + z * w) * s.x; a.m[5] = (1 - 2 * (x * x + z * z)) * s.y; a.m[6] = 2 * (y * z - x * w) * s.z;
    a.m[7] = t.y;
    a.m[8] = 2 * (x * z - y * w) * s.x; a.m[9] = 2 * (y * z + x * w) * s.y; a.m[10] = (1 - 2 * (x * x + y * y)) * s.z;
    a.m[11] = t.z;
    return a;
}

static inline aff aff_mul(const aff *a, const aff *b)
{
    aff r;
    for (int i = 0; i < 3; i++) {
        const float *ar = &a->m[i * 4];
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = ar[0] * b->m[j] + ar[1] * b->m[4 + j] + ar[2] * b->m[8 + j] + (j == 3 ? ar[3] : 0.0f);
    }
    return r;
}

static inline vec3 aff_point(const aff *a, vec3 p)
{
    return v3(a->m[0] * p.x + a->m[1] * p.y + a->m[2] * p.z + a->m[3],
              a->m[4] * p.x + a->m[5] * p.y + a->m[6] * p.z + a->m[7],
              a->m[8] * p.x + a->m[9] * p.y + a->m[10] * p.z + a->m[11]);
}

static inline vec3 aff_dir(const aff *a, vec3 p)
{
    return v3(a->m[0] * p.x + a->m[1] * p.y + a->m[2] * p.z, a->m[4] * p.x + a->m[5] * p.y + a->m[6] * p.z,
              a->m[8] * p.x + a->m[9] * p.y + a->m[10] * p.z);
}

static inline aff aff_inverse(const aff *a)
{
    const float *m = a->m;
    float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[2] * m[9] - m[1] * m[10], c02 = m[1] * m[6] - m[2] * m[5];
    float c10 = m[6] * m[8] - m[4] * m[10], c11 = m[0] * m[10] - m[2] * m[8], c12 = m[2] * m[4] - m[0] * m[6];
    float c20 = m[4] * m[9] - m[5] * m[8], c21 = m[1] * m[8] - m[0] * m[9], c22 = m[0] * m[5] - m[1] * m[4];
    float det = m[0] * c00 + m[1] * c10 + m[2] * c20;
    aff r;
    if (fabsf(det) < 1e-20f) {
        memset(&r, 0, sizeof(r));
        return r;
    }
    float id = 1.0f / det;
    r.m[0] = c00 * id; r.m[1] = c01 * id; r.m[2] = c02 * id;
    r.m[4] = c10 * id; r.m[5] = c11 * id; r.m[6] = c12 * id;
    r.m[8] = c20 * id; r.m[9] = c21 * id; r.m[10] = c22 * id;
    vec3 t = v3(m[3], m[7], m[11]);
    r.m[3] = -(r.m[0] * t.x + r.m[1] * t.y + r.m[2] * t.z);
    r.m[7] = -(r.m[4] * t.x + r.m[5] * t.y + r.m[6] * t.z);
    r.m[11] = -(r.m[8] * t.x + r.m[9] * t.y + r.m[10] * t.z);
    return r;
}

static inline aff aff_ident(void)
{
    aff a = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
    return a;
}

#endif
