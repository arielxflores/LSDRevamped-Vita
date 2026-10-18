/* LSDR's GameSettings, SettingsProfile and SettingsSystem (Load, Save, Apply, profiles) */
#include "settings.h"
#include "controls.h"
#include "audio.h"

GameSettings S;

static void profile_from(SettingsProfile *p) /* SettingsProfile.ApplyFrom */
{
    p->classic = S.classic;
    p->pixelate = S.pixelate;
    p->dither = S.dither;
    p->limit_fps = S.limit_fps;
    p->fov = S.fov;
    p->affine = S.affine;
    p->long_draw = S.long_draw;
}

static void profile_to(const SettingsProfile *p) /* SettingsProfile.ApplyTo */
{
    S.classic = p->classic;
    S.pixelate = p->pixelate;
    S.dither = p->dither;
    S.limit_fps = p->limit_fps;
    S.fov = p->fov;
    S.affine = p->affine;
    S.long_draw = p->long_draw;
}

static void apply_current_profile(void) /* GameSettings.ApplyCurrentProfile */
{
    if (S.cur_prof >= S.nprof || S.cur_prof < 0)
        S.cur_prof = 0;
    profile_to(&S.prof[S.cur_prof]);
    S.match = 1;
}

static void default_profiles(void) /* SettingsProfile.CreateDefaultProfiles */
{
    static const SettingsProfile DEF[2] = {
        {"Classic", 1, 1, 1, 0, 0, 50.0f, 0.4f},
        {"Revamped", 0, 0, 0, 0, 0, 50.0f, 0.4f},
    };
    memcpy(S.prof, DEF, sizeof(DEF));
    S.nprof = 2;
}

static void defaults(void) /* the GameSettings constructor */
{
    memset(&S, 0, sizeof(S));
    S.head_bob = 1;
    S.head_bob_intensity = 1.0f;
    S.smooth_head_bob = 0;
    S.scheme = 0; /* (Classic: first by name) */
    S.classic = 1;
    S.pixelate = 1;
    S.fov = 60.0f;
    S.affine = 0.5f;
    S.footsteps = 1;
    S.music_volume = 1.0f;
    S.sfx_volume = 1.0f;
    S.original_soundtrack = 1;
    S.special_days = 1;
    S.match = 1;
}

static char g_path[256];
static int g_use_file = 1; /* 0 for test runs: the defaults, and nothing saved */

static void load(void)
{
    FILE *f = fopen(g_path, "r");
    if (!f) {
        lsd_log("settings: none saved yet (%s), the defaults", g_path);
        return;
    }
    char line[256];
    int nprof = 0;
    SettingsProfile prof[MAX_PROFILES];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char key[48];
        float v;
        if (!strncmp(line, "profile ", 8)) {
            SettingsProfile p;
            memset(&p, 0, sizeof(p));
            char *bar = strchr(line + 8, '|');
            if (!bar || nprof >= MAX_PROFILES)
                continue;
            *bar = 0;
            snprintf(p.name, sizeof(p.name), "%.31s", line + 8);
            if (sscanf(bar + 1, "%d %d %d %d %f %f %d", &p.classic, &p.pixelate, &p.dither, &p.limit_fps, &p.fov,
                       &p.affine, &p.long_draw) == 7)
                prof[nprof++] = p;
            continue;
        }
        if (sscanf(line, "%47s %f", key, &v) != 2)
            continue;
        int i = (int)v;
#define KI(k, field) else if (!strcmp(key, k)) S.field = i
#define KF(k, field) else if (!strcmp(key, k)) S.field = v
        if (0) {
        }
        KI("HeadBobEnabled", head_bob);
        KI("SmoothHeadBob", smooth_head_bob);
        KF("HeadBobIntensity", head_bob_intensity);
        KI("EnableFootstepSounds", footsteps);
        KI("SpecialDaysEnabled", special_days);
        KI("CurrentModIndex", mod);
        KI("CurrentJournalIndex", journal);
        KI("CurrentControlSchemeIndex", scheme);
        KI("UseClassicShaders", classic);
        KI("UsePixelationShader", pixelate);
        KI("UseDithering", dither);
        KI("LimitFramerate", limit_fps);
        KI("LongDrawDistance", long_draw);
        KF("FOV", fov);
        KF("AffineIntensity", affine);
        KI("UseOriginalSoundtrack", original_soundtrack);
        KF("MusicVolume", music_volume);
        KF("SFXVolume", sfx_volume);
        KI("CurrentProfileIndex", cur_prof);
        KI("SettingsMatchProfile", match);
#undef KI
#undef KF
    }
    fclose(f);
    if (nprof > 0) {
        memcpy(S.prof, prof, sizeof(SettingsProfile) * (size_t)nprof);
        S.nprof = nprof;
    }
    if (S.scheme < 0 || S.scheme >= controls_count()) S.scheme = 0;
    if (S.music_volume < 0 || S.music_volume > 1) S.music_volume = 1;
    if (S.sfx_volume < 0 || S.sfx_volume > 1) S.sfx_volume = 1;
    if (!(S.fov >= 30 && S.fov <= 110)) S.fov = 50;
    lsd_log("settings: loaded %s", g_path);
}

