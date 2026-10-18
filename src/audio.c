/* Sound (see audio.h).
 *
 * Game thread: MusicSystem/MusicPlayer timing, AudioPlayer channels, 3D gains of sources.
 * Mixer (the platform's output thread on the Vita, the frame loop on PC): renders the song with
 * the SEQ synth, plays sources, applies the Master group's Duck Volume (a fast limiter just
 * under full scale). Shared state is only touched under plat_audio_lock. */
#include "audio.h"
#include "synth.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

#define MAX_BANKS 8
#define MAX_DREAMS 32
#define NSTYLES 7
#define MAX_SRC 192
#define MAX_2D 10          /* AudioPlayer.MaxChannels */
#define SE_RATE 11025.0f   /* LSDR imports VAB samples as 11025 Hz clips */
#define MIX_MAX 44100

static const char *const STYLE_NAMES[NSTYLES] = {"Standard", "Ambient", "Lovely", "Human",
                                                 "Electro", "Ethnova", "Cartoon"};
static const char BG_LETTERS[] = "ABCDE";

typedef struct {
    char name[16];
    SndBank bank;
    int state; /* 0 not loaded, 1 loaded, -1 failed */
} Bank;

typedef struct {
    int stage, bg, bank; /* stage < 0: a silent song (of stage -1 - stage, still with a name) */
} SongRef;

typedef struct {
    int n;
    SongRef *song;
} SongList;

static Bank g_banks[MAX_BANKS];
static int g_nbanks;
static SongList g_music[MAX_DREAMS][NSTYLES];
static int g_ndreams;
static int g_link_sample = -1;
static int g_foot_fallback = -1;
static float g_foot_fallback_pitch = 1;
static struct {
    int key, sample;
    float pitch;
} g_foot[64];
static int g_nfoot;

#define MAX_SEQ (MAX_DREAMS * 5)
static SndSeq g_seq[MAX_SEQ];
static int g_seq_state[MAX_SEQ];

static int g_ready;
static int g_output_on;

/* ------------------------------------------------------------------ shared with the mixer */

typedef struct {
    int used;
    /* game thread only */
    int spatial;
    float min_d, max_d, volume;
    vec3 pos;
    /* under the lock */
    const int16_t *pcm;
    int len;
    float step;     /* clip samples per output frame */
    int loop, playing;
    double at;      /* position in the clip */
    float gl, gr;   /* target gains */
    float cl, cr;   /* gains reached */
    uint32_t serial; /* bumped by the game on play/stop, so the mixer never writes back over a restart */
} Src;

static Src g_src[MAX_SRC];
static Src g_mixsrc[MAX_SRC]; /* the mixer's copy of the sources it is playing */
static int g_mixidx[MAX_SRC];

static struct {
    const SndSeq *seq;
    const SndBank *bank;
    const uint8_t *ogg; /* or an Ogg Vorbis song (the Revamped soundtrack) */
    int ogg_size;
    uint32_t gen;
    float vol;
    /* mixer only */
    uint32_t seen;
    float cur;
    int on;
    Sequencer q;
    stb_vorbis *vorb;
    int vrate, vch;
    float vbuf[4096 * 2]; /* decoded frames waiting (stereo) */
    int vn, vpos;
    double vfrac;         /* resampling position between vbuf[vpos] and the next frame */
} g_mus;

/* the Revamped soundtrack (music.bin, music/NNN.ogg) */
typedef struct {
    char name[64], author[64], file[16];
    int rate, ch;
} RevSong;
static RevSong *g_rev;
static int g_nrev;
static int g_use_original = 1;
static int g_last_number;               /* MusicSystem._lastDayNumber */
static char g_cur_author[64];
static uint8_t *g_ogg_next, *g_ogg_live; /* game side: the song waiting to play, the one playing */
static uint8_t *g_ogg_old;               /* replaced, freed once the mixer has moved on */
static uint32_t g_ogg_old_gen;

static vec3 g_lpos, g_lfwd = {0, 0, 1}, g_lright = {1, 0, 0};

/* test recording of the mixer output (under the lock) */
static int16_t *g_rec;
static int g_rec_len, g_rec_pos;
static char g_rec_name[48];

/* ------------------------------------------------------------------ game side: MusicPlayer */

enum { MP_IDLE, MP_PLAYING, MP_FADE, MP_FADE_THEN_PLAY, MP_WAIT };
static int g_mp;
static float g_mp_t;
static const SndSeq *g_next_seq;
static const SndBank *g_next_bank;
static char g_next_name[48], g_song_name[48];
static char g_cur_song[64]; /* MusicSystem.CurrentSong: set by NextSong, cleared by StopSong */
static int g_cur_dream = -1, g_cur_style;
static int g_ch2d[MAX_2D], g_nch2d;
static int g_warned_channels;

static Bank *bank_get(int i)
{
    if (i < 0 || i >= g_nbanks)
        return NULL;
    Bank *b = &g_banks[i];
    if (b->state == 0) {
        char ph[48], pb[48];
        snprintf(ph, sizeof(ph), "snd/%s.VH", b->name);
        snprintf(pb, sizeof(pb), "snd/%s.VB", b->name);
        size_t nh = 0, nb = 0;
        uint8_t *h = vfs_read(ph, &nh), *d = vfs_read(pb, &nb);
        double t0 = plat_seconds();
        b->state = h && d && snd_bank_load(&b->bank, h, nh, d, nb, !strcmp(b->name, "AMBIENT")) ? 1 : -1;
        free(h);
        free(d);
        if (b->state > 0)
            lsd_log("sound bank %s: %d samples, %d programs (%.0f ms)", b->name, b->bank.nsamp, b->bank.nprog,
                    (plat_seconds() - t0) * 1000.0);
        else
            lsd_log("sound bank %s failed to load", b->name);
    }
    return b->state > 0 ? b : NULL;
}

