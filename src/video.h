#ifndef LSD_VIDEO_H
#define LSD_VIDEO_H

#include "common.h"

int video_start(const char *name);  /* movies/NAME.mpg; 0 if it is not available */
void video_update(float dt);        /* decode up to the playback time (its sound goes to the mixer) */
int video_active(void);
int video_showing(void);             /* a picture is on screen (black before the first) */
int video_ended(void);              /* played to the end */
void video_pause(void);             /* stop decoding; the last picture stays */
void video_stop(void);
void video_note_freeze(void);       /* test: the game stood still (a screenshot): holds not counted */
GLuint video_texture(int *w, int *h); /* the current frame (0 if none) */

#endif
