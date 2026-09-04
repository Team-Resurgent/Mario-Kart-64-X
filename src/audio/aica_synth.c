/*
 * AICA hardware-mixed voice driver (Dreamcast) for MK64.
 * Ported from the OoT AICA driver. Drives AICA hardware channels from the
 * finalized NoteSubEu set each frame via the KOS command queue; samples staged
 * into ARAM from a resident offline-transcoded Yamaha-ADPCM pool. Stock KOS
 * firmware only (snd_sh4_to_aica is the one non-public symbol, for live UPDATE).
 */

#include "internal.h"
#include "heap.h"
#include "load.h"
#include "data.h"
#include "aica_synth.h"

#ifdef __DREAMCAST__

#include <stdio.h>
#include <stdlib.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <dc/sound/aica_comm.h>
#include <kos/thread.h>

extern int snd_sh4_to_aica(void* packet, uint32_t size);

#include "aica_sample_table.h"

/* Resident AICA-ADPCM pool base. Loaded by setup_audio_data() in main.c. */
const unsigned char* gAicaAdpcmPoolBase = NULL;
static u8* sTblBase = NULL;        /* shared audiotables blob base (gAlTbl->seqArray[0].offset) */

/* Driver lifecycle (the N64 original never shut down -- you powered the console
   off). On DC the game exits via exit(0) on the A+B+X+Y+Start combo while the
   audio thread is still running AicaSynth_Update, so we need a clean teardown.
   sRunning gates Update; sInUpdate lets Shutdown wait for an in-flight Update on
   the audio thread before it frees ARAM out from under it. */
static volatile int sRunning = 0;
static volatile int sInUpdate = 0;
static int sAtexitDone = 0;

#define NUM_AICA_CHANNELS 64
#define MAX_VOICES 64
#define ARAM_CACHE_ENTRIES 192
#define AICA_LEN_MAX 65534
#define SYNTH_VOL_SCALE 192
#define WAVE_SAMPLE_COUNT 64
#define NUM_WAVEFORMS 6
#define NUM_HARMONICS 4
#define KEY_EMPTY 0xFFFFFFFFu
#define SYNTH_KEY(wf, h) (0x80000000u | ((u32)(wf) << 4) | (u32)(h))

typedef struct {
    u32 key;
    u32 aram;
    u32 len;
    s32 refs;
    u32 lru;
} AramEntry;

static AramEntry sCache[ARAM_CACHE_ENTRIES];
static u32 sTick;
static s8 sChanFree[NUM_AICA_CHANNELS];
static s32 sChanFreeTop;

typedef struct {
    s8 channel;
    u8 active;
    u32 sampleKey;
    AramEntry* entry;
} Voice;
static Voice sVoices[MAX_VOICES];
static u32 sWaveAram[NUM_WAVEFORMS][NUM_HARMONICS];

typedef struct {
    u32 base, type, length, loop, loopstart, loopend;
    u8 downsample_shift;
} Resolved;


