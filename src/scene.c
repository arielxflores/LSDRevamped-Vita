/* Runtime entity scene: Unity-style GameObject hierarchy for the dream's entities. */
#include "scene.h"
#include "luaapi.h"
#include "world.h"

Scene g_scene;
static const Dream *D;

static EntityDraw *g_draws;
static int g_ndraws, g_cap_draws;

/* ------------------------------------------------------------------ transforms */

static void touch(void) { g_scene.ver++; }

const aff *scene_world(int n)
{
    Scene *s = &g_scene;
    if (s->world_ver[n] == s->ver)
        return &s->world[n];
    aff l = aff_trs(s->pos[n], s->rot[n], s->scale[n]);
    if (s->parent[n] >= 0) {
        const aff *p = scene_world(s->parent[n]);
        s->world[n] = aff_mul(p, &l);
    } else {
        s->world[n] = l;
    }
    s->world_ver[n] = s->ver;
    return &s->world[n];
}

vec3 scene_world_pos(int n)
{
    const aff *w = scene_world(n);
    return v3(w->m[3], w->m[7], w->m[11]);
}

quat scene_world_rot(int n)
{
    quat q = g_scene.rot[n];
    for (int p = g_scene.parent[n]; p >= 0; p = g_scene.parent[p])
        q = qmul(g_scene.rot[p], q);
    return q;
}

void scene_set_world_pos(int n, vec3 p)
{
    int par = g_scene.parent[n];
    if (par >= 0) {
        aff inv = aff_inverse(scene_world(par));
        p = aff_point(&inv, p);
    }
    g_scene.pos[n] = p;
    touch();
}

void scene_set_world_rot(int n, quat q)
{
    int par = g_scene.parent[n];
    if (par >= 0)
        q = qmul(qconj(scene_world_rot(par)), q);
    g_scene.rot[n] = qnorm(q);
    touch();
}

void scene_set_local_pos(int n, vec3 p)
{
    g_scene.pos[n] = p;
    touch();
}

void scene_set_local_rot(int n, quat q)
{
    g_scene.rot[n] = qnorm(q);
    touch();
}

void scene_set_scale(int n, vec3 s)
{
    g_scene.scale[n] = s;
    touch();
}

/* ------------------------------------------------------------------ lookup */

int scene_find(const char *id)
{
    for (int i = 0; i < g_scene.nent; i++)
        if (!strcmp(g_scene.ent_id[i], id))
            return g_scene.ent_node[i];
    return -1;
}

int scene_child_by_name(int n, const char *name)
{
    /* Transform.Find: a path of child names separated by '/' */
    char part[128];
    const char *p = name;
    int cur = n;
    while (*p && cur >= 0) {
        size_t k = strcspn(p, "/");
        if (k >= sizeof(part)) return -1;
        memcpy(part, p, k);
        part[k] = 0;
        int found = -1;
        for (int c = g_scene.first_child[cur]; c >= 0; c = g_scene.next_sibling[c])
            if (!strcmp(g_scene.name[c], part)) {
                found = c;
                break;
            }
        cur = found;
        p += k;
        if (*p == '/') p++;
    }
    return cur;
}

int scene_comp_on(int n, int type)
{
    if (n < 0) return -1;
    for (int i = 0; i < g_scene.comp_count[n]; i++) {
        int c = g_scene.comp_first[n] + i;
        if ((int)D->comp[c].type == type)
            return c;
    }
    return -1;
}

int scene_active(int n)
{
    if (n < 0) return 1;
    return g_scene.aih[n];
}

/* ------------------------------------------------------------------ animation */

static int resolve_path(int root, const char *path)
{
    return scene_child_by_name(root, path);
}

static void anim_capture_defaults(SceneAnimator *a)
{
    for (int i = 0; i < a->nbound; i++) {
        int n = a->bound[i];
        a->def_pos[i] = g_scene.pos[n];
        a->def_rot[i] = g_scene.rot[n];
        a->def_scale[i] = g_scene.scale[n];
    }
}

