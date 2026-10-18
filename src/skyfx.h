/* LSDR's DreamEnvironmentEffects: suns (with sunburst rays), clouds and starfields placed around
 * the camera when a dream's environment is applied, drawn behind everything (the effects camera). */
#ifndef LSD_SKYFX_H
#define LSD_SKYFX_H

#include "dream.h"
#include "xform.h"

enum { SKY_TEX_SUN, SKY_TEX_CLOUDS, SKY_TEX_STARS, SKY_NTEX };

typedef struct {
    int sun;         /* 1: LSDR/Sun billboard, 0: LSDR/Background quad */
    int tex;         /* SKY_TEX_* */
    float tint[4];
    aff world;       /* quads */
    vec3 pos;        /* billboards: centre */
    float sx, sy, angle;
    float key;       /* draw order */
} SkyDraw;

void skyfx_clear(void);                 /* ClearEffects */
void skyfx_day_start(void);             /* the effects object's Start: a random yaw for the day */
void skyfx_create(const EnvFx *e);      /* DreamEnvironment.Apply, effects part */
void skyfx_update(float dt);            /* Sun (sunburst) and Cloud updates */
int skyfx_draws(vec3 eye, SkyDraw *out, int max); /* sorted in draw order */
int skyfx_count(int *suns, int *clouds, int *stars);
void skyfx_test(void);                  /* test: everything at once */

#endif
