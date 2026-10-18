/* Player collision against the baked LBD collision mesh.
 * Approximates Unity's CharacterController closely enough for LSD's simple
 * movement: walls stop you, steps up to PLAYER_STEP are climbed, and you fall
 * at a constant speed when nothing is under you. */
#include "collide.h"

static float g_cos_slope = -1.0f;

static float cos_slope(void)
{
    if (g_cos_slope < 0)
        g_cos_slope = cosf(PLAYER_SLOPE_DEG * DEG2RAD);
    return g_cos_slope;
}

/* iterate triangle indices in grid cells overlapping [x0,x1]x[z0,z1] */
#define FOR_CELLS(d, x0, x1, z0, z1, ti)                                        \
    for (int _cz = (int)floorf(((z0) - (d)->gz0) / (d)->cell),                 \
             _cz1 = (int)floorf(((z1) - (d)->gz0) / (d)->cell);                \
         _cz <= _cz1; _cz++)                                                   \
        if (_cz >= 0 && _cz < (d)->gh)                                         \
            for (int _cx = (int)floorf(((x0) - (d)->gx0) / (d)->cell),         \
                     _cx1 = (int)floorf(((x1) - (d)->gx0) / (d)->cell);        \
                 _cx <= _cx1; _cx++)                                           \
                if (_cx >= 0 && _cx < (d)->gw)                                 \
                    for (int _k = (d)->cell_start[_cz * (d)->gw + _cx],        \
                             _ke = (d)->cell_start[_cz * (d)->gw + _cx + 1];   \
                         _k < _ke && ((ti) = (d)->cell_tris[_k], 1); _k++)

/* height of triangle t's plane at (x,z) if (x,z) is inside its XZ projection */
static int tri_height_at(const ColTri *t, float x, float z, float *y)
{
    float ax = t->a.x, az = t->a.z;
    float e0x = t->b.x - ax, e0z = t->b.z - az;
    float e1x = t->c.x - ax, e1z = t->c.z - az;
    float px = x - ax, pz = z - az;
    float den = e0x * e1z - e1x * e0z;
    if (fabsf(den) < 1e-12f)
        return 0;
    float u = (px * e1z - e1x * pz) / den;
    float v = (e0x * pz - px * e0z) / den;
    const float eps = 1e-5f;
    if (u < -eps || v < -eps || u + v > 1.0f + eps)
        return 0;
    *y = t->a.y + u * (t->b.y - t->a.y) + v * (t->c.y - t->a.y);
    return 1;
}

int col_ground(const Dream *d, vec3 feet, float max_up, float max_down, float *out_y)
{
    static const float off[5][2] = {{0, 0}, {0.06f, 0}, {-0.06f, 0}, {0, 0.06f}, {0, -0.06f}};
    const float cs = cos_slope();
    float best = -1e30f;
    int found = 0;
    float top = feet.y + max_up, bottom = feet.y - max_down;
    int ti;
    FOR_CELLS(d, feet.x - 0.07f, feet.x + 0.07f, feet.z - 0.07f, feet.z + 0.07f, ti)
    {
        const ColTri *t = &d->tri[ti];
        if (fabsf(t->n.y) < cs)
            continue;
        if (t->lo[1] > top || t->hi[1] < bottom)
            continue;
        for (int i = 0; i < 5; i++) {
            float y;
            if (tri_height_at(t, feet.x + off[i][0], feet.z + off[i][1], &y) && y <= top && y >= bottom &&
                y > best) {
                best = y;
                found = 1;
            }
        }
    }
    if (found)
        *out_y = best;
    return found;
}