static void anim_write_defaults(SceneAnimator *a)
{
    for (int i = 0; i < a->nbound; i++) {
        int n = a->bound[i];
        g_scene.pos[n] = a->def_pos[i];
        g_scene.rot[n] = a->def_rot[i];
        g_scene.scale[n] = a->def_scale[i];
    }
    if (a->nbound) touch();
}

static SceneAnimator *anim_of(int comp)
{
    if (comp < 0 || comp >= g_scene.ncomp) return NULL;
    int i = g_scene.comp_anim[comp];
    return i >= 0 ? &g_scene.anim[i] : NULL;
}

void anim_play(int comp, int index)
{
    SceneAnimator *a = anim_of(comp);
    if (!a) return;
    if (index < 0 || index >= a->nclips) {
        lsd_log("animation %d not on %s (%d clips)", index, g_scene.name[a->node], a->nclips);
        return;
    }
    a->enabled = 1;
    if (a->state == index) /* Animator.Play does not restart a state that is playing */
        return;
    a->state = index;
    a->time = 0;
}

void anim_stop(int comp)
{
    SceneAnimator *a = anim_of(comp);
    if (a) a->enabled = 0;
}

void anim_resume(int comp)
{
    SceneAnimator *a = anim_of(comp);
    if (!a || a->enabled) return;
    /* re-enabling the Animator rebinds it: back to the idle state, current pose as defaults */
    a->enabled = 1;
    a->state = -1;
    anim_capture_defaults(a);
}

float anim_length(int comp, int index)
{
    SceneAnimator *a = anim_of(comp);
    if (!a || index < 0 || index >= a->nclips || a->model < 0) return 0;
    int ci = a->clip[index];
    if (ci < 0 || ci >= D->model[a->model].nclip) return 0;
    return D->model[a->model].clip[ci].length;
}

/* Unity AnimationCurve with zero tangents: Hermite ease between keys */
static void sample_track(const AnimTrack *t, float time, vec3 *pos, quat *rot, vec3 *scale)
{
    const float *k = t->keys;
    int n = t->nkeys;
    const float *a, *b;
    float s = 0;
    if (n == 1 || time <= k[0]) {
        a = b = k;
    } else if (time >= k[(n - 1) * 11]) {
        a = b = k + (n - 1) * 11;
    } else {
        int lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (k[mid * 11] <= time) lo = mid;
            else hi = mid;
        }
        a = k + lo * 11;
        b = k + hi * 11;
        float span = b[0] - a[0];
        s = span > 1e-9f ? (time - a[0]) / span : 0;
        s = s * s * (3.0f - 2.0f * s);
    }
    float v[10];
    for (int i = 0; i < 10; i++)
        v[i] = a[1 + i] + (b[1 + i] - a[1 + i]) * s;
    *pos = v3(v[0], v[1], v[2]);
    *rot = qnorm(q4(v[3], v[4], v[5], v[6]));
    *scale = v3(v[7], v[8], v[9]);
}

static void anim_update(SceneAnimator *a, float dt)
{
    if (!a->enabled || !g_scene.aih[a->node] || a->model < 0)
        return;
    if (a->state < 0) {
        anim_write_defaults(a);
        return;
    }
    int ci = a->clip[a->state];
    if (ci < 0 || ci >= D->model[a->model].nclip)
        return;
    const AnimClip *clip = &D->model[a->model].clip[ci];
    float t = 0;
    if (clip->length > 0) {
        t = fmodf(a->time, clip->length);
        if (t < 0) t += clip->length;
    }
    for (int i = 0; i < clip->ntrack; i++) {
        int n = a->bind[ci][i];
        if (n < 0) continue;
        sample_track(&clip->track[i], t, &g_scene.pos[n], &g_scene.rot[n], &g_scene.scale[n]);
    }
    touch();
    a->time += dt;
}

static void anim_on_enable(SceneAnimator *a)
{
    /* the Animator restarts in its default state; AnimatedObject.OnEnable plays AutoPlay */
    a->state = -1;
    a->time = 0;
    if (a->autoplay >= 0)
        anim_play(a->comp, a->autoplay);
}

/* ------------------------------------------------------------------ activity */

static void comp_enabled_changed(int c, int on);

