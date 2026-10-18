/* Video playback (LSDR's VideoPlayer in the video_dream scene): the original game's clips,
 * converted to MPEG-1 + MP2 by tools/videodata.py and decoded with pl_mpeg.
 *
 * Decoding runs ahead on a worker (on the Vita, a thread on the second core): it keeps a few
 * pictures converted to RGBA, each with its time, and about a third of a second of sound queued
 * in the mixer. The game thread shows the newest picture whose time has come by the sound's own
 * clock (the samples the mixer has played), so the pictures change on an even beat, in step with
 * the sound, however long each one took to decode. */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "pl_mpeg.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "video.h"
#include "audio.h"

#define NBUF 5         /* picture slots: one shown, the rest decoded ahead */
#define AHEAD 3        /* pictures decoded ahead at most */
#define SOUND_AHEAD 0.5 /* seconds of sound kept queued in the mixer */

enum { SLOT_FREE, SLOT_READY, SLOT_SHOWN };

static struct {
    plm_t *plm;
    char name[32];
    uint8_t *rgba[NBUF];
    int w, h;
    double fps, rate;
    double speed;       /* clip time a real second: 59.94/60 on the Vita, so each picture stays 4 refreshes */
    int has_audio;
    /* shared with the worker (plat_worker_lock) */
    int state[NBUF];
    double ptime[NBUF];
    int video_done, audio_done; /* the decoders have reached the end */
    int stopped;                /* VideoPlayer.Pause: no more decoding */
    int frames;                 /* pictures decoded */
    double decode_s, worst_s, last_time;
    double wclock;              /* the game thread's clock, for the worker: a picture already late is not converted */
    int late_dropped;           /* pictures decoded after their time had passed (not converted) */
    /* game thread */
    int worker, logged, ended;
    int shown;          /* slot on screen (-1: black) */
    double clock;       /* playback time */
    unsigned last_played;
    double stall;       /* game time the sound has been queued but not played (no sound output) */
    int game_clock;     /* the sound output is not running: game time for the rest of the clip */
    double aclock;      /* the sound's clock, smoothed (the mixer takes it in 23 ms blocks) */
    int aclock_ok, since_fix, fixes, resyncs;
    double err_min, err_max, adj_max; /* the sound against the pictures, and the most it was sped or slowed */
    int sound_skips;    /* times the sound fell behind the pictures and skipped ahead to them */
    double sound_skipped; /* seconds of it skipped */
    int expect;         /* the mixer told the sound is still coming */
    /* test runs: what the clocks did (a line a second, and every frame for a while after an event) */
    int trace_left, trace_events;
    double sec_t0, sec_clock0, sec_ref0;
    unsigned sec_played0, sec_dry0;
    int sec_frames, sec_fix0, sec_skip0;
    int disturbed;      /* test: holds not counted (a screenshot froze the game, vsync off...) */
    char seq[1100];     /* each picture's hold, in frames (x: not counted) */
    int nseq;
    long frame_no;      /* game frames since the start */
    double refreshes;   /* display refreshes since the start */
    double last_change; /* the refresh the picture last changed (-1: none yet) */
    double owed;        /* real time not yet in the clock (the display ran on while the game stood) */
    double slow;        /* how often lately a game frame took more than one refresh */
    int long_frames;    /* game frames that took more than one refresh and a half */
    int holds[8];       /* how many display refreshes each picture stayed: 1..6, more */
    int late, skipped;  /* pictures shown a frame or more after their time / never shown */
    GLuint tex;
    int tex_w, tex_h;
    int upload;         /* the shown slot still has to go to the texture */
} V;

