/* Sound: LSDR's MusicSystem/MusicPlayer (the original soundtrack, synthesised live), Torii's
 * AudioPlayer (2D sound effects) and Unity AudioSources for DreamAudio entities, all mixed into
 * one 44.1 kHz stereo stream with the master mixer's limiter. */
#ifndef LSD_AUDIO_H
#define LSD_AUDIO_H

#include "common.h"

int audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);                       /* every frame: music fades, 3D gains */
void audio_listener(vec3 pos, vec3 fwd, vec3 right); /* the camera (AudioListener) */
void audio_log_stats(void);
void audio_output(int on);                           /* test: stop / restart the output */
void audio_record(const char *name, float seconds); /* test: save the next seconds of output as <shots>/name.wav */
void music_force(int stage, int bg, int bank);      /* test: PlaySong of a given SEQ with a given bank */

/* MusicSystem.NextSong for a dream load (style from the day, song from last day's graph position) */
void music_dream_song(int dream, int day, int last_gx, int last_gy);
void music_skip(void);        /* SkipSong: a random song of the current style */
void music_stop(void);        /* StopSong: fade out over a second */
const char *music_name(void); /* "STG00 BGA-STANDERD", "(silent)", or "" */
const char *music_current_song(void); /* MusicSystem.CurrentSong's name (a silent song too); NULL after StopSong */
const char *music_current_author(void);
void music_use_original_songs(int on); /* MusicSystem.UseOriginalSongs */
void audio_set_volumes(float music, float sfx); /* the Music and SFX mixer groups */

/* AudioPlayer.PlayClip on the SFX group: an SE bank sample */
void sfx_play(int se_sample, float pitch);
int sfx_link_sample(void);    /* DreamSystem.LinkSound */
/* LBDFootstepIndex: 0 = no sound */
int sfx_footstep(int key, int *se_sample, float *pitch);
int sfx_footstep_fallback(int *se_sample, float *pitch);

/* a video's sound: interleaved stereo float samples at 44100 Hz, played as they arrive */
void audio_stream_push(const float *lr, int frames);
void audio_stream_clear(void);
int audio_stream_queued(void); /* frames not yet played */
void audio_stream_expect(int on);   /* a video's sound is still coming (its running dry is counted) */
unsigned audio_stream_dry(void);    /* output frames the stream ran dry while its sound was coming */
int audio_stream_skip(int n);       /* drop up to n queued frames; returns how many */
void audio_stream_set_step(double step); /* clip frames an output frame (a video played slower or faster) */
unsigned audio_stream_played(void); /* frames played since the last clear (the video clock) */

/* sound clips (ui/NAME.pcm: u32 rate, channels, frames, 16-bit samples) */
int audio_clip_load(const char *path); /* -1 if missing */
int audio_clip_add(const char *path, uint8_t *buf, size_t n); /* from a file already read (buf is freed) */
void sfx_play_clip(int clip, float pitch); /* mono clips */
void audio_bgm_play(int clip);         /* loop a stereo 44.1 kHz clip (-1 = stop) */
void audio_bgm_stop(void);

/* AudioSource (DreamAudio entities) */
int src_new(void); /* -1 if none left */
void src_free(int h);
void src_play(int h, int se_sample, float pitch, float volume, int loop); /* (re)start from the beginning */
void src_stop(int h);
int src_playing(int h);
void src_place(int h, int spatial, float min_dist, float max_dist, vec3 pos); /* 3D settings / position */

#endif