static void refresh_activity(int n, int parent_active)
{
    Scene *s = &g_scene;
    int now = parent_active && s->active[n];
    int was = s->aih[n];
    s->aih[n] = (uint8_t)now;
    if (now != was) {
        if (now) s->awoken[n] = 1;
        for (int i = 0; i < s->comp_count[n]; i++) {
            comp_enabled_changed(s->comp_first[n] + i, now);
            /* a callback (Chance/AppearOnDay Start, a script) changed this node's activity:
             * the nested refresh has already brought the whole subtree up to date */
            if (s->aih[n] != now) return;
        }
    }
    for (int c = s->first_child[n]; c >= 0; c = s->next_sibling[c]) {
        refresh_activity(c, now);
        if (s->aih[n] != now) return;
    }
}

void scene_set_active(int n, int on)
{
    if (n < 0) return;
    on = on ? 1 : 0;
    if (g_scene.active[n] == on) return;
    g_scene.active[n] = (uint8_t)on;
    int par = g_scene.parent[n];
    refresh_activity(n, par >= 0 ? g_scene.aih[par] : 1);
}

static void comp_start(int c);

static void comp_enabled_changed(int c, int on)
{
    const SceneCompDef *cd = &D->comp[c];
    if (!cd->enabled)
        return;
    switch (cd->type) {
    case COMP_ANIMATED: {
        SceneAnimator *a = anim_of(c);
        if (a && on) anim_on_enable(a);
        break;
    }
    case COMP_INTERACTIVE:
        if (on) luaapi_interactive_enable(c);
        break;
    case COMP_AUDIO:
        luaapi_audio_enable(c, on);
        break;
    default:
        break;
    }
    if (on && g_scene.loaded && !g_scene.comp_started[c])
        comp_start(c);
}

/* ------------------------------------------------------------------ load */

static void *zalloc(size_t n) { return calloc(1, n ? n : 1); }

