/* PS1 SEQ + VAB music, synthesised like LSD Revamped's importer does (see synth.h).
 *
 * This follows libLSD (VAB.cs, VAG.cs, SEQ.cs, MidiEventDataFactory.cs, SEQSequence.cs,
 * VABSoundbank.cs) and EZSynth (Sequencer.cs, Synth.cs, Voice.cs, PCMSampler.cs) closely,
 * including their quirks, because LSDR's songs are what those produce:
 *   - the pitch wheel value is built as MSB << (8 + LSB);
 *   - a tone's own pan and fine tune are replaced by the channel's (default centre, no bend);
 *   - ticks are polled per sample against the absolute time, so a tick can be skipped;
 *   - the song length counts each tempo from the tick it is set on backwards;
 *   - samples play at 44.1 kHz from their root note (CenterNote - 2), never looping.
 * LSDR imports inside the Unity editor, whose Mono runtime evaluates float expressions in double
 * precision and rounds only when storing to a float variable or field; the maths here does the
 * same (the decoded samples then match LSDR's exported .wav files bit for bit). */
#include "synth.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int rd16s(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static int rd16u(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ------------------------------------------------------------------ VAG (libLSD VAG.cs) */

static const float COEF[5][2] = {
    {0.0f, 0.0f},
    {60.0f / 64.0f, 0.0f},
    {115.0f / 64.0f, 52.0f / 64.0f},
    {98.0f / 64.0f, 55.0f / 64.0f},
    {122.0f / 64.0f, 60.0f / 64.0f},
};

static int vag_decode(const uint8_t *vb, size_t vblen, size_t off, SndSample *out, int *warn)
{
    /* scanForBlockCount: the flag byte of each block after the 16-byte lead-in, until the end flag */
    int n = 1;
    size_t p = off + 17;
    for (;;) {
        if (p >= vblen) {
            (*warn)++;
            break;
        }
        if (vb[p] & 1)
            break;
        n++;
        p += 16;
    }
    out->pcm = (int16_t *)malloc((size_t)n * 28 * sizeof(int16_t));
    if (!out->pcm)
        return 0;
    out->len = n * 28;
    float p1 = 0, p2 = 0;
    for (int k = 0; k < n; k++) {
        size_t bo = off + 16 + (size_t)k * 16;
        uint8_t blk[16];
        for (int i = 0; i < 16; i++)
            blk[i] = bo + i < vblen ? vb[bo + i] : 0;
        int range = blk[0] & 0xF, filter = (blk[0] & 0xF0) >> 4;
        int shift = (range + 16) & 31;
        int16_t d[28];
        for (int i = 0; i < 14; i++) {
            uint8_t b = blk[2 + i];
            int32_t lsb = (int32_t)((uint32_t)b << 28);
            int32_t msb = (int32_t)((uint32_t)(b & 0xF0) << 24);
            d[i * 2] = (int16_t)(lsb >> shift);
            d[i * 2 + 1] = (int16_t)(msb >> shift);
        }
        if (filter > 4) { /* (an index error in libLSD: the importer would have failed) */
            (*warn)++;
            filter = 0;
        }
        if (filter > 0) {
            float f1 = COEF[filter][0], f2 = COEF[filter][1];
            float a = p1, c = p2;
            for (int i = 0; i < 28; i++) {
                float t = (float)((double)d[i] + (double)a * f1 - (double)c * f2);
                d[i] = (int16_t)(int32_t)t;
                c = a;
                a = t;
            }
            p1 = a;
            p2 = c;
        } else {
            p2 = d[26];
            p1 = d[27];
        }
        memcpy(out->pcm + k * 28, d, sizeof(d));
    }
    return 1;
}

/* ------------------------------------------------------------------ VAB (libLSD VAB.cs, VABSoundbank.cs) */

static int shift_to_cycles(double factor, int shift)
{
    return (int)(factor * (double)(int32_t)(1u << (shift & 31)));
}

static void tone_envelope(SndTone *t, int adsr1, int adsr2, int long_release)
{
    int attack_shift = (adsr1 & (0x1F << 9)) >> 9;
    int decay_shift = (adsr1 & (0xF << 5)) >> 5;
    int release_shift = adsr2 & 0x1F;
    int attack = shift_to_cycles(0.0023, attack_shift) >> 5;
    int decay = shift_to_cycles(0.0015, decay_shift);
    int release = shift_to_cycles(0.0023, release_shift);
    if (long_release)
        release = 3000;
    const float step = 11025.0f / 20.0f;
    t->attack = (float)((double)attack / step);
    t->decay = (float)((double)decay / step);
    t->sustain = 1.0f;
    t->release = (float)((double)release / step);
}