static const SndSeq *seq_get(int stage, int bg)
{
    int i = stage * 5 + bg;
    if (stage < 0 || stage >= MAX_DREAMS || bg < 0 || bg >= 5)
        return NULL;
    if (g_seq_state[i] == 0) {
        char p[32];
        snprintf(p, sizeof(p), "stg%02d/BG%c.SEQ", stage, BG_LETTERS[bg]);
        size_t n = 0;
        uint8_t *d = vfs_read(p, &n);
        g_seq_state[i] = d && snd_seq_load(&g_seq[i], d, n) ? 1 : -1;
        free(d);
        if (g_seq_state[i] < 0)
            lsd_log("song %s failed to load", p);
    }
    return g_seq_state[i] > 0 ? &g_seq[i] : NULL;
}

static void mus_publish_any(const SndSeq *seq, const SndBank *bank, const uint8_t *ogg, int ogg_size, float vol)
{
    plat_audio_lock();
    g_mus.seq = seq;
    g_mus.bank = bank;
    g_mus.ogg = ogg;
    g_mus.ogg_size = ogg_size;
    g_mus.vol = vol;
    g_mus.gen++;
    uint32_t gen = g_mus.gen;
    plat_audio_unlock();
    /* the Ogg data the mixer was reading goes once it has seen this */
    if (g_ogg_live && g_ogg_live != ogg) {
        free(g_ogg_old);
        g_ogg_old = g_ogg_live;
        g_ogg_old_gen = gen;
    }
    g_ogg_live = (uint8_t *)ogg;
}

static void mus_publish(const SndSeq *seq, const SndBank *bank, float vol)
{
    mus_publish_any(seq, bank, NULL, 0, vol);
}

static void mus_volume(float vol)
{
    plat_audio_lock();
    g_mus.vol = vol;
    plat_audio_unlock();
}

static int g_ogg_next_size;

/* MusicPlayer.PlaySong */
static int ogg_load_poll(int wait);

static void play_song(const SndSeq *seq, const SndBank *bank, const char *name)
{
    ogg_load_poll(1); /* (a Revamped song still being read: replaced) */
    if (g_ogg_next) { /* (a Revamped song still waiting: replaced) */
        free(g_ogg_next);
        g_ogg_next = NULL;
    }
    g_next_seq = seq;
    g_next_bank = bank;
    snprintf(g_next_name, sizeof(g_next_name), "%s", name);
    switch (g_mp) {
    case MP_PLAYING:
        g_mp = MP_FADE_THEN_PLAY;
        g_mp_t = 0;
        break;
    case MP_FADE:
        g_mp = MP_FADE_THEN_PLAY;
        break;
    case MP_FADE_THEN_PLAY:
        break;
    default:
        g_mp = MP_WAIT;
        g_mp_t = 0;
        break;
    }
}

/* a Revamped song is read off the card by a loading thread (a few MB: a third of a second or more),
 * while the last one fades out; it starts once it is in */
static struct {
    char path[48];
    uint8_t *data;
    size_t size;
    volatile int done;
    int task, busy;
} g_ogg_load;

static void ogg_load_job(void *arg)
{
    (void)arg;
    size_t n = 0;
    g_ogg_load.data = vfs_read_bg(g_ogg_load.path, &n);
    g_ogg_load.size = n;
    __atomic_store_n(&g_ogg_load.done, 1, __ATOMIC_RELEASE);
}

/* the song read in (or still coming): 1 when it has finished, its data then in g_ogg_next */
static int ogg_load_poll(int wait)
{
    if (!g_ogg_load.busy)
        return 1;
    if (!wait && !__atomic_load_n(&g_ogg_load.done, __ATOMIC_ACQUIRE))
        return 0;
    plat_task_join(g_ogg_load.task);
    g_ogg_load.busy = 0;
    if (g_ogg_next) free(g_ogg_next);
    g_ogg_next = g_ogg_load.data;
    g_ogg_next_size = (int)g_ogg_load.size;
    g_ogg_load.data = NULL;
    if (!g_ogg_next)
        lsd_log("music: %s could not be read (silent)", g_ogg_load.path);
    return 1;
}

/* the Revamped library (revampedSongLibraryScript.lua): one in eight days silent, otherwise any song */
static void pick_revamped(void)
{
    if (g_nrev <= 0 || rng_int(8) == 0) {
        g_cur_song[0] = g_cur_author[0] = 0;
        lsd_log("music: Revamped soundtrack, no song");
        play_song(NULL, NULL, "");
        return;
    }
    const RevSong *r = &g_rev[rng_int(g_nrev)];
    snprintf(g_cur_song, sizeof(g_cur_song), "%s", r->name);
    snprintf(g_cur_author, sizeof(g_cur_author), "%s", r->author);
    lsd_log("music: Revamped soundtrack: %s - %s%s", r->author, r->name, r->file[0] ? "" : " (missing: silent)");
    play_song(NULL, NULL, r->name);
    if (!r->file[0])
        return;
    ogg_load_poll(1); /* (one read at a time) */
    if (g_ogg_next) {
        free(g_ogg_next);
        g_ogg_next = NULL;
    }
    snprintf(g_ogg_load.path, sizeof(g_ogg_load.path), "music/%s", r->file);
    g_ogg_load.data = NULL;
    g_ogg_load.size = 0;
    g_ogg_load.done = 0;
    g_ogg_load.busy = 1;
    g_ogg_load.task = plat_task_start(ogg_load_job, NULL, 1);
}

