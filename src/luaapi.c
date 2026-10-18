/* LSD Revamped's Lua scripting API (LSDR used MoonSharp) on Lua 5.2.
 *
 * Mirrors Assets/Scripts/Lua (LuaEngine, LSDAPI, UnityAPI, RandomAPI, the proxies) and
 * Packages/.../Lua (LuaAsyncActionRunner, LuaAsyncAction, ActionPredicates, the script hosts).
 * MoonSharp lets scripts call methods with '.', so every method a proxy hands out is a
 * closure already bound to its object. Scripts are converted at bake time (tools/luafix.py)
 * for MoonSharp's "|| expr" lambdas and "!=". */
#include "luaapi.h"
#include "audio.h"
#include "world.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"

PlayerExternal g_player_ext;

/* ------------------------------------------------------------------ state */

typedef struct {
    lua_State *L;
    int comp;
    int f_start, f_update, f_interact, f_interval, f_trig, f_trig_exit;
    char name[64];
    int errors;
} Script;

typedef struct {
    float t, interval;
    int interacted;
    float distance;
    int script;           /* index into g_scripts, -1 */
} Interactive;

typedef struct {
    int root, cur;
    int running, looping, first_begun, enum_active;
    int executing;
    int ngarbage, garbage[16];
} Runner;

typedef struct {
    lua_State *L;
    int fn;               /* LUA_NOREF = no-op */
    int pred;             /* -1 = none */
    int next;
    int runner;
    uint32_t gen;
    int used;
} Act;

enum { P_DEFAULT, P_SECONDS, P_LINEAR, P_POINTING, P_CUSTOM };

typedef struct {
    int type;
    int refs;
    uint32_t gen;
    float secs;
    double start;         /* Time.time is a double here: a float start could round below it */
    int node;
    vec3 target, startpos;
    int begun;
    float init_angle;
    lua_State *L;
    int fn;
} Pred;

/* DreamAudio (and its AudioSource) */
enum { CO_NONE, CO_WAIT_AUDIBLE, CO_TOP, CO_DELAY_BEFORE, CO_PLAYING_FOR, CO_AFTER_PLAY, CO_YIELD, CO_DELAY_BETWEEN };

typedef struct {
    int src;            /* mixer source, -1 */
    int sample;         /* SE sample of the clip, -1 */
    int heard;          /* _playing: the player came within range once */
    float think;        /* _t */
    int co, co_state;   /* the audioPlayer coroutine */
    float co_t;
    uint32_t co_frame;
    float pitch, volume;
} AudioState;

typedef struct {
    lua_State *L;
    int fn;
} Callback;

static const Dream *D;
static Script *g_scripts;
static int g_nscripts, g_cap_scripts;
static int *g_comp_script;
static Interactive *g_inter;
static Runner *g_runners;     /* ncomp + 1 (player) */
static Act *g_acts;
static int g_nacts, g_cap_acts, g_free_act = -1;
static Pred *g_preds;
static int g_npreds, g_cap_preds, g_free_pred = -1;
static AudioState *g_audio;
static int g_naudio;
static uint32_t g_frame_no;
static Callback g_timeout[16];
static int g_ntimeout;
static int g_errors;
static double g_clock;        /* Time.time */
static float g_dt;

#define RUNNER_PLAYER (D->ncomp)

/* DreamSystemProxy's static "next transition" settings */
static struct {
    int has_color;
    float color[3];
    int sound;
    uint32_t spawn;
    int dream;
    int lock;
} g_next = {0, {0, 0, 0}, 1, 0, -1, 0};

int luaapi_errors(void) { return g_errors; }

/* ------------------------------------------------------------------ helpers */

static int call(lua_State *L, int nargs, int nres, const char *what, Script *sc);

static void push_vec3(lua_State *L, vec3 v);
static vec3 check_vec3(lua_State *L, int idx);

static Script *script_of_state(lua_State *L)
{
    for (int i = 0; i < g_nscripts; i++)
        if (g_scripts[i].L == L) return &g_scripts[i];
    return NULL;
}

static int traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    if (msg)
        luaL_traceback(L, L, msg, 1);
    else
        lua_pushliteral(L, "(error object is not a string)");
    return 1;
}

static int call(lua_State *L, int nargs, int nres, const char *what, Script *sc)
{
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int r = lua_pcall(L, nargs, nres, base);
    lua_remove(L, base);
    if (r != LUA_OK) {
        g_errors++;
        /* found again: the call may have created scripts and moved g_scripts */
        (void)sc;
        sc = script_of_state(L);
        if (!sc || sc->errors < 8)
            lsd_log("lua error in %s (%s): %s", sc ? sc->name : "?", what, lua_tostring(L, -1));
        if (sc) sc->errors++;
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

static void call_ref(lua_State *L, int ref, const char *what)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL || !L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    call(L, 0, 0, what, NULL);
}

static int call_ref_bool(lua_State *L, int ref)
{
    if (ref == LUA_NOREF || !L) return 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (!call(L, 0, 1, "condition", NULL)) return 0;
    int b = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return b;
}

/* ------------------------------------------------------------------ nodes (GameObjects) */

extern Player P;


static vec3 camera_forward(void)
{
    float yaw = (P.yaw + P.behind) * DEG2RAD, pitch = P.look * DEG2RAD;
    return v3(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch));
}

static vec3 node_pos(int n)
{
    if (n == NODE_PLAYER) return P.feet;
    if (n == NODE_CAMERA) return v3(P.feet.x, P.feet.y + PLAYER_HEIGHT + 0.15f + P.bob, P.feet.z);
    return scene_world_pos(n);
}

static quat node_rot(int n)
{
    if (n == NODE_PLAYER) return qaxis(P.yaw, v3(0, 1, 0));
    if (n == NODE_CAMERA) return qmul(qaxis(P.yaw + P.behind, v3(0, 1, 0)), qaxis(-P.look, v3(1, 0, 0)));
    return scene_world_rot(n);
}

static vec3 node_forward(int n) { return qrot(node_rot(n), v3(0, 0, 1)); }

static void node_set_pos(int n, vec3 p)
{
    if (n == NODE_PLAYER) P.feet = p;
    else if (n >= 0) scene_set_world_pos(n, p);
}

static void node_set_rot(int n, quat q)
{
    if (n == NODE_PLAYER) {
        vec3 e = qto_euler(q);
        P.yaw = e.y;
    } else if (n >= 0) {
        scene_set_world_rot(n, q);
    }
}

/* Transform.LookAt(target, transform.up) */
static void node_look_at(int n, vec3 target)
{
    vec3 dir = v3sub(target, node_pos(n));
    if (v3len(dir) < 1e-6f) return;
    vec3 up = qrot(node_rot(n), v3(0, 1, 0));
    node_set_rot(n, qlook(dir, up));
}

/* ------------------------------------------------------------------ userdata */

typedef struct { int v; uint32_t gen; } Ud;

#define MT_GO "lsd.go"
#define MT_IO "lsd.io"
#define MT_RUNNER "lsd.runner"
#define MT_ACTION "lsd.action"
#define MT_PRED "lsd.pred"
#define MT_AUDIO "lsd.audio"
#define MT_ANIM "lsd.anim"
#define MT_MOVIE "lsd.movie"
#define MT_PM "lsd.pm"
#define MT_DREAM "lsd.dream"
#define MT_TRIGGER "lsd.trigger"
#define MT_DS "lsd.dreamsystem"

static Ud *push_ud(lua_State *L, const char *mt, int v, uint32_t gen)
{
    Ud *u = (Ud *)lua_newuserdata(L, sizeof(Ud));
    u->v = v;
    u->gen = gen;
    luaL_setmetatable(L, mt);
    return u;
}

static Ud *self_ud(lua_State *L) { return (Ud *)lua_touserdata(L, lua_upvalueindex(1)); }

static void bind(lua_State *L, lua_CFunction f)
{
    lua_pushvalue(L, 1); /* the userdata the method belongs to */
    lua_pushcclosure(L, f, 1);
}

static void push_go(lua_State *L, int node)
{
    if (node == -1) lua_pushnil(L);
    else push_ud(L, MT_GO, node, 0);
}

static int check_node(lua_State *L, int idx)
{
    Ud *u = (Ud *)luaL_checkudata(L, idx, MT_GO);
    return u->v;
}

/* ------------------------------------------------------------------ vectors and colours */

static int vec_index(lua_State *L);

static vec3 check_vec3(lua_State *L, int idx)
{
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "x");
    lua_getfield(L, idx, "y");
    lua_getfield(L, idx, "z");
    vec3 v = v3((float)lua_tonumber(L, -3), (float)lua_tonumber(L, -2), (float)lua_tonumber(L, -1));
    lua_pop(L, 3);
    return v;
}

static void push_vec3(lua_State *L, vec3 v)
{
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z);
    lua_setfield(L, -2, "z");
    luaL_setmetatable(L, "lsd.vec3");
}

static int vec_add(lua_State *L) { push_vec3(L, v3add(check_vec3(L, 1), check_vec3(L, 2))); return 1; }
static int vec_sub(lua_State *L) { push_vec3(L, v3sub(check_vec3(L, 1), check_vec3(L, 2))); return 1; }
static int vec_mul(lua_State *L) { push_vec3(L, v3scale(check_vec3(L, 1), (float)luaL_checknumber(L, 2))); return 1; }
static int vec_div(lua_State *L)
{
    float s = (float)luaL_checknumber(L, 2);
    vec3 v = check_vec3(L, 1);
    push_vec3(L, v3(v.x / s, v.y / s, v.z / s));
    return 1;
}
static int vec_tostring(lua_State *L)
{
    vec3 v = check_vec3(L, 1);
    lua_pushfstring(L, "(%f, %f, %f)", (double)v.x, (double)v.y, (double)v.z);
    return 1;
}
static int vec_length(lua_State *L) { lua_pushnumber(L, v3len(check_vec3(L, lua_upvalueindex(1)))); return 1; }
static int vec_normalise(lua_State *L)
{
    vec3 v = check_vec3(L, lua_upvalueindex(1));
    float l = v3len(v);
    push_vec3(L, l > 1e-5f ? v3scale(v, 1.0f / l) : v3(0, 0, 0)); /* Vector3.Normalize */
    return 1;
}
static int vec_negated(lua_State *L) { push_vec3(L, v3scale(check_vec3(L, lua_upvalueindex(1)), -1)); return 1; }
static int vec_dot(lua_State *L) { lua_pushnumber(L, v3dot(check_vec3(L, lua_upvalueindex(1)), check_vec3(L, 1))); return 1; }
static int vec_project(lua_State *L)
{
    vec3 a = check_vec3(L, lua_upvalueindex(1)), b = check_vec3(L, 1);
    float bb = v3dot(b, b);
    push_vec3(L, bb < 1e-12f ? v3(0, 0, 0) : v3scale(b, v3dot(a, b) / bb));
    return 1;
}

