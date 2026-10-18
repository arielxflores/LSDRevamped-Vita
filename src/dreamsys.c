/* LSD Revamped's dream system, after DreamSystem.cs, DreamJournal.cs, DreamSequence.cs,
 * GameSaveData.cs, TriggerLink.cs, LBDGraphContributor.cs, PlayerFallDetector.cs,
 * TextureSetter.cs and ToriiFader.cs:
 *   - a day lasts 60..360 s, then fades to the colour of the day's graph position
 *   - walking into a wall for 0.7 s links to a random linkable dream (fade 1 s, load, fade out 1 s)
 *   - link triggers (tunnels, the picture/book/birds in the cottage, ...) link to set dreams/spawns
 *   - areas you walk through push the day's position on the dream graph; the next day's
 *     dream comes from the 19x19 graph spawn map
 *   - the texture set depends on the day number, and may change on each link (1 in 2) */
#include "world.h"
#include "luaapi.h"
#include "scene.h"
#include "greyman.h"
#include "audio.h"
#include "skyfx.h"
#include "video.h"
#include "title.h"
#include "settings.h"

#define MIN_SECONDS_IN_DREAM 60.0f
#define MAX_SECONDS_IN_DREAM 360.0f
#define FALLING_UPPER_PENALTY (-9)
#define FADE_OUT_SECS_REGULAR 3.0f
#define FADE_OUT_SECS_FALL 2.5f
#define FADE_OUT_SECS_FORCE 1.0f
#define LINK_FADE_SECS 1.0f
#define LOADED_FADE_SECS 1.0f
#define BEGIN_FADE_SECS 3.0f
#define GRAPH_UPDATE_INTERVAL 0.25f
#define FALL_LOOK_SPEED 45.0f
#define FALL_LOOK_MAX 80.0f

DreamSys DS;

static const char *TEXSET_NAMES[4] = {"Normal", "Kanji", "Downer", "Upper"};
const char *ds_texset_name(int t) { return TEXSET_NAMES[t & 3]; }

static void spawn_player_in_dream(int set_orientation);
static void spawn_at(const DreamSpawn *s, int set_orientation);
static void start_load(int dream, int transitioning);

/* the dream instance and everything living in it */
static void unload_dream(void)
{
    greyman_clear();
    luaapi_dream_end();
    scene_free();
    if (g_dream_loaded) {
        dream_free(&g_dream);
        g_dream_loaded = 0;
    }
}

int ds_day(void) { return DS.day; }

/* ---------------------------------------------------------------- save (day number + last graph position) */

static void save_path(char *out, size_t n) { snprintf(out, n, "%s/save.txt", plat_write_dir()); }

/* DreamSequence._entityGraphContributions of the finished days: where the player was (and facing)
 * when an entity contributed to the graph, in which dream; flashbacks revisit them */
typedef struct {
    int dream;
    float x, y, z, yaw;
    char entity[48];
} PastEvent;
#define MAX_EVENTS 20000
#define MAX_DAY_EVENTS 2000
static PastEvent *g_events, g_day_events[MAX_DAY_EVENTS];
static int g_nevents, g_nday_events;

static void events_add(const PastEvent *e)
{
    if (!g_events && !(g_events = (PastEvent *)malloc(sizeof(PastEvent) * MAX_EVENTS)))
        return;
    if (g_nevents == MAX_EVENTS) { /* (keep the newest) */
        memmove(g_events, g_events + 1, sizeof(PastEvent) * (MAX_EVENTS - 1));
        g_nevents--;
    }
    g_events[g_nevents++] = *e;
}

static void save_write(void)
{
    if (!DS.save_enabled)
        return;
    char p[512];
    save_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) {
        lsd_log("could not write %s", p);
        return;
    }
    fprintf(f, "day %d\nyear %d\ngraph %d %d\n", DS.day, DS.year, DS.last_gx, DS.last_gy);
    for (int i = 0; i < DS.nhist; i++)
        fprintf(f, "seq %d %d\n", DS.hist_x[i], DS.hist_y[i]);
    for (int i = 0; i < g_nevents; i++) {
        const PastEvent *e = &g_events[i];
        fprintf(f, "ev %d %.3f %.3f %.3f %.1f %s\n", e->dream, e->x, e->y, e->z, e->yaw, e->entity[0] ? e->entity : "-");
    }
    if (DS.flash_disabled_days > 0)
        fprintf(f, "noflashback %d\n", DS.flash_disabled_days);
    fclose(f);
}

static void save_read(void)
{
    char p[512], line[128];
    save_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        int a, b;
        if (sscanf(line, "day %d", &a) == 1 && a >= 1 && a <= 365) DS.day = a;
        else if (sscanf(line, "year %d", &a) == 1 && a >= 0) DS.year = a;
        else if (sscanf(line, "graph %d %d", &a, &b) == 2 && a >= -9 && a <= 9 && b >= -9 && b <= 9) {
            DS.last_gx = a;
            DS.last_gy = b;
        } else if (sscanf(line, "seq %d %d", &a, &b) == 2 && a >= -9 && a <= 9 && b >= -9 && b <= 9 &&
                   DS.nhist < (int)sizeof(DS.hist_x)) {
            DS.hist_x[DS.nhist] = (int8_t)a;
            DS.hist_y[DS.nhist] = (int8_t)b;
            DS.nhist++;
        } else if (!strncmp(line, "ev ", 3)) {
            PastEvent e;
            memset(&e, 0, sizeof(e));
            char id[48];
            if (sscanf(line, "ev %d %f %f %f %f %47s", &e.dream, &e.x, &e.y, &e.z, &e.yaw, id) == 6 && e.dream >= 0 &&
                e.dream < DS.journal.n) {
                if (strcmp(id, "-"))
                    snprintf(e.entity, sizeof(e.entity), "%s", id);
                events_add(&e);
            }
        } else if (sscanf(line, "noflashback %d", &a) == 1 && a > 0) {
            DS.flash_disabled_days = a;
        }
    }
    fclose(f);
    lsd_log("save: day %d, year %d, last graph position %d,%d, %d days on the graph, %d past events", DS.day,
            DS.year, DS.last_gx, DS.last_gy, DS.nhist, g_nevents);
}

/* ---------------------------------------------------------------- DreamSequence graph maths */

static void contrib_add(ContributionList *l, int dyn, int up)
{
    l->n++;
    l->sum_dyn += dyn;
    l->sum_up += up;
    l->last_dyn = dyn;
    l->last_up = up;
}

static int eval_component(int v)
{
    if (v <= -6) return -2;
    if (v <= -3) return -1;
    if (v <= 2) return 0;
    if (v <= 5) return 1;
    return 2;
}

static void eval_list(const ContributionList *l, int *x, int *y)
{
    if (l->n == 0) {
        *x = *y = 0;
        return;
    }
    /* (int)Average truncates toward zero, as C does */
    *x = (int)((double)l->sum_dyn / (double)l->n) + eval_component(l->last_dyn);
    *y = (int)((double)l->sum_up / (double)l->n) + eval_component(l->last_up);
}

static int wrap_graph(int c)
{
    if (c < -9) return 9;
    if (c > 9) return -9;
    return c;
}

void ds_graph_position(int *x, int *y)
{
    int ax, ay, ex, ey;
    eval_list(&DS.area, &ax, &ay);
    eval_list(&DS.entity, &ex, &ey);
    *x = wrap_graph((ax + ex) / 2);
    *y = wrap_graph((ay + ey) / 2);
}

static void log_area(int dyn, int up)
{
    if (!DS.in_sequence)
        return;
    contrib_add(&DS.area, dyn, up);
}

void ds_log_area(int dyn, int up) { log_area(dyn, up); }