int scene_load(const Dream *d)
{
    scene_free();
    D = d;
    Scene *s = &g_scene;
    int n = d->nnode;
    s->n = n;
    s->pos = (vec3 *)zalloc(n * sizeof(vec3));
    s->scale = (vec3 *)zalloc(n * sizeof(vec3));
    s->rot = (quat *)zalloc(n * sizeof(quat));
    s->active = (uint8_t *)zalloc(n);
    s->aih = (uint8_t *)zalloc(n);
    s->awoken = (uint8_t *)zalloc(n);
    s->parent = (int *)zalloc(n * sizeof(int));
    s->first_child = (int *)zalloc(n * sizeof(int));
    s->next_sibling = (int *)zalloc(n * sizeof(int));
    s->name = (char **)zalloc(n * sizeof(char *));
    s->world = (aff *)zalloc(n * sizeof(aff));
    s->world_ver = (uint32_t *)zalloc(n * sizeof(uint32_t));
    s->comp_first = (int *)zalloc(n * sizeof(int));
    s->comp_count = (int *)zalloc(n * sizeof(int));
    s->ncomp = d->ncomp;
    s->comp_init = (uint8_t *)zalloc(d->ncomp);
    s->comp_started = (uint8_t *)zalloc(d->ncomp);
    s->comp_anim = (int *)zalloc(d->ncomp * sizeof(int));
    s->ent_id = (char **)zalloc(d->ncomp * sizeof(char *));
    s->ent_node = (int *)zalloc(d->ncomp * sizeof(int));
    if (!s->pos || !s->scale || !s->rot || !s->active || !s->aih || !s->awoken || !s->parent || !s->first_child ||
        !s->next_sibling || !s->name || !s->world || !s->world_ver || !s->comp_first || !s->comp_count ||
        !s->comp_init || !s->comp_started || !s->comp_anim || !s->ent_id || !s->ent_node) {
        lsd_log("out of memory for the entity scene (%d nodes)", n);
        scene_free();
        return 0;
    }
    s->ver = 1;
    int *last_child = (int *)zalloc(n * sizeof(int));
    for (int i = 0; i < n; i++) {
        const SceneNodeDef *nd = &d->node[i];
        s->pos[i] = v3(nd->pos[0], nd->pos[1], nd->pos[2]);
        s->rot[i] = q4(nd->rot[0], nd->rot[1], nd->rot[2], nd->rot[3]);
        s->scale[i] = v3(nd->scale[0], nd->scale[1], nd->scale[2]);
        s->active[i] = (uint8_t)(nd->flags & 1);
        s->parent[i] = nd->parent;
        s->first_child[i] = s->next_sibling[i] = -1;
        last_child[i] = -1;
        s->name[i] = strdup(dream_str(d, nd->name));
        s->comp_first[i] = 0;
        int p = nd->parent;
        if (p >= 0) {
            if (last_child[p] < 0) s->first_child[p] = i;
            else s->next_sibling[last_child[p]] = i;
            last_child[p] = i;
        }
    }
    free(last_child);
    for (int i = 0; i < n; i++) {
        int p = s->parent[i];
        s->aih[i] = (uint8_t)(s->active[i] && (p < 0 || s->aih[p]));
        s->awoken[i] = s->aih[i];
    }
    /* components are stored in node order */
    for (int c = d->ncomp - 1; c >= 0; c--) {
        int nd = d->comp[c].node;
        s->comp_first[nd] = c;
        s->comp_count[nd]++;
        s->comp_anim[c] = -1;
    }
    /* animators */
    for (int c = 0; c < d->ncomp; c++)
        if (d->comp[c].type == COMP_ANIMATED) s->nanim++;
    s->anim = (SceneAnimator *)zalloc(s->nanim * sizeof(SceneAnimator));
    int ai = 0;
    for (int c = 0; c < d->ncomp && s->anim; c++) {
        const SceneCompDef *cd = &d->comp[c];
        if (cd->type != COMP_ANIMATED) continue;
        SceneAnimator *a = &s->anim[ai];
        s->comp_anim[c] = ai++;
        a->comp = c;
        a->node = cd->node;
        a->model = cd->w.i[0];
        if (a->model >= d->nmodel) a->model = -1;
        a->nclips = (int)cd->w.u[1];
        if (a->nclips > 8) a->nclips = 8;
        for (int k = 0; k < 8; k++) a->clip[k] = cd->w.i[2 + k];
        a->autoplay = cd->w.i[10];
        a->enabled = cd->w.i[11];
        a->state = -1;
        if (a->model < 0) continue;
        const Model *m = &d->model[a->model];
        a->bind = (int **)zalloc(m->nclip * sizeof(int *));
        int cap = 16;
        a->bound = (int *)zalloc(cap * sizeof(int));
        for (int ci = 0; ci < m->nclip && a->bind; ci++) {
            a->bind[ci] = (int *)zalloc(m->clip[ci].ntrack * sizeof(int));
            for (int t = 0; t < m->clip[ci].ntrack && a->bind[ci]; t++) {
                int bn = resolve_path(a->node, m->clip[ci].track[t].path);
                a->bind[ci][t] = bn;
                if (bn < 0) continue;
                int dup = 0;
                for (int k = 0; k < a->nbound; k++)
                    if (a->bound[k] == bn) dup = 1;
                if (dup) continue;
                if (a->nbound == cap) {
                    cap *= 2;
                    a->bound = (int *)realloc(a->bound, cap * sizeof(int));
                }
                a->bound[a->nbound++] = bn;
            }
        }
        a->def_pos = (vec3 *)zalloc(a->nbound * sizeof(vec3));
        a->def_scale = (vec3 *)zalloc(a->nbound * sizeof(vec3));
        a->def_rot = (quat *)zalloc(a->nbound * sizeof(quat));
        anim_capture_defaults(a);
    }
    /* BaseEntity.Awake: active entities take their ID as their name and register */
    for (int c = 0; c < d->ncomp; c++) {
        const SceneCompDef *cd = &d->comp[c];
        if (!cd->id || !s->aih[cd->node]) continue;
        const char *id = dream_str(d, cd->id);
        if (!*id) continue;
        free(s->name[cd->node]);
        const char *dd = strstr(id, "$$");
        if (dd) {
            /* BaseEntity.Awake: "$$" becomes a fresh GUID */
            char buf[256];
            static unsigned uniq;
            uniq++;
            snprintf(buf, sizeof(buf), "%.*s%08x-0000-4000-8000-%012u%s", (int)(dd - id), id, uniq, uniq, dd + 2);
            s->name[cd->node] = strdup(buf);
        } else {
            s->name[cd->node] = strdup(id);
        }
        id = s->name[cd->node];
        if (scene_find(id) >= 0) {
            lsd_log("duplicate entity ID '%s'", id);
            continue;
        }
        /* its own copy: a later component on the same node renames the node again */
        s->ent_id[s->nent] = strdup(id);
        if (!s->ent_id[s->nent]) continue;
        s->ent_node[s->nent] = cd->node;
        s->nent++;
    }
    /* OnEnable on instantiation (before Init) */
    for (int c = 0; c < d->ncomp; c++) {
        const SceneCompDef *cd = &d->comp[c];
        if (!cd->enabled || !s->aih[cd->node]) continue;
        if (cd->type == COMP_ANIMATED) {
            SceneAnimator *a = anim_of(c);
            if (a) anim_on_enable(a);
        } else if (cd->type == COMP_AUDIO) {
            luaapi_audio_enable(c, 1);
        }
    }
    s->loaded = 0;
    return 1;
}