void settings_save(void)
{
    if (!g_use_file) {
        lsd_log("settings: (test run: not saved)");
        return;
    }
    char tmp[272];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        lsd_log("settings: cannot write %s", tmp);
        return;
    }
    fprintf(f, "HeadBobEnabled %d\nSmoothHeadBob %d\nHeadBobIntensity %g\nEnableFootstepSounds %d\n"
               "SpecialDaysEnabled %d\nCurrentModIndex %d\nCurrentJournalIndex %d\nCurrentControlSchemeIndex %d\n"
               "UseClassicShaders %d\nUsePixelationShader %d\nUseDithering %d\nLimitFramerate %d\n"
               "LongDrawDistance %d\nFOV %g\nAffineIntensity %g\nUseOriginalSoundtrack %d\nMusicVolume %g\n"
               "SFXVolume %g\nCurrentProfileIndex %d\nSettingsMatchProfile %d\n",
            S.head_bob, S.smooth_head_bob, S.head_bob_intensity, S.footsteps, S.special_days, S.mod, S.journal,
            S.scheme, S.classic, S.pixelate, S.dither, S.limit_fps, S.long_draw, S.fov, S.affine,
            S.original_soundtrack, S.music_volume, S.sfx_volume, S.cur_prof, S.match);
    for (int i = 0; i < S.nprof; i++) {
        const SettingsProfile *p = &S.prof[i];
        fprintf(f, "profile %s|%d %d %d %d %g %g %d\n", p->name, p->classic, p->pixelate, p->dither, p->limit_fps,
                p->fov, p->affine, p->long_draw);
    }
    int ok = fclose(f) == 0;
    remove(g_path);
    if (!ok || rename(tmp, g_path) != 0)
        lsd_log("settings: saving %s failed", g_path);
    else
        lsd_log("settings: saved");
}

void settings_push(void)
{
    audio_set_volumes(S.music_volume, S.sfx_volume);
    music_use_original_songs(S.original_soundtrack);
}

void settings_init(int use_file)
{
    snprintf(g_path, sizeof(g_path), "%s/settings.txt", plat_write_dir());
    g_use_file = use_file;
    defaults();
    if (use_file)
        load();
    if (S.nprof == 0)
        default_profiles();
    apply_current_profile(); /* SettingsSystem.Load: ApplyCurrentProfile, then Apply */
    settings_push();
    lsd_log("settings: profile %s, %s controls, head bob %d (%.2f%s), footsteps %d, special days %d, music %.0f%%, "
            "sound %.0f%%, %s soundtrack",
            settings_profile_label(), controls_current()->name, S.head_bob, S.head_bob_intensity,
            S.smooth_head_bob ? ", smooth" : "", S.footsteps, S.special_days, S.music_volume * 100,
            S.sfx_volume * 100, S.original_soundtrack ? "original" : "Revamped");
}