static int vec_index(lua_State *L)
{
    const char *k = lua_tostring(L, 2);
    lua_CFunction f = NULL;
    if (!k) return 0;
    if (!strcmp(k, "length")) f = vec_length;
    else if (!strcmp(k, "normalise")) f = vec_normalise;
    else if (!strcmp(k, "negated")) f = vec_negated;
    else if (!strcmp(k, "dot")) f = vec_dot;
    else if (!strcmp(k, "project")) f = vec_project;
    if (!f) return 0;
    bind(L, f);
    return 1;
}

static void push_color(lua_State *L, float r, float g, float b, float a)
{
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, r); lua_setfield(L, -2, "r");
    lua_pushnumber(L, g); lua_setfield(L, -2, "g");
    lua_pushnumber(L, b); lua_setfield(L, -2, "b");
    lua_pushnumber(L, a); lua_setfield(L, -2, "a");
}

static void check_color(lua_State *L, int idx, float rgb[3])
{
    luaL_checktype(L, idx, LUA_TTABLE);
    lua_getfield(L, idx, "r");
    lua_getfield(L, idx, "g");
    lua_getfield(L, idx, "b");
    rgb[0] = (float)lua_tonumber(L, -3);
    rgb[1] = (float)lua_tonumber(L, -2);
    rgb[2] = (float)lua_tonumber(L, -1);
    lua_pop(L, 3);
}

/* ------------------------------------------------------------------ predicates */

static int pred_new(int type)
{
    int i;
    if (g_free_pred >= 0) {
        i = g_free_pred;
        g_free_pred = g_preds[i].node; /* free list link */
    } else {
        if (g_npreds == g_cap_preds) {
            g_cap_preds = g_cap_preds ? g_cap_preds * 2 : 64;
            g_preds = (Pred *)realloc(g_preds, g_cap_preds * sizeof(Pred));
        }
        i = g_npreds++;
    }
    uint32_t gen = g_preds[i].gen + 1;
    memset(&g_preds[i], 0, sizeof(Pred));
    g_preds[i].gen = gen;
    g_preds[i].type = type;
    g_preds[i].fn = LUA_NOREF;
    return i;
}

static void pred_unref(int i)
{
    if (i < 0 || i >= g_npreds) return;
    Pred *p = &g_preds[i];
    if (--p->refs > 0) return;
    if (p->L && p->fn != LUA_NOREF) luaL_unref(p->L, LUA_REGISTRYINDEX, p->fn);
    p->type = -1;
    p->gen++;
    p->node = g_free_pred;
    g_free_pred = i;
}

static float signed_angle_sign(vec3 from, vec3 to)
{
    /* sign of Vector3.SignedAngle(from, to, up) */
    float c = from.z * to.x - from.x * to.z; /* cross(from, to).y */
    return c;
}

static void pred_begin(int i)
{
    if (i < 0) return;
    Pred *p = &g_preds[i];
    switch (p->type) {
    case P_SECONDS: p->start = g_clock; break;
    case P_LINEAR: p->startpos = node_pos(p->node); p->begun = 1; break;
    case P_POINTING: {
        vec3 d = v3sub(p->target, node_pos(p->node));
        d.y = 0;
        p->init_angle = signed_angle_sign(d, node_forward(p->node));
        p->begun = 1;
        break;
    }
    default: break;
    }
}

static int pred_test(int i)
{
    if (i < 0) return 1;
    Pred *p = &g_preds[i];
    switch (p->type) {
    case P_DEFAULT: return 1;
    case P_SECONDS: return g_clock > p->start + p->secs;
    case P_LINEAR: {
        if (!p->begun) return 0;
        vec3 d = v3sub(p->target, p->startpos);
        vec3 q = v3sub(node_pos(p->node), p->startpos);
        float dd = v3dot(d, d);
        if (dd <= 0) return 0;
        return v3dot(q, d) / dd >= 1.0f;
    }
    case P_POINTING: {
        vec3 d = v3sub(p->target, node_pos(p->node));
        d.y = 0;
        float cur = signed_angle_sign(d, node_forward(p->node));
        return (p->init_angle < 0) != (cur < 0);
    }
    case P_CUSTOM: return call_ref_bool(p->L, p->fn);
    default: return 1;
    }
}

static int g_closing;

static int pred_gc(lua_State *L)
{
    if (g_closing) return 0;
    Ud *u = (Ud *)luaL_checkudata(L, 1, MT_PRED);
    if (u->v >= 0 && u->v < g_npreds && g_preds[u->v].gen == u->gen) pred_unref(u->v);
    return 0;
}

static void push_pred(lua_State *L, int i)
{
    g_preds[i].refs++;
    push_ud(L, MT_PRED, i, g_preds[i].gen);
}

static int check_pred(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) return -1;
    Ud *u = (Ud *)luaL_checkudata(L, idx, MT_PRED);
    if (u->v < 0 || u->v >= g_npreds || g_preds[u->v].gen != u->gen) return -1;
    return u->v;
}

static int cond_default(lua_State *L) { push_pred(L, pred_new(P_DEFAULT)); return 1; }
static int cond_seconds(lua_State *L)
{
    int i = pred_new(P_SECONDS);
    g_preds[i].secs = (float)luaL_checknumber(L, 1);
    push_pred(L, i);
    return 1;
}
static int cond_linear(lua_State *L)
{
    int i = pred_new(P_LINEAR);
    g_preds[i].node = check_node(L, 1);
    g_preds[i].target = check_vec3(L, 2);
    push_pred(L, i);
    return 1;
}
static int cond_pointing(lua_State *L)
{
    int i = pred_new(P_POINTING);
    g_preds[i].node = check_node(L, 1);
    g_preds[i].target = check_vec3(L, 2);
    push_pred(L, i);
    return 1;
}
static int cond_custom(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    int i = pred_new(P_CUSTOM);
    lua_pushvalue(L, 1);
    g_preds[i].L = L;
    g_preds[i].fn = luaL_ref(L, LUA_REGISTRYINDEX);
    push_pred(L, i);
    return 1;
}

/* ------------------------------------------------------------------ actions and runners */

