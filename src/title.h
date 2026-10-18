#ifndef LSD_TITLE_H
#define LSD_TITLE_H

#include "common.h"
#include "render.h"

void title_enter(int from_dream);  /* show the title screen (back from a dream: the graph first) */
int title_active(void);
void title_update(const Input *in, float dt);
void title_render(View *v, int w, int h);
void title_fade_done(void);        /* a view-switch fade finished */
void title_stop_music(void);
void title_preload(void); /* start reading the title screen's sounds (during the intro) */
void title_show(const char *view); /* test: a view of the title screen ("Settings", "Credits"...) */

/* the pause menu (PauseSystem + UIPauseMenu) */
int pause_active(void);
void pause_open(void);
void pause_close(void);
void pause_update(const Input *in, float dt);
void pause_render(int w, int h); /* over the dream */

int options_capturing(void);
void options_log_selected(void); /* test */ /* the keyboard or a rebinding (or its choice) is up: the pad is theirs */

#endif