/* the worker: keep pictures and sound decoded ahead */
static void decode_job(void *arg)
{
    (void)arg;
    for (;;) {
        plat_worker_lock();
        int stop = V.stopped || !V.plm, vdone = V.video_done, adone = V.audio_done, first = V.frames == 0;
        double wclock = V.wclock;
        int b = -1, ready = 0;
        for (int i = 0; i < NBUF; i++) {
            if (V.state[i] == SLOT_READY) ready++;
            else if (V.state[i] == SLOT_FREE && b < 0) b = i;
        }
        plat_worker_unlock();
        if (stop)
            return;
        int did = 0;
        /* the sound first, all that is wanted (it is the clock, and running dry is heard); but the first
         * picture before any sound, so the two start together */
        if (V.has_audio && !adone && (!first || vdone)) {
            while (audio_stream_queued() < (int)(SOUND_AHEAD * V.rate)) {
                plm_samples_t *s = plm_decode_audio(V.plm);
                did = 1;
                if (!s) {
                    plat_worker_lock();
                    V.audio_done = 1;
                    plat_worker_unlock();
                    break;
                }
                audio_stream_push(s->interleaved, (int)s->count);
                plat_worker_lock();
                stop = V.stopped;
                plat_worker_unlock();
                if (stop)
                    return;
            }
        }
        if (!vdone && b >= 0 && ready < AHEAD) {
            double t0 = plat_seconds();
            plm_frame_t *f = plm_decode_video(V.plm);
            if (f && (int)f->width == V.w && (int)f->height == V.h) {
                /* (one whose time has already passed, behind a stall, would only be passed over: not
                 * converted, so the decoding catches up sooner) */
                int late = !first && f->time + 1.0 / V.fps < wclock;
                if (!late) /* bottom row first, as GL textures (and Unity's) go */
                    plm_frame_to_rgba(f, V.rgba[b] + (size_t)(V.h - 1) * V.w * 4, -V.w * 4);
                double s = plat_seconds() - t0;
                plat_worker_lock();
                if (!late) {
                    V.ptime[b] = f->time;
                    V.state[b] = SLOT_READY;
                } else {
                    V.late_dropped++;
                }
                V.frames++;
                V.decode_s += s;
                if (s > V.worst_s) V.worst_s = s;
                V.last_time = f->time;
                plat_worker_unlock();
            } else if (!f) {
                plat_worker_lock();
                V.video_done = 1;
                plat_worker_unlock();
            }
            did = 1;
        }
        if (!did)
            return;
    }
}