static int act_new(lua_State *L, int fn_idx, int runner)
{
    int i;
    if (g_free_act >= 0) {
        i = g_free_act;
        g_free_act = g_acts[i].next;
    } else {
        if (g_nacts == g_cap_acts) {
            g_cap_acts = g_cap_acts ? g_cap_acts * 2 : 128;
            g_acts = (Act *)realloc(g_acts, g_cap_acts * sizeof(Act));
        }
        i = g_nacts++;
        g_acts[i].gen = 0;
    }
    Act *a = &g_acts[i];
    uint32_t gen = a->gen + 1;
    memset(a, 0, sizeof(*a));
    a->gen = gen;
    a->used = 1;
    a->L = L;
    a->fn = LUA_NOREF;
    a->pred = -1;
    a->next = -1;
    a->runner = runner;
    if (fn_idx && !lua_isnoneornil(L, fn_idx)) {
        luaL_checktype(L, fn_idx, LUA_TFUNCTION);
        lua_pushvalue(L, fn_idx);
        a->fn = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return i;
}

static void act_free_chain(int i)
{
    int guard = 0;
    while (i >= 0 && i < g_nacts && g_acts[i].used && guard++ < 100000) {
        Act *a = &g_acts[i];
        int next = a->next;
        if (a->L && a->fn != LUA_NOREF) luaL_unref(a->L, LUA_REGISTRYINDEX, a->fn);
        if (a->pred >= 0) pred_unref(a->pred);
        a->used = 0;
        a->gen++;
        a->next = g_free_act;
        g_free_act = i;
        i = next;
    }
}

static int chain_contains(int chain, int act)
{
    int guard = 0;
    for (int i = chain; i >= 0 && i < g_nacts && g_acts[i].used && guard++ < 100000; i = g_acts[i].next)
        if (i == act) return 1;
    return 0;
}

static void runner_collect(Runner *r)
{
    if (r->executing) return;
    /* a discarded chain the runner is still walking stays alive until it leaves it (as C#'s GC would) */
    int keep = 0;
    for (int k = 0; k < r->ngarbage; k++) {
        if (r->cur >= 0 && chain_contains(r->garbage[k], r->cur)) r->garbage[keep++] = r->garbage[k];
        else act_free_chain(r->garbage[k]);
    }
    r->ngarbage = keep;
}

/* an action chain that is no longer reachable from the runner's root */
static void runner_discard(int ri, int chain)
{
    if (chain < 0) return;
    Runner *r = (g_runners && D && ri >= 0 && ri <= RUNNER_PLAYER) ? &g_runners[ri] : NULL;
    if (r && r->ngarbage < 16) {
        r->garbage[r->ngarbage++] = chain;
        return;
    }
    if (r && (r->executing || (r->cur >= 0 && chain_contains(chain, r->cur))))
        return; /* (still in use with a full garbage list: it leaks until the dream ends) */
    act_free_chain(chain);
}

static void runner_set_root(Runner *r, int act)
{
    if (r->root >= 0 && r->root != act) {
        if (r->ngarbage < 16) r->garbage[r->ngarbage++] = r->root;
        /* (a full garbage list just leaks until the dream ends) */
    }
    r->root = act;
    r->cur = act;
}

static void runner_stop(Runner *r)
{
    r->running = 0;
    r->first_begun = 0;
    r->enum_active = 0;
}

static void runner_update(int ri)
{
    Runner *r = &g_runners[ri];
    runner_collect(r);
    if (!r->running || r->root < 0) return;
    if (!r->first_begun) {
        pred_begin(g_acts[r->root].pred);
        r->first_begun = 1;
    }
    if (!r->enum_active) {
        r->enum_active = 1;
        r->cur = r->root;
    }
    r->executing = 1;
    for (int guard = 0; guard < 10000; guard++) {
        if (!r->running) break;
        if (r->cur < 0) {
            if (r->looping) {
                r->cur = r->root;
                pred_begin(g_acts[r->root].pred);
            } else {
                runner_stop(r);
                break;
            }
        }
        int ai = r->cur;
        call_ref(g_acts[ai].L, g_acts[ai].fn, "action");
        if (!g_scene.loaded) { r->executing = 0; return; } /* the dream went away */
        if (ai != r->cur && r->cur >= 0) continue; /* the action replaced the chain */
        if (g_acts[ai].pred >= 0 && !pred_test(g_acts[ai].pred))
            break; /* yield: run again next frame */
        r->cur = g_acts[ai].next;
        if (r->cur >= 0) pred_begin(g_acts[r->cur].pred);
    }
    r->executing = 0;
}

void luaapi_runner_update_comp(int comp) { if (g_runners) runner_update(comp); }
void luaapi_player_runner_update(void) { if (g_runners) runner_update(RUNNER_PLAYER); }

static int action_check(lua_State *L, Ud *u)
{
    if (u->v < 0 || u->v >= g_nacts || g_acts[u->v].gen != u->gen || !g_acts[u->v].used)
        luaL_error(L, "this action chain is no longer valid");
    return u->v;
}

static int act_until(lua_State *L)
{
    Ud *u = self_ud(L);
    int a = action_check(L, u);
    int p = check_pred(L, 1);
    if (p >= 0) {
        if (g_acts[a].pred >= 0) pred_unref(g_acts[a].pred);
        g_preds[p].refs++;
        g_acts[a].pred = p;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

static int act_then(lua_State *L)
{
    Ud *u = self_ud(L);
    int a = action_check(L, u);
    int n = act_new(L, 1, g_acts[a].runner);
    int old = g_acts[a].next;
    g_acts[a].next = n;
    runner_discard(g_acts[a].runner, old); /* Then() replaces the chained action */
    push_ud(L, MT_ACTION, n, g_acts[n].gen);
    return 1;
}

static int act_then_wait(lua_State *L)
{
    Ud *u = self_ud(L);
    int a = action_check(L, u);
    int n = act_new(L, 0, g_acts[a].runner);
    int old = g_acts[a].next;
    g_acts[a].next = n;
    runner_discard(g_acts[a].runner, old);
    int p = check_pred(L, 1);
    if (p >= 0) {
        g_preds[p].refs++;
        g_acts[n].pred = p;
    }
    push_ud(L, MT_ACTION, n, g_acts[n].gen);
    return 1;
}

static int act_finish(lua_State *L)
{
    Ud *u = self_ud(L);
    int a = action_check(L, u);
    g_runners[g_acts[a].runner].running = 1;
    return 0;
}

static int act_loop(lua_State *L)
{
    Ud *u = self_ud(L);
    int a = action_check(L, u);
    g_runners[g_acts[a].runner].running = 1;
    g_runners[g_acts[a].runner].looping = 1;
    return 0;
}

static int action_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Until")) bind(L, act_until);
    else if (!strcmp(k, "Then")) bind(L, act_then);
    else if (!strcmp(k, "ThenWaitUntil")) bind(L, act_then_wait);
    else if (!strcmp(k, "ThenFinish")) bind(L, act_finish);
    else if (!strcmp(k, "ThenLoop")) bind(L, act_loop);
    else return luaL_error(L, "LuaAsyncAction has no member '%s'", k);
    return 1;
}

static int runner_do(lua_State *L)
{
    int ri = self_ud(L)->v;
    int a = act_new(L, 1, ri);
    runner_set_root(&g_runners[ri], a);
    push_ud(L, MT_ACTION, a, g_acts[a].gen);
    return 1;
}

static int runner_wait_until(lua_State *L)
{
    int ri = self_ud(L)->v;
    int a = act_new(L, 0, ri);
    int p = check_pred(L, 1);
    if (p >= 0) {
        g_preds[p].refs++;
        g_acts[a].pred = p;
    }
    runner_set_root(&g_runners[ri], a);
    push_ud(L, MT_ACTION, a, g_acts[a].gen);
    return 1;
}

static int runner_stop_l(lua_State *L)
{
    runner_stop(&g_runners[self_ud(L)->v]);
    return 0;
}

static int runner_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Do")) bind(L, runner_do);
    else if (!strcmp(k, "WaitUntil")) bind(L, runner_wait_until);
    else if (!strcmp(k, "Stop")) bind(L, runner_stop_l);
    else return luaL_error(L, "LuaAsyncActionRunner has no member '%s'", k);
    return 1;
}

/* ------------------------------------------------------------------ InteractiveObject proxy */

static int io_node(int comp) { return D->comp[comp].node; }
static int io_anim(lua_State *L, int comp)
{
    int a = scene_comp_on(io_node(comp), COMP_ANIMATED);
    if (a < 0) luaL_error(L, "%s has no AnimatedObject", g_scene.name[io_node(comp)]);
    return a;
}

static int io_play_anim(lua_State *L) { anim_play(io_anim(L, self_ud(L)->v), (int)luaL_checkinteger(L, 1)); return 0; }
static int io_stop_anim(lua_State *L) { anim_stop(io_anim(L, self_ud(L)->v)); return 0; }
static int io_resume_anim(lua_State *L) { anim_resume(io_anim(L, self_ud(L)->v)); return 0; }
static int io_wait_anim(lua_State *L)
{
    int c = self_ud(L)->v;
    float len = anim_length(io_anim(L, c), (int)luaL_checkinteger(L, 1));
    int i = pred_new(P_SECONDS);
    g_preds[i].secs = len + (float)luaL_optnumber(L, 2, 0);
    push_pred(L, i);
    return 1;
}
static int io_anim_len(lua_State *L)
{
    lua_pushnumber(L, anim_length(io_anim(L, self_ud(L)->v), (int)luaL_checkinteger(L, 1)));
    return 1;
}
static void move_dir(int n, vec3 dir, float speed)
{
    vec3 p = scene_world_pos(n);
    scene_set_world_pos(n, v3add(p, v3scale(dir, speed * g_dt)));
}
static int io_move_towards(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    vec3 target = check_vec3(L, 1);
    float speed = (float)luaL_checknumber(L, 2);
    vec3 to = v3sub(target, scene_world_pos(n));
    float len = v3len(to);
    if (len < 0.1f) {
        scene_set_world_pos(n, target);
        lua_pushboolean(L, 1);
        return 1;
    }
    move_dir(n, v3scale(to, 1.0f / len), speed);
    lua_pushboolean(L, 0);
    return 1;
}
static int io_move_dir(lua_State *L)
{
    move_dir(io_node(self_ud(L)->v), check_vec3(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int io_look_at(lua_State *L) { node_look_at(io_node(self_ud(L)->v), check_vec3(L, 1)); return 0; }
static int io_look_at_plane(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    vec3 t = check_vec3(L, 1);
    t.y = scene_world_pos(n).y;
    node_look_at(n, t);
    return 0;
}
static int io_look_dir(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    node_look_at(n, v3add(scene_world_pos(n), check_vec3(L, 1)));
    return 0;
}
static int io_look_towards(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    vec3 target = check_vec3(L, 1);
    float speed = (float)luaL_checknumber(L, 2);
    quat cur = scene_world_rot(n);
    vec3 to = v3sub(target, scene_world_pos(n));
    float l = v3len(to);
    if (l > 1e-9f) to = v3scale(to, 1.0f / l);
    to.y = 0;
    if (v3dot(to, to) < 0.01f) {
        lua_pushboolean(L, 1);
        return 1;
    }
    quat desired = qlook(to, qrot(cur, v3(0, 1, 0)));
    if (qdot(cur, desired) > 0.99f) {
        scene_set_world_rot(n, desired);
        lua_pushboolean(L, 1);
        return 1;
    }
    scene_set_world_rot(n, qrotate_towards(cur, desired, speed * g_dt));
    lua_pushboolean(L, 0);
    return 1;
}
static int io_snap_floor(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    float h = (float)luaL_optnumber(L, 1, 10);
    vec3 p = scene_world_pos(n);
    float y;
    if (g_dream_loaded && col_ray_down(&g_dream, ds_to_dream(v3(p.x, p.y + h, p.z)), &y))
        scene_set_world_pos(n, v3(p.x, ds_from_dream(v3(0, y, 0)).y, p.z));
    return 0;
}
static int io_stretch(lua_State *L)
{
    scene_set_scale(io_node(self_ud(L)->v), v3(1, (float)luaL_checknumber(L, 1), 1));
    return 0;
}
static int io_child_visible(lua_State *L)
{
    int n = io_node(self_ud(L)->v);
    int c = g_scene.first_child[n];
    if (c < 0) return luaL_error(L, "%s has no children", g_scene.name[n]);
    scene_set_active(c, lua_toboolean(L, 1));
    return 0;
}
static void renderers_active(int n, int on)
{
    if (D->node[n].flags & 2) scene_set_active(n, on);
    for (int c = g_scene.first_child[n]; c >= 0; c = g_scene.next_sibling[c]) renderers_active(c, on);
}
static int io_renderers_active(lua_State *L)
{
    renderers_active(io_node(self_ud(L)->v), lua_toboolean(L, 1));
    return 0;
}
static int io_interval(lua_State *L)
{
    g_inter[self_ud(L)->v].interval = (float)luaL_checknumber(L, 1);
    return 0;
}
static int io_contribute(lua_State *L)
{
    ds_log_entity((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), io_node(self_ud(L)->v));
    return 0;
}
static int io_init(lua_State *L) { luaapi_entity_init(self_ud(L)->v); return 0; }

static int io_index(lua_State *L)
{
    int c = ((Ud *)luaL_checkudata(L, 1, MT_IO))->v;
    int n = io_node(c);
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "GameObject")) push_go(L, n);
    else if (!strcmp(k, "Forward")) push_vec3(L, qrot(scene_world_rot(n), v3(0, 0, 1)));
    else if (!strcmp(k, "Up")) push_vec3(L, qrot(scene_world_rot(n), v3(0, 1, 0)));
    else if (!strcmp(k, "Right")) push_vec3(L, qrot(scene_world_rot(n), v3(1, 0, 0)));
    else if (!strcmp(k, "Action")) {
        int r = scene_comp_on(n, COMP_RUNNER);
        if (r < 0) return luaL_error(L, "%s has no LuaAsyncActionRunner", g_scene.name[n]);
        push_ud(L, MT_RUNNER, r, 0);
    } else if (!strcmp(k, "InteractionDistance")) lua_pushnumber(L, g_inter[c].distance);
    else if (!strcmp(k, "PlayAnimation")) bind(L, io_play_anim);
    else if (!strcmp(k, "StopAnimation")) bind(L, io_stop_anim);
    else if (!strcmp(k, "ResumeAnimation")) bind(L, io_resume_anim);
    else if (!strcmp(k, "WaitForAnimation")) bind(L, io_wait_anim);
    else if (!strcmp(k, "GetAnimationLengthSeconds")) bind(L, io_anim_len);
    else if (!strcmp(k, "MoveTowards")) bind(L, io_move_towards);
    else if (!strcmp(k, "MoveInDirection")) bind(L, io_move_dir);
    else if (!strcmp(k, "LookAt")) bind(L, io_look_at);
    else if (!strcmp(k, "LookAtPlane")) bind(L, io_look_at_plane);
    else if (!strcmp(k, "LookInDirection")) bind(L, io_look_dir);
    else if (!strcmp(k, "LookTowards")) bind(L, io_look_towards);
    else if (!strcmp(k, "SnapToFloor")) bind(L, io_snap_floor);
    else if (!strcmp(k, "StretchShrink")) bind(L, io_stretch);
    else if (!strcmp(k, "SetChildVisible")) bind(L, io_child_visible);
    else if (!strcmp(k, "SetRenderersActive")) bind(L, io_renderers_active);
    else if (!strcmp(k, "SetUpdateIntervalSeconds")) bind(L, io_interval);
    else if (!strcmp(k, "LogGraphContribution")) bind(L, io_contribute);
    else if (!strcmp(k, "Init")) bind(L, io_init);
    else return luaL_error(L, "InteractiveObject has no member '%s'", k);
    return 1;
}

static int io_newindex(lua_State *L)
{
    int c = ((Ud *)luaL_checkudata(L, 1, MT_IO))->v;
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "InteractionDistance")) g_inter[c].distance = (float)luaL_checknumber(L, 3);
    else return luaL_error(L, "can't set InteractiveObject.%s", k);
    return 0;
}

