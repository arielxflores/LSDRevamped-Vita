/* Loads a dream baked by tools/bake.py */
#include "dream.h"

typedef struct {
    const uint8_t *p, *end;
    int bad;
    VfsStream *s; /* streamed: bytes are unpacked as the cursor gets to them */
    const uint8_t *base;
    size_t ready;
} Cursor;

static int need(Cursor *c, size_t n)
{
    if (c->bad || (size_t)(c->end - c->p) < n) {
        c->bad = 1;
        return 0;
    }
    if (c->s) {
        size_t upto = (size_t)(c->p - c->base) + n;
        if (upto > c->ready) {
            /* (a little ahead, so small reads do not go back to the stream one by one) */
            size_t want = upto + 65536 < (size_t)(c->end - c->base) ? upto + 65536 : (size_t)(c->end - c->base);
            if (!vfs_stream_need(c->s, want)) {
                c->bad = 1;
                return 0;
            }
            c->ready = want;
        }
    }
    return 1;
}

static uint32_t rd_u32(Cursor *c)
{
    uint32_t v = 0;
    if (need(c, 4)) {
        memcpy(&v, c->p, 4);
        c->p += 4;
    }
    return v;
}

static float rd_f32(Cursor *c)
{
    float v = 0;
    if (need(c, 4)) {
        memcpy(&v, c->p, 4);
        c->p += 4;
    }
    return v;
}

static const void *rd_bytes(Cursor *c, size_t n)
{
    if (!need(c, n))
        return NULL;
    const void *r = c->p;
    c->p += n;
    return r;
}

