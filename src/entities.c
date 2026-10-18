/* Loads a dream's entity scene (entities.bin v2, tools/scene.py) and its MOM models
 * (models.bin), and derives spawn points, link/Lua triggers and the graph contributor. */
#include "dream.h"
#include "xform.h"

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Cur;

static const void *take(Cur *c, size_t n)
{
    if (c->bad || (size_t)(c->end - c->p) < n) {
        c->bad = 1;
        return NULL;
    }
    const void *r = c->p;
    c->p += n;
    return r;
}

static uint32_t u32(Cur *c)
{
    uint32_t v = 0;
    const void *p = take(c, 4);
    if (p) memcpy(&v, p, 4);
    return v;
}

static float f32(Cur *c)
{
    float v = 0;
    const void *p = take(c, 4);
    if (p) memcpy(&v, p, 4);
    return v;
}

static aff node_world(const Dream *d, int n)
{
    aff w = aff_ident();
    int chain[64], depth = 0;
    while (n >= 0 && depth < 64) {
        chain[depth++] = n;
        n = d->node[n].parent;
    }
    for (int i = depth - 1; i >= 0; i--) {
        const SceneNodeDef *s = &d->node[chain[i]];
        aff l = aff_trs(v3(s->pos[0], s->pos[1], s->pos[2]), q4(s->rot[0], s->rot[1], s->rot[2], s->rot[3]),
                        v3(s->scale[0], s->scale[1], s->scale[2]));
        w = aff_mul(&w, &l);
    }
    return w;
}

static int active_in_prefab(const Dream *d, int n)
{
    int guard = 0;
    while (n >= 0 && guard++ < 64) {
        if (!(d->node[n].flags & 1))
            return 0;
        n = d->node[n].parent;
    }
    return 1;
}

static void box_trigger(const Dream *d, DreamTrigger *t, int node, const float *centre, const float *size)
{
    aff w = node_world(d, node);
    aff inv = aff_inverse(&w);
    memcpy(t->inv, inv.m, sizeof(t->inv));
    for (int k = 0; k < 3; k++) {
        float s = fabsf(size[k]) * 0.5f;
        t->lo[k] = centre[k] - s;
        t->hi[k] = centre[k] + s;
        t->scale[k] = sqrtf(w.m[k] * w.m[k] + w.m[4 + k] * w.m[4 + k] + w.m[8 + k] * w.m[8 + k]);
    }
    t->node = node;
}

