/* State shared by the player controller (game.c) and LSDR's dream system (dreamsys.c). */
#ifndef LSD_WORLD_H
#define LSD_WORLD_H

#include "collide.h"
#include "render.h"

typedef struct {
    vec3 feet;
    float yaw;          /* degrees, Unity convention (0 = +Z, 90 = +X) */
    float look;         /* degrees, positive = up */
    float behind;       /* look-behind rotation, degrees */
    float interact_t;
    int stepping;
    float step_t;
    float step_in_x, step_in_y;
    int step_sounded;      /* this step's footstep played */
    int sprinting;
    int moving_into_wall;
    float colliding_t;
    int grounded;
    float bob;
    int can_link;           /* PlayerMovement.CanLink */
    float in_x, in_y;       /* PlayerMovement._inputDir */
    int input_locked;
    float locked_x, locked_y;
    int fallen;             /* PlayerFallDetector._hasFallen */
} Player;

extern Player P;
extern Dream g_dream;
extern int g_dream_loaded;

/* game.c */
void player_on_new_dream(void);
void player_reset_view(void);

/* ---------------------------------------------------------------- dream system */
typedef struct {
    long n;
    long sum_dyn, sum_up;
    int last_dyn, last_up;
} ContributionList;

typedef enum {
    FADE_DONE_NONE,
    FADE_DONE_LINK,       /* link fade-in finished: maybe switch textures, load the dream */
    FADE_DONE_LOADED,     /* fade-out after loading finished: CanLink, no longer transitioning */
    FADE_DONE_DAY_END,    /* day-end fade finished: back to the "title" */
    FADE_DONE_BEGIN,      /* new day fade to black finished: load the day's dream */
    FADE_DONE_VIDEO,      /* MovieClip fade finished: the video_dream scene */
    FADE_DONE_VIDEO_START,/* video_dream's fade from black finished: the clip starts */
    FADE_DONE_SPECIAL,    /* a special day's fade to black finished: its text or video */
    FADE_DONE_TITLE,      /* the title screen's view-switch fade to black finished */
    FADE_DONE_GREY_FLASH, /* the grey man's white flash: hide him, fade back out */
    FADE_DONE_GREY_GONE,
    FADE_DONE_FLASH_BEGIN, /* BeginFlashback's fade to black finished: the first past event */
    FADE_DONE_FLASH_NEXT,  /* a flashback's white fade finished: the next past event */
    FADE_DONE_INTRO_END,   /* the intro's fade to black finished: the title screen */
} FadeDone;

typedef enum { DS_TITLE, DS_DREAM, DS_SPECIAL } DsMode; /* DS_SPECIAL: text_dream / video_dream */

typedef struct {
    Journal journal;
    DsMode mode;
    int cur;                  /* journal index of the current dream, -1 = none */
    int day, year;
    int last_gx, last_gy;     /* graph position of the last finished day */
    int texset;
    int env;                  /* index into g_dream.env */
    /* the current DreamSequence */
    int in_sequence;
    ContributionList area, entity;
    /* DreamSystem */
    int can_transition, transitioning, ending;
    int can_control, gravity;
    int can_pause;            /* PauseSystem.CanPause: from a dream's load until a link or the day's end */
    float timer;              /* seconds left before the day ends (<0 = off) */
    int next_dream;           /* _nextDream from a TriggerLink with AffectsLinksWithin, -1 = none */
    uint32_t forced_spawn;
    float last_yaw;
    int spawned_once;
    /* ToriiFader */
    float fade_a, fade_from, fade_to, fade_t, fade_dur;
    float fade_rgb[3];
    int fading;
    FadeDone fade_done;
    /* pending dream load */
    int load_dream, load_transitioning, load_stage, load_spawn_index;
    /* LBDGraphContributor */
    float graph_t;
    int last_lbd;
    /* "title" between days */
    float title_t;
    /* options */
    int no_wall_link;         /* bench: never link by walking into walls */
    int no_triggers;          /* bench: ignore link triggers */
    int no_fall;              /* bench: teleports may leave the walkable area */
    int save_enabled;
    double load_ms;
    int playing_video;
    /* text_dream / video_dream */
    int special_kind;         /* 0 text, 1 video, 2 the intro (intro.unity) */
    int special_over;         /* the day's end has begun */
    int special_force;        /* test: the choice for the next special day (-1 random) */
    char video_name[24];
    GLuint text_tex;
    int first_load_of_day;
    /* the journal's finished days (DreamSequence graph positions, for the graph screen) */
    int nhist;
    int8_t hist_x[4096], hist_y[4096];
    float stretch_from, stretch_to, stretch_t, stretch_dur, stretch_y;
    /* flashbacks (DreamSystem.BeginFlashback): past entity graph contributions, revisited */
    int in_flashback;
    float flash_t, flash_len;     /* time in this one, how long it lasts (8-16 s) */
    int flash_spawn;              /* the next load puts the player at the event */
    vec3 flash_pos;
    float flash_yaw;
    char flash_entity[48];        /* the first event's entity, shown after each load */
    int flash_disabled_days;      /* DisableFlashbackForDays */
} DreamSys;

extern DreamSys DS;

int ds_init(int script_mode);
void ds_update(const Input *in, float dt);   /* before the player moves */
void ds_after_player(float dt);              /* after the player moved: triggers, graph, falling */
void ds_transition(int dream, const float *rgb, int sound, int lock, uint32_t spawn_id);
void ds_wall_link(void);
int ds_end_dream(int from_fall);
void ds_force_end_dream(void);
int ds_can_flashback(void);                  /* DreamSystem.CanFlashback */
void ds_begin_flashback(void);               /* the title's FLASHBACK */
void ds_reset_progress(void);                /* JournalSaveData.ResetProgress (and save) */               /* the pause menu's MAIN MENU: back to the title, the day not counted */
const DreamEnv *ds_env(void);
void ds_graph_position(int *x, int *y);
int ds_command(const char *cmd);              /* 1 if handled */
int ds_day(void);
void ds_log_entity(int dynamic, int upper, int node); /* LogGraphContributionFromEntity (node: the entity, -1) */
void ds_disable_flashback(int days);         /* DreamSystem.DisableFlashbackForDays */
void ds_log_area(int dynamic, int upper);     /* DreamSystem.LogGraphContributionFromArea */
int ds_fade_try(float target, float secs, const float *rgb, FadeDone done); /* ToriiFader, forced: false */
void ds_play_video(const char *clip, const float rgb[3]); /* DreamSystem.PlayVideo */
void ds_stretch(float amount, float seconds); /* DreamSystem.StretchDream */
float ds_stretch_y(void);
/* StretchDream scales the dream instance (the scene root, otherwise identity) along Y: the LBD
 * collision geometry, trigger boxes and graph tiles live in its space, the player in world space */
static inline vec3 ds_to_dream(vec3 p)
{
    float k = ds_stretch_y();
    if (k != 1.0f && k > 1e-4f) p.y /= k;
    return p;
}
static inline vec3 ds_from_dream(vec3 p)
{
    float k = ds_stretch_y();
    if (k != 1.0f && k > 1e-4f) p.y *= k;
    return p;
}
int ds_col_move(vec3 *feet, vec3 motion);
const char *ds_texset_name(int t);
GLuint ds_special_picture(int *w, int *h);    /* the text or video screen in DS_SPECIAL (0 = black) */
void ds_fade_title(float target, float secs); /* the title screen's black view-switch fades */
void ds_start_day(void);                      /* the title's START: DreamSystem.BeginDream */

#endif
