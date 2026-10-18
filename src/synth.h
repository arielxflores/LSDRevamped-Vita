/* The original soundtrack: PS1 SEQ sequences played with VAB sound banks, synthesised the way
 * LSD Revamped renders them when it imports the game (libLSD's VAB/VAG/SEQ readers driving
 * EZSynth's Sequencer and Synth), so each song sounds like LSDR's pre-rendered .wav of it.
 * Plain C, no platform code: the PC tools and both game builds share it. */
#ifndef LSD_SYNTH_H
#define LSD_SYNTH_H

#include <stddef.h>
#include <stdint.h>

#define SYN_RATE 44100
#define SYN_VOICES 28   /* Synth.DEFAULT_MAX_VOICES */
#define SYN_CHANNELS 16

typedef struct {
    int16_t *pcm; /* decoded VAG (11025 Hz as LSDR exports it; the synth ignores the rate) */
    int len;
} SndSample;

typedef struct {
    uint8_t min_note, max_note, center, volume;
    int sample;                          /* index into SndBank.samp (VagId - 1) */
    float attack, decay, sustain, release; /* EZSynth EnvelopeADSR, seconds */
} SndTone;

typedef struct {
    uint8_t master_volume, pan;
    int ntone;
    SndTone tone[16];
} SndProgram;

typedef struct {
    int nprog;          /* only programs with a master volume, in order (libLSD's VAB.Programs) */
    SndProgram *prog;
    int nsamp;
    SndSample *samp;
    int warnings;       /* things LSDR's importer would have thrown on */
} SndBank;

/* vh/vb: the .VH header and .VB body; long_release: the AMBIENT variant's 3000-cycle release */
int snd_bank_load(SndBank *b, const uint8_t *vh, size_t vhlen, const uint8_t *vb, size_t vblen, int long_release);
void snd_bank_free(SndBank *b);

enum { SEV_NOTE, SEV_CC, SEV_PITCH, SEV_PROGRAM, SEV_TEMPO };

typedef struct {
    int tick;
    uint8_t type, ch, a, b;
    float f;     /* pitch bend amount */
    double spb;  /* tempo: seconds per beat */
} SeqEvent;

typedef struct {
    int resolution;
    int nevent;
    SeqEvent *ev;  /* in file order (ticks never decrease) */
    int nticks;    /* distinct ticks */
    int *tick_val, *tick_first, *tick_count;
    double length; /* SEQSequence.GetLengthSeconds */
    int nsamples;  /* frames in one pass (LSDR's .wav length) */
    int warnings;
} SndSeq;

int snd_seq_load(SndSeq *s, const uint8_t *data, size_t len);
void snd_seq_free(SndSeq *s);

typedef struct {
    int active;               /* Sampler != null */
    int mapped, id, key;      /* in Synth._activeVoices under (id, key) */
    const int16_t *wave;
    int wlen;
    double phase;
    int root, note;
    float vol, pan, pitch;    /* VoiceParameters */
    float att, dec, sus, rel;
    float velocity;
    int times, released;
    float inc_bend;           /* cached phase increment for this pitch bend */
    double inc;
    int inc_ok;
} SynVoice;

typedef struct {
    int exists, program;
    float pan, pitch;
} SynInst;

typedef struct {
    const SndBank *bank;
    SynVoice v[SYN_VOICES];
    SynInst inst[SYN_CHANNELS];
    int nmapped;
    int warnings; /* pan/pitch on a channel EZSynth had not seen yet (it would have thrown) */
} Synth;

typedef struct {
    const SndSeq *seq;
    const SndBank *bank;
    Synth syn;
    double spb;        /* Sequencer.SecondsPerBeat */
    double last;       /* _lastTickProcessedTime */
    int pos;           /* frame within the pass */
    int voices_peak;
} Sequencer;

/* start a song from the beginning; it loops (as Unity loops the rendered clip) */
void seqr_start(Sequencer *q, const SndSeq *seq, const SndBank *bank);
/* render interleaved stereo frames */
void seqr_render(Sequencer *q, int16_t *out, int frames);

#endif
