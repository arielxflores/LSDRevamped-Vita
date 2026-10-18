/* Sky effects (see skyfx.h): DreamEnvironmentEffects.cs, Sun.cs, Cloud.cs, DreamEnvironment.Apply. */
#include "skyfx.h"

#define MAX_QUADS 64

typedef struct {
    vec3 local;      /* in the effects object's space */
    float yaw;       /* world yaw added by Rotate(up, ..., World) */
    float scale;
    float tint[4];
    int tex;
    int can_move;    /* Cloud.Start: OneIn(4) (clouds only) */
    float t, rot;
    vec3 move;
} Quad;

typedef struct {
    vec3 local;
    float tint[4];
    float scale;     /* _ScaleX = _ScaleY = scale / 2 */
    int order;
    int burst;
    float burst_scale, burst_tint[4], t;
    struct {
        float sx, sy, angle;
    } ray[4];
} Sun;

static Quad g_q[MAX_QUADS];
static int g_nq;
static Sun g_sun[4];
static int g_nsun;
static float g_root_yaw;

static int one_in(int n) { return rng_float(0, 1) < 1.0f / (float)n; }

void skyfx_clear(void)
{
    g_nq = 0;
    g_nsun = 0;
}

void skyfx_day_start(void)
{
    g_root_yaw = rng_float(0, 360); /* randomiseRotation */
}

void skyfx_test(void)
{
    /* every effect at once, facing +X (test route) */
    static const EnvFx e = {
        {1, 0.4f, 0, 1}, 0.0001f, 2, 1,
        {0.2f, 0.4f, 1, 1}, 0.6f, 1, -0.6f, 2.6f,
        {1, 0.6f, 0, 1}, 1, 1,
        {1, 1, 1, 1}, 8, 1,
        {1, 1, 1, 1}, 3, 1,
    };
    skyfx_clear();
    g_root_yaw = 0;
    skyfx_create(&e);
}

static void add_quads(int n, const float color[4], int tex)
{
    for (int i = 0; i < n && g_nq < MAX_QUADS; i++) {
        Quad *q = &g_q[g_nq++];
        memset(q, 0, sizeof(*q));
        float x = rng_float(-8, 8), y = rng_float(2, 3), z = rng_float(-8, 8);
        q->local = v3(x, y, z);
        memcpy(q->tint, color, sizeof(q->tint));
        q->scale = rng_float(1, 4);
        q->yaw = rng_float(0, 360);
        q->tex = tex;
    }
}

static void sunburst(Sun *s)
{
    for (int i = 0; i < 4; i++) {
        int lng = one_in(2);
        s->ray[i].sx = (lng ? 4.0f : 2.0f) * s->burst_scale;
        s->ray[i].sy = (1.0f / 16.0f) * s->burst_scale;
        s->ray[i].angle = rng_float(0, 360);
    }
}

static void add_sun(const float color[4], float scale, float height, float offset, int burst, float burst_scale,
                    const float burst_color[4])
{
    if (g_nsun >= 4)
        return;
    Sun *s = &g_sun[g_nsun];
    memset(s, 0, sizeof(*s));
    s->local = v3(5, height, offset);
    s->burst_scale = 1;
    sunburst(s); /* Awake */
    s->order = g_nsun++;
    memcpy(s->tint, color, sizeof(s->tint));
    s->scale = scale;
    if (burst) { /* ShowSunburst */
        s->burst = 1;
        s->burst_scale = burst_scale;
        memcpy(s->burst_tint, burst_color, sizeof(s->burst_tint));
        sunburst(s);
    }
}

void skyfx_create(const EnvFx *e)
{
    int first_cloud = g_nq;
    if (e->clouds_chance > 0 && rng_float(0, 1) < e->clouds_chance)
        add_quads(e->nclouds, e->clouds_color, SKY_TEX_CLOUDS);
    int clouds_end = g_nq;
    /* (the original only shows the sun when the roll is ABOVE its chance) */
    if (e->sun_chance > 0 && rng_float(0, 1) > e->sun_chance) {
        if (e->burst_chance > 0 && rng_float(0, 1) < e->burst_chance)
            add_sun(e->sun_color, e->sun_size, e->sun_height, 0, 1, e->burst_size, e->burst_color);
        else
            add_sun(e->sun_color, e->sun_size, e->sun_height, 0, 0, 0, NULL);
        if (e->sun2_chance > 0 && rng_float(0, 1) < e->sun2_chance)
            add_sun(e->sun2_color, e->sun2_size, e->sun2_height, e->sun2_offset, 0, 0, NULL);
    }
    if (e->stars_chance > 0 && rng_float(0, 1) < e->stars_chance)
        add_quads(e->nstars, e->stars_color, SKY_TEX_STARS);
    /* Cloud.Start (the next frame) */
    for (int i = first_cloud; i < clouds_end; i++)
        g_q[i].can_move = one_in(4);
}