static int sample_lookup(u32 key, const AicaSampleDesc** out) {
    s32 lo = 0, hi = AICA_SAMPLE_COUNT - 1;
    while (lo <= hi) {
        s32 mid = (lo + hi) >> 1;
        u32 k = gAicaSampleTable[mid].src_offset;
        if (k == key) { *out = &gAicaSampleTable[mid]; return 1; }
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static AramEntry* cache_find(u32 key) {
    s32 i;
    for (i = 0; i < ARAM_CACHE_ENTRIES; i++)
        if (sCache[i].key == key) return &sCache[i];
    return NULL;
}

static AramEntry* cache_acquire(u32 key, u32 pool_offset, u32 byte_len) {
    AramEntry* e = cache_find(key);
    s32 i;
    if (e) { e->refs++; e->lru = sTick; return e; }
    u32 aram = (u32)snd_mem_malloc(byte_len);
    if (aram == 0) {
        AramEntry* victim = NULL;
        for (i = 0; i < ARAM_CACHE_ENTRIES; i++)
            if (sCache[i].key != KEY_EMPTY && sCache[i].refs == 0)
                if (!victim || sCache[i].lru < victim->lru) victim = &sCache[i];
        if (victim) {
            snd_mem_free(victim->aram);
            victim->key = KEY_EMPTY; victim->aram = 0;
            aram = (u32)snd_mem_malloc(byte_len);
        }
        if (aram == 0) return NULL;
    }
    spu_memload_sq(aram, (void*)(gAicaAdpcmPoolBase + pool_offset), (byte_len + 31) & ~31);
    e = cache_find(KEY_EMPTY);
    if (!e) { snd_mem_free(aram); return NULL; }
    e->key = key; e->aram = aram; e->len = byte_len; e->refs = 1; e->lru = sTick;
    return e;
}

static void cache_release(AramEntry* e) { if (e && e->refs > 0) e->refs--; }

/* The KOS aica_freq firmware converts Hz -> AICA (octave, mantissa) via
   freq_lo = (freq<<10)/freq_base, but freq_lo overflows its 10 bits when freq
   reaches 2*freq_base (which happens just below each 44100/2^n boundary because
   freq_base is the integer-truncated 44100>>k). That makes the firmware drop an
   octave -> the voice plays at HALF speed. Mirror the firmware's octave search
   and clamp freq to the last representable value so it never lands in that gap. */
static u32 fix_aica_freq_gap(u32 freq) {
    u32 base = 5644800;   /* 44100 * 128 */
    s32 hi = 7;
    while (freq < base && hi > -8) { base >>= 1; hi--; }
    if (base != 0 && freq >= 2 * base) freq = 2 * base - 1;
    return freq;
}

static u32 calc_freq(struct NoteSubEu* sub, u8 shift) {
    u32 freq = ((u32)sub->resamplingRateFixedPoint * (u32)gAudioBufferParameters.frequency) >> 15;
    if (sub->hasTwoAdpcmParts) freq <<= 1;
    freq >>= shift;
    if (freq == 0) freq = 1;
    return fix_aica_freq_gap(freq);
}

static u32 calc_vol(struct NoteSubEu* sub) {
    u32 l = sub->targetVolLeft, r = sub->targetVolRight;
    u32 m = (l > r) ? l : r;
    u32 v = m >> 4;
    if (v > 255) v = 255;
    /* Headroom for full-scale synth waves summing without clipping. */
    if (sub->isSyntheticWave) v = (v * SYNTH_VOL_SCALE) >> 8;
    return v;
}

/* AICA pan (DIPAN) attenuates the opposite channel in ~3 dB steps (matching the
   documented DISDL/EFSDL send-level table). KOS calc_aica_pan maps our 0..255
   pan to a 0..15 step amount: left  -> 127 - amount*8, right -> 128 + amount*8.
   So convert the L/R balance to dB and pick the matching step count, instead of
   the old linear formula which over-attenuated off-center voices. */
static u32 calc_pan(struct NoteSubEu* sub) {
    s32 l = sub->targetVolLeft, r = sub->targetVolRight;
    s32 hi, lo, amount;
    int left;

    if (l == r) return 128;
    if (l > r) { hi = l; lo = r; left = 1; } else { hi = r; lo = l; left = 0; }

    if (lo <= 0) {
        amount = 15;
    } else {
        /* amount = round( 20*log10(hi/lo) / 3 ), via successive 3 dB (x0.7079)
           ratio thresholds -- no float / libm needed. */
        amount = 0;
        while (amount < 15 && (s64)lo * 1000 < (s64)hi * 708) { /* lo/hi < 0.708 ^? */
            lo = (s32)(((s64)lo * 1000) / 708); /* step up by 3 dB */
            amount++;
        }
    }
    return left ? (u32)(127 - amount * 8) : (u32)(128 + amount * 8);
}

static int resolve(struct NoteSubEu* sub, u32* outKey, Resolved* res, AramEntry** outEntry) {
    *outEntry = NULL;
    res->downsample_shift = 0;

    if (sub->isSyntheticWave) {
        s16* addr = sub->sound.samples;
        s32 wf = -1; u32 h = 0, w;
        for (w = 0; w < NUM_WAVEFORMS; w++) {
            if (addr >= gWaveSamples[w] && addr < gWaveSamples[w] + NUM_HARMONICS * WAVE_SAMPLE_COUNT) {
                wf = (s32)w;
                h = (u32)(addr - gWaveSamples[w]) / WAVE_SAMPLE_COUNT;
                break;
            }
        }
        if (wf < 0) return 0;
        if (h >= NUM_HARMONICS) h = NUM_HARMONICS - 1;
        *outKey = SYNTH_KEY(wf, h);
        res->base = sWaveAram[wf][h];
        res->type = AICA_SM_16BIT;
        res->length = WAVE_SAMPLE_COUNT;
        res->loop = 1; res->loopstart = 0; res->loopend = WAVE_SAMPLE_COUNT;
        return 1;
    }

    if (gAicaAdpcmPoolBase == NULL || sub->sound.audioBankSound == NULL) return 0;
    {
        struct AudioBankSample* s = sub->sound.audioBankSound->sample;
        if (s == NULL || s->sampleAddr == NULL) return 0;
        u32 key = (u32)s->sampleAddr - (u32)sTblBase;
        const AicaSampleDesc* d;
        if (!sample_lookup(key, &d)) return 0;
        AramEntry* e = cache_acquire(key, d->pool_offset, d->byte_len);
        if (!e) return 0;
        *outEntry = e; *outKey = key;
        res->base = e->aram;
        res->type = d->fmt;   /* AICA_SM_16BIT/8BIT/ADPCM, chosen offline by SNR/octave */
        res->length = d->nsamples > AICA_LEN_MAX ? AICA_LEN_MAX : d->nsamples;
        res->loop = d->loop_flag;
        res->loopstart = d->loop_start;
        res->loopend = d->loop_end ? d->loop_end : res->length;
        res->downsample_shift = d->downsample_shift;
        return 1;
    }
}

static void chan_start(s32 ch, const Resolved* r, u32 freq, u32 vol, u32 pan) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_START;
    chan->base = r->base; chan->type = r->type; chan->length = r->length;
    chan->loop = r->loop; chan->loopstart = r->loopstart; chan->loopend = r->loopend;
    chan->freq = freq; chan->vol = vol; chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);
}