/* ------------------------------------------------------------------ DreamAudio, AnimatedObject, MovieClip */

static void da_start(int c);
static void da_stop(int c);
static int au_play(lua_State *L) { da_start(self_ud(L)->v); return 0; }
static int au_stop(lua_State *L) { da_stop(self_ud(L)->v); return 0; }
static int audio_index(lua_State *L)
{
    int c = ((Ud *)luaL_checkudata(L, 1, MT_AUDIO))->v;
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Play")) bind(L, au_play);
    else if (!strcmp(k, "Stop")) bind(L, au_stop);
    else if (!strcmp(k, "Pitch")) lua_pushnumber(L, g_audio[c].pitch);
    else if (!strcmp(k, "Volume")) lua_pushnumber(L, g_audio[c].volume);
    else return luaL_error(L, "DreamAudio has no member '%s'", k);
    return 1;
}
static int audio_newindex(lua_State *L)
{
    int c = ((Ud *)luaL_checkudata(L, 1, MT_AUDIO))->v;
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Pitch")) g_audio[c].pitch = (float)luaL_checknumber(L, 3);
    else if (!strcmp(k, "Volume")) g_audio[c].volume = (float)luaL_checknumber(L, 3);
    else return luaL_error(L, "can't set DreamAudio.%s", k);
    return 0;
}

static int an_play(lua_State *L) { anim_play(self_ud(L)->v, (int)luaL_checkinteger(L, 1)); return 0; }
static int an_stop(lua_State *L) { anim_stop(self_ud(L)->v); return 0; }
static int an_resume(lua_State *L) { anim_resume(self_ud(L)->v); return 0; }
static int anim_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Play")) bind(L, an_play);
    else if (!strcmp(k, "Stop")) bind(L, an_stop);
    else if (!strcmp(k, "Resume")) bind(L, an_resume);
    else return luaL_error(L, "AnimatedObject has no member '%s'", k);
    return 1;
}

static int mv_play(lua_State *L)
{
    float rgb[3];
    check_color(L, 1, rgb);
    int c = self_ud(L)->v;
    ds_play_video(dream_str(D, D->comp[c].w.u[0]), rgb);
    return 0;
}
static int movie_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "Play")) bind(L, mv_play);
    else return luaL_error(L, "MovieClip has no member '%s'", k);
    return 1;
}

/* ------------------------------------------------------------------ PlayerMovement */

static int pm_can_control(lua_State *L) { DS.can_control = lua_toboolean(L, 1); return 0; }
static int pm_forward(lua_State *L)
{
    g_player_ext.move_external = 1;
    g_player_ext.move_x = 0;
    g_player_ext.move_y = 1;
    g_player_ext.rot_external = 1;
    g_player_ext.rot = 0;
    return 0;
}
static int pm_override(lua_State *L)
{
    g_player_ext.move_external = 1;
    g_player_ext.move_x = g_player_ext.move_y = 0;
    return 0;
}
static int pm_rotate_towards(lua_State *L)
{
    vec3 target = check_vec3(L, 1);
    vec3 f = camera_forward();
    vec3 d = v3sub(target, P.feet);
    float cy = f.z * d.x - f.x * d.z; /* cross(forward, direction).y */
    g_player_ext.rot_external = 1;
    g_player_ext.rot = cy > 0 ? 1.0f : -1.0f;
    return 0;
}
static int pm_stop(lua_State *L)
{
    memset(&g_player_ext, 0, sizeof(g_player_ext));
    return 0;
}
static int pm_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "SetCanControl")) bind(L, pm_can_control);
    else if (!strcmp(k, "StartMovingForward")) bind(L, pm_forward);
    else if (!strcmp(k, "OverrideMovement")) bind(L, pm_override);
    else if (!strcmp(k, "RotateTowards")) bind(L, pm_rotate_towards);
    else if (!strcmp(k, "StopMovementAndRotation")) bind(L, pm_stop);
    else if (!strcmp(k, "Camera")) push_go(L, NODE_CAMERA);
    else return luaL_error(L, "PlayerMovement has no member '%s'", k);
    return 1;
}

/* ------------------------------------------------------------------ GameObject proxy */

static int go_set_active(lua_State *L)
{
    int n = self_ud(L)->v;
    if (n >= 0) scene_set_active(n, lua_toboolean(L, 1));
    return 0;
}
static int go_child(lua_State *L)
{
    int n = self_ud(L)->v;
    const char *name = luaL_checkstring(L, 1);
    int c = n >= 0 ? scene_child_by_name(n, name) : -1;
    if (c < 0) return luaL_error(L, "unable to find GameObject with name '%s'", name);
    push_go(L, c);
    return 1;
}
static int go_pos_to_world(lua_State *L)
{
    int n = self_ud(L)->v;
    vec3 p = check_vec3(L, 1);
    push_vec3(L, n >= 0 ? aff_point(scene_world(n), p) : v3add(node_pos(n), qrot(node_rot(n), p)));
    return 1;
}
static int go_dir_to_world(lua_State *L)
{
    push_vec3(L, qrot(node_rot(self_ud(L)->v), check_vec3(L, 1)));
    return 1;
}
static int go_pos_from_world(lua_State *L)
{
    int n = self_ud(L)->v;
    vec3 p = check_vec3(L, 1);
    if (n >= 0) {
        aff inv = aff_inverse(scene_world(n));
        push_vec3(L, aff_point(&inv, p));
    } else {
        push_vec3(L, qrot(qconj(node_rot(n)), v3sub(p, node_pos(n))));
    }
    return 1;
}
static int go_dir_from_world(lua_State *L)
{
    push_vec3(L, qrot(qconj(node_rot(self_ud(L)->v)), check_vec3(L, 1)));
    return 1;
}
static int go_look_at(lua_State *L) { node_look_at(self_ud(L)->v, check_vec3(L, 1)); return 0; }
static int go_look_at_plane(lua_State *L)
{
    int n = self_ud(L)->v;
    vec3 t = check_vec3(L, 1);
    t.y = node_pos(n).y;
    node_look_at(n, t);
    return 0;
}
static int go_destroy(lua_State *L)
{
    int n = self_ud(L)->v;
    if (n >= 0) scene_set_active(n, 0);
    return 0;
}

static int go_component(lua_State *L, int n, int type, const char *mt, const char *what)
{
    int c = scene_comp_on(n, type);
    if (c < 0) return luaL_error(L, "GameObject '%s' is not a %s", n >= 0 ? g_scene.name[n] : "player", what);
    push_ud(L, mt, c, 0);
    return 1;
}

