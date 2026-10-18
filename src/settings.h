/* LSDR's GameSettings / SettingsSystem / SettingsProfile: the options, their profiles, and where
 * they are kept (ux0:data/lsdr/settings.txt). */
#ifndef LSD_SETTINGS_H
#define LSD_SETTINGS_H

#include "common.h"

#define MAX_PROFILES 8

typedef struct {
    char name[32];
    int classic, pixelate, dither, limit_fps, long_draw;
    float fov, affine;
} SettingsProfile;

typedef struct {
    /* gameplay */
    int head_bob, smooth_head_bob;
    float head_bob_intensity;
    int footsteps, special_days;
    int mod, journal;
    /* controls */
    int scheme; /* CurrentControlSchemeIndex: in controls.c's list (sorted by name) */
    /* video (the profile's part) */
    int classic, pixelate, dither, limit_fps, long_draw;
    float fov, affine;
    /* audio */
    int original_soundtrack;
    float music_volume, sfx_volume;
    /* profiles */
    SettingsProfile prof[MAX_PROFILES];
    int nprof, cur_prof, match;
} GameSettings;

extern GameSettings S;

void settings_init(int use_file);       /* defaults, then the saved file (0: test runs, never loaded or saved); applies the current profile */
void settings_save(void);               /* SettingsSystem.Save */
void settings_apply(void);              /* SettingsSystem.Apply: the current profile takes the settings */
void settings_push(void);               /* hand the settings to the audio side (volumes, soundtrack) */
void settings_next_profile(void);       /* SettingsSystem.SwitchToNextProfile */
void settings_invalidate_profile(void); /* a profile setting changed in the options ('*') */
void settings_new_profile(void);
void settings_delete_profile(void);
void settings_revert_profile(void);
const char *settings_profile_label(void); /* the profile's name, '*' when changed */
int settings_command(const char *cmd);    /* test commands; 1 if handled */

#endif
