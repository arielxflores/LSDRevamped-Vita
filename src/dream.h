#ifndef LSD_DREAM_H
#define LSD_DREAM_H

#include "common.h"

typedef struct {
    float fog[3];
    int subtractive;
    float fog_start, fog_end;
    float fog_height, fog_gradient;
    float sky[3], sky_fog[3];
} DreamEnv;

enum { SPAWN_DAY_ONE = 1, SPAWN_TUNNEL = 2 };

typedef struct {
    vec3 pos;
    float yaw;
    uint32_t flags; /* SPAWN_* */
    uint32_t id;    /* fnv1a of the entity ID (0 = none) */
    int node;
} DreamSpawn;

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} DreamVertex; /* 24 bytes, matches bake.py */

/* KIND_WATER + i: drawn with the dream's water material i (LSDR/ClassicWater, a UV material override) */
enum { KIND_SOLID = 0, KIND_CUTOUT = 1, KIND_ALPHA = 2, KIND_WATER = 3 };
#define WATER_MAX 4

typedef struct {
    int kind;
    int group;            /* LBD renderer it belongs to (dream batches only) */
    uint32_t nvert, nidx;
    float lo[3], hi[3];
    GLuint vbo, ibo;
} DreamBatch;

typedef struct {
    vec3 a, b, c;
    vec3 n;        /* unit normal */
    float lo[3], hi[3];
} ColTri;

/* LSDR TriggerLink / TriggerLua (BoxCollider triggers) */
enum { TRIG_FORCE_COLOR = 1, TRIG_SOUND = 2, TRIG_LOCK_INPUT = 4, TRIG_WITHIN = 8, TRIG_ONCE = 16 };
enum { TRIGGER_LINK = 0, TRIGGER_LUA = 1 };

typedef struct {
    int kind;             /* TRIGGER_* */
    int node;             /* scene node (active in hierarchy = trigger enabled) */
    int comp;             /* component index */
    float inv[12];        /* world -> box local, 3 rows of 4 */
    float lo[3], hi[3];   /* box in local space */
    float scale[3];       /* local -> world length per axis */
    int linked;           /* journal index, -1 = random linkable dream */
    uint32_t spawn;       /* fnv1a of the spawn point ID, 0 = none */
    uint32_t flags;       /* TRIG_* */
    float color[3];       /* forced fade colour */
    /* runtime */
    int inside, triggered, exit_triggered;
} DreamTrigger;

/* LSDR LBDGraphContributor */
typedef struct {
    int present;
    int layout;           /* 0 horizontal, 1 vertical */
    int width;
    float inv[12];
    int n;
    int (*data)[2];       /* (dynamic, upper) per LBD tile */
} DreamGraph;

/* ---------------------------------------------------------------- entity scene (entities.bin v2) */
enum {
    COMP_INTERACTIVE = 1, COMP_RUNNER, COMP_ANIMATED, COMP_TARGET, COMP_CHANCE, COMP_APPEAR_DAY, COMP_AUDIO,
    COMP_MOVIE, COMP_TRIGGER_LUA, COMP_TRIGGER_LINK, COMP_SPAWN, COMP_GRAPH, COMP_PREFAB
};

typedef struct {
    int parent;
    uint32_t name;        /* offset into Dream.strings */
    float pos[3], rot[4], scale[3];
    uint32_t flags;       /* bit0 active, bit1 has renderer, bit2 renderer enabled */
    int mesh;             /* model << 16 | mesh, or -1 */
} SceneNodeDef;

typedef struct {
    uint32_t type;
    int node;
    uint32_t id;          /* entity ID string offset (0 = none) */
    uint32_t enabled;
    union {
        uint32_t u[14];
        int32_t i[14];
        float f[14];
    } w;
} SceneCompDef;

typedef struct {
    char *path;           /* relative to the animator, e.g. "0/5/6" */
    int nkeys;
    float *keys;          /* nkeys x (t, pos3, rot4, scale3) */
} AnimTrack;

typedef struct {
    float length;
    int ntrack;
    AnimTrack *track;
} AnimClip;

typedef struct {
    int nbatch;
    DreamBatch *batch;
    float lo[3], hi[3];
} ModelMesh;

typedef struct {
    char name[32];
    int nmesh;
    ModelMesh *mesh;
    int nclip;
    AnimClip *clip;
} Model;