static int go_index(lua_State *L)
{
    int n = ((Ud *)luaL_checkudata(L, 1, MT_GO))->v;
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "WorldPosition")) push_vec3(L, node_pos(n));
    else if (!strcmp(k, "Name")) lua_pushstring(L, n >= 0 ? g_scene.name[n] : n == NODE_PLAYER ? "Player" : "Camera");
    else if (!strcmp(k, "LocalPosition")) push_vec3(L, n >= 0 ? g_scene.pos[n] : node_pos(n));
    else if (!strcmp(k, "WorldRotation")) push_vec3(L, qto_euler(node_rot(n)));
    else if (!strcmp(k, "LocalRotation")) push_vec3(L, qto_euler(n >= 0 ? g_scene.rot[n] : node_rot(n)));
    else if (!strcmp(k, "Scale")) push_vec3(L, n >= 0 ? g_scene.scale[n] : v3(1, 1, 1));
    else if (!strcmp(k, "ForwardDirection")) push_vec3(L, qrot(node_rot(n), v3(0, 0, 1)));
    else if (!strcmp(k, "RightDirection")) push_vec3(L, qrot(node_rot(n), v3(1, 0, 0)));
    else if (!strcmp(k, "UpDirection")) push_vec3(L, qrot(node_rot(n), v3(0, 1, 0)));
    else if (!strcmp(k, "Active")) lua_pushboolean(L, scene_active(n));
    else if (!strcmp(k, "SetActive")) bind(L, go_set_active);
    else if (!strcmp(k, "GetChildByName")) bind(L, go_child);
    else if (!strcmp(k, "PositionToWorld")) bind(L, go_pos_to_world);
    else if (!strcmp(k, "DirectionToWorld")) bind(L, go_dir_to_world);
    else if (!strcmp(k, "PositionFromWorld")) bind(L, go_pos_from_world);
    else if (!strcmp(k, "DirectionFromWorld")) bind(L, go_dir_from_world);
    else if (!strcmp(k, "LookAt")) bind(L, go_look_at);
    else if (!strcmp(k, "LookAtPlane")) bind(L, go_look_at_plane);
    else if (!strcmp(k, "DestroySelf")) bind(L, go_destroy);
    else if (!strcmp(k, "Action")) {
        if (n == NODE_PLAYER) push_ud(L, MT_RUNNER, RUNNER_PLAYER, 0);
        else return go_component(L, n, COMP_RUNNER, MT_RUNNER, "LuaAsyncActionRunner");
    } else if (!strcmp(k, "InteractiveObject")) return go_component(L, n, COMP_INTERACTIVE, MT_IO, "InteractiveObject");
    else if (!strcmp(k, "DreamAudio")) return go_component(L, n, COMP_AUDIO, MT_AUDIO, "DreamAudio");
    else if (!strcmp(k, "AnimatedObject")) return go_component(L, n, COMP_ANIMATED, MT_ANIM, "AnimatedObject");
    else if (!strcmp(k, "VideoClip")) return go_component(L, n, COMP_MOVIE, MT_MOVIE, "MovieClip");
    else if (!strcmp(k, "PlayerMovement")) {
        if (n != NODE_PLAYER) return luaL_error(L, "GameObject is not a PlayerMovement");
        push_ud(L, MT_PM, 0, 0);
    } else return luaL_error(L, "GameObject has no member '%s'", k);
    return 1;
}

static int go_newindex(lua_State *L)
{
    int n = ((Ud *)luaL_checkudata(L, 1, MT_GO))->v;
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "WorldPosition")) node_set_pos(n, check_vec3(L, 3));
    else if (!strcmp(k, "LocalPosition")) {
        if (n >= 0) scene_set_local_pos(n, check_vec3(L, 3));
        else node_set_pos(n, check_vec3(L, 3));
    } else if (!strcmp(k, "WorldRotation")) node_set_rot(n, qeuler(check_vec3(L, 3)));
    else if (!strcmp(k, "LocalRotation")) {
        if (n >= 0) scene_set_local_rot(n, qeuler(check_vec3(L, 3)));
        else node_set_rot(n, qeuler(check_vec3(L, 3)));
    } else if (!strcmp(k, "Scale")) {
        if (n >= 0) scene_set_scale(n, check_vec3(L, 3));
    } else return luaL_error(L, "can't set GameObject.%s", k);
    return 0;
}

static int go_eq(lua_State *L)
{
    Ud *a = (Ud *)luaL_checkudata(L, 1, MT_GO), *b = (Ud *)luaL_checkudata(L, 2, MT_GO);
    lua_pushboolean(L, a->v == b->v);
    return 1;
}

/* ------------------------------------------------------------------ dreams and the DreamSystem */

static void push_dream(lua_State *L, int idx)
{
    if (idx < 0) lua_pushnil(L);
    else push_ud(L, MT_DREAM, idx, 0);
}

static int dream_index(lua_State *L)
{
    int i = ((Ud *)luaL_checkudata(L, 1, MT_DREAM))->v;
    const char *k = luaL_checkstring(L, 2);
    const JournalDream *jd = &DS.journal.d[i];
    if (!strcmp(k, "Name")) lua_pushstring(L, jd->name);
    else if (!strcmp(k, "Author")) lua_pushstring(L, "OutSide Directors Company");
    else if (!strcmp(k, "GreyMan")) lua_pushboolean(L, (jd->flags & JD_GREY_MAN) != 0);
    else if (!strcmp(k, "Linkable")) lua_pushboolean(L, (jd->flags & JD_LINKABLE) != 0);
    else return luaL_error(L, "Dream has no member '%s'", k);
    return 1;
}

static int dream_eq(lua_State *L)
{
    Ud *a = (Ud *)luaL_checkudata(L, 1, MT_DREAM), *b = (Ud *)luaL_checkudata(L, 2, MT_DREAM);
    lua_pushboolean(L, a->v == b->v);
    return 1;
}

static int dream_tostring(lua_State *L)
{
    int i = ((Ud *)luaL_checkudata(L, 1, MT_DREAM))->v;
    lua_pushfstring(L, "%s (OutSide Directors Company)", DS.journal.d[i].name);
    return 1;
}

static int check_dream_or_nil(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) return -1;
    return ((Ud *)luaL_checkudata(L, idx, MT_DREAM))->v;
}

static int ds_set_dream(lua_State *L) { g_next.dream = check_dream_or_nil(L, 1); return 0; }
static int ds_set_color(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) g_next.has_color = 0;
    else {
        check_color(L, 1, g_next.color);
        g_next.has_color = 1;
    }
    return 0;
}
static uint32_t fnv1a(const char *s)
{
    if (!s || !*s) return 0;
    uint32_t h = 0x811C9DC5u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 0x01000193u;
    }
    return h ? h : 1;
}
static int ds_set_spawn(lua_State *L) { g_next.spawn = lua_isnoneornil(L, 1) ? 0 : fnv1a(luaL_checkstring(L, 1)); return 0; }
static int ds_set_lock(lua_State *L) { g_next.lock = lua_toboolean(L, 1); return 0; }
static int ds_set_sounds(lua_State *L) { g_next.sound = lua_toboolean(L, 1); return 0; }
static void next_reset(void)
{
    g_next.has_color = 0;
    g_next.spawn = 0;
    g_next.dream = -1;
    g_next.lock = 0;
    g_next.sound = 1;
}
static int ds_go(lua_State *L)
{
    int dream = g_next.dream, lock = g_next.lock, sound = g_next.sound, has = g_next.has_color;
    uint32_t spawn = g_next.spawn;
    float col[3];
    memcpy(col, g_next.color, sizeof(col));
    next_reset();
    ds_transition(dream, has ? col : NULL, sound, lock, spawn);
    return 0;
}
static int ds_end(lua_State *L)
{
    g_next.has_color = 0;
    g_next.spawn = 0;
    g_next.dream = -1;
    ds_end_dream(0);
    return 0;
}
static int ds_stretch_l(lua_State *L)
{
    ds_stretch((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int ds_on_timeout(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_ntimeout < 16) {
        lua_pushvalue(L, 1);
        g_timeout[g_ntimeout].L = L;
        g_timeout[g_ntimeout].fn = luaL_ref(L, LUA_REGISTRYINDEX);
        g_ntimeout++;
    }
    return 0;
}
static int ds_set_texset(lua_State *L) { DS.texset = (int)luaL_checkinteger(L, 1) & 3; return 0; }
static int ds_contribute(lua_State *L)
{
    int node = -1; /* (the source entity: an InteractiveObject or a GameObject) */
    Ud *u = (Ud *)luaL_testudata(L, 3, MT_IO);
    if (u) node = io_node(u->v);
    else if ((u = (Ud *)luaL_testudata(L, 3, MT_GO))) node = u->v;
    ds_log_entity((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2), node);
    return 0;
}

static int ds_index(lua_State *L)
{
    const char *k = luaL_checkstring(L, 2);
    if (!strcmp(k, "DayNumber")) lua_pushinteger(L, DS.day);
    else if (!strcmp(k, "YearNumber")) lua_pushinteger(L, DS.year);
    else if (!strcmp(k, "CurrentDream")) push_dream(L, DS.cur);
    else if (!strcmp(k, "CurrentDreamIndex")) lua_pushinteger(L, DS.cur);
    else if (!strcmp(k, "SetNextTransitionDream")) lua_pushcfunction(L, ds_set_dream);
    else if (!strcmp(k, "SetNextTransitionColor")) lua_pushcfunction(L, ds_set_color);
    else if (!strcmp(k, "SetNextTransitionSpawnID")) lua_pushcfunction(L, ds_set_spawn);
    else if (!strcmp(k, "SetNextTransitionLockControls")) lua_pushcfunction(L, ds_set_lock);
    else if (!strcmp(k, "SetTransitionSounds")) lua_pushcfunction(L, ds_set_sounds);
    else if (!strcmp(k, "TransitionToDream")) lua_pushcfunction(L, ds_go);
    else if (!strcmp(k, "EndDream")) lua_pushcfunction(L, ds_end);
    else if (!strcmp(k, "StretchDream")) lua_pushcfunction(L, ds_stretch_l);
    else if (!strcmp(k, "OnDreamTimeout")) lua_pushcfunction(L, ds_on_timeout);
    else if (!strcmp(k, "SetTextureSet")) lua_pushcfunction(L, ds_set_texset);
    else if (!strcmp(k, "LogGraphContributionFromEntity")) lua_pushcfunction(L, ds_contribute);
    else return luaL_error(L, "DreamSystem has no member '%s'", k);
    return 1;
}

/* ------------------------------------------------------------------ global API */

static int l_get_entity(lua_State *L)
{
    const char *id = luaL_checkstring(L, 1);
    if (!strcmp(id, "__player")) { push_go(L, NODE_PLAYER); return 1; }
    if (!strcmp(id, "__camera")) { push_go(L, NODE_CAMERA); return 1; }
    int n = scene_find(id);
    if (n < 0) {
        Script *sc = script_of_state(L);
        lsd_log("No entity with ID '%s' (%s)", id, sc ? sc->name : "?");
        lua_pushnil(L);
        return 1;
    }
    push_go(L, n);
    return 1;
}
static int l_day_even(lua_State *L) { lua_pushboolean(L, DS.day % 2 == 0); return 1; }
static int l_week_day(lua_State *L) { lua_pushboolean(L, (DS.day - 1) % 7 == (int)luaL_checkinteger(L, 1) - 1); return 1; }
static int l_day_linear(lua_State *L)
{
    int slope = (int)luaL_checkinteger(L, 1), c = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, slope != 0 && (DS.day - c) % slope == 0);
    return 1;
}
static int l_can_control(lua_State *L) { DS.can_control = lua_toboolean(L, 1); return 0; }
static int l_dream_by_name(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    for (int i = 0; i < DS.journal.n; i++)
        if (!strcmp(DS.journal.d[i].name, name)) { push_dream(L, i); return 1; }
    lsd_log("unable to find dream with name '%s'", name);
    lua_pushnil(L);
    return 1;
}
static int l_print(lua_State *L)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    Script *sc = script_of_state(L); /* after __tostring calls */
    lsd_log("lua %s: %s", sc ? sc->name : "?", lua_tostring(L, -1));
    return 0;
}
/* script sources, read from the pack once per dream (many entities share a script) */
typedef struct {
    char path[96];
    uint8_t *src;
    size_t size;
    /* the first state's compiled chunk (lua_dump): later states load that instead of parsing again */
    uint8_t *bc;
    size_t bc_size, bc_cap;
    char bc_name[96];
} SrcCache;
static SrcCache *g_srccache;
static int g_nsrccache, g_cap_srccache;
static struct {
    double read, state, compile, run, start;
    int scripts, reads;
} g_init_t;