static int is_entity_type(int t)
{
    return t == COMP_INTERACTIVE || t == COMP_TARGET || t == COMP_AUDIO || t == COMP_MOVIE || t == COMP_TRIGGER_LUA ||
           t == COMP_TRIGGER_LINK || t == COMP_SPAWN || t == COMP_PREFAB;
}

static void comp_start(int c)
{
    const SceneCompDef *cd = &D->comp[c];
    g_scene.comp_started[c] = 1;
    switch (cd->type) {
    case COMP_CHANCE: {
        int one_in = (int)cd->w.u[0];
        if (one_in < 1) one_in = 1;
        /* RandUtil.OneIn: Random.value < 1 / n */
        if (!(rng_float(0, 1) < 1.0f / (float)one_in))
            scene_set_active(cd->node, 0);
        break;
    }
    case COMP_APPEAR_DAY: {
        int day = ds_day(), flags = (int)cd->w.u[0], ok = 0;
        if (flags == 0) ok = 0;
        if (flags & 4) ok |= day % 2 == 0;  /* Even */
        if (flags & 2) ok |= day % 2 != 0;  /* Odd */
        for (int k = 0; k < 7; k++)
            if (flags & (8 << k)) ok |= ((day - 1) % 7) + 1 == k + 1;
        if (!ok) scene_set_active(cd->node, 0);
        break;
    }
    case COMP_ANIMATED: {
        SceneAnimator *a = anim_of(c);
        if (a && a->autoplay >= 0) anim_play(c, a->autoplay);
        break;
    }
    default:
        break;
    }
}

void scene_init_entities(void)
{
    Scene *s = &g_scene;
    /* EntityIndex.AllRegistered: Init() on each registered GameObject's first BaseEntity */
    for (int e = 0; e < s->nent; e++) {
        int n = s->ent_node[e];
        for (int i = 0; i < s->comp_count[n]; i++) {
            int c = s->comp_first[n] + i;
            if (!is_entity_type((int)D->comp[c].type)) continue;
            s->comp_init[c] = 1;
            if (D->comp[c].type == COMP_INTERACTIVE || D->comp[c].type == COMP_TRIGGER_LUA)
                luaapi_entity_init(c);
            break;
        }
    }
}

void scene_start_components(void)
{
    Scene *s = &g_scene;
    s->loaded = 1;
    /* Start() of every component that is active now */
    for (int c = 0; c < D->ncomp; c++)
        if (!s->comp_started[c] && D->comp[c].enabled && s->aih[D->comp[c].node])
            comp_start(c);
}

