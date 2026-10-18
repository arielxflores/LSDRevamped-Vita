/* The Grey Man (GreymanSpawner.cs, Greyman.cs): on even days, in dreams that allow him,
 * a 1-in-10 roll every 10-30 s (and on every new dream) puts him 6 units in front of the
 * player, walking towards where the player stood. Meeting him (within 2 units) flashes
 * the screen white and pushes the day's graph position hard towards static/downer. */
#include "greyman.h"
#include "luaapi.h"
#include "scene.h"
#include "world.h"

#define MIN_WAIT 10.0f
#define MAX_WAIT 30.0f
#define CHANCE 10.0f
#define SPAWN_DISTANCE 6.0f   /* Player.prefab GreymanSpawnDistance */
#define MOVE_SPEED 0.5f       /* Greyman.prefab */
#define FLASH_DISTANCE 2.0f
#define GREYMAN_VCI 3.0f      /* GreyMan.mat */

typedef struct {
    int alive, visible, encountered;
    vec3 pos;
    quat rot;
} GreyMan;

static Model *g_model;
static int g_nmodel;
static GreyMan g_men[4];
static int g_rolling;
static float g_wait;

int greyman_init(void)
{
    uint32_t vram = 0;
    if (!models_load("greyman.bin", &g_model, &g_nmodel, &vram) || g_nmodel < 1) {
        lsd_log("grey man model missing");
        return 0;
    }
    return 1;
}

void greyman_shutdown(void)
{
    models_free(g_model, g_nmodel);
    g_model = NULL;
    g_nmodel = 0;
}

void greyman_clear(void)
{
    memset(g_men, 0, sizeof(g_men));
}

int greyman_spawn(void)
{
    vec3 fwd = v3(sinf(P.yaw * DEG2RAD), 0, cosf(P.yaw * DEG2RAD));
    vec3 pos = v3add(P.feet, v3scale(fwd, SPAWN_DISTANCE));
    for (int i = 0; i < 4; i++) {
        if (g_men[i].alive) continue;
        GreyMan *g = &g_men[i];
        memset(g, 0, sizeof(*g));
        g->alive = g->visible = 1;
        g->pos = pos;
        g->rot = qlook(v3sub(P.feet, pos), v3(0, 1, 0));
        lsd_log("grey man appears at %.2f %.2f %.2f", pos.x, pos.y, pos.z);
        return 1;
    }
    return 0;
}

static void roll(void)
{
    /* GreymanSpawner.RollSpawn */
    if (DS.mode != DS_DREAM || DS.cur < 0) return;
    if (DS.day % 2 != 0) return;
    int hit = rng_float(0, 1) < 1.0f / CHANCE;
    if ((DS.journal.d[DS.cur].flags & JD_GREY_MAN) && hit)
        greyman_spawn();
}

void greyman_day_start(void)
{
    /* GreymanSpawner.Start: RollForGreyman begins (it rolls straight away) */
    g_rolling = 1;
    roll();
    g_wait = rng_float(MIN_WAIT, MAX_WAIT);
}

void greyman_new_dream(void)
{
    greyman_clear();
    roll();
}

void greyman_day_end(void)
{
    g_rolling = 0;
    greyman_clear();
}

void greyman_update(float dt)
{
    if (g_rolling) {
        g_wait -= dt;
        if (g_wait <= 0) {
            roll();
            g_wait = rng_float(MIN_WAIT, MAX_WAIT);
        }
    }
    if (DS.mode != DS_DREAM || DS.cur < 0) return;
    for (int i = 0; i < 4; i++) {
        GreyMan *g = &g_men[i];
        if (!g->alive || g->encountered) continue;
        vec3 fwd = qrot(g->rot, v3(0, 0, 1));
        g->pos = v3add(g->pos, v3scale(fwd, MOVE_SPEED * dt));
        if (v3len(v3sub(g->pos, P.feet)) < FLASH_DISTANCE) {
            g->encountered = 1;
            lsd_log("the grey man meets the player");
            ds_log_area(-10, -10);
            static const float WHITE[3] = {1, 1, 1};
            ds_fade_try(1.0f, 0.1f, WHITE, FADE_DONE_GREY_FLASH);
        }
    }
}

void greyman_flash_done(void)
{
    for (int i = 0; i < 4; i++)
        if (g_men[i].encountered) g_men[i].visible = 0;
}

void greyman_gone(void)
{
    for (int i = 0; i < 4; i++)
        if (g_men[i].encountered) g_men[i].alive = 0;
}

void greyman_draws(vec3 eye, float max_dist)
{
    if (!g_model) return;
    const ModelMesh *mm = &g_model[0].mesh[0];
    for (int i = 0; i < 4; i++) {
        GreyMan *g = &g_men[i];
        if (!g->alive || !g->visible) continue;
        if (v3len(v3sub(g->pos, eye)) > max_dist + 1) continue;
        aff w = aff_trs(g->pos, g->rot, v3(1, 1, 1));
        for (int b = 0; b < mm->nbatch; b++) {
            EntityDraw *e = scene_add_draw();
            e->batch = &mm->batch[b];
            e->world = w;
            e->vci = GREYMAN_VCI;
            e->cull = 0;
        }
    }
}

int greyman_count(void)
{
    int n = 0;
    for (int i = 0; i < 4; i++) n += g_men[i].alive;
    return n;
}
