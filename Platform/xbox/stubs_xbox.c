/* stubs_xbox.c — the last few symbols the Xbox image needs.
 *
 * Everything here stands in for something the Dreamcast port provides from a
 * file this build does not compile: the SH-4 audio mixer (src/dcaudio), the
 * VMU browser (src/vmu.c), or the MIPS boot/libultra glue. Each block says
 * what it replaces and what the consequence is.
 *
 * The audio block is the significant one: it makes this build SILENT. It is
 * deliberately a separate, clearly marked section so that replacing it with a
 * real DirectSound mixer is a self-contained change.
 */

/* Only one C translation unit may emit the D3DX math helpers -- they have
 * external linkage in C (see gfx_xbox.c). kos_xbox.c owns them; claim the
 * .inl's include guard here so this file does not emit a second copy. */
#define __D3DX8MATH_INL__

#include <xtl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kos.h"

/* ===========================================================================
 * AUDIO — the RSP microcode command implementations.
 *
 * On N64 these run on the RSP; the Dreamcast port reimplements them for the
 * SH-4 in src/dcaudio, hand-optimised (its README credits that work as what
 * made full-speed audio possible). The Xbox equivalent is to mix in software
 * and stream to the MCPX APU through DirectSound, which is genuinely easier
 * than what they did -- the APU mixes in hardware.
 *
 * Until that exists these are no-ops, so the game runs SILENT. The audio
 * front-end above them (sequence player, heap, synthesis) is fully compiled
 * and will call these normally; nothing else has to change when they are
 * filled in.
 * ========================================================================= */

typedef int16_t ADPCM_STATE_T[16];
typedef int16_t RESAMPLE_STATE_T[16];

void aClearBufferImpl(uint16_t addr, int nbytes) { (void)addr; (void)nbytes; }
void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes) {
    (void)source_addr; (void)dest_addr; (void)nbytes;
}
void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    /* The caller treats this as "the mixed frame is now here". Zeroing keeps
     * the output buffer silent rather than full of whatever was there. */
    if (dest_addr && nbytes) {
        for (uint16_t i = 0; i < (uint16_t)(nbytes / sizeof(int16_t)); i++) dest_addr[i] = 0;
    }
    (void)source_addr;
}
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    (void)num_entries_times_16; (void)book_source_addr;
}
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    (void)flags; (void)in; (void)out; (void)nbytes;
}
void aInterleaveImpl(uint16_t left, uint16_t right) { (void)left; (void)right; }
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    (void)in_addr; (void)out_addr; (void)nbytes;
}
void aSetLoopImpl(void *adpcm_loop_state) { (void)adpcm_loop_state; }
void aADPCMdecImpl(uint8_t flags, int16_t *state) { (void)flags; (void)state; }
void aResampleImpl(uint8_t flags, uint16_t pitch, int16_t *state) {
    (void)flags; (void)pitch; (void)state;
}
void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet,
                    uint16_t rate_left, uint16_t rate_right) {
    (void)initial_vol_wet; (void)rate_wet; (void)rate_left; (void)rate_right;
}
void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right) {
    (void)initial_vol_left; (void)initial_vol_right;
}
void aEnvMixerImpl(uint16_t in_addr, uint16_t n_samples, int swap_reverb,
                   int neg_left, int neg_right,
                   uint16_t dry_left_addr, uint16_t dry_right_addr,
                   uint16_t wet_left_addr, uint16_t wet_right_addr) {
    (void)in_addr; (void)n_samples; (void)swap_reverb; (void)neg_left; (void)neg_right;
    (void)dry_left_addr; (void)dry_right_addr; (void)wet_left_addr; (void)wet_right_addr;
}
void aDMEMMove2Impl(uint8_t t, uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void)t; (void)in_addr; (void)out_addr; (void)count;
}
void aDownsampleHalfImpl(uint16_t n_samples, uint16_t in_addr, uint16_t out_addr) {
    (void)n_samples; (void)in_addr; (void)out_addr;
}