void settings_apply(void)
{
    /* GameSettings.UpdateCurrentProfile: the profile takes what is set now */
    profile_from(&S.prof[S.cur_prof]);
    apply_current_profile();
    settings_push();
}

void settings_next_profile(void)
{
    if (S.match)
        S.cur_prof = (S.cur_prof + 1) % S.nprof;
    apply_current_profile();
    settings_apply();
    lsd_log("settings profile: %s", S.prof[S.cur_prof].name);
}

void settings_invalidate_profile(void) { S.match = 0; }

void settings_new_profile(void)
{
    if (S.nprof >= MAX_PROFILES)
        return;
    SettingsProfile *p = &S.prof[S.nprof];
    profile_from(p);
    snprintf(p->name, sizeof(p->name), "Profile %d", S.nprof);
    S.cur_prof = S.nprof++;
    apply_current_profile();
}

void settings_delete_profile(void)
{
    if (S.nprof <= 1)
        return; /* (cannot delete the last profile) */
    memmove(&S.prof[S.cur_prof], &S.prof[S.cur_prof + 1], sizeof(SettingsProfile) * (size_t)(S.nprof - S.cur_prof - 1));
    S.nprof--;
    if (S.cur_prof == S.nprof)
        S.cur_prof--;
    apply_current_profile();
    settings_apply();
}

void settings_revert_profile(void) { apply_current_profile(); }

const char *settings_profile_label(void)
{
    static char buf[40];
    snprintf(buf, sizeof(buf), "%s%s", S.prof[S.cur_prof].name, S.match ? "" : "*");
    return buf;
}

int settings_command(const char *cmd)
{
    char key[48];
    float v;
    if (!strcmp(cmd, "settings")) {
        lsd_log("settings: profile %s, %s controls, classic %d pixelate %d dither %d limit %d long %d fov %.0f "
                "affine %.2f, head bob %d %.2f smooth %d, footsteps %d, special days %d, music %.2f sound %.2f, "
                "original soundtrack %d",
                settings_profile_label(), controls_current()->name, S.classic, S.pixelate, S.dither, S.limit_fps,
                S.long_draw, S.fov, S.affine, S.head_bob, S.head_bob_intensity, S.smooth_head_bob, S.footsteps,
                S.special_days, S.music_volume, S.sfx_volume, S.original_soundtrack);
        return 1;
    }
    if (sscanf(cmd, "set %47s %f", key, &v) == 2) {
        int i = (int)v;
        if (!strcmp(key, "scheme")) controls_select(i);
        else if (!strcmp(key, "headbob")) S.head_bob = i;
        else if (!strcmp(key, "smoothbob")) S.smooth_head_bob = i;
        else if (!strcmp(key, "bobintensity")) S.head_bob_intensity = v;
        else if (!strcmp(key, "footsteps")) S.footsteps = i;
        else if (!strcmp(key, "specialdays")) S.special_days = i;
        else if (!strcmp(key, "classic")) S.classic = i;
        else if (!strcmp(key, "pixelate")) S.pixelate = i;
        else if (!strcmp(key, "dither")) S.dither = i;
        else if (!strcmp(key, "limitfps")) S.limit_fps = i;
        else if (!strcmp(key, "longdraw")) S.long_draw = i;
        else if (!strcmp(key, "fov")) S.fov = v;
        else if (!strcmp(key, "affine")) S.affine = v;
        else if (!strcmp(key, "original")) S.original_soundtrack = i;
        else if (!strcmp(key, "music")) S.music_volume = v;
        else if (!strcmp(key, "sfx")) S.sfx_volume = v;
        else return 0;
        settings_push();
        return 1;
    }
    return 0;
}