static void chan_update(s32 ch, u32 freq, u32 vol, u32 pan) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_VOL | AICA_CH_UPDATE_SET_PAN;
    chan->freq = freq; chan->vol = vol; chan->pan = pan;
    snd_sh4_to_aica(tmp, cmd->size);
}

static void chan_stop(s32 ch) {
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN; cmd->timestamp = 0; cmd->size = AICA_CMDSTR_CHANNEL_SIZE; cmd->cmd_id = ch;
    chan->cmd = AICA_CH_CMD_STOP;
    snd_sh4_to_aica(tmp, cmd->size);
}

static s32 chan_alloc(void) { return sChanFreeTop ? sChanFree[--sChanFreeTop] : -1; }
static void chan_release(s32 ch) { if (sChanFreeTop < NUM_AICA_CHANNELS) sChanFree[sChanFreeTop++] = (s8)ch; }

static void voice_stop(s32 i) {
    Voice* v = &sVoices[i];
    if (v->channel >= 0) { chan_stop(v->channel); chan_release(v->channel); }
    cache_release(v->entry);
    v->channel = -1; v->active = 0; v->entry = NULL; v->sampleKey = KEY_EMPTY;
}

/* Release every ARAM allocation the driver owns (cached ADPCM samples + the per-
   waveform wavetable blocks; sWaveAram[w][0] is each block's base). Caller must
   ensure no AicaSynth_Update is in flight. */
static void aica_free_aram(void) {
    s32 i; u32 w, h;
    for (i = 0; i < ARAM_CACHE_ENTRIES; i++) {
        if (sCache[i].key != KEY_EMPTY && sCache[i].aram) snd_mem_free(sCache[i].aram);
        sCache[i].key = KEY_EMPTY; sCache[i].aram = 0; sCache[i].refs = 0; sCache[i].len = 0;
    }
    for (w = 0; w < NUM_WAVEFORMS; w++) {
        if (sWaveAram[w][0]) snd_mem_free(sWaveAram[w][0]);
        for (h = 0; h < NUM_HARMONICS; h++) sWaveAram[w][h] = 0;
    }
}

/* Stop the driver cleanly: make Update a no-op, wait for any in-flight Update on
   the audio thread to drain, silence the AICA voices, then free ARAM. Safe (and
   a no-op) to call more than once. Registered via atexit() and also called from
   the game's exit path. */