/* DreamSystem.fadeColorFromCurrentSequence */
static void fade_colour_from_sequence(float rgb[3])
{
    static const float WHITE[3] = {1, 1, 1}, BLACK[3] = {0, 0, 0}, GREEN[3] = {0, 1, 0}, BLUE[3] = {0, 0, 1},
                       MAGENTA[3] = {1, 0, 1}, YELLOW[3] = {1, 0.921568632f, 0.0156862754f}, RED[3] = {1, 0, 0};
    const float *c = WHITE;
    if (DS.cur < 0) {
        c = BLACK;
    } else if (!DS.in_sequence) {
        rgb[0] = rng_float(0, 1);
        rgb[1] = rng_float(0, 1);
        rgb[2] = rng_float(0, 1);
        return;
    } else {
        int x, y;
        ds_graph_position(&x, &y);
        if (x <= -4 && y >= 1) c = GREEN;
        else if (x <= 3 && y >= 4) c = BLUE;
        else if (x >= 4 && y >= 4) c = MAGENTA;
        else if (x >= 4 && y >= -3) c = BLACK;
        else if (x <= -4 && y <= -4) c = YELLOW;
        else if (x <= 3 && y <= -4) c = RED;
    }
    memcpy(rgb, c, sizeof(float) * 3);
}

/* ---------------------------------------------------------------- ToriiFader */

static void run_fade_done(FadeDone what);

static void fade(float target, float secs, const float *rgb, FadeDone done)
{
    /* a fade already running is cut short and its callback runs first (forced fades) */
    for (int guard = 0; DS.fading && guard < 4; guard++) {
        /* (a callback may start a fade of its own, e.g. the Grey Man's flash -> fade-out:
         * that one is cut short too, so no callback is ever lost) */
        FadeDone prev = DS.fade_done;
        DS.fading = 0;
        DS.fade_done = FADE_DONE_NONE;
        run_fade_done(prev);
    }
    if (rgb)
        memcpy(DS.fade_rgb, rgb, sizeof(DS.fade_rgb));
    DS.fade_from = DS.fade_a;
    DS.fade_to = target;
    DS.fade_t = 0;
    DS.fade_dur = secs;
    DS.fade_done = done;
    DS.fading = 1;
}

int ds_fade_try(float target, float secs, const float *rgb, FadeDone done)
{
    if (DS.fading)
        return 0;
    fade(target, secs, rgb, done);
    return 1;
}

static void fade_update(float dt)
{
    if (!DS.fading)
        return;
    DS.fade_t += dt;
    float b = DS.fade_dur > 0 ? DS.fade_t / DS.fade_dur : 1.0f;
    if (b > 1) b = 1;
    DS.fade_a = DS.fade_from + (DS.fade_to - DS.fade_from) * b;
    if (DS.fade_t >= DS.fade_dur) {
        DS.fade_a = DS.fade_to;
        DS.fading = 0;
        FadeDone d = DS.fade_done;
        DS.fade_done = FADE_DONE_NONE;
        run_fade_done(d);
    }
}

/* ---------------------------------------------------------------- journal */

static int get_linkable(int current)
{
    int cand[JOURNAL_MAX], n = 0, nlink = 0, only = -1;
    for (int i = 0; i < DS.journal.n; i++)
        if (DS.journal.d[i].flags & JD_LINKABLE) {
            nlink++;
            only = i;
            if (i != current)
                cand[n++] = i;
        }
    if (nlink == 1)
        return only;
    if (n == 0)
        return current >= 0 ? current : 0;
    return cand[rng_int(n)];
}

static int get_first_dream(void)
{
    int cand[JOURNAL_MAX], n = 0;
    for (int i = 0; i < DS.journal.n; i++)
        if (DS.journal.d[i].flags & JD_FIRST_DAY)
            cand[n++] = i;
    if (!n)
        return rng_int(DS.journal.n);
    return cand[rng_int(n)];
}

static int get_dream_from_graph(int x, int y)
{
    int gx = x + 9, gy = y + 9;
    if (gx >= 0 && gx < GRAPH_SIZE && gy >= 0 && gy < GRAPH_SIZE) {
        int d = DS.journal.graph[gy * GRAPH_SIZE + gx];
        if (d >= 0)
            return d;
    }
    return rng_int(DS.journal.n);
}

/* TextureSetter.SetRandomTextureSetFromDayNumber */
static int texset_from_day(int day)
{
    int d = day % 41;
    if (d <= 10) return 0;
    if (d <= 20) return rng_int(2);
    if (d <= 30) return rng_int(3);
    if (d <= 40) return rng_int(4);
    return 0;
}

/* Dream.ChooseEnvironment */
static int choose_environment(void)
{
    if (DS.cur < 0 || g_dream.nenv <= 0)
        return 0;
    int n = g_dream.nenv;
    int i = (DS.day - 1 + DS.journal.d[DS.cur].env_offset) % n;
    return i < 0 ? i + n : i;
}

const DreamEnv *ds_env(void)
{
    static DreamEnv fallback = {{1, 1, 1}, 0, 10, 30, 0.24f, 0.24f, {0, 1, 1}, {1, 1, 1}};
    if (!g_dream_loaded || g_dream.nenv <= 0)
        return &fallback;
    int i = DS.env;
    if (i < 0 || i >= g_dream.nenv) i = 0;
    return &g_dream.env[i];
}

/* ---------------------------------------------------------------- days */

static void end_timer_off(void) { DS.timer = -1; }

/* ---------------------------------------------------------------- special days */

static void go_to_title(void);

#define SP_MAX_DAYS 128
#define SP_MAX_CHOICES 8
typedef struct {
    int day, always, dyn, up, n;
    int kind[SP_MAX_CHOICES], idx[SP_MAX_CHOICES];
} SpecialDay;

static struct {
    int n, nvideo, ntext;
    SpecialDay day[SP_MAX_DAYS];
    char video[64][16];
} SP;

/* specialdays.bin (tools/specialdays.py): DreamJournal.SpecialDays */
static void special_load(void)
{
    memset(&SP, 0, sizeof(SP));
    size_t size = 0;
    uint8_t *buf = vfs_read("specialdays.bin", &size);
    if (!buf)
        return;
    const uint8_t *p = buf, *end = buf + size;
#define SP_U32(v) do { if (end - p < 4) goto bad; memcpy(&(v), p, 4); p += 4; } while (0)
    uint32_t ver, n, k;
    if (size < 8 || memcmp(p, "LSDS", 4) != 0)
        goto bad;
    p += 4;
    SP_U32(ver);
    SP_U32(n);
    if (ver != 1 || n > SP_MAX_DAYS)
        goto bad;
    for (uint32_t i = 0; i < n; i++) {
        SpecialDay *d = &SP.day[i];
        SP_U32(d->day);
        SP_U32(d->always);
        SP_U32(d->dyn);
        SP_U32(d->up);
        SP_U32(k);
        if (k == 0 || k > SP_MAX_CHOICES)
            goto bad;
        d->n = (int)k;
        for (uint32_t c = 0; c < k; c++) {
            SP_U32(d->kind[c]);
            SP_U32(d->idx[c]);
        }
    }
    SP_U32(k);
    if (k > 64 || end - p < (long)k * 16)
        goto bad;
    for (uint32_t i = 0; i < k; i++, p += 16) {
        memcpy(SP.video[i], p, 15);
        SP.video[i][15] = 0;
    }
    SP.nvideo = (int)k;
    SP_U32(k);
    SP.ntext = (int)k;
    for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < SP.day[i].n; c++)
            if (SP.day[i].idx[c] < 0 || SP.day[i].idx[c] >= (SP.day[i].kind[c] ? SP.nvideo : SP.ntext))
                goto bad;
    SP.n = (int)n;
#undef SP_U32
    free(buf);
    lsd_log("special days: %d (%d videos, %d text screens)", SP.n, SP.nvideo, SP.ntext);
    return;
bad:
    free(buf);
    memset(&SP, 0, sizeof(SP));
    lsd_log("specialdays.bin is damaged: no special days");
}

/* DreamJournal.HasSpecialDay */
static const SpecialDay *special_for_day(int day)
{
    for (int i = 0; i < SP.n; i++)
        if (SP.day[i].day == day)
            return &SP.day[i];
    return NULL;
}

static GLuint load_text_screen(int idx)
{
    char path[48];
    snprintf(path, sizeof(path), "special/text%02d.bin", idx);
    size_t size = 0;
    uint8_t *buf = vfs_read(path, &size);
    if (!buf)
        return 0;
    uint32_t w = 0, h = 0;
    if (size >= 8) {
        memcpy(&w, buf, 4);
        memcpy(&h, buf + 4, 4);
    }
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || size != 8 + (size_t)w * h) {
        lsd_log("%s is damaged", path);
        free(buf);
        return 0;
    }
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!rgba) {
        free(buf);
        return 0;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t c = buf[8 + i]; /* white text on black */
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = c;
        rgba[i * 4 + 3] = 255;
    }
    free(buf);
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(rgba);
    return t;
}