static void pick_song(int dream, int style, int number)
{
    g_last_number = number;
    if (!g_use_original) {
        pick_revamped();
        return;
    }
    if (dream < 0 || g_ndreams <= 0)
        return;
    int dn = dream % g_ndreams; /* OriginalSongLibrary.DreamNumber */
    const SongList *l = &g_music[dn][style];
    if (l->n <= 0)
        return;
    const SongRef *s = &l->song[number % l->n];
    char name[64];
    const SndSeq *seq = NULL;
    const Bank *b = NULL;
    if (s->stage >= 0) {
        seq = seq_get(s->stage, s->bg);
        b = bank_get(s->bank);
        snprintf(name, sizeof(name), "STG%02d BG%c-%s", s->stage, BG_LETTERS[s->bg], b ? b->name : "?");
        snprintf(g_cur_song, sizeof(g_cur_song), "%s", name);
        snprintf(g_cur_author, sizeof(g_cur_author), "OutSide Directors Company");
    } else {
        snprintf(g_cur_author, sizeof(g_cur_author), "OutSide Directors Company");
        /* a song without a clip: it plays nothing, but the pause menu still names it */
        if (s->bg >= 0 && s->bg < 5 && s->bank >= 0 && s->bank < g_nbanks)
            snprintf(g_cur_song, sizeof(g_cur_song), "STG%02d BG%c-%s", -1 - s->stage, BG_LETTERS[s->bg],
                     g_banks[s->bank].name);
        else
            g_cur_song[0] = 0;
        snprintf(name, sizeof(name), "(silent: %.48s)", g_cur_song);
    }
    lsd_log("music: %s style, song %d of %d: %s", STYLE_NAMES[style], number % l->n + 1, l->n, name);
    play_song(seq && b ? seq : NULL, b ? &b->bank : NULL, name);
}

void music_dream_song(int dream, int day, int last_gx, int last_gy)
{
    if (!g_ready)
        return;
    int style = ((day - 1) % NSTYLES + NSTYLES) % NSTYLES;
    int number = day == 1 ? 2 : (last_gy + 9) * 19 + (last_gx + 9);
    if (number < 0) number = 0;
    g_cur_dream = dream;
    g_cur_style = style;
    pick_song(dream, style, number);
}

void music_force(int stage, int bg, int bank)
{
    if (!g_ready)
        return;
    const SndSeq *seq = seq_get(stage, bg);
    Bank *b = bank_get(bank);
    if (!seq || !b)
        return;
    char name[48];
    snprintf(name, sizeof(name), "STG%02d BG%c-%s", stage, BG_LETTERS[bg], b->name);
    snprintf(g_cur_song, sizeof(g_cur_song), "%s", name);
    lsd_log("music: %s (forced)", name);
    play_song(seq, &b->bank, name);
}

void music_skip(void)
{
    if (g_ready && g_cur_dream >= 0)
        pick_song(g_cur_dream, g_cur_style, rng_int(100));
}

/* MusicPlayer.StopSong */
void music_stop(void)
{
    if (!g_ready)
        return;
    g_cur_song[0] = 0;
    switch (g_mp) {
    case MP_PLAYING:
        g_mp = MP_FADE;
        g_mp_t = 0;
        break;
    case MP_FADE_THEN_PLAY:
        g_mp = MP_FADE;
        break;
    case MP_WAIT:
        g_mp = MP_IDLE;
        break;
    default:
        break;
    }
}

const char *music_name(void) { return g_mp == MP_PLAYING || g_mp == MP_FADE ? g_song_name : ""; }
const char *music_current_song(void) { return g_cur_song[0] ? g_cur_song : NULL; }
const char *music_current_author(void) { return g_cur_author; }

void music_use_original_songs(int on)
{
    on = on != 0;
    int change = g_mp == MP_PLAYING && g_use_original != on; /* MusicPlayer.IsPlaying */
    g_use_original = on;
    if (change && g_cur_dream >= 0)
        pick_song(g_cur_dream, g_cur_style, g_last_number);
}