static const uint8_t *cached_source(const char *path, size_t *size)
{
    for (int i = 0; i < g_nsrccache; i++)
        if (!strcmp(g_srccache[i].path, path)) {
            *size = g_srccache[i].size;
            return g_srccache[i].src;
        }
    double t0 = plat_seconds();
    size_t n = 0;
    uint8_t *src = vfs_read(path, &n);
    g_init_t.read += plat_seconds() - t0;
    g_init_t.reads++;
    if (!src)
        return NULL;
    if (g_nsrccache == g_cap_srccache) {
        int nc = g_cap_srccache ? g_cap_srccache * 2 : 32;
        SrcCache *c = (SrcCache *)realloc(g_srccache, (size_t)nc * sizeof(SrcCache));
        if (!c) {
            free(src);
            return NULL;
        }
        g_srccache = c;
        g_cap_srccache = nc;
    }
    SrcCache *e = &g_srccache[g_nsrccache++];
    memset(e, 0, sizeof(*e));
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->src = src;
    e->size = n;
    *size = n;
    return src;
}

static int bc_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    SrcCache *e = (SrcCache *)ud;
    if (e->bc_size + sz > e->bc_cap) {
        size_t nc = e->bc_cap ? e->bc_cap * 2 : 4096;
        while (nc < e->bc_size + sz) nc *= 2;
        uint8_t *nb = (uint8_t *)realloc(e->bc, nc);
        if (!nb) return 1;
        e->bc = nb;
        e->bc_cap = nc;
    }
    memcpy(e->bc + e->bc_size, p, sz);
    e->bc_size += sz;
    return 0;
}

/* luaL_loadbuffer of a cached script: compiled once, then loaded from its dumped chunk */
static int load_cached(lua_State *L, const char *path, const uint8_t *src, size_t size, const char *chunkname)
{
    SrcCache *e = NULL;
    for (int i = 0; i < g_nsrccache; i++)
        if (!strcmp(g_srccache[i].path, path)) e = &g_srccache[i];
    if (e && e->bc && !strcmp(e->bc_name, chunkname))
        return luaL_loadbufferx(L, (const char *)e->bc, e->bc_size, chunkname, "b");
    int r = luaL_loadbuffer(L, (const char *)src, size, chunkname);
    if (r == LUA_OK && e && !e->bc) {
        snprintf(e->bc_name, sizeof(e->bc_name), "%s", chunkname);
        if (lua_dump(L, bc_writer, e) != 0 || strcmp(e->bc_name, chunkname)) {
            free(e->bc);
            e->bc = NULL;
            e->bc_size = e->bc_cap = 0;
        }
    }
    return r;
}

static void free_source_cache(void)
{
    for (int i = 0; i < g_nsrccache; i++) {
        free(g_srccache[i].src);
        free(g_srccache[i].bc);
    }
    free(g_srccache);
    g_srccache = NULL;
    g_nsrccache = g_cap_srccache = 0;
}

void luaapi_log_init_times(void)
{
    lsd_log("scripts: %d states; reading %.0f ms (%d files), new states %.0f ms, compiling %.0f ms, running %.0f ms, "
            "start() %.0f ms",
            g_init_t.scripts, g_init_t.read * 1000.0, g_init_t.reads, g_init_t.state * 1000.0,
            g_init_t.compile * 1000.0, g_init_t.run * 1000.0, g_init_t.start * 1000.0);
}