int video_start(const char *name)
{
    video_stop();
    char path[64];
    snprintf(path, sizeof(path), "movies/%s.mpg", name);
    size_t size = 0;
    uint8_t *buf = vfs_read_quiet(path, &size);
    if (!buf) {
        lsd_log("video %s: the movie packs are missing or do not have it", name);
        return 0;
    }
    V.plm = plm_create_with_memory(buf, size, 1);
    if (!V.plm) {
        free(buf);
        lsd_log("video %s: could not open", name);
        return 0;
    }
    if (!plm_probe(V.plm, 512 * 1024) || !plm_get_num_video_streams(V.plm)) {
        lsd_log("video %s: no video stream", name);
        plm_destroy(V.plm);
        V.plm = NULL;
        return 0;
    }
    snprintf(V.name, sizeof(V.name), "%s", name);
    V.w = plm_get_width(V.plm);
    V.h = plm_get_height(V.plm);
    V.fps = plm_get_framerate(V.plm);
    if (V.fps <= 0) V.fps = 15.0;
    for (int b = 0; b < NBUF; b++) {
        V.rgba[b] = (uint8_t *)malloc((size_t)V.w * V.h * 4);
        if (!V.rgba[b]) {
            video_stop();
            return 0;
        }
        V.state[b] = SLOT_FREE;
    }
    V.shown = -1; /* (black until the first picture) */
    V.video_done = V.audio_done = V.stopped = 0;
    V.frames = V.logged = V.ended = V.upload = 0;
    V.decode_s = V.worst_s = V.last_time = 0;
    V.wclock = 0;
    V.late_dropped = 0;
    V.sound_skips = 0;
    V.sound_skipped = 0;
    V.trace_left = V.trace_events = 0;
    V.sec_t0 = 0;
    V.clock = 0;
    V.last_played = 0;
    V.stall = 0;
    V.game_clock = 0;
    V.aclock = 0;
    V.aclock_ok = V.since_fix = V.fixes = V.resyncs = V.disturbed = 0;
    V.err_min = V.err_max = V.adj_max = 0;
    V.nseq = 0;
    V.seq[0] = 0;
    V.frame_no = 0;
    V.refreshes = 0;
    V.last_change = -1;
    V.owed = V.slow = 0;
    V.long_frames = 0;
    memset(V.holds, 0, sizeof(V.holds));
    V.late = V.skipped = 0;
    plm_set_loop(V.plm, 0);
    V.has_audio = plm_get_num_audio_streams(V.plm) > 0;
    plm_set_audio_enabled(V.plm, V.has_audio);
    V.rate = 44100.0;
    if (V.has_audio) {
        plm_set_audio_stream(V.plm, 0);
        int r = plm_get_samplerate(V.plm);
        if (r > 0) V.rate = r;
    }
    V.audio_done = !V.has_audio;
    audio_stream_clear();
    /* the display's 59.94 refreshes a second against 15 pictures: played 0.1% slower (sound and pictures
     * together; 2 cents lower), each picture stays exactly 4 refreshes */
    {
        double per = plat_refresh_hz() / V.fps;
        int m = (int)(per + 0.5);
        V.speed = (m >= 1 && per / m > 0.995 && per / m < 1.005) ? per / m : 1.0;
    }
    audio_stream_set_step(V.speed * V.rate / 44100.0);
    V.expect = V.has_audio;
    audio_stream_expect(V.expect);
    V.worker = plat_worker_start(decode_job, NULL);
    if (V.worker)
        plat_worker_kick(); /* (the first pictures and sound start at once) */
    lsd_log("video %s: %dx%d, %.1f fps, %.1f s, %s, %.1f MB, speed %.4f%s", name, V.w, V.h, V.fps,
            plm_get_duration(V.plm), V.has_audio ? "with sound" : "silent", size / 1e6, V.speed,
            V.worker ? "" : " (decoding on the game thread)");
    return 1;
}

static void log_end(const char *how)
{
    plat_worker_lock();
    int n = V.frames;
    double d = V.decode_s, w = V.worst_s;
    plat_worker_unlock();
    char hold[96];
    snprintf(hold, sizeof(hold), "%d/%d/%d/%d/%d/%d/%d", V.holds[1], V.holds[2], V.holds[3], V.holds[4], V.holds[5],
             V.holds[6], V.holds[7]);
    lsd_log("video %s %s: %d pictures in %.1f s, decoding %.2f ms per picture (worst %.1f ms); each picture shown "
            "for 1/2/3/4/5/6/more refreshes: %s; %d late, %d skipped; %d clock steps, %d resyncs, %d long frames; "
            "sound %+.1f..%+.1f ms from the pictures, its speed changed up to %+.2f%%; sound ran dry %.0f ms, "
            "skipped ahead %d times (%.0f ms); %d pictures decoded too late to show",
            V.name, how, n, V.clock, n ? d * 1000.0 / n : 0.0, w * 1000.0, hold, V.late, V.skipped, V.fixes,
            V.resyncs, V.long_frames, V.err_min * 1000.0, V.err_max * 1000.0, V.adj_max * 100.0,
            audio_stream_dry() * 1000.0 / 44100.0, V.sound_skips, V.sound_skipped * 1000.0, V.late_dropped);
    if (script_active())
        lsd_log("video %s refreshes per picture: %s", V.name, V.seq);
}

static void trace_start(void)
{
    if (script_active() && V.trace_events < 4 && V.trace_left <= 0) {
        V.trace_events++;
        V.trace_left = 90;
    }
}

void video_note_freeze(void)
{
    V.disturbed = 2;
    if (V.plm) trace_start();
}