int snd_bank_load(SndBank *b, const uint8_t *vh, size_t vhlen, const uint8_t *vb, size_t vblen, int long_release)
{
    memset(b, 0, sizeof(*b));
    const size_t prog_off = 32, tone_off = 32 + 128 * 16;
    if (vhlen < tone_off)
        return 0;
    int prog_count = rd16u(vh + 18), vag_count = rd16u(vh + 22);
    size_t ntones = (size_t)prog_count * 16;
    size_t vag_off = tone_off + ntones * 32;
    if (vhlen < vag_off + (size_t)vag_count * 2)
        return 0;
    /* the samples, at the running sum of the size table (sample i is VAG i + 1) */
    b->samp = (SndSample *)calloc(vag_count ? vag_count : 1, sizeof(SndSample));
    if (!b->samp)
        return 0;
    b->nsamp = vag_count;
    size_t off = 0;
    for (int i = 0; i < vag_count; i++) {
        off += (size_t)rd16u(vh + vag_off + i * 2) << 3;
        if (!vag_decode(vb, vblen, off, &b->samp[i], &b->warnings)) {
            snd_bank_free(b);
            return 0;
        }
    }
    /* programs with a master volume, their tones that have a sample */
    b->prog = (SndProgram *)calloc(128, sizeof(SndProgram));
    if (!b->prog) {
        snd_bank_free(b);
        return 0;
    }
    int tone_base = 0;
    for (int p = 0; p < 128; p++) {
        const uint8_t *pa = vh + prog_off + p * 16;
        int tone_count = pa[0], master = pa[1];
        if (master <= 0)
            continue;
        SndProgram *pr = &b->prog[b->nprog++];
        pr->master_volume = (uint8_t)master;
        pr->pan = pa[4];
        for (int i = 0; i < tone_count; i++) {
            size_t ti = (size_t)tone_base + i;
            if (ti >= ntones)
                continue; /* never read from the file: all zero, so no sample */
            const uint8_t *ta = vh + tone_off + ti * 32;
            int vag = rd16s(ta + 22);
            if (vag == 0)
                continue;
            if (vag - 1 >= vag_count || vag < 0 || pr->ntone >= 16) {
                b->warnings++;
                continue;
            }
            SndTone *t = &pr->tone[pr->ntone++];
            t->volume = ta[2];
            t->center = ta[4];
            t->min_note = ta[6];
            t->max_note = ta[7];
            t->sample = vag - 1;
            tone_envelope(t, rd16s(ta + 16), rd16s(ta + 18), long_release);
        }
        if (tone_count != 0)
            tone_base += 16;
    }
    return 1;
}

