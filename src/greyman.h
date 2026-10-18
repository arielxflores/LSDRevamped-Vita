#ifndef LSD_GREYMAN_H
#define LSD_GREYMAN_H

#include "common.h"

int greyman_init(void);
void greyman_shutdown(void);
void greyman_day_start(void);   /* first dream of a day loaded (the player object starts) */
void greyman_new_dream(void);   /* every dream load */
void greyman_day_end(void);
void greyman_update(float dt);
void greyman_draws(vec3 eye, float max_dist);
void greyman_flash_done(void);
void greyman_gone(void);
void greyman_clear(void);
int greyman_spawn(void);
int greyman_count(void);

#endif