void skyfx_update(float dt)
{
    for (int i = 0; i < g_nsun; i++) {
        Sun *s = &g_sun[i];
        if (!s->burst)
            continue;
        if (s->t > 0.1f) {
            sunburst(s);
            s->t = 0;
        }
        s->t += dt;
    }
    for (int i = 0; i < g_nq; i++) {
        Quad *q = &g_q[i];
        if (!q->can_move)
            continue;
        if (q->t > 5.0f) {
            q->t = 0;
            int choice = rng_int(4);
            if (choice == 0) {
                q->move = v3(0, 0, 0);
                q->rot = 0;
            }
            if (choice == 1 || choice == 3)
                q->rot = rng_float(20, 50) * dt;
            if (choice == 2 || choice == 3) {
                float mx = rng_float(-1, 1), mz = rng_float(-1, 1);
                vec3 m = v3(mx, 0, mz);
                float l = v3len(m);
                m = l > 1e-5f ? v3scale(m, 1.0f / l) : v3(0, 0, 0);
                q->move = v3scale(m, dt * rng_float(1, 3));
            }
        }
        q->local = v3add(q->local, q->move);
        if (q->rot > 0) q->yaw += q->rot;
        q->t += dt;
    }
}

int skyfx_count(int *suns, int *clouds, int *stars)
{
    int c = 0, s = 0;
    for (int i = 0; i < g_nq; i++) {
        if (g_q[i].tex == SKY_TEX_CLOUDS) c++;
        else s++;
    }
    if (suns) *suns = g_nsun;
    if (clouds) *clouds = c;
    if (stars) *stars = s;
    return g_nsun + g_nq;
}

static int by_key(const void *a, const void *b)
{
    float d = ((const SkyDraw *)a)->key - ((const SkyDraw *)b)->key;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

int skyfx_draws(vec3 eye, SkyDraw *out, int max)
{
    /* the effects object follows the camera's position and keeps its own yaw */
    quat root = qaxis(g_root_yaw, v3(0, 1, 0));
    static const quat PREFAB = {-0.70710576f, 0, 0, 0.70710784f}; /* Clouds/Stars quads face down */
    int n = 0;
    /* Unity's Background queue (1000): sun i at 1000 + i, its rays one before; within a queue
     * nearer objects first */
    for (int i = 0; i < g_nq && n < max; i++) {
        const Quad *q = &g_q[i];
        SkyDraw *d = &out[n++];
        memset(d, 0, sizeof(*d));
        vec3 p = v3add(eye, qrot(root, q->local));
        quat r = qmul(qaxis(q->yaw + g_root_yaw, v3(0, 1, 0)), PREFAB);
        d->world = aff_trs(p, r, v3(q->scale, q->scale, q->scale));
        d->tex = q->tex;
        memcpy(d->tint, q->tint, sizeof(d->tint));
        d->key = 1000.0f * 1000.0f + v3len(v3sub(p, eye));
    }
    for (int i = 0; i < g_nsun; i++) {
        const Sun *s = &g_sun[i];
        vec3 p = v3add(eye, qrot(root, s->local));
        float dist = v3len(v3sub(p, eye));
        if (s->burst)
            for (int k = 0; k < 4 && n < max; k++) {
                SkyDraw *d = &out[n++];
                memset(d, 0, sizeof(*d));
                d->sun = 1;
                d->tex = SKY_TEX_SUN;
                d->pos = p;
                d->sx = s->ray[k].sx;
                d->sy = s->ray[k].sy;
                d->angle = s->ray[k].angle;
                memcpy(d->tint, s->burst_tint, sizeof(d->tint));
                d->key = (float)(1000 + s->order - 1) * 1000.0f + dist;
            }
        if (n < max) {
            SkyDraw *d = &out[n++];
            memset(d, 0, sizeof(*d));
            d->sun = 1;
            d->tex = SKY_TEX_SUN;
            d->pos = p;
            d->sx = d->sy = s->scale / 2.0f;
            memcpy(d->tint, s->tint, sizeof(d->tint));
            d->key = (float)(1000 + s->order) * 1000.0f + dist;
        }
    }
    qsort(out, (size_t)n, sizeof(SkyDraw), by_key);
    return n;
}