static int l_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "lsd.loaded");
    lua_getfield(L, -1, name);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    char path[128];
    snprintf(path, sizeof(path), "scripts/include/%s.lua", name);
    size_t size = 0;
    const uint8_t *src = cached_source(path, &size);
    if (!src) return luaL_error(L, "unable to load Lua script %s", name);
    int r = load_cached(L, path, src, size, name);
    if (r != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

static int u_vector3(lua_State *L)
{
    push_vec3(L, v3((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3)));
    return 1;
}
static int u_rgb(lua_State *L)
{
    push_color(L, (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), 1);
    return 1;
}
static int u_rgba(lua_State *L)
{
    push_color(L, (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
               (float)luaL_checknumber(L, 4));
    return 1;
}
static int u_dt(lua_State *L) { lua_pushnumber(L, g_dt); return 1; }
static int u_time(lua_State *L) { lua_pushnumber(L, plat_seconds()); return 1; }

static int d_log(lua_State *L) { lsd_log("lua: %s", luaL_tolstring(L, 1, NULL)); return 0; }

/* RandUtil (UnityEngine.Random) */
static int r_int_max(lua_State *L) { lua_pushinteger(L, rng_int((int)luaL_checkinteger(L, 1))); return 1; }
static int r_int_min_max(lua_State *L)
{
    int lo = (int)luaL_checkinteger(L, 1), hi = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, hi > lo ? lo + rng_int(hi - lo) : lo);
    return 1;
}
static int r_float(lua_State *L) { lua_pushnumber(L, rng_float(0, 1)); return 1; }
static int r_float_max(lua_State *L) { lua_pushnumber(L, rng_float(0, (float)luaL_checknumber(L, 1))); return 1; }
static int r_float_min_max(lua_State *L)
{
    lua_pushnumber(L, rng_float((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2)));
    return 1;
}
static int r_one_in(lua_State *L)
{
    float c = (float)luaL_checknumber(L, 1);
    lua_pushboolean(L, rng_float(0, 1) < 1.0f / c);
    return 1;
}
static int r_color(lua_State *L) { push_color(L, rng_float(0, 1), rng_float(0, 1), rng_float(0, 1), 1); return 1; }

static void set_fn(lua_State *L, const char *name, lua_CFunction f)
{
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
}

static void new_mt(lua_State *L, const char *name, lua_CFunction index, lua_CFunction newindex, lua_CFunction eq,
                   lua_CFunction gc)
{
    luaL_newmetatable(L, name);
    if (index) set_fn(L, "__index", index);
    if (newindex) set_fn(L, "__newindex", newindex);
    if (eq) set_fn(L, "__eq", eq);
    if (gc) set_fn(L, "__gc", gc);
    lua_pop(L, 1);
}

static lua_State *new_state(void)
{
    lua_State *L = luaL_newstate();
    if (!L) return NULL;
    luaL_requiref(L, "_G", luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_BITLIBNAME, luaopen_bit32, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 6);
    /* soft sandbox: no file access */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, l_print); lua_setglobal(L, "print");
    lua_pushcfunction(L, l_require); lua_setglobal(L, "require");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "lsd.loaded");

    luaL_newmetatable(L, "lsd.vec3");
    set_fn(L, "__add", vec_add);
    set_fn(L, "__sub", vec_sub);
    set_fn(L, "__mul", vec_mul);
    set_fn(L, "__div", vec_div);
    set_fn(L, "__tostring", vec_tostring);
    set_fn(L, "__index", vec_index);
    lua_pop(L, 1);
    new_mt(L, MT_GO, go_index, go_newindex, go_eq, NULL);
    new_mt(L, MT_IO, io_index, io_newindex, NULL, NULL);
    new_mt(L, MT_RUNNER, runner_index, NULL, NULL, NULL);
    new_mt(L, MT_ACTION, action_index, NULL, NULL, NULL);
    new_mt(L, MT_PRED, NULL, NULL, NULL, pred_gc);
    new_mt(L, MT_AUDIO, audio_index, audio_newindex, NULL, NULL);
    new_mt(L, MT_ANIM, anim_index, NULL, NULL, NULL);
    new_mt(L, MT_MOVIE, movie_index, NULL, NULL, NULL);
    new_mt(L, MT_PM, pm_index, NULL, NULL, NULL);
    new_mt(L, MT_TRIGGER, NULL, NULL, NULL, NULL);
    new_mt(L, MT_DS, ds_index, NULL, NULL, NULL);
    luaL_newmetatable(L, MT_DREAM);
    set_fn(L, "__index", dream_index);
    set_fn(L, "__eq", dream_eq);
    set_fn(L, "__tostring", dream_tostring);
    lua_pop(L, 1);

    static const luaL_Reg globals[] = {
        {"GetEntity", l_get_entity}, {"IsDayEven", l_day_even}, {"IsWeekDay", l_week_day},
        {"IsDayLinear", l_day_linear}, {"SetCanControlPlayer", l_can_control}, {"GetDreamByName", l_dream_by_name},
        {NULL, NULL}};
    lua_pushglobaltable(L);
    luaL_setfuncs(L, globals, 0);
    lua_pop(L, 1);

    lua_newtable(L);
    set_fn(L, "Vector3", u_vector3);
    set_fn(L, "ColorRGB", u_rgb);
    set_fn(L, "ColorRGBA", u_rgba);
    set_fn(L, "DeltaTime", u_dt);
    set_fn(L, "CurrentTime", u_time);
    lua_setglobal(L, "Unity");
    lua_newtable(L);
    set_fn(L, "Log", d_log);
    set_fn(L, "LogWarning", d_log);
    set_fn(L, "LogError", d_log);
    lua_setglobal(L, "Debug");
    lua_newtable(L);
    set_fn(L, "IntMax", r_int_max);
    set_fn(L, "IntMinMax", r_int_min_max);
    set_fn(L, "Float", r_float);
    set_fn(L, "FloatMax", r_float_max);
    set_fn(L, "FloatMinMax", r_float_min_max);
    set_fn(L, "OneIn", r_one_in);
    set_fn(L, "Color", r_color);
    lua_setglobal(L, "Random");
    lua_newtable(L);
    set_fn(L, "Default", cond_default);
    set_fn(L, "WaitForSeconds", cond_seconds);
    set_fn(L, "WaitForLinearMove", cond_linear);
    set_fn(L, "PointingAt", cond_pointing);
    set_fn(L, "Custom", cond_custom);
    lua_setglobal(L, "Condition");
    lua_newtable(L);
    static const struct { const char *n; float r, g, b, a; } cols[] = {
        {"Red", 1, 0, 0, 1}, {"Black", 0, 0, 0, 1}, {"Blue", 0, 0, 1, 1}, {"Clear", 0, 0, 0, 0},
        {"Cyan", 0, 1, 1, 1}, {"Gray", 0.5f, 0.5f, 0.5f, 1}, {"Grey", 0.5f, 0.5f, 0.5f, 1},
        {"Magenta", 1, 0, 1, 1}, {"White", 1, 1, 1, 1}, {"Yellow", 1, 0.921568632f, 0.0156862754f, 1},
        {"Green", 0, 1, 0, 1}};
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        push_color(L, cols[i].r, cols[i].g, cols[i].b, cols[i].a);
        lua_setfield(L, -2, cols[i].n);
    }
    lua_setglobal(L, "Colors");
    push_ud(L, MT_DS, 0, 0);
    lua_setglobal(L, "DreamSystem");
    return L;
}

/* ------------------------------------------------------------------ script hosts */

