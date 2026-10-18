#ifndef LSD_COLLIDE_H
#define LSD_COLLIDE_H

#include "dream.h"

/* LSDR player CharacterController (Player.prefab) */
#define PLAYER_RADIUS 0.08f
#define PLAYER_HEIGHT 0.40f
#define PLAYER_STEP 0.20f
#define PLAYER_SKIN 0.01f
#define PLAYER_SLOPE_DEG 48.0f

enum { COL_SIDES = 1, COL_BELOW = 2 };

/* Highest walkable surface under the player's footprint, searching from
 * feet.y + max_up downwards to feet.y - max_down. Returns 1 if found. */
int col_ground(const Dream *d, vec3 feet, float max_up, float max_down, float *out_y);

/* Would a player standing with feet at p overlap a wall? */
int col_wall(const Dream *d, vec3 p);

/* Move the player like Unity's CharacterController.Move, returning COL_* flags.
 * Horizontal blocking stops the move (LSD never slides along walls). */
int col_move(const Dream *d, vec3 *feet, vec3 motion);

/* Physics.Raycast straight down from p: highest surface hit (any slope) */
int col_ray_down(const Dream *d, vec3 p, float *out_y);
int col_ray_down_tri(const Dream *d, vec3 p, float max_down, float *out_y, int *out_tri);

/* Anything under the player at all (PlayerFallDetector)? */
int col_anything_below(const Dream *d, vec3 feet, float start_up, float radius);

#endif