static void music_update(float dt)
{
    if (g_ogg_old) {
        plat_audio_lock();
        int done = (int32_t)(g_mus.seen - g_ogg_old_gen) >= 0;
        plat_audio_unlock();
        if (done) {
            free(g_ogg_old);
            g_ogg_old = NULL;
        }
    }
    switch (g_mp) {
    case MP_FADE:
    case MP_FADE_THEN_PLAY:
        g_mp_t += dt;
        if (g_mp_t >= 1.0f) {
            mus_publish(NULL, NULL, 0); /* AudioSource.Stop */
            g_song_name[0] = 0;
            if (g_mp == MP_FADE) {
                g_mp = MP_IDLE;
            } else {
                g_mp = MP_WAIT;
                g_mp_t = 0;
            }
        } else {
            mus_volume(1.0f - g_mp_t);
        }
        break;
    case MP_WAIT:
        g_mp_t += dt;
        if (g_mp_t >= 0.5f && ogg_load_poll(0)) { /* WaitForSecondsRealtime(0.5f) (and the song read in) */
            if (g_next_seq) {
                mus_publish(g_next_seq, g_next_bank, 1.0f);
                snprintf(g_song_name, sizeof(g_song_name), "%s", g_next_name);
                g_mp = MP_PLAYING;
            } else if (g_ogg_next) {
                mus_publish_any(NULL, NULL, g_ogg_next, g_ogg_next_size, 1.0f);
                g_ogg_next = NULL;
                snprintf(g_song_name, sizeof(g_song_name), "%s", g_next_name);
                g_mp = MP_PLAYING;
            } else {
                g_mp = MP_IDLE; /* a silent song */
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ game side: sources */

int src_new(void)
{
    for (int i = 0; i < MAX_SRC; i++) {
        if (g_src[i].used)
            continue;
        plat_audio_lock();
        uint32_t serial = g_src[i].serial + 1;
        memset(&g_src[i], 0, sizeof(Src));
        g_src[i].serial = serial;
        g_src[i].used = 1;
        g_src[i].volume = 1;
        plat_audio_unlock();
        return i;
    }
    return -1;
}

void src_free(int h)
{
    if (h < 0 || h >= MAX_SRC)
        return;
    plat_audio_lock();
    g_src[h].playing = 0;
    g_src[h].used = 0;
    g_src[h].serial++;
    plat_audio_unlock();
}

static void gains(const Src *s, float *gl, float *gr)
{
    if (!s->spatial) {
        *gl = *gr = s->volume;
        return;
    }
    /* linear rolloff between min and max distance, panned by direction (spread 0) */
    vec3 d = v3sub(s->pos, g_lpos);
    float dist = v3len(d);
    float att;
    if (dist <= s->min_d) att = 1;
    else if (dist >= s->max_d) att = 0;
    else att = (s->max_d - dist) / (s->max_d - s->min_d);
    float pan = dist > 1e-4f ? v3dot(d, g_lright) / dist : 0;
    if (pan > 1) pan = 1;
    if (pan < -1) pan = -1;
    float th = (pan + 1) * 0.78539816f;
    float l = 1.41421356f * cosf(th), r = 1.41421356f * sinf(th);
    *gl = s->volume * att * (l > 1 ? 1 : l);
    *gr = s->volume * att * (r > 1 ? 1 : r);
}

static void src_play_pcm(int h, const int16_t *pcm, int len, int rate, float pitch, float volume, int loop)
{
    Src *s = &g_src[h];
    s->volume = volume;
    float gl, gr;
    gains(s, &gl, &gr);
    if (pitch > 3) pitch = 3; /* AudioSource.pitch range */
    if (pitch < 0) pitch = 0;
    plat_audio_lock();
    s->pcm = pcm;
    s->len = len;
    s->step = (float)rate * pitch / 44100.0f;
    s->loop = loop;
    s->at = 0;
    s->playing = 1;
    s->serial++;
    s->gl = s->cl = gl;
    s->gr = s->cr = gr;
    plat_audio_unlock();
}

void src_play(int h, int se_sample, float pitch, float volume, int loop)
{
    if (h < 0 || h >= MAX_SRC || !g_src[h].used)
        return;
    Bank *se = bank_get(0);
    if (!se || se_sample < 0 || se_sample >= se->bank.nsamp)
        return;
    src_play_pcm(h, se->bank.samp[se_sample].pcm, se->bank.samp[se_sample].len, SE_RATE, pitch, volume, loop);
}

/* ------------------------------------------------------------------ clips (the title screen's sounds) */

#define MAX_CLIPS 16
static struct {
    int16_t *pcm;
    int frames, rate, ch;
    char name[32];
} g_clips[MAX_CLIPS];
static int g_nclips;

int audio_clip_load(const char *path)
{
    for (int i = 0; i < g_nclips; i++)
        if (!strcmp(g_clips[i].name, path))
            return i;
    size_t n = 0;
    uint8_t *buf = g_nclips < MAX_CLIPS ? vfs_read(path, &n) : NULL;
    return audio_clip_add(path, buf, n);
}

/* a clip from a file already read (buf is freed) */
int audio_clip_add(const char *path, uint8_t *buf, size_t n)
{
    for (int i = 0; i < g_nclips; i++)
        if (!strcmp(g_clips[i].name, path)) {
            free(buf);
            return i;
        }
    if (g_nclips >= MAX_CLIPS) {
        free(buf);
        return -1;
    }
    if (!buf)
        return -1;
    uint32_t h[3] = {0, 0, 0};
    if (n >= 12) memcpy(h, buf, 12);
    if (n < 12 || !h[0] || h[0] > 96000 || (h[1] != 1 && h[1] != 2) || n != 12 + (size_t)h[2] * h[1] * 2) {
        lsd_log("%s: not a sound clip", path);
        free(buf);
        return -1;
    }
    int i = g_nclips++;
    g_clips[i].pcm = (int16_t *)malloc((size_t)h[2] * h[1] * 2 + 2);
    if (!g_clips[i].pcm) {
        g_nclips--;
        free(buf);
        return -1;
    }
    memcpy(g_clips[i].pcm, buf + 12, (size_t)h[2] * h[1] * 2);
    g_clips[i].frames = (int)h[2];
    g_clips[i].rate = (int)h[0];
    g_clips[i].ch = (int)h[1];
    snprintf(g_clips[i].name, sizeof(g_clips[i].name), "%s", path);
    free(buf);
    return i;
}

/* AudioPlayer.PlayClip of a mono clip (SFX group) */
void sfx_play_clip(int clip, float pitch)
{
    if (!g_ready || clip < 0 || clip >= g_nclips || g_clips[clip].ch != 1)
        return;
    int ch = -1;
    for (int i = 0; i < g_nch2d && ch < 0; i++)
        if (!src_playing(g_ch2d[i]))
            ch = g_ch2d[i];
    if (ch < 0) {
        if (g_nch2d < MAX_2D && (ch = src_new()) >= 0)
            g_ch2d[g_nch2d++] = ch;
        else
            return;
    }
    g_src[ch].spatial = 0;
    src_play_pcm(ch, g_clips[clip].pcm, g_clips[clip].frames, g_clips[clip].rate, pitch, 1.0f, 0);
}

/* a looping stereo clip at 44.1 kHz (the title screen's music) */
static const int16_t *g_bgm;
static int g_bgm_frames, g_bgm_pos;

void audio_bgm_play(int clip)
{
    const int16_t *p = NULL;
    int n = 0;
    if (clip >= 0 && clip < g_nclips && g_clips[clip].ch == 2 && g_clips[clip].rate == 44100) {
        p = g_clips[clip].pcm;
        n = g_clips[clip].frames;
    }
    plat_audio_lock();
    g_bgm = p;
    g_bgm_frames = n;
    g_bgm_pos = 0;
    plat_audio_unlock();
}

void audio_bgm_stop(void) { audio_bgm_play(-1); }

void src_stop(int h)
{
    if (h < 0 || h >= MAX_SRC)
        return;
    plat_audio_lock();
    g_src[h].playing = 0;
    g_src[h].serial++;
    plat_audio_unlock();
}

int src_playing(int h)
{
    if (h < 0 || h >= MAX_SRC)
        return 0;
    plat_audio_lock();
    int p = g_src[h].playing;
    plat_audio_unlock();
    return p;
}

void src_place(int h, int spatial, float min_dist, float max_dist, vec3 pos)
{
    if (h < 0 || h >= MAX_SRC)
        return;
    g_src[h].spatial = spatial;
    g_src[h].min_d = min_dist;
    g_src[h].max_d = max_dist;
    g_src[h].pos = pos;
}

/* AudioPlayer.PlayClip */
void sfx_play(int se_sample, float pitch)
{
    if (!g_ready)
        return;
    int ch = -1;
    for (int i = 0; i < g_nch2d && ch < 0; i++)
        if (!src_playing(g_ch2d[i]))
            ch = g_ch2d[i];
    if (ch < 0) {
        if (g_nch2d < MAX_2D && (ch = src_new()) >= 0) {
            g_ch2d[g_nch2d++] = ch;
        } else {
            if (!g_warned_channels++)
                lsd_log("sound: no free channel for SE%d", se_sample);
            return;
        }
    }
    g_src[ch].spatial = 0;
    src_play(ch, se_sample, pitch, 1.0f, 0);
}

int sfx_link_sample(void) { return g_link_sample; }

int sfx_footstep(int key, int *se_sample, float *pitch)
{
    for (int i = 0; i < g_nfoot; i++)
        if (g_foot[i].key == key) {
            *se_sample = g_foot[i].sample;
            *pitch = g_foot[i].pitch;
            return g_foot[i].sample >= 0;
        }
    return sfx_footstep_fallback(se_sample, pitch);
}

int sfx_footstep_fallback(int *se_sample, float *pitch)
{
    *se_sample = g_foot_fallback;
    *pitch = g_foot_fallback_pitch;
    return g_foot_fallback >= 0;
}

void audio_listener(vec3 pos, vec3 fwd, vec3 right)
{
    g_lpos = pos;
    g_lfwd = fwd;
    g_lright = right;
}

void audio_update(float dt)
{
    if (!g_ready)
        return;
    music_update(dt);
    float gl[MAX_SRC], gr[MAX_SRC];
    for (int i = 0; i < MAX_SRC; i++)
        if (g_src[i].used)
            gains(&g_src[i], &gl[i], &gr[i]);
    plat_audio_lock();
    for (int i = 0; i < MAX_SRC; i++)
        if (g_src[i].used) {
            g_src[i].gl = gl[i];
            g_src[i].gr = gr[i];
        }
    int done = g_rec && g_rec_pos >= g_rec_len;
    plat_audio_unlock();
    plat_audio_pump(dt);
    if (done) {
        prof_mark_test(); /* (a test recording written out) */
        char path[160];
        snprintf(path, sizeof(path), "%s/%s.wav", plat_shot_dir(), g_rec_name);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t data = (uint32_t)g_rec_len * 4, v;
            uint16_t h;
            fwrite("RIFF", 1, 4, f);
            v = 36 + data;
            fwrite(&v, 4, 1, f);
            fwrite("WAVEfmt ", 1, 8, f);
            v = 16;
            fwrite(&v, 4, 1, f);
            h = 1;
            fwrite(&h, 2, 1, f);
            h = 2;
            fwrite(&h, 2, 1, f);
            v = 44100;
            fwrite(&v, 4, 1, f);
            v = 44100 * 4;
            fwrite(&v, 4, 1, f);
            h = 4;
            fwrite(&h, 2, 1, f);
            h = 16;
            fwrite(&h, 2, 1, f);
            fwrite("data", 1, 4, f);
            fwrite(&data, 4, 1, f);
            fwrite(g_rec, 4, (size_t)g_rec_len, f);
            fclose(f);
            lsd_log("recorded %.1f s of sound to %s", g_rec_len / 44100.0, path);
        } else {
            lsd_log("cannot write %s", path);
        }
        plat_audio_lock();
        free(g_rec);
        g_rec = NULL;
        plat_audio_unlock();
    }
}

void audio_record(const char *name, float seconds)
{
    if (!g_ready || g_rec || seconds <= 0 || seconds > 30)
        return;
    int len = (int)(seconds * 44100);
    int16_t *buf = (int16_t *)malloc((size_t)len * 4);
    if (!buf)
        return;
    plat_audio_lock();
    g_rec = buf;
    g_rec_len = len;
    g_rec_pos = 0;
    snprintf(g_rec_name, sizeof(g_rec_name), "%s", name);
    plat_audio_unlock();
}

void audio_log_stats(void)
{
    if (!g_ready)
        return;
    int playing = 0;
    for (int i = 0; i < MAX_SRC; i++)
        playing += g_src[i].used && g_src[i].playing;
    lsd_log("audio: mixer %.1f%% of a core, song %s (%d voices at most), %d sounds playing, %.1f ms lock waits",
            plat_audio_load() * 100.0, g_song_name[0] ? g_song_name : "-", g_mus.q.voices_peak, playing,
            plat_audio_lock_wait_ms());
}

/* ------------------------------------------------------------------ the mixer */

static int32_t g_acc[MIX_MAX * 2];
static int16_t g_mbuf[MIX_MAX * 2];

/* a video's sound (VideoPlayer, audio output mode Direct: not through the mixer groups) */
#define STREAM_CAP 32768 /* frames, power of two */
static int16_t g_stream[STREAM_CAP * 2];
static unsigned g_s_r, g_s_w; /* read / write positions in frames (wrapping) */
static double g_s_step = 1.0, g_s_frac; /* frames read an output frame (a video's speed), and the part read */
static float g_s_prev[2];               /* the frame before the read position (for the interpolation) */
static int g_s_expect;                  /* a video's sound is still coming */
static unsigned g_s_dry;                /* output frames the stream was empty while sound was still coming */

void audio_stream_expect(int on)
{
    plat_audio_lock();
    g_s_expect = on;
    plat_audio_unlock();
}

unsigned audio_stream_dry(void)
{
    plat_audio_lock();
    unsigned n = g_s_dry;
    plat_audio_unlock();
    return n;
}

/* drop up to n queued frames (a video's sound that fell behind its pictures catches up) */
int audio_stream_skip(int n)
{
    plat_audio_lock();
    int have = (int)(g_s_w - g_s_r);
    if (n > have) n = have;
    if (n > 0) {
        g_s_r += (unsigned)n;
        unsigned k = (g_s_r - 1) & (STREAM_CAP - 1);
        g_s_prev[0] = g_stream[k * 2];
        g_s_prev[1] = g_stream[k * 2 + 1];
    }
    plat_audio_unlock();
    return n > 0 ? n : 0;
}

void audio_stream_set_step(double step)
{
    plat_audio_lock();
    g_s_step = step > 0.5 && step < 2.0 ? step : 1.0;
    plat_audio_unlock();
}

void audio_stream_push(const float *lr, int frames)
{
    plat_audio_lock();
    for (int i = 0; i < frames && g_s_w - g_s_r < STREAM_CAP; i++) {
        unsigned k = g_s_w & (STREAM_CAP - 1);
        for (int c = 0; c < 2; c++) {
            float v = lr[i * 2 + c] * 32767.0f;
            g_stream[k * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        g_s_w++;
    }
    plat_audio_unlock();
}

void audio_stream_clear(void)
{
    plat_audio_lock();
    g_s_r = g_s_w = 0;
    g_s_step = 1.0;
    g_s_frac = 0;
    g_s_prev[0] = g_s_prev[1] = 0;
    g_s_expect = 0;
    g_s_dry = 0;
    plat_audio_unlock();
}

unsigned audio_stream_played(void)
{
    plat_audio_lock();
    unsigned n = g_s_r;
    plat_audio_unlock();
    return n;
}

int audio_stream_queued(void)
{
    plat_audio_lock();
    int n = (int)(g_s_w - g_s_r);
    plat_audio_unlock();
    return n;
}

/* the Ogg song into 44.1 kHz stereo (looped, as MusicPlayer's AudioSource.loop), resampled linearly */
static int ogg_more(void)
{
    /* keep from the frame being read (or the last one), then decode after it */
    int keep = g_mus.vpos < g_mus.vn ? g_mus.vpos : g_mus.vn - 1;
    if (keep > 0) {
        memmove(g_mus.vbuf, g_mus.vbuf + keep * 2, (size_t)(g_mus.vn - keep) * 2 * sizeof(float));
        g_mus.vn -= keep;
        g_mus.vpos -= keep;
    }
    int room = 4096 - g_mus.vn;
    if (room > 2048) room = 2048;
    for (int tries = 0; tries < 2; tries++) {
        int got;
        if (g_mus.vch == 1) {
            float mono[2048];
            got = stb_vorbis_get_samples_float_interleaved(g_mus.vorb, 1, mono, room);
            for (int i = 0; i < got; i++)
                g_mus.vbuf[(g_mus.vn + i) * 2] = g_mus.vbuf[(g_mus.vn + i) * 2 + 1] = mono[i];
        } else {
            got = stb_vorbis_get_samples_float_interleaved(g_mus.vorb, 2, g_mus.vbuf + g_mus.vn * 2, room * 2);
        }
        if (got > 0) {
            g_mus.vn += got;
            return got;
        }
        stb_vorbis_seek_start(g_mus.vorb); /* the end: loop */
    }
    return 0;
}

static void ogg_render(int16_t *out, int frames)
{
    double step = g_mus.vrate > 0 ? (double)g_mus.vrate / 44100.0 : 1.0;
    for (int i = 0; i < frames; i++) {
        while (g_mus.vpos + 1 >= g_mus.vn) {
            if (!ogg_more()) {
                memset(out + i * 2, 0, (size_t)(frames - i) * 4);
                return;
            }
        }
        const float *a = g_mus.vbuf + g_mus.vpos * 2, *b = a + 2;
        float f = (float)g_mus.vfrac;
        for (int c = 0; c < 2; c++) {
            float v = (a[c] + (b[c] - a[c]) * f) * 32767.0f;
            out[i * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        g_mus.vfrac += step;
        int adv = (int)g_mus.vfrac;
        g_mus.vfrac -= adv;
        g_mus.vpos += adv;
    }
}

/* the Music and SFX mixer groups' volumes (GameSettings.MusicVolume / SFXVolume; 20 log10 v dB = v) */
static volatile float g_vol_music = 1.0f, g_vol_sfx = 1.0f;

void audio_set_volumes(float music, float sfx)
{
    g_vol_music = music < 0 ? 0 : music > 1 ? 1 : music;
    g_vol_sfx = sfx < 0 ? 0 : sfx > 1 ? 1 : sfx;
}

static void mix(int16_t *out, int frames)
{
    if (frames > MIX_MAX) frames = MIX_MAX;
    const float vm = g_vol_music, vs = g_vol_sfx;
    /* the song: a new one is set up without holding the lock (opening an Ogg song takes a while, and
     * the game thread takes the lock every frame); its data stays until the mixer has said it has
     * seen the change */
    plat_audio_lock();
    int changed = g_mus.gen != g_mus.seen;
    uint32_t gen = g_mus.gen;
    const SndSeq *seq = g_mus.seq;
    const SndBank *bank = g_mus.bank;
    const uint8_t *ogg = g_mus.ogg;
    int ogg_size = g_mus.ogg_size;
    plat_audio_unlock();
    if (changed) {
        if (g_mus.vorb) {
            stb_vorbis_close(g_mus.vorb);
            g_mus.vorb = NULL;
        }
        int on = seq && bank;
        if (on) {
            seqr_start(&g_mus.q, seq, bank);
            g_mus.q.voices_peak = 0;
        } else if (ogg) {
            int err = 0;
            g_mus.vorb = stb_vorbis_open_memory(ogg, ogg_size, &err, NULL);
            if (g_mus.vorb) {
                stb_vorbis_info vi = stb_vorbis_get_info(g_mus.vorb);
                g_mus.vrate = (int)vi.sample_rate;
                g_mus.vch = vi.channels;
                g_mus.vn = g_mus.vpos = 0;
                g_mus.vfrac = 0;
                on = 2;
            }
        }
        plat_audio_lock();
        g_mus.on = on;
        g_mus.seen = gen;
        g_mus.cur = g_mus.vol;
        plat_audio_unlock();
    }
    plat_audio_lock();
    int on = g_mus.on;
    float v0 = g_mus.cur, v1 = g_mus.vol;
    g_mus.cur = v1;
    plat_audio_unlock();
    if (on == 2) {
        ogg_render(g_mbuf, frames);
        float dv = (v1 - v0) / frames;
        for (int i = 0; i < frames; i++) {
            float v = (v0 + dv * (i + 1)) * vm;
            g_acc[i * 2] = (int32_t)(g_mbuf[i * 2] * v);
            g_acc[i * 2 + 1] = (int32_t)(g_mbuf[i * 2 + 1] * v);
        }
    } else if (on) {
        seqr_render(&g_mus.q, g_mbuf, frames);
        float dv = (v1 - v0) / frames;
        for (int i = 0; i < frames; i++) {
            float v = (v0 + dv * (i + 1)) * vm;
            g_acc[i * 2] = (int32_t)(g_mbuf[i * 2] * v);
            g_acc[i * 2 + 1] = (int32_t)(g_mbuf[i * 2 + 1] * v);
        }
    } else {
        memset(g_acc, 0, (size_t)frames * 2 * sizeof(int32_t));
    }
    /* the title music (under the lock: a short loop over a few frames) */
    plat_audio_lock();
    if (g_bgm && g_bgm_frames > 0) {
        for (int i = 0; i < frames; i++) {
            g_acc[i * 2] += (int32_t)(g_bgm[g_bgm_pos * 2] * vm);
            g_acc[i * 2 + 1] += (int32_t)(g_bgm[g_bgm_pos * 2 + 1] * vm);
            if (++g_bgm_pos >= g_bgm_frames) g_bgm_pos = 0;
        }
    }
    plat_audio_unlock();
    /* sound effects: copied under the lock, mixed without it, positions written back after */
    int nmix = 0;
    plat_audio_lock();
    for (int k = 0; k < MAX_SRC; k++) {
        const Src *s = &g_src[k];
        if (!s->used || !s->playing || !s->pcm || s->len <= 0)
            continue;
        g_mixsrc[nmix] = *s;
        g_mixidx[nmix++] = k;
    }
    plat_audio_unlock();
    for (int m = 0; m < nmix; m++) {
        Src *s = &g_mixsrc[m];
        const double step = s->step;
        double at = s->at;
        if (s->cl == 0 && s->cr == 0 && s->gl == 0 && s->gr == 0) {
            /* out of earshot: only time passes */
            at += step * frames;
            if (at >= s->len) {
                if (s->loop) at = fmod(at, (double)s->len);
                else s->playing = 0;
            }
            s->at = at;
            continue;
        }
        float l0 = s->cl * vs, r0 = s->cr * vs, dl = (s->gl - s->cl) * vs / frames, dr = (s->gr - s->cr) * vs / frames;
        for (int i = 0; i < frames; i++) {
            int ia = (int)at;
            if (ia >= s->len) {
                if (!s->loop) {
                    s->playing = 0;
                    break;
                }
                at = fmod(at, (double)s->len);
                ia = (int)at;
            }
            float fr = (float)(at - ia);
            int ib = ia + 1 < s->len ? ia + 1 : (s->loop ? 0 : ia);
            float v = s->pcm[ia] + (s->pcm[ib] - s->pcm[ia]) * fr;
            g_acc[i * 2] += (int32_t)(v * (l0 + dl * (i + 1)));
            g_acc[i * 2 + 1] += (int32_t)(v * (r0 + dr * (i + 1)));
            at += step;
        }
        s->at = at;
        s->cl = s->gl;
        s->cr = s->gr;
    }
    plat_audio_lock();
    for (int m = 0; m < nmix; m++) {
        Src *d = &g_src[g_mixidx[m]];
        const Src *s = &g_mixsrc[m];
        if (d->serial != s->serial)
            continue; /* restarted or stopped meanwhile */
        d->at = s->at;
        d->playing = s->playing;
        d->cl = s->cl;
        d->cr = s->cr;
    }
    plat_audio_unlock();
    /* Master: Duck Volume (threshold -0.7 dB, ratio 10, no attack or release) */
    const int32_t th = 30231;
    for (int i = 0; i < frames; i++) {
        int32_t l = g_acc[i * 2], r = g_acc[i * 2 + 1];
        int32_t pk = abs(l) > abs(r) ? abs(l) : abs(r);
        if (pk > th) {
            float g = (th + (pk - th) * 0.1f) / (float)pk;
            l = (int32_t)(l * g);
            r = (int32_t)(r * g);
        }
        out[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    plat_audio_lock();
    int mixed = 0;
    for (int i = 0; i < frames && g_s_r != g_s_w; i++, mixed++) {
        unsigned avail = g_s_w - g_s_r;
        unsigned k1 = g_s_r & (STREAM_CAP - 1), k2 = (g_s_r + 1) & (STREAM_CAP - 1), k3 = (g_s_r + 2) & (STREAM_CAP - 1);
        float f = (float)g_s_frac; /* (0 at speed 1: the samples as they are) */
        for (int c = 0; c < 2; c++) {
            /* (between frames: a cubic through the four around, which keeps the high notes) */
            float y0 = g_s_prev[c], y1 = g_stream[k1 * 2 + c];
            float y2 = avail > 1 ? g_stream[k2 * 2 + c] : y1, y3 = avail > 2 ? g_stream[k3 * 2 + c] : y2;
            float y = y1;
            if (f > 0.0f) {
                float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                y = ((c3 * f + c2) * f + c1) * f + y1;
            }
            int32_t v = out[i * 2 + c] + (int32_t)(y * vm); /* (VideoPlayerVolumeSetter) */
            out[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        g_s_frac += g_s_step;
        unsigned adv = (unsigned)g_s_frac;
        g_s_frac -= adv;
        for (; adv > 0 && g_s_r != g_s_w; adv--, g_s_r++) {
            unsigned k = g_s_r & (STREAM_CAP - 1);
            g_s_prev[0] = g_stream[k * 2];
            g_s_prev[1] = g_stream[k * 2 + 1];
        }
    }
    if (g_s_expect && g_s_w > 0) { /* (the video's sound ran out before its end: it was not decoded in time) */
        g_s_dry += (unsigned)(frames - mixed);
    }
    if (g_rec && g_rec_pos < g_rec_len) {
        int n = g_rec_len - g_rec_pos < frames ? g_rec_len - g_rec_pos : frames;
        memcpy(g_rec + (size_t)g_rec_pos * 2, out, (size_t)n * 4);
        g_rec_pos += n;
    }
    plat_audio_unlock();
}

/* ------------------------------------------------------------------ setup */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} Cur;

static uint32_t u32(Cur *c)
{
    uint32_t v = 0;
    if (c->end - c->p < 4) {
        c->bad = 1;
        return 0;
    }
    memcpy(&v, c->p, 4);
    c->p += 4;
    return v;
}

static float f32(Cur *c)
{
    uint32_t u = u32(c);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* music.bin (tools/musicdata.py): the Revamped soundtrack's songs */
static void load_revamped(void)
{
    size_t size = 0;
    uint8_t *d = vfs_read_quiet("music.bin", &size);
    if (!d) {
        lsd_log("music.bin missing: no Revamped soundtrack");
        return;
    }
    Cur c = {d, d + size, 0};
    if (size < 12 || memcmp(d, "LSDM", 4)) c.bad = 1;
    c.p += 4;
    if (u32(&c) != 1) c.bad = 1;
    int n = (int)u32(&c);
    if (n < 0 || n > 4096 || (size_t)(c.end - c.p) < (size_t)n * 152) c.bad = 1;
    if (!c.bad && (g_rev = (RevSong *)calloc((size_t)(n ? n : 1), sizeof(RevSong)))) {
        for (int i = 0; i < n; i++) {
            RevSong *r = &g_rev[i];
            memcpy(r->name, c.p, 63);
            memcpy(r->author, c.p + 64, 63);
            c.p += 128;
            r->rate = (int)u32(&c);
            r->ch = (int)u32(&c);
            memcpy(r->file, c.p, 15);
            c.p += 16;
        }
        g_nrev = n;
        lsd_log("Revamped soundtrack: %d songs", n);
    } else {
        lsd_log("music.bin is damaged: no Revamped soundtrack");
    }
    free(d);
}

int audio_init(void)
{
    load_revamped();
    size_t size = 0;
    uint8_t *data = vfs_read("audio.bin", &size);
    if (!data) {
        lsd_log("audio.bin missing: no sound");
        return 0;
    }
    Cur c = {data, data + size, 0};
    if (size < 8 || memcmp(data, "LSDA", 4)) c.bad = 1;
    c.p += 4;
    if (u32(&c) != 1) c.bad = 1;
    g_nbanks = (int)u32(&c);
    if (g_nbanks > MAX_BANKS) c.bad = 1;
    for (int i = 0; i < g_nbanks && !c.bad; i++) {
        if (c.end - c.p < 16) {
            c.bad = 1;
            break;
        }
        memcpy(g_banks[i].name, c.p, 15);
        g_banks[i].name[15] = 0;
        c.p += 16;
    }
    g_ndreams = (int)u32(&c);
    if (g_ndreams > MAX_DREAMS) c.bad = 1;
    for (int d = 0; d < g_ndreams && !c.bad; d++)
        for (int s = 0; s < NSTYLES && !c.bad; s++) {
            uint32_t n = u32(&c);
            if (n > 64) {
                c.bad = 1;
                break;
            }
            SongList *l = &g_music[d][s];
            l->song = (SongRef *)calloc(n ? n : 1, sizeof(SongRef));
            l->n = (int)n;
            for (uint32_t k = 0; k < n && l->song; k++) {
                l->song[k].stage = (int32_t)u32(&c);
                l->song[k].bg = (int32_t)u32(&c);
                l->song[k].bank = (int32_t)u32(&c);
            }
        }
    g_link_sample = (int32_t)u32(&c);
    g_foot_fallback = (int32_t)u32(&c);
    g_foot_fallback_pitch = f32(&c);
    g_nfoot = (int)u32(&c);
    if (g_nfoot > 64) c.bad = 1;
    for (int i = 0; i < g_nfoot && !c.bad; i++) {
        g_foot[i].key = (int32_t)u32(&c);
        g_foot[i].sample = (int32_t)u32(&c);
        g_foot[i].pitch = f32(&c);
    }
    free(data);
    if (c.bad || !bank_get(0)) {
        lsd_log("audio.bin is damaged or the SE bank is missing: no sound");
        g_nbanks = g_ndreams = g_nfoot = 0;
        return 0;
    }
    for (int i = 0; i < MAX_SRC; i++)
        g_src[i].used = 0;
    g_ready = 1;
    plat_audio_start(mix);
    g_output_on = 1;
    lsd_log("audio ready: %d sound banks, soundtrack for %d dreams", g_nbanks, g_ndreams);
    return 1;
}

void audio_output(int on)
{
    if (!g_ready || on == g_output_on)
        return;
    if (on) plat_audio_start(mix);
    else plat_audio_stop();
    g_output_on = on;
    lsd_log("sound output %s", on ? "on" : "off");
}

void audio_shutdown(void)
{
    if (!g_ready)
        return;
    ogg_load_poll(1);
    plat_audio_stop();
    g_ready = 0;
    for (int i = 0; i < g_nbanks; i++)
        if (g_banks[i].state > 0)
            snd_bank_free(&g_banks[i].bank);
    for (int i = 0; i < MAX_SEQ; i++)
        if (g_seq_state[i] > 0)
            snd_seq_free(&g_seq[i]);
    for (int d = 0; d < MAX_DREAMS; d++)
        for (int s = 0; s < NSTYLES; s++)
            free(g_music[d][s].song);
    memset(g_banks, 0, sizeof(g_banks));
    memset(g_seq_state, 0, sizeof(g_seq_state));
    memset(g_music, 0, sizeof(g_music));
}