/* closest point on triangle to p (Ericson, Real-Time Collision Detection 5.1.5) */
static vec3 closest_on_tri(vec3 p, vec3 a, vec3 b, vec3 c)
{
    vec3 ab = v3sub(b, a), ac = v3sub(c, a), ap = v3sub(p, a);
    float d1 = v3dot(ab, ap), d2 = v3dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    vec3 bp = v3sub(p, b);
    float d3 = v3dot(ab, bp), d4 = v3dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return v3add(a, v3scale(ab, d1 / (d1 - d3)));
    vec3 cp = v3sub(p, c);
    float d5 = v3dot(ab, cp), d6 = v3dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return v3add(a, v3scale(ac, d2 / (d2 - d6)));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return v3add(b, v3scale(v3sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    float den = 1.0f / (va + vb + vc);
    float v = vb * den, w = vc * den;
    return v3add(a, v3add(v3scale(ab, v), v3scale(ac, w)));
}

int col_wall(const Dream *d, vec3 p)
{
    const float cs = cos_slope();
    const float r = PLAYER_RADIUS + PLAYER_SKIN;
    /* sphere centres covering the capsule above step height */
    const float hs[2] = {PLAYER_STEP + PLAYER_RADIUS, PLAYER_HEIGHT - PLAYER_RADIUS};
    int ti;
    FOR_CELLS(d, p.x - r, p.x + r, p.z - r, p.z + r, ti)
    {
        const ColTri *t = &d->tri[ti];
        if (fabsf(t->n.y) >= cs)
            continue;
        if (t->hi[1] < p.y + hs[0] - r || t->lo[1] > p.y + hs[1] + r)
            continue;
        if (t->hi[0] < p.x - r || t->lo[0] > p.x + r || t->hi[2] < p.z - r || t->lo[2] > p.z + r)
            continue;
        for (int i = 0; i < 2; i++) {
            vec3 c = v3(p.x, p.y + hs[i], p.z);
            vec3 q = closest_on_tri(c, t->a, t->b, t->c);
            vec3 dv = v3sub(c, q);
            if (v3dot(dv, dv) < r * r)
                return 1;
        }
    }
    return 0;
}

/* the highest too-steep-to-walk (but not wall-like) surface under the player's footprint between
 * y0 and y1: Unity's character controller cannot pass through those, though it cannot stand on them */
static int steep_at(const Dream *d, vec3 feet, float y0, float y1, float *out_y)
{
    static const float off[5][2] = {{0, 0}, {0.06f, 0}, {-0.06f, 0}, {0, 0.06f}, {0, -0.06f}};
    const float cs = cos_slope();
    float best = -1e30f;
    int found = 0, ti;
    FOR_CELLS(d, feet.x - 0.07f, feet.x + 0.07f, feet.z - 0.07f, feet.z + 0.07f, ti)
    {
        const ColTri *t = &d->tri[ti];
        float ny = fabsf(t->n.y);
        if (ny >= cs || ny < 0.2f || t->lo[1] > y1 || t->hi[1] < y0)
            continue;
        for (int i = 0; i < 5; i++) {
            float y;
            if (tri_height_at(t, feet.x + off[i][0], feet.z + off[i][1], &y) && y >= y0 && y <= y1 && y > best) {
                best = y;
                found = 1;
            }
        }
    }
    if (found)
        *out_y = best;
    return found;
}

int col_move(const Dream *d, vec3 *feet, vec3 motion)
{
    int flags = 0;
    vec3 p = *feet;
    float hl = sqrtf(motion.x * motion.x + motion.z * motion.z);
    if (hl > 0) {
        int steps = (int)ceilf(hl / 0.03f);
        if (steps < 1) steps = 1;
        float sx = motion.x / steps, sz = motion.z / steps;
        for (int i = 0; i < steps; i++) {
            vec3 q = v3(p.x + sx, p.y, p.z + sz);
            float g;
            if (col_ground(d, q, PLAYER_STEP, 0.0f, &g) && g > q.y)
                q.y = g; /* climb a step */
            if (col_wall(d, q)) {
                flags |= COL_SIDES;
                break;
            }
            /* off the floor and into a steep slope rising from it (Happytown's kerbs at its edges):
             * the capsule's bottom would be inside the slope, Unity's stops against it */
            float sy;
            if (!col_ground(d, q, PLAYER_STEP, PLAYER_STEP, &g) &&
                steep_at(d, q, q.y - 0.02f, q.y + PLAYER_STEP + PLAYER_RADIUS, &sy)) {
                flags |= COL_SIDES;
                break;
            }
            p = q;
        }
    }
    float g;
    if (motion.y < 0) {
        float ny = p.y + motion.y;
        if (col_ground(d, p, 0.001f, -motion.y + 0.001f, &g) && g >= ny) {
            ny = g;
            flags |= COL_BELOW;
        } else if (steep_at(d, p, ny, p.y + 0.001f, &g)) {
            ny = g; /* (falling onto a steep slope: stopped by it, not standing on it) */
        }
        p.y = ny;
    } else if (motion.y > 0) {
        p.y += motion.y;
    } else if (col_ground(d, p, 0.002f, 0.002f, &g)) {
        flags |= COL_BELOW;
    }
    *feet = p;
    return flags;
}

/* squared distance from (px,pz) to the segment (ax,az)-(bx,bz) */
static float seg_dist2(float px, float pz, float ax, float az, float bx, float bz)
{
    float ex = bx - ax, ez = bz - az;
    float l2 = ex * ex + ez * ez;
    float t = l2 > 1e-12f ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float dx = ax + ex * t - px, dz = az + ez * t - pz;
    return dx * dx + dz * dz;
}

/* PlayerFallDetector's SphereCast(feet + up * start_up, radius, down): is there anything at all
 * (floor, slope or wall) under the player, at any depth? */
int col_anything_below(const Dream *d, vec3 feet, float start_up, float radius)
{
    float top = feet.y + start_up + radius;
    float r2 = radius * radius;
    int ti;
    FOR_CELLS(d, feet.x - radius, feet.x + radius, feet.z - radius, feet.z + radius, ti)
    {
        const ColTri *t = &d->tri[ti];
        if (t->lo[1] > top)
            continue;
        if (t->hi[0] < feet.x - radius || t->lo[0] > feet.x + radius || t->hi[2] < feet.z - radius ||
            t->lo[2] > feet.z + radius)
            continue;
        float y;
        if (tri_height_at(t, feet.x, feet.z, &y))
            return 1;
        /* footprint edges (also covers walls, whose footprint is a line) */
        if (seg_dist2(feet.x, feet.z, t->a.x, t->a.z, t->b.x, t->b.z) <= r2 ||
            seg_dist2(feet.x, feet.z, t->b.x, t->b.z, t->c.x, t->c.z) <= r2 ||
            seg_dist2(feet.x, feet.z, t->c.x, t->c.z, t->a.x, t->a.z) <= r2)
            return 1;
    }
    return 0;
}

int col_ray_down(const Dream *d, vec3 p, float *out_y)
{
    float best = -1e30f;
    int found = 0, ti;
    FOR_CELLS(d, p.x, p.x, p.z, p.z, ti)
    {
        const ColTri *t = &d->tri[ti];
        if (t->lo[1] > p.y) continue;
        float y;
        if (tri_height_at(t, p.x, p.z, &y) && y <= p.y && y > best) {
            best = y;
            found = 1;
        }
    }
    if (found) *out_y = best;
    return found;
}

/* the highest triangle under p (at most max_down below it): its height and index */
int col_ray_down_tri(const Dream *d, vec3 p, float max_down, float *out_y, int *out_tri)
{
    float best = -1e30f;
    int found = -1, ti;
    FOR_CELLS(d, p.x, p.x, p.z, p.z, ti)
    {
        const ColTri *t = &d->tri[ti];
        if (t->lo[1] > p.y || t->hi[1] < p.y - max_down) continue;
        float y;
        if (tri_height_at(t, p.x, p.z, &y) && y <= p.y && y >= p.y - max_down && y > best) {
            best = y;
            found = ti;
        }
    }
    if (found < 0) return 0;
    *out_y = best;
    *out_tri = found;
    return 1;
}