void snd_bank_free(SndBank *b)
{
    if (b->samp)
        for (int i = 0; i < b->nsamp; i++)
            free(b->samp[i].pcm);
    free(b->samp);
    free(b->prog);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ SEQ (libLSD SEQ.cs, SEQSequence.cs) */

typedef struct {
    const uint8_t *d;
    size_t len, pos;
    int eof;
} Rd;

static int rb(Rd *r)
{
    if (r->pos >= r->len) {
        r->eof = 1;
        return 0;
    }
    return r->d[r->pos++];
}

static float spb_of(int tempo)
{
    /* microSecondsPerQuarterNoteToSecondsPerBeat */
    return (float)((double)tempo / 1000000.0);
}

static int add_event(SndSeq *s, int *cap, const SeqEvent *e)
{
    if (s->nevent == *cap) {
        int nc = *cap ? *cap * 2 : 256;
        SeqEvent *ne = (SeqEvent *)realloc(s->ev, (size_t)nc * sizeof(SeqEvent));
        if (!ne)
            return 0;
        s->ev = ne;
        *cap = nc;
    }
    s->ev[s->nevent++] = *e;
    return 1;
}

int snd_seq_load(SndSeq *s, const uint8_t *data, size_t len)
{
    memset(s, 0, sizeof(*s));
    if (len < 15)
        return 0;
    s->resolution = (data[8] << 8) + data[9];
    int tempo = (data[10] << 16) + (data[11] << 8) + data[12];
    if (s->resolution <= 0)
        return 0;
    int cap = 0;
    SeqEvent e;
    memset(&e, 0, sizeof(e));
    e.type = SEV_TEMPO;
    e.spb = (double)spb_of(tempo);
    if (!add_event(s, &cap, &e))
        return 0;

    Rd r = {data, len, 15, 0};
    int last_status = -1, tick = 0;
    for (;;) {
        /* VariableLength delta time */
        int delta = 0;
        for (int i = 0; i < 4; i++) {
            int b = rb(&r);
            delta = (delta << 7) + (b & 0x7F);
            if (!(b & 0x80))
                break;
        }
        int status = rb(&r);
        if (r.eof)
            break;
        if (!(status & 0x80)) {
            if (last_status < 0) {
                s->warnings++;
                break;
            }
            status = last_status;
            r.pos--;
        }
        last_status = status;
        tick += delta;
        memset(&e, 0, sizeof(e));
        e.tick = tick;
        if (status == 0xFF) {
            int type = rb(&r);
            rb(&r); /* "length" */
            if (type == 0x2F)
                break;
            if (type != 0x51) {
                s->warnings++;
                break;
            }
            int t0 = rb(&r), t1 = rb(&r), t2 = rb(&r);
            e.type = SEV_TEMPO;
            e.spb = (double)spb_of((t0 << 16) + (t1 << 8) + t2);
        } else if (status >= 0x80 && status < 0xF0) {
            int hi = status >> 4;
            e.ch = (uint8_t)(status & 0xF);
            if (hi == 0x8 || hi == 0x9 || hi == 0xA) {
                e.type = SEV_NOTE;
                e.a = (uint8_t)(rb(&r) & 0x7F);
                e.b = (uint8_t)(rb(&r) & 0x7F);
            } else if (hi == 0xC) {
                e.type = SEV_PROGRAM;
                e.a = (uint8_t)(rb(&r) & 0x7F);
            } else if (hi == 0xE) {
                int msb = rb(&r) & 0x7F, lsb = rb(&r) & 0x7F;
                int32_t v = (int32_t)((uint32_t)msb << ((8 + lsb) & 31));
                e.type = SEV_PITCH;
                e.f = (float)(((double)v - 8192.0) / 8192.0);
            } else if (hi == 0xB) {
                e.type = SEV_CC;
                e.a = (uint8_t)(rb(&r) & 0x7F);
                e.b = (uint8_t)(rb(&r) & 0x7F);
            } else {
                s->warnings++;
                break;
            }
        } else {
            s->warnings++;
            break;
        }
        if (r.eof) {
            s->warnings++;
            break;
        }
        if (!add_event(s, &cap, &e)) {
            snd_seq_free(s);
            return 0;
        }
    }

    /* distinct ticks, in order */
    s->tick_val = (int *)malloc((size_t)s->nevent * sizeof(int));
    s->tick_first = (int *)malloc((size_t)s->nevent * sizeof(int));
    s->tick_count = (int *)malloc((size_t)s->nevent * sizeof(int));
    if (!s->tick_val || !s->tick_first || !s->tick_count) {
        snd_seq_free(s);
        return 0;
    }
    for (int i = 0; i < s->nevent; i++) {
        if (s->nticks && s->tick_val[s->nticks - 1] == s->ev[i].tick) {
            s->tick_count[s->nticks - 1]++;
            continue;
        }
        s->tick_val[s->nticks] = s->ev[i].tick;
        s->tick_first[s->nticks] = i;
        s->tick_count[s->nticks] = 1;
        s->nticks++;
    }

    /* GetLengthSeconds */
    double length = 0;
    double spt = (double)spb_of(500000) / s->resolution;
    int last_tick = 0;
    for (int k = 0; k < s->nticks; k++) {
        const SeqEvent *te = NULL;
        for (int i = 0; i < s->tick_count[k]; i++)
            if (s->ev[s->tick_first[k] + i].type == SEV_TEMPO)
                te = &s->ev[s->tick_first[k] + i];
        if (te)
            spt = te->spb / s->resolution;
        length += spt * (s->tick_val[k] - last_tick);
        last_tick = s->tick_val[k];
    }
    if (length == 0)
        length = 0.1;
    s->length = length;
    s->nsamples = (int)ceil(length * SYN_RATE);
    if (s->nsamples < 1)
        s->nsamples = 1;
    return 1;
}

void snd_seq_free(SndSeq *s)
{
    free(s->ev);
    free(s->tick_val);
    free(s->tick_first);
    free(s->tick_count);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ Synth (EZSynth Synth.cs, Voice.cs, PCMSampler.cs) */

static double note_freq(int note)
{
    return 440.0 * pow(2.0, (double)(note - 69) / 12.0);
}

static void synth_reset(Synth *s, const SndBank *bank)
{
    memset(s, 0, sizeof(*s));
    s->bank = bank;
}

static SynInst *inst(Synth *s, int id)
{
    SynInst *in = &s->inst[id & (SYN_CHANNELS - 1)];
    if (!in->exists) {
        in->exists = 1;
        in->program = id;
        in->pan = 0;
        in->pitch = 0;
    }
    return in;
}

static SynVoice *mapped_voice(Synth *s, int id, int note)
{
    for (int i = 0; i < SYN_VOICES; i++)
        if (s->v[i].mapped && s->v[i].id == id && s->v[i].key == note)
            return &s->v[i];
    return NULL;
}

static void note_off(Synth *s, int id, int note)
{
    SynVoice *v = mapped_voice(s, id, note);
    if (!v)
        return;
    v->released = 1;
    v->times = 0;
}

static void note_on(Synth *s, int id, int note, int velocity)
{
    if (velocity <= 0) {
        note_off(s, id, note);
        return;
    }
    SynInst *in = inst(s, id);
    const SndBank *b = s->bank;
    if (in->program < 0 || in->program >= b->nprog)
        return; /* (an index error in VABSoundbank) */
    const SndProgram *pr = &b->prog[in->program];
    const SndTone *t = NULL;
    for (int i = 0; i < pr->ntone; i++)
        if (note >= pr->tone[i].min_note && note <= pr->tone[i].max_note) {
            t = &pr->tone[i];
            break;
        }
    if (!t)
        return;
    SynVoice *v = mapped_voice(s, id, note);
    if (!v) {
        for (int i = 0; i < SYN_VOICES; i++)
            if (!s->v[i].active) {
                v = &s->v[i];
                break;
            }
        if (!v)
            return; /* out of voices */
        v->mapped = 1;
        v->id = id;
        v->key = note;
        s->nmapped++;
    }
    const SndSample *smp = &b->samp[t->sample];
    v->active = 1;
    v->wave = smp->pcm;
    v->wlen = smp->len;
    v->phase = 0;
    v->note = note;
    v->root = t->center - 2;
    int vol = t->volume < pr->master_volume ? t->volume : pr->master_volume;
    v->vol = (float)((double)vol / 127.0);
    v->pan = in->pan;
    v->pitch = in->pitch;
    float vel = (float)((double)velocity / 127.0);
    v->velocity = vel < 0 ? 0 : vel > 1 ? 1 : vel;
    v->att = t->attack;
    v->dec = t->decay;
    v->sus = t->sustain;
    v->rel = t->release;
    v->times = 0;
    v->released = 0;
    v->inc_ok = 0;
}

static void pitch_bend(Synth *s, int id, float pitch)
{
    if (!s->inst[id & (SYN_CHANNELS - 1)].exists)
        s->warnings++;
    for (int i = 0; i < SYN_VOICES; i++)
        if (s->v[i].mapped && s->v[i].id == id)
            s->v[i].pitch = pitch;
    inst(s, id)->pitch = pitch;
}

static void adjust_pan(Synth *s, int id, float pan)
{
    if (!s->inst[id & (SYN_CHANNELS - 1)].exists)
        s->warnings++;
    for (int i = 0; i < SYN_VOICES; i++)
        if (s->v[i].mapped && s->v[i].id == id)
            s->v[i].pan = pan;
    inst(s, id)->pan = pan;
}

static void voice_sample(SynVoice *v, int *out_l, int *out_r)
{
    /* PCMSampler.Sample */
    float bend = v->pitch * 2.0f; /* Parameters.Pitch * MaxPitchBend */
    int16_t s = 0;
    if (!(v->phase > (double)v->wlen)) {
        if (!v->inc_ok || bend != v->inc_bend) {
            double octaves = (double)bend / 12.0;
            double ratio = pow(2.0, octaves);
            double playing = note_freq(v->note) * ratio;
            v->inc = playing / note_freq(v->root);
            v->inc_bend = bend;
            v->inc_ok = 1;
        }
        int ia = (int)v->phase;                /* phase >= 0: the same as Math.Floor */
        double amt = v->phase - (double)ia;
        if (ia >= v->wlen)
            ia %= v->wlen;
        int ib = ia + 1;
        if (ib >= v->wlen)
            ib %= v->wlen;
        s = (int16_t)(int32_t)((double)v->wave[ia] * (1 - amt) + (double)v->wave[ib] * amt);
        v->phase += v->inc;
    }
    /* applyVolume */
    int16_t sv = (int16_t)(int32_t)((double)s * 1.0 * (double)v->velocity * (double)v->vol);
    /* applyVolumeEnvelope */
    double t = (double)v->times * (1.0 / SYN_RATE);
    double env = 1;
    if (!v->released) {
        if (t < (double)v->att) {
            env = t / (double)v->att;
        } else if (t < (double)v->att + (double)v->dec) {
            double td = t - (double)v->att;
            env = (double)v->sus + (double)(1 - v->sus) * (1 - td / (double)v->dec);
        } else {
            env = v->sus;
        }
    } else {
        if (t < (double)v->rel) {
            env = (double)v->sus * (1 - t / (double)v->rel);
        } else {
            env = 0;
            v->times = 0;
            v->active = 0;
        }
    }
    if (env < 0) env = 0;
    if (env > 1) env = 1;
    int16_t se = (int16_t)(int32_t)((double)sv * env);
    /* applyPan */
    float lm, rm;
    if (v->pan < 0) {
        rm = (float)(1.0 + (double)v->pan);
        lm = 1;
    } else {
        lm = (float)(1.0 - (double)v->pan);
        rm = 1;
    }
    *out_l = (int16_t)(int32_t)((double)se * lm);
    *out_r = (int16_t)(int32_t)((double)se * rm);
    v->times++;
}

static int clip_mix(int sample)
{
    double d = sample / 32768.0;
    double c = tanh(d * 0.5);
    if (c < -1) c = -1;
    if (c > 1) c = 1;
    return (int)(c * 32767);
}

static void synth_sample(Synth *s, int16_t *out)
{
    if (s->nmapped == 0) {
        out[0] = out[1] = 0;
        return;
    }
    int ml = 0, mr = 0;
    for (int i = 0; i < SYN_VOICES; i++) {
        SynVoice *v = &s->v[i];
        if (!v->mapped)
            continue;
        int l, r;
        voice_sample(v, &l, &r);
        ml += l;
        mr += r;
    }
    out[0] = (int16_t)clip_mix(ml);
    out[1] = (int16_t)clip_mix(mr);
    for (int i = 0; i < SYN_VOICES; i++)
        if (s->v[i].mapped && !s->v[i].active) {
            s->v[i].mapped = 0;
            s->nmapped--;
        }
}

/* ------------------------------------------------------------------ Sequencer (EZSynth Sequencer.cs) */

void seqr_start(Sequencer *q, const SndSeq *seq, const SndBank *bank)
{
    q->seq = seq;
    q->bank = bank;
    synth_reset(&q->syn, bank);
    q->spb = 0.5;
    q->last = -1;
    q->pos = 0;
}

static void handle_tick(Sequencer *q, int tick)
{
    const SndSeq *s = q->seq;
    int lo = 0, hi = s->nticks - 1, k = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s->tick_val[mid] == tick) {
            k = mid;
            break;
        }
        if (s->tick_val[mid] < tick) lo = mid + 1;
        else hi = mid - 1;
    }
    if (k < 0)
        return;
    for (int i = 0; i < s->tick_count[k]; i++) {
        const SeqEvent *e = &s->ev[s->tick_first[k] + i];
        switch (e->type) {
        case SEV_NOTE: note_on(&q->syn, e->ch, e->a, e->b); break;
        case SEV_CC:
            if (e->a == 10) adjust_pan(&q->syn, e->ch, (float)(((double)e->b - 64.0) / 64.0));
            break;
        case SEV_PITCH: pitch_bend(&q->syn, e->ch, e->f); break;
        case SEV_PROGRAM: inst(&q->syn, e->ch)->program = e->a; break;
        case SEV_TEMPO: q->spb = e->spb; break;
        }
    }
}

void seqr_render(Sequencer *q, int16_t *out, int frames)
{
    if (!q->seq || !q->bank) {
        memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    for (int i = 0; i < frames; i++) {
        if (q->pos >= q->seq->nsamples)
            seqr_start(q, q->seq, q->bank); /* the clip loops */
        double t = (double)q->pos / SYN_RATE;
        double spt = 1.0 / ((double)q->seq->resolution / q->spb);
        int tick = (int)(t / spt);
        if (t - q->last > spt) {
            handle_tick(q, tick);
            q->last = t;
        }
        synth_sample(&q->syn, out + i * 2);
        if (q->syn.nmapped > q->voices_peak)
            q->voices_peak = q->syn.nmapped;
        q->pos++;
    }
}