/* dcaudio's driver object is a VTABLE, not an opaque handle: _AudioInit does
 *
 *     audio_api = &audio_dc;
 *     audio_api->init();
 *
 * so it must be a real struct AudioAPI with callable entries. Declaring it as
 * a null void* made ->init() read the null and call address 0, which is
 * exactly where the title crashed.
 *
 * init() returning true is deliberate: the caller treats false as "audio
 * unavailable" and the game's audio thread still needs to run its timing, so
 * the driver reports success and simply produces nothing. */
struct AudioAPI {
    bool (*init)(void);
    int  (*buffered)(void);
    int  (*get_desired_buffered)(void);
    void (*play)(uint8_t *, size_t);
};

static bool xb_audio_init(void) { return true; }

/* The mixer keeps its queue topped up by comparing buffered() against
 * get_desired_buffered(). Reporting "already at target" means it never busy
 * -loops trying to push more into a sink that consumes nothing. */
static int  xb_audio_buffered(void)             { return 1024; }
static int  xb_audio_get_desired_buffered(void) { return 1024; }
static void xb_audio_play(uint8_t *buf, size_t len) { (void) buf; (void) len; }

struct AudioAPI audio_dc = {
    xb_audio_init,
    xb_audio_buffered,
    xb_audio_get_desired_buffered,
    xb_audio_play,
};

/* Base of the AICA ADPCM sample pool. Only the (stubbed) mixer decodes out of
 * it, so it is never dereferenced -- but main.c assigns the loaded pool buffer
 * to it, so it has to exist. */
uint8_t *gAicaAdpcmPoolBase = NULL;

/* ===========================================================================
 * VMU — src/vmu.c is the Dreamcast memory-card browser: it enumerates VMUs,
 * draws the save icon on the unit's LCD, and reports card status. None of that
 * has an Xbox equivalent; saves go to the title's own directory instead (see
 * get_vmu_fn in kos_xbox.c).
 * ========================================================================= */

/* Not a buffer despite the name: src/vmu.c declares `int32_t Pak_Memory` and
 * uses it as the file count on the card. */
int32_t Pak_Memory = 0;

void draw_vmu_icon(int controller, int charid) { (void) controller; (void) charid; }

/* Signature and return convention both matter here: callers do
 * `if (!vmu_status(0))` to mean "pack present", so 0 is success. Saves go to
 * the title's own directory, which is always available. */
int vmu_status(int channel) {
    (void) channel;
    Pak_Memory = 200;   /* what the real implementation reports on success */
    return 0;
}

/* ===========================================================================
 * libultra / boot glue that the MIPS build gets from assembly.
 * ========================================================================= */

/* Task yielding: the N64 could preempt a running RSP task and resume it. There
 * is no RSP here -- display lists are consumed synchronously by the renderer --
 * so a task is never in a yielded state. */
void osSpTaskYield(void) { }
int  osSpTaskYielded(void *task) { (void)task; return 0; }

/* libultra's internal formatter, used by the debug printf path. RXDK's own
 * printf is already linked, so this only needs to exist. */
int _Printf(void *pfn, void *arg, const char *fmt, void *ap) {
    (void)pfn; (void)arg; (void)fmt; (void)ap;
    return 0;
}

/* func_8000522C / func_80005310 / func_80005AE8 are NOT stubbed: they are real
 * functions in src/staff_ghosts.c, which this build compiles. */

/* Segment ROM markers are NOT stubbed here any more.
 *
 * They were, as empty arrays, on the assumption that this build links the
 * segment data directly and so never DMAs from them. That was wrong: the game
 * MIO0-DECODES four segments from a compressed blob bracketed by those
 * symbols, and decoding from a one-byte stub read a garbage header and wrote
 * far outside its output buffer. The real blobs are generated by
 * tools/gen_segblobs.py into Platform/xbox/gen_seg. */