static int ref_global(lua_State *L, const char *name)
{
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static int script_create(int comp, const char *script_name)
{
    if (g_nscripts == g_cap_scripts) {
        g_cap_scripts = g_cap_scripts ? g_cap_scripts * 2 : 32;
        g_scripts = (Script *)realloc(g_scripts, g_cap_scripts * sizeof(Script));
    }
    int si = g_nscripts;
    Script *sc = &g_scripts[si];
    memset(sc, 0, sizeof(*sc));
    sc->comp = comp;
    sc->f_start = sc->f_update = sc->f_interact = sc->f_interval = sc->f_trig = sc->f_trig_exit = LUA_NOREF;
    snprintf(sc->name, sizeof(sc->name), "%s:%s", script_name, g_scene.name[D->comp[comp].node]);
    char path[128];
    snprintf(path, sizeof(path), "scripts/%s.lua", script_name);
    size_t size = 0;
    const uint8_t *src = cached_source(path, &size);
    if (!src) return -1;
    double t0 = plat_seconds();
    lua_State *L = new_state();
    double t1 = plat_seconds();
    g_init_t.state += t1 - t0;
    g_init_t.scripts++;
    if (!L)
        return -1;
    sc->L = L;
    g_nscripts++;
    /* this */
    if (D->comp[comp].type == COMP_INTERACTIVE) push_ud(L, MT_IO, comp, 0);
    else push_ud(L, MT_TRIGGER, comp, 0);
    lua_setglobal(L, "this");
    if (load_cached(L, path, src, size, script_name) != LUA_OK) {
        lsd_log("lua: %s does not compile: %s", script_name, lua_tostring(L, -1));
        lua_pop(L, 1);
        g_errors++;
        return si;
    }
    double t2 = plat_seconds();
    g_init_t.compile += t2 - t1;
    call(L, 0, 0, "load", sc);
    g_init_t.run += plat_seconds() - t2;
    sc = &g_scripts[si];
    sc->f_start = ref_global(L, "start");
    sc->f_update = ref_global(L, "update");
    sc->f_interact = ref_global(L, "interact");
    sc->f_interval = ref_global(L, "intervalUpdate");
    return si;
}

static void script_call(int si, int which, const char *what)
{
    if (si < 0 || si >= g_nscripts) return;
    Script *sc = &g_scripts[si];
    if (!sc->L || which == LUA_NOREF) return;
    lua_rawgeti(sc->L, LUA_REGISTRYINDEX, which);
    call(sc->L, 0, 0, what, sc);
}

void luaapi_entity_init(int comp)
{
    if (!g_comp_script) return;
    const SceneCompDef *cd = &D->comp[comp];
    const char *name = dream_str(D, cd->w.u[0]);
    if (cd->type == COMP_INTERACTIVE) {
        Interactive *io = &g_inter[comp];
        io->interval = 0.25f;
        io->distance = cd->w.f[2];
        if (!*name) return;
        int si = script_create(comp, name);
        g_comp_script[comp] = si;
        io->script = si;
        if (si < 0) return;
        double t0 = plat_seconds();
        script_call(si, g_scripts[si].f_start, "start");
        script_call(si, g_scripts[si].f_interval, "intervalUpdate");
        g_init_t.start += plat_seconds() - t0;
    } else if (cd->type == COMP_TRIGGER_LUA) {
        if (!*name) return;
        int si = script_create(comp, name);
        g_comp_script[comp] = si;
        if (si < 0) return;
        Script *sc = &g_scripts[si];
        lua_getglobal(sc->L, dream_str(D, cd->w.u[1]));
        sc->f_trig = lua_isfunction(sc->L, -1) ? luaL_ref(sc->L, LUA_REGISTRYINDEX) : (lua_pop(sc->L, 1), LUA_NOREF);
        lua_getglobal(sc->L, dream_str(D, cd->w.u[2]));
        sc->f_trig_exit = lua_isfunction(sc->L, -1) ? luaL_ref(sc->L, LUA_REGISTRYINDEX) : (lua_pop(sc->L, 1), LUA_NOREF);
        double t0 = plat_seconds();
        script_call(si, sc->f_start, "start");
        g_init_t.start += plat_seconds() - t0;
    }
}

/* test: the cost of a fresh scripting state, small allocations and plain arithmetic */
void luaapi_perftest(void)
{
    double t0 = plat_seconds();
    for (int i = 0; i < 40; i++) {
        lua_State *L = new_state();
        if (!L) break;
        (void)luaL_dostring(L, "local t = {} for i = 1, 300 do t[i] = {i, tostring(i)} end");
        lua_close(L);
    }
    double t1 = plat_seconds();
    static void *ptrs[4096];
    for (int r = 0; r < 25; r++) {
        for (int i = 0; i < 4096; i++) ptrs[i] = malloc(16 + (size_t)((i * 37) % 240));
        for (int i = 0; i < 4096; i++) free(ptrs[i]);
    }
    double t2 = plat_seconds();
    volatile float acc = 0;
    volatile uint32_t h = 2166136261u;
    for (int i = 0; i < 2000000; i++) {
        acc += (float)(i & 255) * 0.5f;
        h = (h ^ (uint32_t)i) * 16777619u;
    }
    double t3 = plat_seconds();
    lsd_log("perftest: 40 Lua states %.1f ms, 102400 malloc/free %.1f ms, 2M arithmetic steps %.1f ms",
            (t1 - t0) * 1000.0, (t2 - t1) * 1000.0, (t3 - t2) * 1000.0);
}

void luaapi_interactive_enable(int comp)
{
    if (!g_comp_script) return;
    int si = g_comp_script[comp];
    if (si < 0) return;
    if (D->comp[comp].w.u[3]) /* CallStartOnEnable */
        script_call(si, g_scripts[si].f_start, "start");
}

void luaapi_interactive_update(int comp, float dt)
{
    if (!g_comp_script || !g_scene.comp_init[comp]) return;
    Interactive *io = &g_inter[comp];
    int si = io->script;
    if (si >= 0) script_call(si, g_scripts[si].f_update, "update");
    if (!g_scene.loaded) return;
    io->t += dt;
    if (io->t > io->interval) {
        io->t = 0;
        const SceneCompDef *cd = &D->comp[comp];
        int kind = (int)cd->w.u[1];
        if (!io->interacted && kind != 2) {
            float dist = v3len(v3sub(scene_world_pos(cd->node), P.feet));
            if (dist <= io->distance) {
                if (si >= 0) script_call(si, g_scripts[si].f_interact, "interact");
                io->interacted = 1;
            }
        }
        if (!g_scene.loaded) return;
        if (si >= 0) script_call(si, g_scripts[si].f_interval, "intervalUpdate");
    }
}

void luaapi_trigger_lua(int comp, int enter)
{
    if (!g_comp_script) return;
    int si = g_comp_script[comp];
    if (si < 0) return;
    script_call(si, enter ? g_scripts[si].f_trig : g_scripts[si].f_trig_exit, enter ? "onTrigger" : "onTriggerExit");
}

/* DreamAudio.cs: Loop/OneShot sounds play from OnEnable (or Play()); the others run the
 * audioPlayer coroutine: once the player has come within range (checked every half second, if
 * WaitUntilAudibleBeforePlaying), wait DelayBeforePlayingSeconds, play, stop after
 * PlayClipForSeconds (if set), wait DelayBetweenPlaysSeconds (or a frame), and again. */
#define DA_SPATIAL(cd) ((cd)->w.u[1])
#define DA_MIN(cd) ((cd)->w.f[2])
#define DA_MAX(cd) ((cd)->w.f[3])
#define DA_PITCH(cd) ((cd)->w.f[4])
#define DA_VOLUME(cd) ((cd)->w.f[5])
#define DA_BETWEEN(cd) ((cd)->w.f[6])
#define DA_PLAY_FOR(cd) ((cd)->w.f[7])
#define DA_BEFORE(cd) ((cd)->w.f[8])
#define DA_WAIT_AUDIBLE(cd) ((cd)->w.u[9])
#define DA_LOOP(cd) ((cd)->w.u[10])
#define DA_ONESHOT(cd) ((cd)->w.u[11])
#define DA_CONTROLLED(cd) ((cd)->w.u[12])

static int da_ok(int c) { return g_audio && c >= 0 && c < g_naudio && D->comp[c].type == COMP_AUDIO; }

static void da_place(int c)
{
    const SceneCompDef *cd = &D->comp[c];
    src_place(g_audio[c].src, (int)DA_SPATIAL(cd), DA_MIN(cd), DA_MAX(cd), scene_world_pos(cd->node));
}

static void da_coroutine(int c, float dt)
{
    AudioState *a = &g_audio[c];
    const SceneCompDef *cd = &D->comp[c];
    int st = a->co_state;
    if (st == CO_DELAY_BEFORE || st == CO_PLAYING_FOR || st == CO_DELAY_BETWEEN) {
        a->co_t += dt; /* WaitForSeconds */
        float need = st == CO_DELAY_BEFORE ? DA_BEFORE(cd) : st == CO_PLAYING_FOR ? DA_PLAY_FOR(cd) : DA_BETWEEN(cd);
        if (a->co_t < need)
            return;
    }
    for (int guard = 0; guard < 8; guard++) {
        switch (st) {
        case CO_WAIT_AUDIBLE:
            if (!a->heard) {
                a->co_state = st;
                return;
            }
            st = CO_TOP;
            break;
        case CO_TOP:
            if (DA_BEFORE(cd) > 0) {
                a->co_state = CO_DELAY_BEFORE;
                a->co_t = 0;
                return;
            }
            st = CO_DELAY_BEFORE;
            break;
        case CO_DELAY_BEFORE: /* (waited) _audioSource.pitch = Pitch; .volume = Volume; .Play() */
            da_place(c);
            src_play(a->src, a->sample, a->pitch, a->volume, (int)DA_LOOP(cd));
            if (DA_PLAY_FOR(cd) > 0) {
                a->co_state = CO_PLAYING_FOR;
                a->co_t = 0;
                return;
            }
            st = CO_AFTER_PLAY;
            break;
        case CO_PLAYING_FOR:
            src_stop(a->src);
            st = CO_AFTER_PLAY;
            break;
        case CO_AFTER_PLAY:
            a->co_t = 0;
            a->co_state = DA_BETWEEN(cd) <= 0 ? CO_YIELD : CO_DELAY_BETWEEN;
            return;
        default: /* CO_YIELD / CO_DELAY_BETWEEN resumed */
            st = CO_TOP;
            break;
        }
    }
    a->co_state = st;
}

/* DreamAudio.StartPlaying */
static void da_start(int c)
{
    if (!da_ok(c))
        return;
    AudioState *a = &g_audio[c];
    const SceneCompDef *cd = &D->comp[c];
    if (a->co)
        return;
    if (DA_LOOP(cd) || DA_ONESHOT(cd)) {
        /* the AudioSource keeps the pitch and volume it was given in the editor */
        da_place(c);
        src_play(a->src, a->sample, DA_PITCH(cd), DA_VOLUME(cd), (int)DA_LOOP(cd));
        return;
    }
    a->co = 1;
    a->co_frame = g_frame_no;
    if (!DA_WAIT_AUDIBLE(cd) || DA_CONTROLLED(cd))
        a->heard = 1;
    a->co_state = CO_WAIT_AUDIBLE;
    da_coroutine(c, 0);
}

/* DreamAudio.StopPlaying */
static void da_stop(int c)
{
    if (!da_ok(c))
        return;
    src_stop(g_audio[c].src);
    g_audio[c].co = 0;
    g_audio[c].co_state = CO_NONE;
}

/* OnEnable / OnDisable */
void luaapi_audio_enable(int comp, int on)
{
    if (!da_ok(comp))
        return;
    if (!on)
        da_stop(comp);
    else if (!DA_CONTROLLED(&D->comp[comp]))
        da_start(comp);
}

/* DreamAudio.Update, then the coroutine */
void luaapi_audio_update(int comp, float dt)
{
    if (!da_ok(comp))
        return;
    AudioState *a = &g_audio[comp];
    const SceneCompDef *cd = &D->comp[comp];
    if (g_scene.comp_init[comp] && !a->heard && !DA_CONTROLLED(cd)) { /* (_player is set by Init) */
        if (a->think < 0.5f) {
            a->think += dt;
        } else {
            a->think = 0;
            vec3 d = v3sub(P.feet, scene_world_pos(cd->node));
            if (v3dot(d, d) < DA_MAX(cd) * DA_MAX(cd))
                a->heard = 1;
        }
    }
    if (a->co && a->co_frame != g_frame_no)
        da_coroutine(comp, dt);
    da_place(comp);
}

void luaapi_dream_timeout(void)
{
    for (int i = 0; i < g_ntimeout; i++) call_ref(g_timeout[i].L, g_timeout[i].fn, "OnDreamTimeout");
}

void luaapi_tick(float dt)
{
    g_frame_no++;
    g_dt = dt;
    g_clock += dt;
}

/* ------------------------------------------------------------------ lifetime */

int luaapi_dream_begin(void)
{
    luaapi_dream_end();
    D = &g_dream;
    int nc = D->ncomp;
    g_comp_script = (int *)malloc((nc + 1) * sizeof(int));
    g_inter = (Interactive *)calloc(nc + 1, sizeof(Interactive));
    g_runners = (Runner *)calloc(nc + 1, sizeof(Runner));
    g_audio = (AudioState *)calloc(nc + 1, sizeof(AudioState));
    if (!g_comp_script || !g_inter || !g_runners || !g_audio) {
        luaapi_dream_end();
        return 0;
    }
    for (int i = 0; i <= nc; i++) {
        g_comp_script[i] = -1;
        g_inter[i].script = -1;
        g_runners[i].root = g_runners[i].cur = -1;
    }
    g_naudio = nc;
    int nsrc = 0, nnone = 0;
    for (int i = 0; i < nc; i++) {
        g_audio[i].src = -1;
        g_audio[i].sample = -1;
        if (D->comp[i].type != COMP_AUDIO)
            continue;
        AudioState *a = &g_audio[i];
        a->pitch = DA_PITCH(&D->comp[i]);
        a->volume = DA_VOLUME(&D->comp[i]);
        const char *clip = dream_str(D, D->comp[i].w.u[0]);
        const char *k = strstr(clip, "SE/Sample");
        if (k) a->sample = atoi(k + 9);
        a->src = src_new();
        if (a->src >= 0) nsrc++;
        else nnone++;
    }
    if (nsrc || nnone)
        lsd_log("%d dream sounds%s", nsrc, nnone ? " (some had no free source)" : "");
    memset(&g_player_ext, 0, sizeof(g_player_ext));
    memset(&g_init_t, 0, sizeof(g_init_t));
    next_reset();
    return 1;
}

void luaapi_dream_end(void)
{
    g_closing = 1;
    for (int i = 0; i < g_nscripts; i++)
        if (g_scripts[i].L) lua_close(g_scripts[i].L);
    g_closing = 0;
    free(g_scripts);
    g_scripts = NULL;
    g_nscripts = g_cap_scripts = 0;
    free(g_comp_script);
    free(g_inter);
    free(g_runners);
    for (int i = 0; g_audio && i < g_naudio; i++)
        src_free(g_audio[i].src);
    g_naudio = 0;
    free(g_audio);
    g_comp_script = NULL;
    g_inter = NULL;
    g_runners = NULL;
    g_audio = NULL;
    free(g_acts);
    g_acts = NULL;
    g_nacts = g_cap_acts = 0;
    g_free_act = -1;
    free(g_preds);
    g_preds = NULL;
    g_npreds = g_cap_preds = 0;
    g_free_pred = -1;
    g_ntimeout = 0;
    memset(&g_player_ext, 0, sizeof(g_player_ext));
    free_source_cache();
}