static void special_clear(void)
{
    video_stop();
    if (DS.text_tex) {
        glDeleteTextures(1, &DS.text_tex);
        DS.text_tex = 0;
    }
    DS.special_over = 0;
}

/* the video_dream scene: fade from black, then VideoSpecialDayControl.BeginVideoClip */
static void enter_video_scene(const char *name)
{
    static const float BLACK[3] = {0, 0, 0};
    char nm[sizeof(DS.video_name)];
    snprintf(nm, sizeof(nm), "%s", name); /* (name may be DS.video_name itself) */
    special_clear();
    unload_dream(); /* SceneManager.LoadScene("video_dream") */
    DS.mode = DS_SPECIAL;
    DS.special_kind = 1;
    memcpy(DS.video_name, nm, sizeof(nm));
    fade(0.0f, 1.0f, BLACK, FADE_DONE_VIDEO_START);
}

/* AbstractSpecialDay.HandleDay, then LogGraphContributionFromArea(its contribution) */
static void handle_special_day(void)
{
    static const float BLACK[3] = {0, 0, 0};
    const SpecialDay *d = special_for_day(DS.day);
    if (!d) {
        go_to_title();
        return;
    }
    int c = (DS.special_force >= 0 && DS.special_force < d->n) ? DS.special_force : (d->n > 1 ? rng_int(d->n) : 0);
    DS.special_force = -1;
    if (d->kind[c]) {
        lsd_log("special day %d: video %s", DS.day, SP.video[d->idx[c]]);
        enter_video_scene(SP.video[d->idx[c]]);
    } else {
        /* TextSpecialDay: the text_dream scene, fading in from black over 3 seconds */
        lsd_log("special day %d: text screen %d", DS.day, d->idx[c]);
        special_clear();
        DS.mode = DS_SPECIAL;
        DS.special_kind = 0;
        DS.text_tex = load_text_screen(d->idx[c]);
        fade(0.0f, 3.0f, BLACK, FADE_DONE_NONE);
    }
    log_area(d->dyn, d->up);
}

/* the text or video ends (a button, or the clip's end): VideoFinished(), EndDream() */
static void end_special(void)
{
    if (DS.special_over)
        return;
    DS.special_over = 1;
    DS.playing_video = 0;
    ds_end_dream(0);
}

/* the title screen as a launch shows it (UITitleScreenInitialisation.ShowTitleScreen: a five-second
 * fade from black) */
static void enter_title_first(void)
{
    static const float BLACK[3] = {0, 0, 0};
    video_stop();
    audio_stream_clear();
    special_clear();
    DS.mode = DS_TITLE;
    DS.title_t = 0;
    DS.fade_a = 1;
    memcpy(DS.fade_rgb, BLACK, sizeof(BLACK));
    fade(0.0f, 5.0f, BLACK, FADE_DONE_NONE);
    title_enter(0);
}

/* intro.unity's IntroVideoController: one of the openings at random, fading in from black over a
 * tenth of a second; at its end, or on Cross or Start, it pauses and fades to black over a second,
 * then the title screen */
static void begin_intro(void)
{
    static const char *const OPENINGS[7] = {"OPENINGA", "OPENINGB", "OPENINGC", "OPENINGD",
                                            "OPENINGE", "OPENINGF", "OPENINGG"};
    static const float BLACK[3] = {0, 0, 0};
    int pick = rng_int(7);
    special_clear();
    if (!video_start(OPENINGS[pick])) {
        lsd_log("no intro videos (intro1.pak...): straight to the title screen");
        enter_title_first();
        return;
    }
    lsd_log("intro: %s", OPENINGS[pick]);
    title_preload(); /* (the title screen's sounds come in while it plays) */
    DS.mode = DS_SPECIAL;
    DS.special_kind = 2;
    snprintf(DS.video_name, sizeof(DS.video_name), "%s", OPENINGS[pick]);
    DS.fade_a = 1;
    memcpy(DS.fade_rgb, BLACK, sizeof(BLACK));
    fade(0.0f, 0.1f, BLACK, FADE_DONE_NONE); /* ToriiFader.FadeOut(0.1f) */
}

static void intro_update(const Input *in, float dt)
{
    static const float BLACK[3] = {0, 0, 0};
    if (DS.special_over)
        return; /* (paused on the last picture while it fades) */
    video_update(dt);
    int skip = (in->pressed & (BTN_CROSS | BTN_START)) != 0 && video_active();
    if (video_ended() || skip) { /* endIntro */
        lsd_log("intro %s", skip && !video_ended() ? "skipped" : "ended");
        DS.special_over = 1;
        video_pause();
        audio_stream_clear();
        fade(1.0f, 1.0f, BLACK, FADE_DONE_INTRO_END);
    }
}

static void special_update(const Input *in, float dt)
{
    int skip = (in->pressed & (BTN_CROSS | BTN_START)) != 0;
    plat_keep_awake(); /* (nobody presses anything while reading or watching) */
    if (DS.special_kind == 2) {
        intro_update(in, dt);
        return;
    }
    if (DS.special_kind == 0) {
        if (skip && DS.text_tex) /* (a press during the fade-in counts too) */
            end_special();
        else if (!DS.text_tex && !DS.fading)
            end_special(); /* (the screen could not be loaded) */
        return;
    }
    if (!video_active() || DS.special_over)
        return; /* (before the clip starts, and paused on its last frame while the day ends) */
    video_update(dt);
    if (video_ended()) {
        lsd_log("video %s ended", DS.video_name);
        end_special();
    } else if (skip) {
        lsd_log("video %s skipped", DS.video_name);
        video_pause(); /* VideoPlayer.Pause: the picture stays, the sound stops */
        audio_stream_clear();
        end_special();
    }
}

GLuint ds_special_picture(int *w, int *h)
{
    if (DS.mode != DS_SPECIAL)
        return 0;
    if (DS.special_kind == 0) {
        *w = 960; /* (the screens are drawn at the Vita's resolution) */
        *h = 544;
        return DS.text_tex;
    }
    return video_texture(w, h);
}

/* DreamSystem.BeginDream / BeginPlayableDream */
static void begin_dream(void)
{
    static const float BLACK[3] = {0, 0, 0};
    P.fallen = 0;
    player_reset_view();
    DS.in_sequence = 1;
    memset(&DS.area, 0, sizeof(DS.area));
    memset(&DS.entity, 0, sizeof(DS.entity));
    g_nday_events = 0;
    DS.texset = texset_from_day(DS.day);
    const SpecialDay *sp = special_for_day(DS.day);
    if (sp && (S.special_days || sp->always)) { /* GameSettings.SpecialDaysEnabled */
        DS.timer = -1;
        DS.can_transition = 0;
        lsd_log("day %d begins: a special day (%d choices)", DS.day, sp->n);
        fade(1.0f, BEGIN_FADE_SECS, BLACK, FADE_DONE_SPECIAL);
        return;
    }
    int dream = DS.day == 1 ? get_first_dream() : get_dream_from_graph(DS.last_gx, DS.last_gy);
    DS.forced_spawn = 0;
    DS.can_transition = 1;
    DS.timer = rng_float(MIN_SECONDS_IN_DREAM, MAX_SECONDS_IN_DREAM);
    DS.first_load_of_day = 1;
    lsd_log("day %d begins: %s, %s textures, ends in %.0f s", DS.day, DS.journal.d[dream].name,
            ds_texset_name(DS.texset), DS.timer);
    DS.load_dream = dream;
    fade(1.0f, BEGIN_FADE_SECS, BLACK, FADE_DONE_BEGIN);
}

/* DreamSystem.BeginFlashback: past entity contributions, a random one every 8-16 seconds */
#define MIN_SECONDS_IN_FLASHBACK 8.0f
#define MAX_SECONDS_IN_FLASHBACK 16.0f