static GLuint load_texture(const char *path, uint32_t *bytes)
{
    size_t n = 0;
    uint8_t *px = vfs_read(path, &n);
    if (!px)
        return 0;
    const int w = 2048, h = 512;
    if (n != (size_t)w * h * 4) {
        lsd_log("texture %s has wrong size %u", path, (unsigned)n);
        free(px);
        return 0;
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    *bytes += (uint32_t)n;
    return t;
}

int journal_load(Journal *j)
{
    memset(j, 0, sizeof(*j));
    memset(j->graph, -1, sizeof(j->graph));
    size_t size = 0;
    uint8_t *buf = vfs_read("journal.bin", &size);
    if (!buf)
        return 0;
    Cursor c = {buf, buf + size, 0, NULL, buf, 0};
    const uint8_t *magic = (const uint8_t *)rd_bytes(&c, 4);
    uint32_t ver = rd_u32(&c);
    int n = (int)rd_u32(&c);
    if (!magic || memcmp(magic, "LSDJ", 4) != 0 || ver != 1 || n <= 0 || n > JOURNAL_MAX) {
        lsd_log("journal.bin: bad header");
        free(buf);
        return 0;
    }
    j->n = n;
    for (int i = 0; i < n && !c.bad; i++) {
        JournalDream *e = &j->d[i];
        const char *dir = (const char *)rd_bytes(&c, 16);
        const char *name = (const char *)rd_bytes(&c, 64);
        if (c.bad) break;
        memcpy(e->dir, dir, 15);
        memcpy(e->name, name, 63);
        e->flags = rd_u32(&c);
        e->env_offset = (int)rd_u32(&c);
    }
    const int8_t *g = (const int8_t *)rd_bytes(&c, GRAPH_SIZE * GRAPH_SIZE);
    if (g)
        for (int i = 0; i < GRAPH_SIZE * GRAPH_SIZE; i++)
            j->graph[i] = (g[i] >= 0 && g[i] < n) ? g[i] : -1;
    free(buf);
    if (c.bad) {
        lsd_log("journal.bin: truncated");
        return 0;
    }
    lsd_log("journal: %d dreams", n);
    return 1;
}

/* collision triangles: corners, normal and bounds (on another core while the load goes on) */
typedef struct {
    ColTri *tri;
    const uint8_t *src;
    int n;
} TriPrep;

static void prep_tris(void *arg)
{
    const TriPrep *tp = (const TriPrep *)arg;
    for (int i = 0; i < tp->n; i++) {
        ColTri *t = &tp->tri[i];
        float f[9];
        memcpy(f, tp->src + (size_t)i * 36, 36);
        t->a = v3(f[0], f[1], f[2]);
        t->b = v3(f[3], f[4], f[5]);
        t->c = v3(f[6], f[7], f[8]);
        vec3 n = v3cross(v3sub(t->b, t->a), v3sub(t->c, t->a));
        float l2 = v3dot(n, n);
        t->n = l2 > 1e-24f ? v3scale(n, 1.0f / sqrtf(l2)) : v3(0, 1, 0);
        for (int k = 0; k < 3; k++) {
            float a = f[k], b = f[3 + k], cc = f[6 + k];
            float lo = a < b ? a : b, hi = a < b ? b : a;
            t->lo[k] = cc < lo ? cc : lo;
            t->hi[k] = cc > hi ? cc : hi;
        }
    }
}

int dream_load(Dream *d, const char *dir, int texset)
{
    double t_start = plat_seconds();
    memset(d, 0, sizeof(*d));
    char path[512];
    snprintf(path, sizeof(path), "%s/dream.bin", dir);
    VfsStream *st_in = vfs_stream_open(path);
    if (!st_in)
        return 0;
    size_t size = vfs_stream_size(st_in);
    const uint8_t *buf = vfs_stream_need(st_in, 0);
    if (!buf) {
        vfs_stream_abort(st_in);
        return 0;
    }
    snprintf(d->dir, sizeof(d->dir), "%s", dir);
    snprintf(d->name, sizeof(d->name), "%s", dir);
    Cursor c = {buf, buf + size, 0, st_in, buf, 0};
    TriPrep tp = {NULL, NULL, 0};
    int tri_task = -1;
    uint8_t *owned = NULL;
    double t_unpacked = t_start;
    const uint8_t *magic = (const uint8_t *)rd_bytes(&c, 4);
    uint32_t ver = rd_u32(&c);
    if (!magic || memcmp(magic, "LSDV", 4) != 0 || ver != 4) {
        lsd_log("%s: not a v4 baked dream (got v%u)", path, (unsigned)ver);
        vfs_stream_abort(st_in);
        return 0;
    }

    d->nenv = (int)rd_u32(&c);
    if (d->nenv <= 0 || d->nenv > 100000)
        c.bad = 1;
    else
        d->env = (DreamEnv *)calloc(d->nenv, sizeof(DreamEnv));
    if (!d->env) c.bad = 1;
    for (int i = 0; i < d->nenv && !c.bad; i++) {
        DreamEnv *e = &d->env[i];
        for (int k = 0; k < 3; k++) e->fog[k] = rd_f32(&c);
        e->subtractive = (int)rd_u32(&c);
        e->fog_start = rd_f32(&c);
        e->fog_end = rd_f32(&c);
        e->fog_height = rd_f32(&c);
        e->fog_gradient = rd_f32(&c);
        for (int k = 0; k < 3; k++) e->sky[k] = rd_f32(&c);
        for (int k = 0; k < 3; k++) e->sky_fog[k] = rd_f32(&c);
    }

    d->nspawn = (int)rd_u32(&c);
    if (d->nspawn < 0 || d->nspawn > 10000)
        c.bad = 1;
    else
        d->spawn = (DreamSpawn *)calloc(d->nspawn ? d->nspawn : 1, sizeof(DreamSpawn));
    if (!d->spawn) c.bad = 1;
    for (int i = 0; i < d->nspawn && !c.bad; i++) {
        d->spawn[i].pos.x = rd_f32(&c);
        d->spawn[i].pos.y = rd_f32(&c);
        d->spawn[i].pos.z = rd_f32(&c);
        d->spawn[i].yaw = rd_f32(&c);
        d->spawn[i].flags = rd_u32(&c);
    }
    d->brightness = rd_f32(&c);
    d->vci = rd_f32(&c);
    d->cull = (int)rd_u32(&c);

    d->nbatch = (int)rd_u32(&c);
    if (d->nbatch < 0 || d->nbatch > 100000)
        c.bad = 1;
    else
        d->batch = (DreamBatch *)calloc(d->nbatch ? d->nbatch : 1, sizeof(DreamBatch));
    if (!d->batch) c.bad = 1;
    for (int k = 0; k < 3; k++) {
        d->lo[k] = 1e30f;
        d->hi[k] = -1e30f;
    }
    for (int i = 0; i < d->nbatch && !c.bad; i++) {
        DreamBatch *b = &d->batch[i];
        b->kind = (int)rd_u32(&c);
        b->group = (int)rd_u32(&c);
        if (b->kind < 0 || b->kind >= KIND_WATER + WATER_MAX)
            c.bad = 1;
        b->nvert = rd_u32(&c);
        b->nidx = rd_u32(&c);
        for (int k = 0; k < 3; k++) b->lo[k] = rd_f32(&c);
        for (int k = 0; k < 3; k++) b->hi[k] = rd_f32(&c);
        if (b->nvert > 65536 || b->nidx % 3 || b->nidx > (1u << 24)) {
            c.bad = 1;
            break;
        }
        const void *verts = rd_bytes(&c, b->nvert * sizeof(DreamVertex));
        const void *idx = rd_bytes(&c, (size_t)b->nidx * 2);
        if (b->nidx % 2)
            rd_bytes(&c, 2);
        if (c.bad)
            break;
        const uint16_t *ix = (const uint16_t *)idx;
        for (uint32_t k = 0; k < b->nidx; k++)
            if (ix[k] >= b->nvert) {
                lsd_log("batch %d index %u out of range", i, (unsigned)k);
                c.bad = 1;
                break;
            }
        if (c.bad)
            break;
        glGenBuffers(1, &b->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
        glBufferData(GL_ARRAY_BUFFER, b->nvert * sizeof(DreamVertex), verts, GL_STATIC_DRAW);
        glGenBuffers(1, &b->ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b->ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, b->nidx * 2, idx, GL_STATIC_DRAW);
        d->vram_bytes += b->nvert * sizeof(DreamVertex) + b->nidx * 2;
        for (int k = 0; k < 3; k++) {
            if (b->lo[k] < d->lo[k]) d->lo[k] = b->lo[k];
            if (b->hi[k] > d->hi[k]) d->hi[k] = b->hi[k];
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* LBD renderer bounds */
    d->ngroup = (int)rd_u32(&c);
    if (d->ngroup < 0 || d->ngroup > 100000)
        c.bad = 1;
    else
        d->group = (DreamGroup *)calloc(d->ngroup ? d->ngroup : 1, sizeof(DreamGroup));
    if (!d->group) c.bad = 1;
    for (int i = 0; i < d->ngroup && !c.bad; i++)
        for (int k = 0; k < 6; k++) (k < 3 ? d->group[i].lo : d->group[i].hi)[k % 3] = rd_f32(&c);
    for (int i = 0; i < d->nbatch && !c.bad; i++)
        if (d->batch[i].group < 0 || d->batch[i].group >= d->ngroup) c.bad = 1;

    /* water materials and their textures */
    d->nwater = (int)rd_u32(&c);
    if (d->nwater < 0 || d->nwater > WATER_MAX)
        c.bad = 1;
    for (int i = 0; i < d->nwater && !c.bad; i++) {
        WaterMat *m = &d->water[i];
        m->alpha = rd_f32(&c);
        m->speed = rd_f32(&c);
        m->brightness = rd_f32(&c);
        m->cull = (int)rd_u32(&c);
        for (int k = 0; k < 4; k++) m->tint[k] = rd_f32(&c);
        for (int k = 0; k < TEXSETS; k++) m->map[k] = (int)rd_u32(&c);
        for (int k = 0; k < TEXSETS; k++) m->pal[k] = (int)rd_u32(&c);
    }
    for (int i = 0; i < d->nbatch && !c.bad; i++)
        if (d->batch[i].kind >= KIND_WATER + d->nwater) c.bad = 1;
    if (!c.bad) {
        /* each group's transparent batches, in submesh order (alpha, then water 0, 1, ...) */
        d->tlist = (int *)malloc((d->nbatch ? d->nbatch : 1) * sizeof(int));
        if (!d->tlist) c.bad = 1;
        for (int i = 0; i < d->nbatch && !c.bad; i++)
            if (d->batch[i].kind >= KIND_ALPHA) d->group[d->batch[i].group].tcount++;
        int nt = 0;
        for (int g = 0; g < d->ngroup && !c.bad; g++) {
            d->group[g].tfirst = nt;
            nt += d->group[g].tcount;
            d->group[g].tcount = 0;
        }
        for (int kind = KIND_ALPHA; kind < KIND_WATER + d->nwater && !c.bad; kind++)
            for (int i = 0; i < d->nbatch; i++)
                if (d->batch[i].kind == kind) {
                    DreamGroup *g = &d->group[d->batch[i].group];
                    d->tlist[g->tfirst + g->tcount++] = i;
                }
    }
    d->nwtex = (int)rd_u32(&c);
    if (d->nwtex < 0 || d->nwtex > 64)
        c.bad = 1;
    else if (d->nwtex)
        d->wtex = (GLuint *)calloc(d->nwtex, sizeof(GLuint));
    if (d->nwtex && !d->wtex) c.bad = 1;
    for (int i = 0; i < d->nwtex && !c.bad; i++) {
        uint32_t w = rd_u32(&c), h = rd_u32(&c);
        if (w == 0 || h == 0 || w > 1024 || h > 1024) {
            c.bad = 1;
            break;
        }
        const void *px = rd_bytes(&c, (size_t)w * h * 4);
        if (!px)
            break;
        glGenTextures(1, &d->wtex[i]);
        glBindTexture(GL_TEXTURE_2D, d->wtex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        /* Unity wrap mode Repeat (the palette index wraps; the water maps tile) */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        d->vram_bytes += w * h * 4;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int i = 0; i < d->nwater && !c.bad; i++)
        for (int k = 0; k < TEXSETS; k++)
            if (d->water[i].map[k] < 0 || d->water[i].map[k] >= d->nwtex || d->water[i].pal[k] < 0 ||
                d->water[i].pal[k] >= d->nwtex)
                c.bad = 1;

    double t_geom = plat_seconds();
    d->ntri = (int)rd_u32(&c);
    if (d->ntri < 0 || d->ntri > 4000000)
        c.bad = 1;
    else
        d->tri = (ColTri *)malloc((d->ntri ? d->ntri : 1) * sizeof(ColTri));
    if (!d->tri) {
        lsd_log("out of memory for %d collision triangles", d->ntri);
        c.bad = 1;
    }
    const uint8_t *tri_src = c.bad ? NULL : (const uint8_t *)rd_bytes(&c, (size_t)d->ntri * 36);
    if (tri_src) {
        tp.tri = d->tri;
        tp.src = tri_src;
        tp.n = d->ntri;
        tri_task = plat_task_start(prep_tris, &tp, 2);
    }
    /* the collision grid, bucketed at bake time (tools/bake.py collision_grid) */
    if (!c.bad) {
        d->gx0 = rd_f32(&c);
        d->gz0 = rd_f32(&c);
        d->cell = rd_f32(&c);
        d->gw = (int)rd_u32(&c);
        d->gh = (int)rd_u32(&c);
        if (d->gw <= 0 || d->gh <= 0 || d->gw > 4096 || d->gh > 4096 || !(d->cell > 0)) {
            c.bad = 1;
        } else {
            size_t ncell = (size_t)d->gw * d->gh;
            const uint8_t *st = (const uint8_t *)rd_bytes(&c, (ncell + 1) * 4);
            uint32_t n = rd_u32(&c);
            const uint8_t *en = st ? (const uint8_t *)rd_bytes(&c, (size_t)n * 4) : NULL;
            d->cell_start = (int *)malloc((ncell + 1) * sizeof(int));
            d->cell_tris = (int *)malloc((n ? n : 1) * sizeof(int));
            if (!st || !en || !d->cell_start || !d->cell_tris) {
                c.bad = 1;
            } else {
                memcpy(d->cell_start, st, (ncell + 1) * 4);
                memcpy(d->cell_tris, en, (size_t)n * 4);
                int prev = 0;
                for (size_t i = 0; i <= ncell && !c.bad; i++) {
                    if (d->cell_start[i] < prev || d->cell_start[i] > (int)n) c.bad = 1;
                    prev = d->cell_start[i];
                }
                if (d->cell_start[0] != 0 || d->cell_start[ncell] != (int)n) c.bad = 1;
                for (uint32_t i = 0; i < n && !c.bad; i++)
                    if ((unsigned)d->cell_tris[i] >= (unsigned)d->ntri) c.bad = 1;
            }
        }
    }
    double t_grid_done = plat_seconds();
    if (c.p != c.end && !c.bad)
        lsd_log("%s: %u trailing bytes ignored", path, (unsigned)(c.end - c.p));
    /* the whole entry is in: wait for the reader, check it */
    int read_ok = !c.bad && vfs_stream_check(st_in);
    owned = vfs_stream_take(st_in, NULL); /* (kept until the triangle task is done with it) */
    t_unpacked = plat_seconds();
    if (!read_ok) {
        if (c.bad)
            lsd_log("%s: truncated or corrupt", path);
        plat_task_join(tri_task);
        free(owned);
        dream_free(d);
        return 0;
    }

    double t_grid = plat_seconds();
    int ok = dream_load_entities(d, dir);
    if (ok) {
        dream_load_footsteps(d, dir); /* (footstep sounds only: fine without) */
        dream_load_envfx(d, dir);     /* (sky effects only) */
    }
    double t_tex = plat_seconds();
    if (ok)
        ok = dream_load_texset(d, texset);
    double t_tex_end = plat_seconds();
    plat_task_join(tri_task); /* (the triangles were set up meanwhile) */
    free(owned);
    if (!ok) {
        dream_free(d);
        return 0;
    }
    double t_end = plat_seconds();
    /* (reading and unpacking overlap the set-up now: "streamed" is all of it up to the check) */
    lsd_log("dream %s (%s) loaded in %.0f ms: %d batches (%d LBDs, %d water materials), %d collision tris, %d spawns, "
            "%d links, %d environments, grid %dx%d, %u KB GPU data; times: streamed geometry %.0f ms, triangles+grid "
            "%.0f ms, finishing the read %.0f ms, entities %.0f ms, texture %.0f ms, waiting for the triangles %.0f ms",
            d->dir, d->name, (t_end - t_start) * 1000.0, d->nbatch, d->ngroup, d->nwater, d->ntri, d->nspawn,
            d->ntrig, d->nenv, d->gw, d->gh, (unsigned)(d->vram_bytes / 1024), (t_geom - t_start) * 1000.0,
            (t_grid_done - t_geom) * 1000.0, (t_unpacked - t_grid_done) * 1000.0, (t_tex - t_grid) * 1000.0,
            (t_tex_end - t_tex) * 1000.0, (t_end - t_tex_end) * 1000.0);
    return 1;
}

/* one texture set at a time (each is a 2048x512 RGBA atlas); others load when asked for */
int dream_load_texset(Dream *d, int texset)
{
    static const char sets[TEXSETS] = {'A', 'B', 'C', 'D'};
    texset &= 3;
    if (d->tex[texset])
        return 1;
    char path[64];
    snprintf(path, sizeof(path), "%s/tex_%c.bin", d->dir, sets[texset]);
    d->tex[texset] = load_texture(path, &d->vram_bytes);
    if (!d->tex[texset]) {
        lsd_log("texture set %c missing in %s", sets[texset], d->dir);
        return 0;
    }
    return 1;
}

void dream_free(Dream *d)
{
    for (int i = 0; i < d->nbatch; i++) {
        if (d->batch[i].vbo) glDeleteBuffers(1, &d->batch[i].vbo);
        if (d->batch[i].ibo) glDeleteBuffers(1, &d->batch[i].ibo);
    }
    GLuint done[TEXSETS];
    int ndone = 0;
    for (int i = 0; i < TEXSETS; i++) {
        int dup = 0;
        for (int k = 0; k < ndone; k++)
            if (done[k] == d->tex[i]) dup = 1;
        if (d->tex[i] && !dup) {
            glDeleteTextures(1, &d->tex[i]);
            done[ndone++] = d->tex[i];
        }
    }
    if (d->nwtex && d->wtex)
        glDeleteTextures(d->nwtex, d->wtex);
    free(d->wtex);
    free(d->group);
    free(d->tlist);
    dream_free_entities(d);
    free(d->env);
    free(d->spawn);
    free(d->trig);
    free(d->graph.data);
    free(d->batch);
    free(d->tri);
    free(d->cell_start);
    free(d->cell_tris);
    memset(d, 0, sizeof(*d));
}