void AicaSynth_Shutdown(void) {
    s32 i, spin;
    if (!sRunning) return;
    sRunning = 0;                                  /* Update bails from here on */
    __asm__ __volatile__("" ::: "memory");
    /* Drain: wait out a concurrent Update so we don't free ARAM under it. Bounded
       so a wedged audio thread can't hang exit. */
    for (spin = 0; sInUpdate && spin < 100000; spin++)
        thd_pass();
    for (i = 0; i < MAX_VOICES; i++)
        if (sVoices[i].active) voice_stop(i);
    aica_free_aram();
}

void AicaSynth_Init(void) {
    s32 i, ch; u32 w, h;

    /* Re-init (e.g. course reload): tear the previous instance down first so we
       don't leak its ARAM. Drains any in-flight Update via AicaSynth_Shutdown. */
    if (sRunning) AicaSynth_Shutdown();

    for (i = 0; i < ARAM_CACHE_ENTRIES; i++) { sCache[i].key = KEY_EMPTY; sCache[i].aram = 0; sCache[i].refs = 0; }
    for (i = 0; i < MAX_VOICES; i++) { sVoices[i].channel = -1; sVoices[i].active = 0; sVoices[i].entry = NULL; sVoices[i].sampleKey = KEY_EMPTY; }
    sChanFreeTop = 0;
    for (ch = NUM_AICA_CHANNELS - 1; ch >= 0; ch--) sChanFree[sChanFreeTop++] = (s8)ch;

    /* gAicaAdpcmPoolBase is loaded earlier by setup_audio_data() in main.c. */
    sTblBase = (u8*)gAlTbl->seqArray[0].offset;

    for (w = 0; w < NUM_WAVEFORMS; w++) {
        u32 bytes = NUM_HARMONICS * WAVE_SAMPLE_COUNT * sizeof(s16);
        u32 aram = (u32)snd_mem_malloc(bytes);
        spu_memload_sq(aram, (void*)gWaveSamples[w], (bytes + 31) & ~31);
        for (h = 0; h < NUM_HARMONICS; h++)
            sWaveAram[w][h] = aram + h * WAVE_SAMPLE_COUNT * sizeof(s16);
    }

    sRunning = 1;
    if (!sAtexitDone) { atexit(AicaSynth_Shutdown); sAtexitDone = 1; }
}

void AicaSynth_Update(void) {
    s32 numNotes = gMaxSimultaneousNotes;
    s32 base, i;

    if (!sRunning) return;                                      /* shutting down / not inited */
    if (gAicaAdpcmPoolBase == NULL && sTblBase == NULL) return; /* not initialized yet */

    /* Mark in-flight, then re-check sRunning so AicaSynth_Shutdown either waits for
       us here or we see its flag and bail before touching snd_mem (single-core
       handshake; the barrier orders the store before the re-load). */
    sInUpdate = 1;
    __asm__ __volatile__("" ::: "memory");
    if (!sRunning) { sInUpdate = 0; return; }

    sTick++;
    if (numNotes > MAX_VOICES) numNotes = MAX_VOICES;
    base = gMaxSimultaneousNotes * (gAudioBufferParameters.updatesPerFrame - 1);

    static u32 sDbgFrame = 0;
    s32 nEnabled = 0, nResolved = 0, nStarted = 0, nFail = 0, nSynth = 0;
    u32 dbgVolMax = 0, dbgVolMin = 999;

    for (i = 0; i < numNotes; i++) {
        struct NoteSubEu* sub = &gNoteSubsEu[base + i];
        Voice* v = &sVoices[i];
        u32 key, freq, vol, pan;
        Resolved res;
        AramEntry* entry;
        int retrigger;

        if (!sub->enabled) { if (v->active) voice_stop(i); continue; }
        nEnabled++;
        if (sub->isSyntheticWave) nSynth++;
        if (!resolve(sub, &key, &res, &entry)) { nFail++; if (v->active) voice_stop(i); continue; }
        nResolved++;

        freq = calc_freq(sub, res.downsample_shift);
        vol = calc_vol(sub);
        pan = calc_pan(sub);
        if (vol > dbgVolMax) dbgVolMax = vol;
        if (vol < dbgVolMin) dbgVolMin = vol;

        retrigger = (!v->active) || (v->sampleKey != key) || sub->needsInit;
        if (retrigger) {
            s32 chn;
            if (v->active) voice_stop(i);
            chn = chan_alloc();
            if (chn < 0) { cache_release(entry); continue; }
            v->channel = chn; v->active = 1; v->sampleKey = key; v->entry = entry;
            chan_start(chn, &res, freq, vol, pan);
            nStarted++;
        } else {
            if (entry) cache_release(entry);
            chan_update(v->channel, freq, vol, pan);
        }
    }

    __asm__ __volatile__("" ::: "memory");
    sInUpdate = 0;
}