int models_load(const char *path, Model **out_models, int *out_n, uint32_t *vram_bytes)
{
    Dream tmp;
    memset(&tmp, 0, sizeof(tmp));
    Dream *d = &tmp;
    *out_models = NULL;
    *out_n = 0;
    size_t size = 0;
    uint8_t *buf = vfs_read(path, &size);
    if (!buf)
        return 0;
    Cur c = {buf, buf + size, 0};
    const uint8_t *magic = (const uint8_t *)take(&c, 4);
    uint32_t ver = u32(&c);
    int nm = (int)u32(&c);
    if (!magic || memcmp(magic, "LSDM", 4) != 0 || ver != 1 || nm < 0 || nm > 4096) {
        lsd_log("%s: not a model file", path);
        free(buf);
        return 0;
    }
    d->model = (Model *)calloc(nm ? nm : 1, sizeof(Model));
    d->nmodel = d->model ? nm : 0;
    if (!d->model) c.bad = 1;
    for (int mi = 0; mi < d->nmodel && !c.bad; mi++) {
        Model *m = &d->model[mi];
        const char *nmz = (const char *)take(&c, 32);
        if (nmz) memcpy(m->name, nmz, 31);
        m->nmesh = (int)u32(&c);
        if (m->nmesh < 0 || m->nmesh > 4096) { c.bad = 1; break; }
        m->mesh = (ModelMesh *)calloc(m->nmesh ? m->nmesh : 1, sizeof(ModelMesh));
        if (!m->mesh) { c.bad = 1; break; }
        for (int j = 0; j < m->nmesh && !c.bad; j++) {
            ModelMesh *mm = &m->mesh[j];
            mm->nbatch = (int)u32(&c);
            if (mm->nbatch < 0 || mm->nbatch > 3) { c.bad = 1; break; }
            mm->batch = (DreamBatch *)calloc(mm->nbatch ? mm->nbatch : 1, sizeof(DreamBatch));
            if (!mm->batch) { c.bad = 1; break; }
            for (int k = 0; k < 3; k++) {
                mm->lo[k] = 1e30f;
                mm->hi[k] = -1e30f;
            }
            for (int bi = 0; bi < mm->nbatch && !c.bad; bi++) {
                DreamBatch *b = &mm->batch[bi];
                b->kind = (int)u32(&c);
                b->nvert = u32(&c);
                b->nidx = u32(&c);
                for (int k = 0; k < 3; k++) b->lo[k] = f32(&c);
                for (int k = 0; k < 3; k++) b->hi[k] = f32(&c);
                if (b->kind < 0 || b->kind > 2 || b->nvert > 65536 || b->nidx % 3 || b->nidx > (1u << 22)) {
                    c.bad = 1;
                    break;
                }
                const void *verts = take(&c, (size_t)b->nvert * sizeof(DreamVertex));
                const uint16_t *idx = (const uint16_t *)take(&c, (size_t)b->nidx * 2);
                if (b->nidx % 2) take(&c, 2);
                if (c.bad) break;
                for (uint32_t k = 0; k < b->nidx; k++)
                    if (idx[k] >= b->nvert) { c.bad = 1; break; }
                if (c.bad) break;
                glGenBuffers(1, &b->vbo);
                glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
                glBufferData(GL_ARRAY_BUFFER, b->nvert * sizeof(DreamVertex), verts, GL_STATIC_DRAW);
                glGenBuffers(1, &b->ibo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, b->nidx * 2, idx, GL_STATIC_DRAW);
                d->vram_bytes += b->nvert * sizeof(DreamVertex) + b->nidx * 2;
                for (int k = 0; k < 3; k++) {
                    if (b->lo[k] < mm->lo[k]) mm->lo[k] = b->lo[k];
                    if (b->hi[k] > mm->hi[k]) mm->hi[k] = b->hi[k];
                }
            }
        }
        m->nclip = (int)u32(&c);
        if (m->nclip < 0 || m->nclip > 64) { c.bad = 1; break; }
        m->clip = (AnimClip *)calloc(m->nclip ? m->nclip : 1, sizeof(AnimClip));
        if (!m->clip) { c.bad = 1; break; }
        for (int ci = 0; ci < m->nclip && !c.bad; ci++) {
            AnimClip *cl = &m->clip[ci];
            cl->length = f32(&c);
            cl->ntrack = (int)u32(&c);
            if (cl->ntrack < 0 || cl->ntrack > 1024) { c.bad = 1; break; }
            cl->track = (AnimTrack *)calloc(cl->ntrack ? cl->ntrack : 1, sizeof(AnimTrack));
            if (!cl->track) { c.bad = 1; break; }
            for (int ti = 0; ti < cl->ntrack && !c.bad; ti++) {
                AnimTrack *t = &cl->track[ti];
                uint32_t pl = u32(&c);
                if (pl > 255) { c.bad = 1; break; }
                const char *ps = (const char *)take(&c, (pl + 3) & ~3u);
                if (!ps) break;
                t->path = (char *)malloc(pl + 1);
                if (!t->path) { c.bad = 1; break; }
                memcpy(t->path, ps, pl);
                t->path[pl] = 0;
                t->nkeys = (int)u32(&c);
                if (t->nkeys < 0 || t->nkeys > 100000) { c.bad = 1; break; }
                const float *k = (const float *)take(&c, (size_t)t->nkeys * 11 * 4);
                if (!k) break;
                t->keys = (float *)malloc((size_t)(t->nkeys ? t->nkeys : 1) * 11 * 4);
                if (!t->keys) { c.bad = 1; break; }
                memcpy(t->keys, k, (size_t)t->nkeys * 11 * 4);
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    free(buf);
    *out_models = d->model;
    *out_n = d->nmodel;
    if (vram_bytes) *vram_bytes += d->vram_bytes;
    if (c.bad) {
        lsd_log("%s: truncated or corrupt", path);
        return 0;
    }
    return 1;
}

static int load_models(Dream *d, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/models.bin", dir);
    return models_load(path, &d->model, &d->nmodel, &d->vram_bytes);
}

int dream_load_entities(Dream *d, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/entities.bin", dir);
    size_t size = 0;
    uint8_t *buf = vfs_read(path, &size);
    if (!buf)
        return 0;
    Cur c = {buf, buf + size, 0};
    const uint8_t *magic = (const uint8_t *)take(&c, 4);
    uint32_t ver = u32(&c);
    if (!magic || memcmp(magic, "LSDE", 4) != 0 || ver != 2) {
        lsd_log("%s: not a v2 entity file (v%u)", path, (unsigned)ver);
        free(buf);
        return 0;
    }
    const char *name = (const char *)take(&c, 64);
    if (name) {
        memcpy(d->name, name, 63);
        d->name[63] = 0;
    }
    uint32_t ns = u32(&c);
    const char *strs = (const char *)take(&c, ns);
    if (strs && ns) {
        d->strings = (char *)malloc(ns + 1);
        if (d->strings) {
            memcpy(d->strings, strs, ns);
            d->strings[ns] = 0;
            d->nstrings = ns;
        } else c.bad = 1;
    }
    uint32_t nints = u32(&c);
    const int32_t *ints = nints < (1u << 20) ? (const int32_t *)take(&c, (size_t)nints * 4) : NULL;
    if (!ints && nints) c.bad = 1;
    uint32_t nn = u32(&c);
    if (nn > 65536) c.bad = 1;
    const uint8_t *nodes = c.bad ? NULL : (const uint8_t *)take(&c, (size_t)nn * 56);
    uint32_t nc = u32(&c);
    if (nc > 65536) c.bad = 1;
    const uint8_t *comps = c.bad ? NULL : (const uint8_t *)take(&c, (size_t)nc * 72);
    if (c.bad) {
        lsd_log("%s: truncated or corrupt", path);
        free(buf);
        return 0;
    }
    d->nnode = (int)nn;
    d->node = (SceneNodeDef *)calloc(nn ? nn : 1, sizeof(SceneNodeDef));
    d->ncomp = (int)nc;
    d->comp = (SceneCompDef *)calloc(nc ? nc : 1, sizeof(SceneCompDef));
    if (!d->node || !d->comp) {
        free(buf);
        return 0;
    }
    for (uint32_t i = 0; i < nn; i++) {
        const uint8_t *p = nodes + i * 56;
        SceneNodeDef *s = &d->node[i];
        memcpy(&s->parent, p, 4);
        memcpy(&s->name, p + 4, 4);
        memcpy(s->pos, p + 8, 12);
        memcpy(s->rot, p + 20, 16);
        memcpy(s->scale, p + 36, 12);
        memcpy(&s->flags, p + 48, 4);
        memcpy(&s->mesh, p + 52, 4);
        if (s->parent >= (int)i || s->parent < -1) {
            lsd_log("%s: node %u has a bad parent", path, (unsigned)i);
            s->parent = -1;
        }
        if (s->name >= d->nstrings) s->name = 0;
    }
    for (uint32_t i = 0; i < nc; i++) {
        const uint8_t *p = comps + i * 72;
        SceneCompDef *s = &d->comp[i];
        memcpy(&s->type, p, 4);
        memcpy(&s->node, p + 4, 4);
        memcpy(&s->id, p + 8, 4);
        memcpy(&s->enabled, p + 12, 4);
        memcpy(s->w.u, p + 16, 56);
        if (s->node < 0 || s->node >= (int)nn) s->node = 0;
        if (s->id >= d->nstrings) s->id = 0;
    }

    /* spawn points (only those active in the prefab: GetComponentsInChildren skips inactive ones) */
    int nsp = 0, ntr = 0;
    for (uint32_t i = 0; i < nc; i++) {
        if (d->comp[i].type == COMP_SPAWN) nsp++;
        if (d->comp[i].type == COMP_TRIGGER_LINK || d->comp[i].type == COMP_TRIGGER_LUA) ntr++;
    }
    free(d->spawn);
    d->spawn = (DreamSpawn *)calloc(nsp ? nsp : 1, sizeof(DreamSpawn));
    d->trig = (DreamTrigger *)calloc(ntr ? ntr : 1, sizeof(DreamTrigger));
    d->nspawn = d->ntrig = 0;
    if (!d->spawn || !d->trig) {
        free(buf);
        return 0;
    }
    for (uint32_t i = 0; i < nc; i++) {
        const SceneCompDef *s = &d->comp[i];
        if (s->type == COMP_SPAWN) {
            if (!active_in_prefab(d, s->node) || !s->enabled)
                continue;
            aff w = node_world(d, s->node);
            DreamSpawn *sp = &d->spawn[d->nspawn++];
            sp->pos = v3(w.m[3], w.m[7], w.m[11]);
            sp->yaw = atan2f(w.m[2], w.m[10]) / DEG2RAD;
            sp->flags = s->w.u[0];
            sp->id = s->w.u[1];
            sp->node = s->node;
        } else if (s->type == COMP_TRIGGER_LINK) {
            DreamTrigger *t = &d->trig[d->ntrig++];
            t->kind = TRIGGER_LINK;
            t->comp = (int)i;
            t->linked = s->w.i[0];
            t->spawn = s->w.u[1];
            t->flags = s->w.u[2];
            memcpy(t->color, &s->w.f[3], 12);
            box_trigger(d, t, s->node, &s->w.f[6], &s->w.f[9]);
        } else if (s->type == COMP_TRIGGER_LUA) {
            DreamTrigger *t = &d->trig[d->ntrig++];
            t->kind = TRIGGER_LUA;
            t->comp = (int)i;
            t->linked = -1;
            t->flags = s->w.u[3] ? TRIG_ONCE : 0;
            box_trigger(d, t, s->node, &s->w.f[4], &s->w.f[7]);
        } else if (s->type == COMP_GRAPH && !d->graph.present) {
            uint32_t off = s->w.u[2], n = s->w.u[3];
            if (off + (uint64_t)n * 2 > nints) continue;
            d->graph.present = 1;
            d->graph.layout = (int)s->w.u[0];
            d->graph.width = s->w.i[1];
            aff w = node_world(d, s->node);
            aff inv = aff_inverse(&w);
            memcpy(d->graph.inv, inv.m, sizeof(d->graph.inv));
            d->graph.n = (int)n;
            d->graph.data = (int (*)[2])calloc(n ? n : 1, sizeof(int[2]));
            if (d->graph.data)
                memcpy(d->graph.data, ints + off, (size_t)n * 8);
            else
                d->graph.present = 0;
        }
    }
    /* spawn order as before: day-one spawns first, then by name (stable) */
    for (int i = 1; i < d->nspawn; i++) {
        DreamSpawn key = d->spawn[i];
        int j = i - 1;
        while (j >= 0) {
            const DreamSpawn *o = &d->spawn[j];
            int ka = (key.flags & 1) ? 0 : 1, kb = (o->flags & 1) ? 0 : 1;
            int c2 = ka != kb ? ka - kb : strcmp(dream_str(d, d->node[key.node].name), dream_str(d, d->node[o->node].name));
            if (c2 >= 0)
                break;
            d->spawn[j + 1] = d->spawn[j];
            j--;
        }
        d->spawn[j + 1] = key;
    }
    free(buf);
    if (!load_models(d, dir))
        return 0;
    return 1;
}

void models_free(Model *models, int n)
{
    for (int mi = 0; mi < n; mi++) {
        Model *m = &models[mi];
        for (int j = 0; j < m->nmesh; j++) {
            for (int b = 0; b < m->mesh[j].nbatch; b++) {
                if (m->mesh[j].batch[b].vbo) glDeleteBuffers(1, &m->mesh[j].batch[b].vbo);
                if (m->mesh[j].batch[b].ibo) glDeleteBuffers(1, &m->mesh[j].batch[b].ibo);
            }
            free(m->mesh[j].batch);
        }
        free(m->mesh);
        for (int ci = 0; ci < m->nclip; ci++) {
            for (int t = 0; t < m->clip[ci].ntrack; t++) {
                free(m->clip[ci].track[t].path);
                free(m->clip[ci].track[t].keys);
            }
            free(m->clip[ci].track);
        }
        free(m->clip);
    }
    free(models);
}

void dream_free_entities(Dream *d)
{
    models_free(d->model, d->nmodel);
    free(d->node);
    free(d->comp);
    free(d->strings);
    d->model = NULL;
    d->node = NULL;
    d->comp = NULL;
    d->strings = NULL;
    d->nmodel = d->nnode = d->ncomp = 0;
    d->nstrings = 0;
    free(d->fchunk);
    free(d->tri_chunk);
    d->fchunk = NULL;
    d->tri_chunk = NULL;
    d->nfchunk = 0;
    free(d->envfx);
    d->envfx = NULL;
    d->nenvfx = 0;
}

/* footsteps.bin (tools/audiodata.py): LBD chunk tiles and each collision triangle's chunk */
int dream_load_footsteps(Dream *d, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/footsteps.bin", dir);
    size_t size = 0;
    uint8_t *data = vfs_read(path, &size);
    if (!data)
        return 0;
    Cur c = {data, data + size, 0};
    const void *magic = take(&c, 4);
    int ok = magic && !memcmp(magic, "LSDF", 4) && u32(&c) == 1;
    uint32_t nch = ok ? u32(&c) : 0;
    if (nch > 4096) ok = 0;
    if (ok) {
        d->fchunk = (FootChunk *)calloc(nch ? nch : 1, sizeof(FootChunk));
        ok = d->fchunk != NULL;
    }
    for (uint32_t i = 0; ok && i < nch; i++) {
        FootChunk *f = &d->fchunk[i];
        for (int k = 0; k < 3; k++) f->pos[k] = f32(&c);
        f->ntile = (int)u32(&c);
        const void *t = take(&c, 400);
        if (!t || f->ntile < 0 || f->ntile > 400) {
            ok = 0;
            break;
        }
        memcpy(f->footstep, t, 400);
    }
    uint32_t nt = ok ? u32(&c) : 0;
    if (ok && (int)nt != d->ntri) ok = 0;
    if (ok) {
        const void *t = take(&c, nt * 2);
        d->tri_chunk = (uint16_t *)malloc(nt ? nt * 2 : 2);
        ok = t && d->tri_chunk && !c.bad;
        if (ok) memcpy(d->tri_chunk, t, nt * 2);
    }
    free(data);
    if (!ok) {
        lsd_log("%s: bad footstep data", path);
        free(d->fchunk);
        free(d->tri_chunk);
        d->fchunk = NULL;
        d->tri_chunk = NULL;
        return 0;
    }
    d->nfchunk = (int)nch;
    return 1;
}

/* envfx.bin (tools/skydata.py): the sky effect fields of each environment */
typedef char envfx_is_132_bytes[sizeof(EnvFx) == 132 ? 1 : -1];
int dream_load_envfx(Dream *d, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/envfx.bin", dir);
    size_t size = 0;
    uint8_t *data = vfs_read(path, &size);
    if (!data)
        return 0;
    uint32_t n = 0;
    int ok = size >= 12 && !memcmp(data, "LSDX", 4);
    if (ok) {
        uint32_t ver;
        memcpy(&ver, data + 4, 4);
        memcpy(&n, data + 8, 4);
        ok = ver == 1 && n < 100000 && size >= 12 + (size_t)n * sizeof(EnvFx);
    }
    if (ok) {
        d->envfx = (EnvFx *)malloc((n ? n : 1) * sizeof(EnvFx));
        ok = d->envfx != NULL;
    }
    if (ok) {
        memcpy(d->envfx, data + 12, (size_t)n * sizeof(EnvFx));
        d->nenvfx = (int)n;
    } else {
        lsd_log("%s: bad sky effect data", path);
    }
    free(data);
    return ok;
}