void video_update(float dt)
{
    if (!V.plm)
        return;
    plat_worker_lock();
    int stopped = V.stopped, vdone = V.video_done, adone = V.audio_done, ready = 0;
    for (int i = 0; i < NBUF; i++)
        if (V.state[i] == SLOT_READY) ready++;
    double last = V.last_time;
    plat_worker_unlock();
    if (stopped)
        return;
    V.frame_no++;
    /* the clock (the clip's own time): one display refresh a game frame, so each picture stays the same
     * number of them (4). Game frames come at uneven times (the display takes them from a queue), so
     * their times are only used for a real stall: one the queue could not cover is caught up at once.
     * The sound the mixer has played is kept in step with it; that moves in 23 ms blocks on the Vita,
     * so its clock is smoothed. Small differences are made up by the sound (played a little faster or
     * slower); only a large one moves the pictures. */
    const double frame = 1.0 / plat_refresh_hz(); /* (real seconds) */
    const double cframe = frame * V.speed;        /* (clip seconds) */
    double real = plat_frame_interval();
    if (real < 0) real = 0;
    double n;
    if (!plat_vsync_on() || game_frame_limit() > 0) {
        n = real / frame; /* (no display beat to keep: real time) */
        if (!plat_vsync_on()) V.disturbed = V.disturbed > 1 ? V.disturbed : 1; /* (test: faster than the display) */
    } else {
        V.slow = V.slow * 0.9 + (real > frame * 1.5 ? 0.1 : 0.0);
        n = 1;
        if (V.slow > 0.5) { /* (the game is slower than the display: each frame stays several refreshes) */
            n = (int)(real / frame + 0.5);
            if (n < 1) n = 1;
        }
        V.owed += real - n * frame;
        if (V.owed > frame * 3) { /* (a stall the display's queue could not cover: the display ran on) */
            int extra = (int)(V.owed / frame + 0.5);
            n += extra;
            V.owed -= extra * frame;
        }
        if (V.owed < -frame * 3) V.owed = -frame * 3;
    }
    if (real > frame * 1.5 && V.shown >= 0) V.long_frames++;
    V.clock += n * cframe;
    V.aclock += n * cframe;
    V.refreshes += n;
    V.since_fix++;
    int queued = V.has_audio ? audio_stream_queued() : 0, waiting = 0;
    if (V.has_audio && !V.game_clock && !(adone && queued == 0)) {
        unsigned played = audio_stream_played();
        if (played == V.last_played && queued > 0) {
            V.stall += dt;
            if (V.stall > 0.5) { /* (the sound is not being played: no output) */
                V.game_clock = 1;
                lsd_log("video %s: the sound is not playing; timed by the game instead", V.name);
            }
        } else {
            V.stall = 0;
        }
        V.last_played = played;
        if (!V.game_clock) {
            if (played == 0) {
                V.clock = V.aclock = 0; /* (the first picture waits for the sound to start) */
                V.owed = 0;
                waiting = 1;
            } else {
                double a = played / V.rate - plat_audio_latency() * V.speed;
                if (!V.aclock_ok) { /* (the sound has started: the first picture goes up now, at time 0) */
                    V.clock = 0;
                    V.aclock = a;
                    V.aclock_ok = 1;
                    V.since_fix = 0;
                } else if (a - V.aclock > 0.25 || a - V.aclock < -0.25) {
                    V.aclock = a;
                } else {
                    V.aclock += (a - V.aclock) * 0.05;
                }
                double err = V.aclock - V.clock;
                if (err > 0.25 || err < -0.25) {
                    V.clock = V.aclock; /* (far off: the sound stalled, or the game stood still) */
                    V.owed = 0;
                    V.resyncs++; trace_start();
                    V.disturbed = 2;
                } else if (err < -cframe * 2.5) {
                    /* the sound is behind the pictures (its output was held up): it skips ahead to them,
                     * and the pictures keep their beat */
                    int got = audio_stream_skip((int)(-err * V.rate));
                    if (got > 0) {
                        V.aclock += got / V.rate;
                        V.sound_skips++; trace_start();
                        V.sound_skipped += got / V.rate;
                    }
                } else if (err > cframe * 2.5) {
                    /* the pictures are behind (the display stood still longer than the game knew): they
                     * catch up */
                    double k = (int)(err / cframe + 0.5);
                    V.clock += k * cframe; /* (not refreshes: the display did not run on, the clock jumped) */
                    V.owed -= k * frame;
                    V.fixes++; trace_start();
                    V.since_fix = 0;
                } else {
                    /* within that: the pictures keep their beat, and the sound follows them, played up to
                     * 0.3% faster or slower (5 cents: not heard) until the two meet */
                    double adj = -err * 0.3;
                    adj = adj > 0.003 ? 0.003 : adj < -0.003 ? -0.003 : adj;
                    audio_stream_set_step(V.speed * V.rate / 44100.0 * (1.0 + adj));
                    if (V.since_fix > 60) { /* (after the start: how far apart they were) */
                        if (err < V.err_min) V.err_min = err;
                        if (err > V.err_max) V.err_max = err;
                        if (adj * adj > V.adj_max * V.adj_max) V.adj_max = adj;
                    }
                }
            }
        }
    }
    if (script_active() && V.aclock_ok) {
        double now = plat_seconds();
        unsigned pl = V.has_audio ? audio_stream_played() : 0, dry = audio_stream_dry();
        if (V.trace_left > 0) {
            V.trace_left--;
            lsd_log("vtrace %s: real %.1f ms, n %g, owed %.1f, played %u, queued %d, a-clock %.1f, aclock-clock %.1f ms, "
                    "clock %.4f",
                    V.name, real * 1000.0, n, V.owed * 1000.0, pl, queued,
                    (pl / V.rate - plat_audio_latency() * V.speed - V.clock) * 1000.0, (V.aclock - V.clock) * 1000.0,
                    V.clock);
        }
        if (V.sec_t0 <= 0) {
            V.sec_t0 = now;
            V.sec_clock0 = V.clock;
            V.sec_ref0 = V.refreshes;
            V.sec_played0 = pl;
            V.sec_dry0 = dry;
            V.sec_frames = 0;
            V.sec_fix0 = V.fixes;
            V.sec_skip0 = V.sound_skips;
        }
        V.sec_frames++;
        if (now - V.sec_t0 >= 1.0) {
            double el = now - V.sec_t0;
            lsd_log("vsec %s: %.3f s: %d frames, %.0f refreshes, pictures' clock %.3f s, sound %.3f s, dry %.0f ms, "
                    "%d steps, %d skips, aclock-clock %.1f ms",
                    V.name, el, V.sec_frames, V.refreshes - V.sec_ref0, V.clock - V.sec_clock0,
                    (pl - V.sec_played0) / V.rate, (dry - V.sec_dry0) * 1000.0 / 44100.0, V.fixes - V.sec_fix0,
                    V.sound_skips - V.sec_skip0, (V.aclock - V.clock) * 1000.0);
            V.sec_t0 = 0;
        }
    }
    if (V.expect && (adone || V.game_clock)) {
        V.expect = 0;
        audio_stream_expect(0);
    }
    /* the newest decoded picture whose time has come */
    plat_worker_lock();
    V.wclock = waiting ? 0 : V.clock;
    int pick = -1;
    for (int i = 0; i < NBUF; i++)
        if (!waiting && V.state[i] == SLOT_READY && V.ptime[i] <= V.clock + 0.002 &&
            (pick < 0 || V.ptime[i] > V.ptime[pick]))
            pick = i;
    if (pick >= 0) {
        for (int i = 0; i < NBUF; i++) {
            if (i == pick) continue;
            if (V.state[i] == SLOT_READY && V.ptime[i] < V.ptime[pick]) {
                V.state[i] = SLOT_FREE; /* (its time passed while a newer one was ready) */
                V.skipped++;
            } else if (V.state[i] == SLOT_SHOWN) {
                V.state[i] = SLOT_FREE;
            }
        }
        V.state[pick] = SLOT_SHOWN;
        if (V.clock - V.ptime[pick] > 1.0 / 60.0 + 0.002)
            V.late++;
    }
    plat_worker_unlock();
    if (pick >= 0) {
        if (V.last_change >= 0) {
            long h = (long)(V.refreshes - V.last_change + 0.5);
            if (V.disturbed) { /* (a test froze or sped up the game around this picture: not counted) */
                V.disturbed--;
                if (V.nseq < (int)sizeof(V.seq) - 1) V.seq[V.nseq++] = 'x';
            } else {
                V.holds[h < 1 ? 1 : h > 7 ? 7 : h]++;
                if (V.nseq < (int)sizeof(V.seq) - 1) V.seq[V.nseq++] = (char)(h > 9 ? '+' : '0' + h);
            }
            V.seq[V.nseq] = 0;
        }
        V.last_change = V.refreshes;
        V.shown = pick;
        V.upload = 1;
    }
    if (V.worker)
        plat_worker_kick();
    else
        decode_job(NULL);
    /* played to the end: the last picture has had its time, and the sound is out */
    if (vdone && ready == 0 && pick < 0 && V.clock >= last + 1.0 / V.fps &&
        (adone && (V.game_clock || audio_stream_queued() == 0))) {
        if (!V.ended) {
            V.ended = 1;
            if (!V.logged) {
                V.logged = 1;
                log_end("finished");
            }
        }
    }
}