void scene_update(float dt)
{
    Scene *s = &g_scene;
    if (!s->loaded) return;
    s->time += dt;
    luaapi_player_runner_update();
    if (!s->loaded) return;
    for (int c = 0; c < D->ncomp; c++) {
        const SceneCompDef *cd = &D->comp[c];
        if (!cd->enabled || !s->aih[cd->node]) continue;
        if (cd->type == COMP_INTERACTIVE) luaapi_interactive_update(c, dt);
        else if (cd->type == COMP_RUNNER) luaapi_runner_update_comp(c);
        else if (cd->type == COMP_AUDIO) luaapi_audio_update(c, dt);
        if (!s->loaded) return; /* a script ended the dream */
    }
    for (int i = 0; i < s->nanim; i++)
        anim_update(&s->anim[i], dt);
}

/* ------------------------------------------------------------------ drawing */

EntityDraw *scene_add_draw(void)
{
    if (g_ndraws == g_cap_draws) {
        g_cap_draws = g_cap_draws ? g_cap_draws * 2 : 256;
        g_draws = (EntityDraw *)realloc(g_draws, g_cap_draws * sizeof(EntityDraw));
    }
    EntityDraw *e = &g_draws[g_ndraws++];
    e->vci = 0;
    e->cull = -1;
    return e;
}

int scene_draw_list(const EntityDraw **out)
{
    *out = g_draws;
    return g_ndraws;
}

void scene_build_draws(vec3 eye, float max_dist)
{
    g_ndraws = 0;
    Scene *s = &g_scene;
    if (!s->loaded && !s->n) return;
    for (int n = 0; n < s->n; n++) {
        const SceneNodeDef *nd = &D->node[n];
        if (nd->mesh < 0 || !s->aih[n] || (nd->flags & 6) != 6) continue;
        int mi = nd->mesh >> 16, j = nd->mesh & 0xFFFF;
        if (mi >= D->nmodel || j >= D->model[mi].nmesh) continue;
        const ModelMesh *mm = &D->model[mi].mesh[j];
        if (!mm->nbatch) continue;
        const aff *w = scene_world(n);
        vec3 c = aff_point(w, v3((mm->lo[0] + mm->hi[0]) * 0.5f, (mm->lo[1] + mm->hi[1]) * 0.5f,
                                 (mm->lo[2] + mm->hi[2]) * 0.5f));
        vec3 e = v3((mm->hi[0] - mm->lo[0]) * 0.5f, (mm->hi[1] - mm->lo[1]) * 0.5f, (mm->hi[2] - mm->lo[2]) * 0.5f);
        float sc = fmaxf(fmaxf(fabsf(s->scale[n].x), fabsf(s->scale[n].y)), fabsf(s->scale[n].z));
        float r = v3len(e) * (sc > 1 ? sc * 4 : 4); /* generous: parents may scale too */
        if (v3len(v3sub(c, eye)) - r > max_dist) continue;
        for (int b = 0; b < mm->nbatch; b++) {
            EntityDraw *e = scene_add_draw();
            e->batch = &mm->batch[b];
            e->world = *w;
        }
    }
}

void scene_free(void)
{
    Scene *s = &g_scene;
    if (s->name)
        for (int i = 0; i < s->n; i++) free(s->name[i]);
    for (int i = 0; i < s->nanim && s->anim; i++) {
        SceneAnimator *a = &s->anim[i];
        if (a->bind && a->model >= 0 && D)
            for (int ci = 0; ci < D->model[a->model].nclip; ci++) free(a->bind[ci]);
        free(a->bind);
        free(a->bound);
        free(a->def_pos);
        free(a->def_rot);
        free(a->def_scale);
    }
    free(s->anim);
    free(s->pos);
    free(s->scale);
    free(s->rot);
    free(s->active);
    free(s->aih);
    free(s->awoken);
    free(s->parent);
    free(s->first_child);
    free(s->next_sibling);
    free(s->name);
    free(s->world);
    free(s->world_ver);
    free(s->comp_first);
    free(s->comp_count);
    free(s->comp_init);
    free(s->comp_started);
    free(s->comp_anim);
    if (s->ent_id)
        for (int i = 0; i < s->nent; i++) free(s->ent_id[i]);
    free(s->ent_id);
    free(s->ent_node);
    memset(s, 0, sizeof(*s));
    g_ndraws = 0;
}