#elif defined(TARGET_XBOX)

/* DirectSound hardware-voice driver (Xbox) -- structural twin of the AICA
 * driver above. The game's audio engine already finalizes gNoteSubsEu[] every
 * frame; this maps each active note onto an Xbox DirectSound voice, which the
 * MCPX DSP mixes in hardware exactly as the AICA did on Dreamcast.
 *
 * Differences from the AICA path, all forced by the hardware:
 *  - No ARAM: samples live in main RAM. The offline pool is the SAME
 *    adpcm_pool.bin the DC build uses, but the Xbox can't play Yamaha ADPCM,
 *    so each sample is decoded to PCM16 once on first use and kept resident
 *    (pool is 2.2MB; fully decoded worst case ~8.6MB against 64MB).
 *  - Pan: Xbox has no stepped DIPAN. Better -- the L/R mixbin volumes are set
 *    straight from targetVolLeft/targetVolRight, which is more faithful than
 *    the AICA's 3dB-step approximation of the same pair.
 *  - No freq gap workaround needed: SetFrequency is linear up to 191983Hz. */

/* Suppress the C-linkage D3DX math inlines: exactly one TU (kos_xbox.c) may
 * emit them; every other C file including xtl.h defines this first. */
#define __D3DX8MATH_INL__
#include <xtl.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../../Platform/xbox/gen/aica_sample_table.h"
#include "xbox_debug.h"

extern const unsigned char* gAicaAdpcmPoolBase;  /* defined in stubs_xbox.c, loaded by main.c */
static u8* sTblBase = NULL;
static volatile int sRunning = 0;
static volatile int sInUpdate = 0;   /* same handshake as the DC driver */

#define MAX_VOICES        64
#define WAVE_SAMPLE_COUNT 64
#define NUM_WAVEFORMS     6
#define NUM_HARMONICS     4
#define KEY_EMPTY         0xFFFFFFFFu
#define SYNTH_KEY(wf, h)  (0x80000000u | ((u32)(wf) << 4) | (u32)(h))
#define SYNTH_VOL_SCALE   192

/* Decoded-PCM cache: one slot per table entry, decoded lazily, kept forever. */
static s16* sPcm[AICA_SAMPLE_COUNT];

typedef struct {
    LPDIRECTSOUNDBUFFER buf;
    u8  active;
    u32 sampleKey;
} Voice;
static Voice sVoices[MAX_VOICES];
static LPDIRECTSOUND sDS = NULL;

/* v (0..255) -> DSound hundredths-of-dB attenuation. */
static LONG sVolDb[256];

/* --- Yamaha AICA ADPCM decode, bit-exact vs tools/aica/yamaha_adpcm.py:
 *   delta = trunc(quant * DIFF[code] / 8)
 *   cur   = clamp(cur + delta, -32768, 32767)
 *   quant = clamp((quant * SCALE[code&7]) >> 8, 127, 24576)
 * low nibble first. */
static void xadpcm_decode(const u8* src, s16* dst, u32 nsamples) {
    static const s32 DIFF[16]  = { 1,3,5,7,9,11,13,15,-1,-3,-5,-7,-9,-11,-13,-15 };
    static const s32 SCALE[8]  = { 0xE6,0xE6,0xE6,0xE6,0x133,0x199,0x200,0x266 };
    s32 cur = 0, quant = 127;
    for (u32 i = 0; i < nsamples; i++) {
        u32 code = (i & 1) ? (src[i >> 1] >> 4) & 0xF : src[i >> 1] & 0xF;
        s32 delta = (quant * DIFF[code]) / 8;    /* C division truncates toward 0 */
        cur += delta;
        if (cur > 32767) cur = 32767; else if (cur < -32768) cur = -32768;
        quant = (quant * SCALE[code & 7]) >> 8;
        if (quant < 127) quant = 127; else if (quant > 24576) quant = 24576;
        dst[i] = (s16) cur;
    }
}