#define TEXSETS 4

/* DreamEnvironment's sky effect fields (envfx.bin), one per DreamEnv */
typedef struct {
    float sun_color[4], sun_chance, sun_height, sun_size;
    float sun2_color[4], sun2_size, sun2_chance, sun2_offset, sun2_height;
    float burst_color[4], burst_size, burst_chance;
    float clouds_color[4];
    int32_t nclouds;
    float clouds_chance;
    float stars_color[4];
    int32_t nstars;
    float stars_chance;
} EnvFx;

/* an LBD chunk's tiles, for footstep sounds (footsteps.bin) */
typedef struct {
    float pos[3];
    int ntile;
    uint8_t footstep[400]; /* FootstepSoundAndCollision per tile, x * 20 + y */
} FootChunk;

/* LSDR/ClassicWater material: a water map's red channel indexes a 256-colour palette that cycles */
typedef struct {
    float alpha, speed, brightness;
    int cull;             /* Unity CullMode: 0 off, 1 front, 2 back */
    float tint[4];
    int map[TEXSETS], pal[TEXSETS]; /* per texture set: index into Dream.wtex */
} WaterMat;

/* an LBD chunk's renderer bounds (Unity sorts transparent renderers by their centres) */
typedef struct {
    float lo[3], hi[3];
    int tfirst, tcount;   /* its transparent batches in Dream.tlist: alpha first, then water by material */
} DreamGroup;

typedef struct {
    char name[64];
    char dir[16];
    int nenv;
    DreamEnv *env;
    int nspawn;
    DreamSpawn *spawn;
    int ntrig;
    DreamTrigger *trig;
    DreamGraph graph;
    /* entity scene */
    char *strings;
    uint32_t nstrings;
    int nnode;
    SceneNodeDef *node;
    int ncomp;
    SceneCompDef *comp;
    int nmodel;
    Model *model;
    float brightness, vci;
    int cull;
    int nbatch;
    DreamBatch *batch;
    int ngroup;
    DreamGroup *group;
    int *tlist;
    int nwater;
    WaterMat water[WATER_MAX];
    int nwtex;
    GLuint *wtex;
    int ntri;
    ColTri *tri;
    int nenvfx;
    EnvFx *envfx;
    int nfchunk;
    FootChunk *fchunk;
    uint16_t *tri_chunk; /* LBD chunk of each collision triangle (0xFFFF none) */
    float lo[3], hi[3];
    GLuint tex[TEXSETS];
    /* collision grid over XZ */
    float gx0, gz0, cell;
    int gw, gh;
    int *cell_start; /* gw*gh+1 */
    int *cell_tris;
    uint32_t vram_bytes;
} Dream;

int dream_load(Dream *d, const char *dir, int texset); /* dir = "stg00" ... inside the data pack */
int dream_load_texset(Dream *d, int texset);            /* load another texture set on demand */
int dream_load_entities(Dream *d, const char *dir);     /* entities.c */
void dream_free_entities(Dream *d);
int dream_load_footsteps(Dream *d, const char *dir);   /* entities.c */
int dream_load_envfx(Dream *d, const char *dir);       /* entities.c */
int models_load(const char *path, Model **out, int *n, uint32_t *vram_bytes); /* models.bin format */
void models_free(Model *models, int n);
static inline const char *dream_str(const Dream *d, uint32_t off) { return off < d->nstrings ? d->strings + off : ""; }
void dream_free(Dream *d);

/* the dream journal (OriginalDreams) */
enum { JD_LINKABLE = 1, JD_FIRST_DAY = 2, JD_GREY_MAN = 4, JD_ENV_EFFECTS = 8 };

typedef struct {
    char dir[16];
    char name[64];
    uint32_t flags;
    int env_offset;
} JournalDream;

#define JOURNAL_MAX 32
#define GRAPH_SIZE 19

typedef struct {
    int n;
    JournalDream d[JOURNAL_MAX];
    int8_t graph[GRAPH_SIZE * GRAPH_SIZE]; /* [(upper + 9) * 19 + dynamic + 9] = journal index or -1 */
} Journal;

int journal_load(Journal *j);

static inline void affine3x4(const float *m, vec3 p, float out[3])
{
    out[0] = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    out[1] = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    out[2] = m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11];
}

#endif
