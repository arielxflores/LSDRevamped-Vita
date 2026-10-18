/* The dream's entity scene at runtime: GameObject hierarchy, transforms, activity,
 * MOM animation (Unity Animator semantics) and the per-frame draw list. */
#ifndef LSD_SCENE_H
#define LSD_SCENE_H

#include "dream.h"
#include "xform.h"

/* special node ids for the player and its camera (not part of the dream prefab) */
#define NODE_PLAYER (-2)
#define NODE_CAMERA (-3)

typedef struct {
    int comp;             /* AnimatedObject component */
    int node;
    int model;            /* index into dream models, -1 = none */
    int nclips;
    int clip[8];          /* model clip index for AnimatedObject.Clips[i] */
    int autoplay;
    int enabled;          /* Animator.enabled */
    int state;            /* -1 = idle, else AnimatedObject clip slot */
    float time;
    /* bindings: for each model clip, track -> node (-1 unbound) */
    int **bind;
    /* nodes animated by any clip, with their default (bind) pose */
    int nbound;
    int *bound;
    vec3 *def_pos, *def_scale;
    quat *def_rot;
} SceneAnimator;

typedef struct {
    int n;
    vec3 *pos, *scale;
    quat *rot;
    uint8_t *active;      /* activeSelf */
    uint8_t *aih;         /* activeInHierarchy */
    uint8_t *awoken;      /* Awake has run (was active at some point) */
    int *parent, *first_child, *next_sibling;
    char **name;          /* GameObject name (entities take their ID) */
    aff *world;
    uint32_t *world_ver;
    uint32_t ver;         /* bumped on any transform change */
    /* components */
    int ncomp;
    uint8_t *comp_init;   /* BaseEntity.Init has run */
    uint8_t *comp_started;
    int *comp_first;      /* per node: first component index (components are sorted by node) */
    int *comp_count;
    SceneAnimator *anim;
    int nanim;
    int *comp_anim;       /* component -> animator index */
    /* entity index (ID -> node), registration order */
    int nent;
    char **ent_id;
    int *ent_node;
    float time;           /* Time.time since the dream loaded */
    int loaded;
} Scene;

extern Scene g_scene;

int scene_load(const Dream *d);
void scene_free(void);
void scene_init_entities(void);         /* EntityIndex.AllRegistered: Init() (before the player spawns) */
void scene_start_components(void);      /* Start() of the active components */
void scene_update(float dt);            /* entity Updates, then animation */

int scene_find(const char *id);         /* EntityIndex.Get: node, or -1 */
int scene_child_by_name(int node, const char *name);
const aff *scene_world(int node);
vec3 scene_world_pos(int node);
quat scene_world_rot(int node);
void scene_set_world_pos(int node, vec3 p);
void scene_set_world_rot(int node, quat q);
void scene_set_local_pos(int node, vec3 p);
void scene_set_local_rot(int node, quat q);
void scene_set_scale(int node, vec3 s);
void scene_set_active(int node, int active);
int scene_active(int node);             /* activeInHierarchy */
int scene_comp_on(int node, int type);  /* first component of a type on a node, -1 */

void anim_play(int comp, int index);
void anim_stop(int comp);
void anim_resume(int comp);
float anim_length(int comp, int index);

/* draw list: visible entity meshes this frame */
typedef struct {
    const DreamBatch *batch;
    aff world;
    float vci;            /* material vertex colour intensity override, 0 = the dream's */
    int cull;             /* material cull override, -1 = the dream's */
} EntityDraw;
EntityDraw *scene_add_draw(void);
int scene_draw_list(const EntityDraw **out);
void scene_build_draws(vec3 eye, float max_dist);

#endif