static void flashback_to_event(int first)
{
    const PastEvent *e = &g_events[rng_int(g_nevents)];
    int dream = e->dream >= 0 && e->dream < DS.journal.n ? e->dream : rng_int(DS.journal.n);
    DS.flash_spawn = 1;
    DS.flash_pos = v3(e->x, e->y, e->z);
    DS.flash_yaw = e->yaw;
    if (first)
        snprintf(DS.flash_entity, sizeof(DS.flash_entity), "%s", e->entity);
    lsd_log("flashback: %s at (%.2f %.2f %.2f)%s%s", DS.journal.d[dream].name, e->x, e->y, e->z,
            e->entity[0] ? ", entity " : "", e->entity);
    DS.load_dream = dream;
}

void ds_begin_flashback(void)
{
    static const float BLACK[3] = {0, 0, 0};
    if (DS.mode != DS_TITLE || DS.in_sequence || DS.in_flashback || DS.load_stage || g_nevents <= 0)
        return;
    P.fallen = 0;
    player_reset_view();
    DS.texset = texset_from_day(DS.day); /* SetRandomTextureSetFromDayNumber */
    DS.in_flashback = 1;
    DS.forced_spawn = 0;
    DS.can_transition = 1;
    DS.timer = rng_float(MIN_SECONDS_IN_DREAM, MAX_SECONDS_IN_DREAM);
    DS.first_load_of_day = 1;
    lsd_log("flashback begins (%d past events), ends in %.0f s", g_nevents, DS.timer);
    fade(1.0f, BEGIN_FADE_SECS, BLACK, FADE_DONE_FLASH_BEGIN);
}

void ds_fade_title(float target, float secs)
{
    static const float BLACK[3] = {0, 0, 0};
    fade(target, secs, BLACK, target > 0.5f ? FADE_DONE_TITLE : FADE_DONE_NONE);
}

void ds_start_day(void)
{
    if (DS.mode == DS_TITLE && !DS.in_sequence && !DS.load_stage)
        begin_dream();
}

static void go_to_title(void)
{
    /* ReturnToTitle: the dream is gone; the title screen fades in from the day-end colour */
    unload_dream();
    special_clear();
    greyman_day_end();
    skyfx_clear();
    DS.cur = -1;
    DS.mode = DS_TITLE;
    DS.title_t = 0;
    title_enter(1);
    fade(0.0f, 1.0f, NULL, FADE_DONE_NONE);
}

/* commonEndDream: whether the day ended or the player quit to the menu */
static void common_end_dream(void)
{
    music_stop();
    DS.ending = 1;
    DS.can_transition = 0;
    DS.in_sequence = 0;
    DS.in_flashback = 0; /* (and the flashback coroutine stops) */
    DS.flash_spawn = 0;
    DS.can_pause = 0;
    DS.last_yaw = P.yaw;
    end_timer_off();
}

/* DreamSystem.EndDream */
int ds_end_dream(int from_fall)
{
    /* (LSDR refuses it only while the dream is ending or a video plays; here also while a link fades
     * out and loads, when the player cannot move - but not during the link's fade-in, when its fall
     * detector can end the day) */
    if (DS.ending || DS.playing_video || (DS.mode != DS_DREAM && DS.mode != DS_SPECIAL) || DS.load_stage)
        return 0;
    if (DS.transitioning && !(DS.fading && DS.fade_done == FADE_DONE_LOADED))
        return 0;
    if (from_fall) {
        static const float PITCHES[2] = {0.25f, 0.5f};
        sfx_play(sfx_link_sample(), PITCHES[rng_int(2)]);
        if (DS.in_sequence)
            log_area(0, FALLING_UPPER_PENALTY);
        DS.can_control = 0;
    } else {
        DS.gravity = 0;
    }
    if (DS.in_flashback) { /* a flashback: no day passes; white */
        static const float WHITE[3] = {1, 1, 1};
        lsd_log("flashback ends%s", from_fall ? " (fell)" : "");
        fade(1.0f, from_fall ? FADE_OUT_SECS_FALL : FADE_OUT_SECS_REGULAR, WHITE, FADE_DONE_DAY_END);
        common_end_dream();
        return 1;
    }
    if (DS.flash_disabled_days > 0)
        DS.flash_disabled_days--;
    for (int i = 0; i < g_nday_events; i++) /* (the day's sequence joins the save) */
        events_add(&g_day_events[i]);
    g_nday_events = 0;
    float rgb[3];
    fade_colour_from_sequence(rgb);
    /* JournalSaveData.IncrementDayNumberWithSequence */
    ds_graph_position(&DS.last_gx, &DS.last_gy);
    if (DS.nhist >= (int)sizeof(DS.hist_x)) { /* (keep the newest) */
        memmove(DS.hist_x, DS.hist_x + 1, sizeof(DS.hist_x) - 1);
        memmove(DS.hist_y, DS.hist_y + 1, sizeof(DS.hist_y) - 1);
        DS.nhist--;
    }
    DS.hist_x[DS.nhist] = (int8_t)DS.last_gx;
    DS.hist_y[DS.nhist] = (int8_t)DS.last_gy;
    DS.nhist++;
    int finished = DS.day;
    DS.day++;
    if (DS.day > 365) {
        DS.day = 1;
        DS.year++;
    }
    save_write();
    lsd_log("day %d ends%s: graph position %d,%d (%ld area contributions), fade %.2f %.2f %.2f", finished,
            from_fall ? " (fell)" : "", DS.last_gx, DS.last_gy, DS.area.n, rgb[0], rgb[1], rgb[2]);
    fade(1.0f, from_fall ? FADE_OUT_SECS_FALL : FADE_OUT_SECS_REGULAR, rgb, FADE_DONE_DAY_END);
    common_end_dream();
    return 1;
}

/* DreamSystem.ForceEndDream: no day passes, nothing is saved; black, then the title (and its graph) */
void ds_force_end_dream(void)
{
    if (DS.ending || DS.mode != DS_DREAM || DS.load_stage)
        return;
    static const float BLACK[3] = {0, 0, 0};
    lsd_log("day %d: back to the title screen (the day does not count)", DS.day);
    common_end_dream();
    fade(1.0f, FADE_OUT_SECS_FORCE, BLACK, FADE_DONE_DAY_END);
}

/* ---------------------------------------------------------------- links */

/* DreamSystem.Transition */
void ds_transition(int dream, const float *rgb, int sound, int lock, uint32_t spawn_id)
{
    /* a video (or the day ending) owns the screen: linking now would cut its fade short */
    if (!DS.can_transition || DS.mode != DS_DREAM || DS.playing_video || DS.ending || DS.load_stage)
        return;
    DS.can_transition = 0;
    if (dream < 0 && DS.next_dream >= 0) {
        dream = DS.next_dream;
        DS.next_dream = -1;
    }
    if (dream >= DS.journal.n)
        dream = -1;
    if (dream < 0)
        dream = get_linkable(DS.cur);
    float col[3];
    if (rgb)
        memcpy(col, rgb, sizeof(col));
    else
        fade_colour_from_sequence(col);
    lsd_log("linking to %s%s", DS.journal.d[dream].name, lock ? " (input locked)" : "");
    if (dream != DS.cur)
        music_stop();
    if (sound) {
        static const float PITCHES[4] = {0.25f, 0.5f, 1.0f, 2.0f};
        sfx_play(sfx_link_sample(), PITCHES[rng_int(4)]);
    }
    DS.can_control = 0;
    DS.can_pause = 0; /* (until the next dream has loaded) */
    if (spawn_id)
        DS.forced_spawn = spawn_id;
    if (lock) {
        P.input_locked = 1;
        P.locked_x = P.in_x;
        P.locked_y = P.in_y;
    }
    DS.last_yaw = P.yaw;
    DS.load_dream = dream;
    DS.load_transitioning = !lock;
    fade(1.0f, LINK_FADE_SECS, col, FADE_DONE_LINK);
    /* set after fade(): cutting short a running "loaded" fade-out clears it (a ToriiFader quirk that
     * would let the day end in the middle of this link) */
    DS.transitioning = 1;
}

/* PlayerMovement: walked into a wall for LinkDelay seconds */
void ds_wall_link(void)
{
    if (DS.no_wall_link || !P.can_link)
        return;
    P.can_link = 0;
    ds_transition(-1, NULL, 1, 0, 0);
}