int video_active(void) { return V.plm != NULL; }

int video_showing(void) { return V.plm && V.shown >= 0; }

int video_ended(void) { return V.plm && V.ended; }

/* VideoPlayer.Pause: no more decoding (the last picture stays) */
void video_pause(void)
{
    if (!V.plm)
        return;
    plat_worker_lock();
    V.stopped = 1; /* (the worker sees this after its picture and goes idle: not waited for here, where a
                    * skip starts a fade, but in video_stop, under black) */
    plat_worker_unlock();
    if (!V.logged)
        log_end("stopped");
    V.logged = 1;
    V.ended = 1;
}

void video_stop(void)
{
    if (V.plm) {
        video_pause();
        plat_worker_stop();
        V.worker = 0;
        plm_destroy(V.plm);
        V.plm = NULL;
        audio_stream_clear();
    }
    for (int b = 0; b < NBUF; b++) {
        free(V.rgba[b]);
        V.rgba[b] = NULL;
    }
    V.shown = -1;
    if (V.tex) { /* (the next clip starts black, not on this one's last picture) */
        glDeleteTextures(1, &V.tex);
        V.tex = 0;
    }
}

GLuint video_texture(int *w, int *h)
{
    if (!V.plm || !V.rgba[0])
        return 0;
    if (V.shown < 0 && !V.tex) {
        /* black until the first picture */
        static const uint8_t BLACK[4] = {0, 0, 0, 255};
        glGenTextures(1, &V.tex);
        glBindTexture(GL_TEXTURE_2D, V.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, BLACK);
        V.tex_w = V.tex_h = 1;
    }
    if (V.upload && V.shown >= 0) {
        V.upload = 0;
        /* (the worker never writes a shown slot) */
        if (!V.tex || V.tex_w != V.w || V.tex_h != V.h) {
            if (V.tex) glDeleteTextures(1, &V.tex);
            glGenTextures(1, &V.tex);
            glBindTexture(GL_TEXTURE_2D, V.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, V.w, V.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, V.rgba[V.shown]);
            V.tex_w = V.w;
            V.tex_h = V.h;
        } else {
            glBindTexture(GL_TEXTURE_2D, V.tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, V.w, V.h, GL_RGBA, GL_UNSIGNED_BYTE, V.rgba[V.shown]);
        }
    }
    *w = V.w;
    *h = V.h;
    return V.tex;
}