/* fmt per the generated table: 0 = PCM16, 1 = PCM8, 2 = ADPCM. */
static s16* pcm_acquire(const AicaSampleDesc* d) {
    u32 idx = (u32)(d - gAicaSampleTable);
    if (sPcm[idx]) return sPcm[idx];
    /* DirectSound voices DMA from physically contiguous memory; plain malloc
     * doesn't guarantee that on Xbox, XPhysicalAlloc does. */
    s16* pcm = (s16*) XPhysicalAlloc((SIZE_T) d->nsamples * 2u,
                                     MAXULONG_PTR, 0, PAGE_READWRITE);
    if (!pcm) return NULL;
    const u8* src = gAicaAdpcmPoolBase + d->pool_offset;
    if (d->fmt == 2) {
        xadpcm_decode(src, pcm, d->nsamples);
    } else if (d->fmt == 1) {
        for (u32 i = 0; i < d->nsamples; i++) pcm[i] = (s16)((s8) src[i] << 8);
    } else {
        memcpy(pcm, src, (size_t) d->nsamples * 2u);
    }
    sPcm[idx] = pcm;
    return pcm;
}

static int sample_lookup(u32 key, const AicaSampleDesc** out) {
    s32 lo = 0, hi = AICA_SAMPLE_COUNT - 1;
    while (lo <= hi) {
        s32 mid = (lo + hi) >> 1;
        u32 k = gAicaSampleTable[mid].src_offset;
        if (k == key) { *out = &gAicaSampleTable[mid]; return 1; }
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

typedef struct {
    s16* pcm;
    u32  nsamples, loop, loopstart, loopend;
    u8   downsample_shift;
} Resolved;

static int resolve(struct NoteSubEu* sub, u32* outKey, Resolved* res) {
    res->downsample_shift = 0;

    if (sub->isSyntheticWave) {
        s16* addr = sub->sound.samples;
        s32 wf = -1; u32 h = 0, w;
        for (w = 0; w < NUM_WAVEFORMS; w++) {
            if (addr >= gWaveSamples[w] && addr < gWaveSamples[w] + NUM_HARMONICS * WAVE_SAMPLE_COUNT) {
                wf = (s32) w;
                h = (u32)(addr - gWaveSamples[w]) / WAVE_SAMPLE_COUNT;
                break;
            }
        }
        if (wf < 0) return 0;
        if (h >= NUM_HARMONICS) h = NUM_HARMONICS - 1;
        *outKey = SYNTH_KEY(wf, h);
        res->pcm = gWaveSamples[wf] + h * WAVE_SAMPLE_COUNT;   /* already PCM16 in RAM */
        res->nsamples = WAVE_SAMPLE_COUNT;
        res->loop = 1; res->loopstart = 0; res->loopend = WAVE_SAMPLE_COUNT;
        return 1;
    }

    if (gAicaAdpcmPoolBase == NULL || sub->sound.audioBankSound == NULL) return 0;
    {
        struct AudioBankSample* s = sub->sound.audioBankSound->sample;
        if (s == NULL || s->sampleAddr == NULL) return 0;
        u32 key = (u32) s->sampleAddr - (u32) sTblBase;
        const AicaSampleDesc* d;
        if (!sample_lookup(key, &d)) return 0;
        s16* pcm = pcm_acquire(d);
        if (!pcm) return 0;
        *outKey = key;
        res->pcm = pcm;
        res->nsamples = d->nsamples;
        res->loop = d->loop_flag;
        res->loopstart = d->loop_start;
        res->loopend = d->loop_end ? d->loop_end : d->nsamples;
        res->downsample_shift = d->downsample_shift;
        return 1;
    }
}

static u32 calc_freq(struct NoteSubEu* sub, u8 shift) {
    u32 freq = ((u32) sub->resamplingRateFixedPoint * (u32) gAudioBufferParameters.frequency) >> 15;
    if (sub->hasTwoAdpcmParts) freq <<= 1;
    freq >>= shift;
    if (freq < DSBFREQUENCY_MIN) freq = DSBFREQUENCY_MIN;
    if (freq > DSBFREQUENCY_MAX) freq = DSBFREQUENCY_MAX;
    return freq;
}

/* Per-ear volumes straight from the note's stereo pair. */
static void voice_set_lr(LPDIRECTSOUNDBUFFER buf, struct NoteSubEu* sub) {
    u32 l = (u32) sub->targetVolLeft  >> 4;
    u32 r = (u32) sub->targetVolRight >> 4;
    if (l > 255) l = 255;
    if (r > 255) r = 255;
    if (sub->isSyntheticWave) { l = (l * SYNTH_VOL_SCALE) >> 8; r = (r * SYNTH_VOL_SCALE) >> 8; }
    DSMIXBINVOLUMEPAIR pair[2] = {
        { DSMIXBIN_FRONT_LEFT,  sVolDb[l] },
        { DSMIXBIN_FRONT_RIGHT, sVolDb[r] },
    };
    DSMIXBINS mb = { 2, pair };
    IDirectSoundBuffer_SetMixBinVolumes(buf, &mb);
}

static void voice_stop(s32 i) {
    Voice* v = &sVoices[i];
    if (v->buf) IDirectSoundBuffer_Stop(v->buf);
    v->active = 0; v->sampleKey = KEY_EMPTY;
}

void AicaSynth_Shutdown(void) {
    s32 i, spin;
    if (!sRunning) return;
    sRunning = 0;
    /* Drain a concurrent Update on the audio thread before touching the
     * voices from this one -- concurrent DSound calls on the same buffer are
     * exactly the kind of thing real hardware punishes at transitions. */
    for (spin = 0; sInUpdate && spin < 100000; spin++) Sleep(0);
    for (i = 0; i < MAX_VOICES; i++) voice_stop(i);
    /* Decoded PCM stays resident deliberately: Init/Shutdown cycles on course
     * change would otherwise re-decode the whole working set. */
}

void AicaSynth_Init(void) {
    s32 i;

    if (sRunning) AicaSynth_Shutdown();

    if (!sDS) {
        if (FAILED(DirectSoundCreate(NULL, &sDS, NULL))) {
            printf("AUDIO DirectSoundCreate FAILED\n");
            return;
        }
        for (i = 0; i < 256; i++)
            sVolDb[i] = (i == 0) ? DSBVOLUME_MIN
                                 : (LONG)(2000.0 * log10((double) i / 255.0));

        WAVEFORMATEX wfx;
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = 32000;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2;
        wfx.nAvgBytesPerSec = 64000;

        for (i = 0; i < MAX_VOICES; i++) {
            DSBUFFERDESC dsbd;
            memset(&dsbd, 0, sizeof(dsbd));
            dsbd.dwSize        = sizeof(dsbd);
            dsbd.dwBufferBytes = 0;              /* data attached per-start */
            dsbd.lpwfxFormat   = &wfx;
            if (FAILED(DirectSoundCreateBuffer(&dsbd, &sVoices[i].buf)))
                sVoices[i].buf = NULL;
        }
#if MK64X_DEBUG_TOOLS
        printf("AUDIO DirectSound up: %d hw voices\n", MAX_VOICES);
#endif
    }

    for (i = 0; i < MAX_VOICES; i++) { sVoices[i].active = 0; sVoices[i].sampleKey = KEY_EMPTY; }

    sTblBase = (u8*) gAlTbl->seqArray[0].offset;
    sRunning = 1;
}

void AicaSynth_Update(void) {
    s32 numNotes = gMaxSimultaneousNotes;
    s32 base, i;

    if (!sRunning || !sDS) return;
    if (gAicaAdpcmPoolBase == NULL && sTblBase == NULL) return;

    sInUpdate = 1;
    if (!sRunning) { sInUpdate = 0; return; }

    DirectSoundDoWork();

    if (numNotes > MAX_VOICES) numNotes = MAX_VOICES;
    base = gMaxSimultaneousNotes * (gAudioBufferParameters.updatesPerFrame - 1);

    /* WHITE-button one-shot: voice census. Own latch -- the GFX one is
     * cleared on the render thread and would race this thread.
     *
     * When the tools are off dbg is a compile-time zero, so the census block
     * below is dead code and the counters feeding it are dead stores; both go
     * away entirely at -O3 without the mixing loop needing its own #ifdefs. */
#if MK64X_DEBUG_TOOLS
    s32 dbg = xbox_snd_log_once;
    if (dbg) xbox_snd_log_once = 0;
#else
    enum { dbg = 0 };
#endif
    s32 nEn = 0, nRes = 0, nAct = 0, nFail = 0;
    u32 dbgFreq = 0, dbgL = 0, dbgR = 0, dbgN = 0;

    for (i = 0; i < numNotes; i++) {
        struct NoteSubEu* sub = &gNoteSubsEu[base + i];
        Voice* v = &sVoices[i];
        u32 key, freq;
        Resolved res;
        int retrigger;

        if (!v->buf) continue;
        if (!sub->enabled) { if (v->active) voice_stop(i); continue; }
        nEn++;
        if (!resolve(sub, &key, &res)) { nFail++; if (v->active) voice_stop(i); continue; }
        nRes++;

        freq = calc_freq(sub, res.downsample_shift);

        retrigger = (!v->active) || (v->sampleKey != key) || sub->needsInit;
        if (retrigger) {
            IDirectSoundBuffer_Stop(v->buf);
            IDirectSoundBuffer_SetBufferData(v->buf, (LPVOID) res.pcm, res.nsamples * 2u);
            IDirectSoundBuffer_SetPlayRegion(v->buf, 0, res.nsamples * 2u);
            if (res.loop && res.loopend > res.loopstart)
                IDirectSoundBuffer_SetLoopRegion(v->buf, res.loopstart * 2u,
                                                 (res.loopend - res.loopstart) * 2u);
            IDirectSoundBuffer_SetFrequency(v->buf, freq);
            voice_set_lr(v->buf, sub);
            IDirectSoundBuffer_Play(v->buf, 0, 0, res.loop ? DSBPLAY_LOOPING : 0);
            v->active = 1; v->sampleKey = key;
        } else {
            IDirectSoundBuffer_SetFrequency(v->buf, freq);
            voice_set_lr(v->buf, sub);
        }
        if (v->active) {
            nAct++;
            if (!dbgN) { dbgFreq = freq; dbgL = sub->targetVolLeft; dbgR = sub->targetVolRight; dbgN = res.nsamples; }
        }
    }

    if (dbg) {
        extern int xbox_snd_cmds_processed, xbox_snd_seqreq, xbox_snd_seqgated;
        extern unsigned char xbox_snd_ops[16];
        s32 sp0 = gSequencePlayers[0].enabled, sp1 = gSequencePlayers[1].enabled,
            sp2 = gSequencePlayers[2].enabled;
        s32 allEn = 0, liveEn = 0, k;
        for (k = 0; k < gMaxSimultaneousNotes * gAudioBufferParameters.updatesPerFrame; k++)
            if (gNoteSubsEu[k].enabled) allEn++;
        for (k = 0; k < gMaxSimultaneousNotes; k++)
            if (gNotes[k].noteSubEu.enabled) liveEn++;
        printf("SND2 cmds=%d req=%d gated=%d sp=%d/%d/%d seq0=%d allSubs=%d liveNotes=%d "
               "ops=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
               xbox_snd_cmds_processed, xbox_snd_seqreq, xbox_snd_seqgated,
               (int) sp0, (int) sp1, (int) sp2,
               (int) gSequencePlayers[0].seqId, (int) allEn, (int) liveEn,
               xbox_snd_ops[0], xbox_snd_ops[1], xbox_snd_ops[2], xbox_snd_ops[3],
               xbox_snd_ops[4], xbox_snd_ops[5], xbox_snd_ops[6], xbox_snd_ops[7],
               xbox_snd_ops[8], xbox_snd_ops[9], xbox_snd_ops[10], xbox_snd_ops[11]);
        printf("SND notes=%d enabled=%d resolved=%d fail=%d active=%d "
               "v0: freq=%u L=%u R=%u nsamp=%u maxN=%d upf=%d\n",
               (int) numNotes, (int) nEn, (int) nRes, (int) nFail, (int) nAct,
               (unsigned) dbgFreq, (unsigned) dbgL, (unsigned) dbgR, (unsigned) dbgN,
               (int) gMaxSimultaneousNotes, (int) gAudioBufferParameters.updatesPerFrame);
    }
}

#else
void AicaSynth_Init(void) {}
void AicaSynth_Update(void) {}
void AicaSynth_Shutdown(void) {}
#endif