static void run_fade_done(FadeDone what)
{
    switch (what) {
    case FADE_DONE_LINK:
        if (rng_int(2) == 0) /* RandUtil.OneIn(CHANCE_TO_SWITCH_TEXTURES_WHEN_LINKING) */
            DS.texset = texset_from_day(DS.day);
        start_load(DS.load_dream, DS.load_transitioning);
        break;
    case FADE_DONE_BEGIN:
        title_stop_music(); /* (the title scene goes) */
        start_load(DS.load_dream, 0);
        break;
    case FADE_DONE_FLASH_BEGIN:
        title_stop_music();
        if (!DS.in_flashback)
            break;
        flashback_to_event(1);
        DS.flash_t = 0;
        DS.flash_len = rng_float(MIN_SECONDS_IN_FLASHBACK, MAX_SECONDS_IN_FLASHBACK);
        start_load(DS.load_dream, 1);
        break;
    case FADE_DONE_FLASH_NEXT:
        if (!DS.in_flashback || DS.ending)
            break;
        if (rng_int(2) == 0) /* RandUtil.OneIn(CHANCE_TO_SWITCH_TEXTURES_WHEN_LINKING) */
            DS.texset = texset_from_day(DS.day);
        flashback_to_event(0);
        if (DS.load_dream != DS.cur)
            music_stop();
        start_load(DS.load_dream, 1);
        break;
    case FADE_DONE_LOADED:
        P.can_link = 1;
        DS.transitioning = 0;
        break;
    case FADE_DONE_DAY_END:
        DS.ending = 0;
        go_to_title();
        break;
    case FADE_DONE_GREY_FLASH: {
        static const float WHITE[3] = {1, 1, 1};
        greyman_flash_done();
        fade(0.0f, 3.0f, WHITE, FADE_DONE_GREY_GONE);
        break;
    }
    case FADE_DONE_GREY_GONE:
        greyman_gone();
        break;
    case FADE_DONE_VIDEO:
        /* DreamSystem.PlayVideo: the video_dream scene */
        enter_video_scene(DS.video_name);
        break;
    case FADE_DONE_INTRO_END:
        enter_title_first(); /* SceneManager.LoadScene("titlescreen") */
        break;
    case FADE_DONE_VIDEO_START:
        if (DS.mode == DS_SPECIAL && DS.special_kind == 1 && !video_start(DS.video_name)) {
            lsd_log("video %s is not available: the day ends", DS.video_name);
            end_special();
        }
        break;
    case FADE_DONE_SPECIAL:
        title_stop_music();
        handle_special_day();
        break;
    case FADE_DONE_TITLE:
        title_fade_done();
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- loading */

static void start_load(int dream, int transitioning)
{
    DS.gravity = 0;
    DS.load_dream = dream;
    DS.load_transitioning = transitioning;
    DS.load_stage = 1; /* free the old dream now, load after the GPU has let go of it */
}

static void apply_sky_effects(void);

static void finish_load(void)
{
    int dream = DS.load_dream;
    double t0 = plat_seconds();
    if (!dream_load(&g_dream, DS.journal.d[dream].dir, DS.texset)) {
        lsd_log("loading %s failed (texture set %d)", DS.journal.d[dream].name, DS.texset);
        int ok = 0;
        if (DS.texset != 0) {
            DS.texset = 0;
            ok = dream_load(&g_dream, DS.journal.d[dream].dir, 0);
        }
        if (!ok && dream != 0) {
            lsd_log("falling back to %s", DS.journal.d[0].name);
            dream = 0;
            DS.load_dream = 0;
            DS.forced_spawn = 0;
            ok = dream_load(&g_dream, DS.journal.d[0].dir, 0);
        }
        if (!ok) {
            plat_present_message("Could not load the dreams.\nSee ux0:data/lsdr/log.txt\n"
                                 "Hold START+SELECT to quit.");
            DS.load_stage = -1;
            return;
        }
    }
    g_dream_loaded = 1;
    if (dream != DS.cur) /* (not when a link leads back into the same dream) */
        music_dream_song(dream, DS.day, DS.last_gx, DS.last_gy);
    DS.cur = dream;
    DS.mode = DS_DREAM;
    DS.playing_video = 0;
    DS.stretch_y = 1;
    DS.stretch_dur = 0;
    /* instantiate the dream's entities: Awake/OnEnable, then EntityIndex.AllRegistered (Init) */
    double t1 = plat_seconds();
    int lua_ok = luaapi_dream_begin(); /* (first: OnEnable during instantiation starts DreamAudio) */
    double t2 = plat_seconds();
    int scene_ok = scene_load(&g_dream);
    double t3 = plat_seconds();
    if (scene_ok && lua_ok)
        scene_init_entities();
    double t4 = plat_seconds();
    DS.load_ms = (t4 - t0) * 1000.0;
    lsd_log("loaded %s in %.0f ms (%d entities, %d scripts' errors so far; entity setup %.0f ms, scene %.0f ms, "
            "scripts %.0f ms)",
            DS.journal.d[dream].name, DS.load_ms, g_scene.nent, luaapi_errors(), (t2 - t1) * 1000.0,
            (t3 - t2) * 1000.0, (t4 - t3) * 1000.0);
    luaapi_log_init_times();
    plat_log_memory("after loading");

    /* LoadDream */
    if (DS.flash_spawn) {
        /* a flashback: the player where the past event happened, facing the way they faced */
        P.feet = v3(DS.flash_pos.x + 0.01f, DS.flash_pos.y + PLAYER_SKIN, DS.flash_pos.z - 0.01f);
        P.yaw = DS.flash_yaw;
        DS.spawned_once = 1;
        DS.flash_spawn = 0;
    } else if (DS.load_spawn_index >= 0 && DS.load_spawn_index < g_dream.nspawn) {
        spawn_at(&g_dream.spawn[DS.load_spawn_index], 1);
        DS.spawned_once = 1;
    } else {
        spawn_player_in_dream(!DS.load_transitioning);
    }
    DS.load_spawn_index = -1;
    DS.forced_spawn = 0;
    DS.env = choose_environment();
    /* DreamEnvironmentEffects: the effects object starts with the day's first dream, then
     * ApplyEnvironment makes this dream's suns, clouds and stars */
    if (DS.first_load_of_day)
        skyfx_day_start();
    apply_sky_effects();
    DS.can_control = 1;
    DS.gravity = 1;
    P.input_locked = 0;
    player_on_new_dream();
    DS.can_transition = 1;
    DS.can_pause = 1; /* reenable pausing */
    DS.next_dream = -1;
    DS.graph_t = 0;
    DS.last_lbd = -1;
    DS.load_stage = 0;
    if (scene_ok)
        scene_start_components();
    if (DS.first_load_of_day) {
        DS.first_load_of_day = 0;
        greyman_day_start();
    }
    greyman_new_dream();
    if (DS.in_flashback) {
        /* onLoadFinished: control back, the flashback's time from now, the first event's entity shown */
        DS.flash_t = 0;
        if (DS.flash_entity[0]) {
            int n = scene_find(DS.flash_entity);
            if (n >= 0)
                scene_set_active(n, 1);
        }
    }
    fade(0.0f, LOADED_FADE_SECS, NULL, FADE_DONE_LOADED);
}

static void apply_sky_effects(void)
{
    skyfx_clear();
    if (DS.cur < 0 || !(DS.journal.d[DS.cur].flags & JD_ENV_EFFECTS) || g_dream.nenvfx <= 0)
        return;
    int e = DS.env < g_dream.nenvfx ? DS.env : g_dream.nenvfx - 1;
    skyfx_create(&g_dream.envfx[e]);
    int suns, clouds, stars;
    if (skyfx_count(&suns, &clouds, &stars))
        lsd_log("sky: %d suns, %d clouds, %d starfields", suns, clouds, stars);
}

static void load_update(void)
{
    switch (DS.load_stage) {
    case 1:
        unload_dream();
        DS.load_stage = 2;
        break;
    case 2: case 3: case 4: case 5: case 6:
        /* frames presented without the old dream: vitaGL frees deleted objects after 4 frames */
        DS.load_stage++;
        break;
    case 7:
        finish_load();
        break;
    default:
        break;
    }
}

/* DreamSystem.spawnPlayerInDream */
static void spawn_at(const DreamSpawn *s, int set_orientation)
{
    P.feet = v3(s->pos.x + 0.01f, s->pos.y + PLAYER_SKIN, s->pos.z - 0.01f);
    /* a spawn point a little under its floor (Violence District's 35th, 2 cm): Unity's character
     * controller is pushed out of the floor, here it would fall through it */
    float y;
    if (col_ray_down(&g_dream, ds_to_dream(v3(P.feet.x, P.feet.y + 0.25f, P.feet.z)), &y)) {
        y = ds_from_dream(v3(0, y, 0)).y;
        if (y > P.feet.y - PLAYER_SKIN)
            P.feet.y = y + PLAYER_SKIN;
    }
    if (set_orientation)
        P.yaw = s->yaw;
}

/* something to stand on at a spawn point (Violence District's 35th is on a steep roof over nothing:
 * whoever links there falls out of the dream as it fades in) */
static int spawn_standable(const DreamSpawn *s)
{
    float y;
    return col_ground(&g_dream, v3(s->pos.x + 0.01f, s->pos.y + PLAYER_SKIN, s->pos.z - 0.01f), 0.25f, 0.5f, &y);
}

static void spawn_player_in_dream(int set_orientation)
{
    const Dream *d = &g_dream;
    int done = 0;
    if (d->nspawn <= 0) {
        lsd_log("no spawn points in %s", d->name);
        P.feet = v3((d->lo[0] + d->hi[0]) * 0.5f, d->hi[1], (d->lo[2] + d->hi[2]) * 0.5f);
        done = 1;
    }
    if (!done && DS.forced_spawn) {
        for (int i = 0; i < d->nspawn; i++)
            if (d->spawn[i].id == DS.forced_spawn) {
                spawn_at(&d->spawn[i], set_orientation);
                done = 1;
                break;
            }
        if (!done)
            lsd_log("forced spawn point %08x not found in %s", (unsigned)DS.forced_spawn, d->name);
    }
    int cand[256], n = 0;
    if (!done && DS.day == 1) {
        for (int i = 0; i < d->nspawn && n < 256; i++)
            if (d->spawn[i].flags & SPAWN_DAY_ONE)
                cand[n++] = i;
        if (n) {
            spawn_at(&d->spawn[cand[rng_int(n)]], set_orientation);
            done = 1;
        }
    }
    if (!done) {
        n = 0;
        for (int i = 0; i < d->nspawn && n < 256; i++)
            if (!(d->spawn[i].flags & SPAWN_TUNNEL) && spawn_standable(&d->spawn[i]))
                cand[n++] = i;
        if (n) {
            spawn_at(&d->spawn[cand[rng_int(n)]], set_orientation);
        } else {
            lsd_log("only tunnel entrances to spawn on in %s", d->name);
            spawn_at(&d->spawn[rng_int(d->nspawn)], set_orientation);
        }
    }
    if (DS.spawned_once && !set_orientation)
        P.yaw = DS.last_yaw;
    DS.spawned_once = 1;
    lsd_log("spawned in %s at (%.2f %.2f %.2f) yaw %.0f", d->name, P.feet.x, P.feet.y, P.feet.z, P.yaw);
}

/* ---------------------------------------------------------------- entities */

/* the player's CharacterController capsule against a trigger box */
static int capsule_in_box(const DreamTrigger *t)
{
    const float r = PLAYER_RADIUS;
    const float hs[3] = {r, PLAYER_HEIGHT * 0.5f, PLAYER_HEIGHT - r};
    for (int i = 0; i < 3; i++) {
        float l[3];
        affine3x4(t->inv, ds_to_dream(v3(P.feet.x, P.feet.y + hs[i], P.feet.z)), l);
        float d2 = 0;
        for (int k = 0; k < 3; k++) {
            float d = 0;
            if (l[k] < t->lo[k]) d = t->lo[k] - l[k];
            else if (l[k] > t->hi[k]) d = l[k] - t->hi[k];
            d *= t->scale[k];
            d2 += d * d;
        }
        if (d2 <= r * r)
            return 1;
    }
    return 0;
}

/* TriggerLink.onTrigger / onTriggerExit */
static void trigger_enter(DreamTrigger *t)
{
    if (t->kind == TRIGGER_LUA) {
        luaapi_trigger_lua(t->comp, 1);
        return;
    }
    lsd_log("link trigger entered in %s (to %s)", g_dream.name,
            (t->linked >= 0 && t->linked < DS.journal.n) ? DS.journal.d[t->linked].name : "a random dream");
    if (t->linked >= DS.journal.n)
        t->linked = -1;
    if (t->flags & TRIG_WITHIN) {
        DS.forced_spawn = t->spawn; /* SetNextLinkDream -> SetNextSpawn (may clear it) */
        DS.next_dream = t->linked;
        return;
    }
    ds_transition(t->linked, (t->flags & TRIG_FORCE_COLOR) ? t->color : NULL, (t->flags & TRIG_SOUND) != 0,
                  (t->flags & TRIG_LOCK_INPUT) != 0, t->spawn);
}

static void trigger_exit(DreamTrigger *t)
{
    if (t->kind == TRIGGER_LUA) {
        luaapi_trigger_lua(t->comp, 0);
        return;
    }
    if (t->flags & TRIG_WITHIN) {
        DS.forced_spawn = 0;
        DS.next_dream = -1;
    }
}

static void update_triggers(void)
{
    if (DS.no_triggers)
        return;
    for (int i = 0; i < g_dream.ntrig; i++) {
        DreamTrigger *t = &g_dream.trig[i];
        if (!scene_active(t->node)) {
            /* a disabled trigger collider: no events; re-enabling it while inside counts as entering */
            t->inside = 0;
            continue;
        }
        int now = capsule_in_box(t);
        if (now && !t->inside) {
            if (!((t->flags & TRIG_ONCE) && t->triggered)) {
                t->inside = 1;
                trigger_enter(t);
                t->triggered = 1;
                if (DS.load_stage) /* a link started: this dream is about to go */
                    return;
                continue;
            }
        } else if (!now && t->inside) {
            if (!((t->flags & TRIG_ONCE) && t->exit_triggered)) {
                trigger_exit(t);
                t->exit_triggered = 1;
            }
        }
        t->inside = now;
    }
}

/* LBDGraphContributor.positionToLBDIndex */
static int lbd_index(const DreamGraph *g, vec3 p)
{
    float l[3];
    affine3x4(g->inv, p, l);
    if (g->layout == 0) {
        int y = (int)l[2] / 20;
        int xmod = (y % 2 == 1) ? 10 : 0;
        int x = (int)(l[0] + (float)xmod) / 20;
        if (y < 0 || y >= g->width || x < 0 || x > g->width)
            return -1;
        return y * g->width + x;
    }
    int fy = (int)l[1];
    return fy < 0 ? -1 : fy;
}

static void update_graph(float dt)
{
    const DreamGraph *g = &g_dream.graph;
    if (!g->present || DS.cur < 0)
        return;
    DS.graph_t += dt;
    if (DS.graph_t <= GRAPH_UPDATE_INTERVAL)
        return;
    DS.graph_t = 0;
    int idx = lbd_index(g, ds_to_dream(P.feet));
    if (idx == -1 || idx == DS.last_lbd)
        return;
    if (idx < g->n)
        log_area(g->data[idx][0], g->data[idx][1]);
    else
        lsd_log("LBD tile %d has no graph data (only %d)", idx, g->n);
    DS.last_lbd = idx;
}

/* PlayerFallDetector: SphereCast straight down found nothing */
static void update_fall(float dt)
{
    if (P.fallen) {
        if (P.look < FALL_LOOK_MAX) {
            P.look += FALL_LOOK_SPEED * dt;
            if (P.look > FALL_LOOK_MAX) P.look = FALL_LOOK_MAX;
        }
        return;
    }
    if (!DS.can_control || !DS.gravity || DS.no_fall)
        return;
    if (!col_anything_below(&g_dream, ds_to_dream(P.feet), PLAYER_RADIUS * 2, PLAYER_RADIUS)) {
        vec3 at = P.feet;
        if (ds_end_dream(1)) { /* (otherwise tried again next frame: never stuck looking up, falling) */
            P.fallen = 1;
            lsd_log("fell out of %s at (%.2f %.2f %.2f)", g_dream.name, at.x, at.y, at.z);
        }
    }
}

/* ---------------------------------------------------------------- per frame */

void ds_update(const Input *in, float dt)
{
    fade_update(dt);
    if (DS.load_stage > 0)
        load_update();
    /* EndDreamAfterSeconds: counts from BeginPlayableDream, waits while linking */
    if (DS.in_flashback && DS.mode == DS_DREAM && !DS.ending && !DS.load_stage && !DS.transitioning &&
        DS.can_transition) {
        DS.flash_t += dt;
        if (DS.flash_t >= DS.flash_len) { /* the next past event, through white */
            static const float WHITE[3] = {1, 1, 1};
            DS.flash_t = 0;
            DS.flash_len = rng_float(MIN_SECONDS_IN_FLASHBACK, MAX_SECONDS_IN_FLASHBACK);
            DS.can_control = 0;
            DS.can_transition = 0;
            DS.can_pause = 0;
            fade(1.0f, 2.0f, WHITE, FADE_DONE_FLASH_NEXT);
            DS.transitioning = 1; /* (after fade(), as for links) */
        }
    }
    if ((DS.in_sequence || DS.in_flashback) && DS.timer >= 0 && !DS.ending) {
        DS.timer -= dt;
        if (DS.timer < 0) {
            DS.timer = 0;
            if (!DS.transitioning && DS.mode == DS_DREAM && !DS.load_stage && !DS.playing_video) {
                lsd_log("day timer finished");
                luaapi_dream_timeout(); /* DreamSystem.OnDreamTimeout */
                ds_end_dream(0);
            }
        }
    }
    if (DS.stretch_dur > 0 && g_scene.n > 0) {
        /* StretchDream: lerp the dream instance's Y scale */
        DS.stretch_t += dt;
        float k = DS.stretch_t / DS.stretch_dur;
        if (k >= 1) {
            k = 1;
            DS.stretch_dur = 0;
        }
        DS.stretch_y = DS.stretch_from + (DS.stretch_to - DS.stretch_from) * k;
        vec3 sc = g_scene.scale[0];
        scene_set_scale(0, v3(sc.x, DS.stretch_y, sc.z));
    }
    if (DS.mode == DS_SPECIAL)
        special_update(in, dt);
    if (DS.mode == DS_TITLE && !DS.in_sequence && DS.load_stage == 0)
        title_update(in, dt);
}

void ds_after_player(float dt)
{
    if (DS.mode != DS_DREAM || !g_dream_loaded || DS.load_stage)
        return;
    update_triggers();
    if (DS.load_stage || !g_dream_loaded)
        return;
    if (DS.in_sequence)
        update_graph(dt);
    update_fall(dt);
}

/* ---------------------------------------------------------------- entity hooks */

void ds_log_entity(int dynamic, int upper, int node)
{
    if (!DS.in_sequence) {
        lsd_log("graph contribution with no sequence");
        return;
    }
    contrib_add(&DS.entity, dynamic, upper);
    /* GraphContribution(dynamicness, upperness, playerTransform, sourceEntity, dream) */
    if (g_nday_events < MAX_DAY_EVENTS && DS.cur >= 0) {
        PastEvent *e = &g_day_events[g_nday_events++];
        memset(e, 0, sizeof(*e));
        e->dream = DS.cur;
        e->x = P.feet.x;
        e->y = P.feet.y;
        e->z = P.feet.z;
        e->yaw = P.yaw;
        if (node >= 0 && node < g_scene.n && g_scene.name[node])
            snprintf(e->entity, sizeof(e->entity), "%s", g_scene.name[node]);
    }
}

void ds_disable_flashback(int days) { DS.flash_disabled_days = days; }

int ds_can_flashback(void) { return DS.flash_disabled_days <= 0 && g_nevents > 100; }

/* JournalSaveData.ResetProgress */
void ds_reset_progress(void)
{
    DS.day = 1;
    DS.year = 0;
    DS.nhist = 0;
    DS.last_gx = DS.last_gy = 0;
    g_nevents = 0;
    lsd_log("progress reset: day 1");
    save_write();
}

/* DreamSystem.PlayVideo: fade to the clip's colour, (the video plays), then the day ends */
void ds_play_video(const char *clip, const float rgb[3])
{
    if (DS.mode != DS_DREAM || DS.ending || DS.playing_video || DS.load_stage)
        return;
    if (DS.fading) { /* FadeIn(forced: false) does nothing while another fade runs */
        lsd_log("video %s not played: a fade is running", clip);
        return;
    }
    /* the clip's file name: "Original/FILM/EVENT4.STR[0].webm" -> EVENT4 */
    const char *b = strrchr(clip, '/');
    b = b ? b + 1 : clip;
    size_t n = strcspn(b, ".");
    if (n >= sizeof(DS.video_name)) n = sizeof(DS.video_name) - 1;
    memcpy(DS.video_name, b, n);
    DS.video_name[n] = 0;
    lsd_log("video %s (%s)", DS.video_name, clip);
    DS.playing_video = 1;
    DS.can_transition = 0;
    music_stop();
    end_timer_off();
    fade(1.0f, 3.0f, rgb, FADE_DONE_VIDEO);
}

void ds_stretch(float amount, float seconds)
{
    if (DS.stretch_dur > 0 || g_scene.n <= 0) /* already stretching */
        return;
    DS.stretch_from = g_scene.scale[0].y;
    DS.stretch_to = DS.stretch_from * amount;
    DS.stretch_t = 0;
    DS.stretch_dur = seconds > 0 ? seconds : 0.0001f;
}

float ds_stretch_y(void) { return g_scene.n > 0 ? g_scene.scale[0].y : 1.0f; }

int ds_col_move(vec3 *feet, vec3 motion)
{
    float k = ds_stretch_y();
    if (k == 1.0f || k <= 1e-4f)
        return col_move(&g_dream, feet, motion);
    vec3 f = ds_to_dream(*feet);
    motion.y /= k;
    int flags = col_move(&g_dream, &f, motion);
    *feet = ds_from_dream(f);
    return flags;
}

/* ---------------------------------------------------------------- setup and test commands */

static void debug_load(int dream, int spawn_index)
{
    if (dream < 0 || dream >= DS.journal.n)
        return;
    unload_dream();
    special_clear();
    title_stop_music();
    DS.fading = 0;
    DS.fade_a = 0;
    DS.ending = 0;
    DS.transitioning = 0;
    DS.load_transitioning = 0;
    DS.load_dream = dream;
    DS.load_spawn_index = spawn_index;
    DS.in_sequence = 1;
    DS.first_load_of_day = 1;
    finish_load();
    DS.fading = 0;
    DS.fade_a = 0;
    DS.timer = -1; /* test loads: no day timer unless a script sets one */
    P.can_link = 1;
    P.fallen = 0;
    player_reset_view();
}

int ds_init(int script_mode)
{
    memset(&DS, 0, sizeof(DS));
    DS.cur = -1;
    DS.day = 1;
    DS.next_dream = -1;
    DS.load_spawn_index = -1;
    DS.last_lbd = -1;
    DS.timer = -1;
    if (!journal_load(&DS.journal))
        return 0;
    DS.special_force = -1;
    special_load();
    if (script_mode) {
        /* repeatable test runs: day 1, Bright Moon Cottage, no day timer until the script sets one */
        rng_seed(12345);
        DS.save_enabled = 0;
        debug_load(0, -1);
        return DS.load_stage != -1 && g_dream_loaded;
    }
    rng_seed((uint32_t)(plat_seconds() * 1000003.0) ^ 0x9E3779B9u);
    DS.save_enabled = 1;
    save_read();
    begin_intro(); /* (loading_screen -> intro -> titlescreen) */
    return 1;
}

int ds_command(const char *cmd)
{
    int a, b;
    float f, f2;
    if (sscanf(cmd, "seed %d", &a) == 1) {
        rng_seed((uint32_t)a);
    } else if (sscanf(cmd, "dream %d %d", &a, &b) == 2) {
        debug_load(a, b);
    } else if (sscanf(cmd, "dream %d", &a) == 1) {
        debug_load(a, -1);
    } else if (sscanf(cmd, "goto %d", &a) == 1) {
        ds_transition(a, NULL, 1, 0, 0);
    } else if (!strcmp(cmd, "linkrandom")) {
        ds_transition(-1, NULL, 1, 0, 0);
    } else if (!strcmp(cmd, "luaerrors")) {
        lsd_log("lua errors so far: %d", luaapi_errors());
    } else if (!strcmp(cmd, "greyman")) {
        greyman_spawn();
    } else if (sscanf(cmd, "env %d", &a) == 1) { /* test: apply environment N (with its sky effects) */
        if (g_dream_loaded && a >= 0 && a < g_dream.nenv) {
            DS.env = a;
            apply_sky_effects();
        }
    } else if (!strcmp(cmd, "skytest")) {
        skyfx_test();
    } else if (!strcmp(cmd, "song")) { /* DreamSystem.SkipSong */
        music_skip();
    } else if (sscanf(cmd, "sound %d %f", &a, &f) == 2) { /* test: AudioPlayer.PlayClip of an SE sample */
        sfx_play(a, f);
    } else if (!strcmp(cmd, "audio")) {
        audio_log_stats();
    } else if (!strcmp(cmd, "audio off")) { /* test: stop the sound output (and its thread) */
        audio_output(0);
    } else if (!strcmp(cmd, "audio on")) {
        audio_output(1);
    } else if (!strcmp(cmd, "perftest")) {
        luaapi_perftest();
    } else if (!strncmp(cmd, "audiorec ", 9)) { /* test: audiorec NAME SECONDS */
        char name[32];
        if (sscanf(cmd + 9, "%31s %f", name, &f) == 2)
            audio_record(name, f);
    } else if (!strncmp(cmd, "music ", 6)) { /* test: music STAGE BG(0-4) BANK(1-7) */
        int st, bg, bk;
        if (sscanf(cmd + 6, "%d %d %d", &st, &bg, &bk) == 3)
            music_force(st, bg, bk);
    } else if (!strcmp(cmd, "video")) { /* test: a MovieClip.Play */
        static const float RED[3] = {1, 0, 0};
        ds_play_video("Original/FILM/EVENT3.STR[0].webm", RED);
    } else if (!strncmp(cmd, "playvideo ", 10)) { /* test: playvideo NAME (a MovieClip.Play of that clip) */
        static const float RED[3] = {1, 0, 0};
        ds_play_video(cmd + 10, RED);
    } else if (sscanf(cmd, "specialday %d %d", &a, &b) >= 1) { /* test: specialday DAY [CHOICE]: begin that day */
        if (sscanf(cmd, "specialday %d %d", &a, &b) < 2) b = -1;
        unload_dream();
        special_clear();
        DS.cur = -1;
        DS.mode = DS_TITLE;
        DS.ending = DS.transitioning = DS.playing_video = 0;
        DS.fading = 0;
        DS.fade_done = FADE_DONE_NONE;
        DS.day = a;
        DS.special_force = b;
        begin_dream();
    } else if (sscanf(cmd, "title %d", &a) == 1) { /* test: the title screen (1: as after a day, graph first) */
        unload_dream();
        special_clear();
        DS.cur = -1;
        DS.in_sequence = 0;
        DS.ending = DS.transitioning = DS.playing_video = 0;
        DS.fading = 0;
        DS.fade_done = FADE_DONE_NONE;
        DS.fade_a = 0;
        DS.mode = DS_TITLE;
        DS.timer = -1;
        title_enter(a);
    } else if (sscanf(cmd, "graphday %d %d", &a, &b) == 2) { /* test: add a finished day to the graph */
        if (DS.nhist < (int)sizeof(DS.hist_x) && a >= -9 && a <= 9 && b >= -9 && b <= 9) {
            DS.hist_x[DS.nhist] = (int8_t)a;
            DS.hist_y[DS.nhist] = (int8_t)b;
            DS.nhist++;
        }
    } else if (sscanf(cmd, "specialdays %d", &a) == 1) {
        S.special_days = a;
    } else if (sscanf(cmd, "pastevents %d", &a) == 1) {
        /* test: past events for flashbacks, at the loaded dream's spawn points */
        for (int i = 0; i < a && g_dream_loaded && g_dream.nspawn > 0 && DS.cur >= 0; i++) {
            PastEvent e;
            memset(&e, 0, sizeof(e));
            const DreamSpawn *sp = &g_dream.spawn[i % g_dream.nspawn];
            e.dream = DS.cur;
            e.x = sp->pos.x;
            e.y = sp->pos.y;
            e.z = sp->pos.z;
            e.yaw = sp->yaw;
            events_add(&e);
        }
        lsd_log("past events: %d (flashback %s)", g_nevents, ds_can_flashback() ? "possible" : "not possible");
    } else if (!strcmp(cmd, "flashback")) {
        ds_begin_flashback();
    } else if (!strcmp(cmd, "intro")) { /* test: intro.unity, as a launch shows it, then the title screen */
        title_stop_music();
        music_stop();
        unload_dream();
        special_clear();
        DS.cur = -1;
        DS.in_sequence = DS.in_flashback = 0;
        DS.ending = DS.transitioning = DS.playing_video = 0;
        DS.fading = 0;
        DS.fade_done = FADE_DONE_NONE;
        DS.timer = -1;
        begin_intro();
    } else if (sscanf(cmd, "stretch %f %f", &f, &f2) == 2) { /* test: DreamSystem.StretchDream */
        ds_stretch(f, f2);
    } else if (!strncmp(cmd, "setactive ", 10)) { /* test: GameObject.SetActive on an entity */
        char id[128];
        int on = 1;
        if (sscanf(cmd + 10, "%127s %d", id, &on) == 2) {
            int n = scene_find(id);
            if (n >= 0) scene_set_active(n, on);
            lsd_log("setactive %s: %s", id, n >= 0 ? (g_scene.aih[n] ? "active" : "inactive") : "not found");
        }
    } else if (!strcmp(cmd, "aihcheck")) { /* test: active-in-hierarchy is consistent */
        int bad = 0;
        for (int n = 0; n < g_scene.n; n++) {
            int p = g_scene.parent[n];
            int want = g_scene.active[n] && (p < 0 || g_scene.aih[p]);
            if (want != g_scene.aih[n] && bad++ < 5)
                lsd_log("inconsistent node %s: active in hierarchy %d, should be %d", g_scene.name[n], g_scene.aih[n], want);
        }
        lsd_log("aihcheck: %d inconsistent nodes", bad);
    } else if (!strcmp(cmd, "endday")) {
        ds_end_dream(0);
    } else if (sscanf(cmd, "timer %f", &f) == 1) {
        DS.timer = f;
    } else if (sscanf(cmd, "day %d", &a) == 1) {
        DS.day = a;
        if (g_dream_loaded) {
            DS.env = choose_environment();
            apply_sky_effects();
        }
    } else if (sscanf(cmd, "texset %d", &a) == 1) {
        DS.texset = a & 3;
    } else if (sscanf(cmd, "classic %d", &a) == 1) {
        S.classic = a;
    } else if (sscanf(cmd, "linking %d", &a) == 1) {
        DS.no_wall_link = !a;
    } else if (sscanf(cmd, "falling %d", &a) == 1) {
        DS.no_fall = !a;
    } else if (sscanf(cmd, "triggers %d", &a) == 1) {
        DS.no_triggers = !a;
    } else if (sscanf(cmd, "contribute %d %d", &a, &b) == 2) {
        log_area(a, b);
    } else if (!strcmp(cmd, "graph")) {
        int x, y;
        ds_graph_position(&x, &y);
        float rgb[3];
        fade_colour_from_sequence(rgb);
        lsd_log("graph: position %d,%d from %ld area contributions (last %d,%d); next day's dream %s; fade %.2f %.2f %.2f",
                x, y, DS.area.n, DS.area.last_dyn, DS.area.last_up, DS.journal.d[get_dream_from_graph(x, y)].name,
                rgb[0], rgb[1], rgb[2]);
    } else if (!strcmp(cmd, "state")) {
        lsd_log("state: mode %s, dream %s, day %d, texset %s, env %d, timer %.1f, can_transition %d, transitioning %d, "
                "ending %d, control %d, gravity %d, fade %.2f, load stage %d",
                DS.mode == DS_DREAM ? "dream" : DS.mode == DS_SPECIAL ? "special" : "title", DS.cur >= 0 ? DS.journal.d[DS.cur].name : "-", DS.day,
                ds_texset_name(DS.texset), DS.env, DS.timer, DS.can_transition, DS.transitioning, DS.ending,
                DS.can_control, DS.gravity, DS.fade_a, DS.load_stage);
    } else {
        return 0;
    }
    return 1;
}
