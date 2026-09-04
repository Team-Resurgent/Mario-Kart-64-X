#ifndef XBOX_DEBUG_H
#define XBOX_DEBUG_H

/* Three independent build switches, all off in a shipping build. They are kept
 * separate because each is useful without the others, and because none can be
 * enabled casually: the xbdm channel blocks once its buffer fills, which stalls
 * the game, so nothing here prints periodically.
 *
 * NOT gated: the guards these probes were written to diagnose -- the NaN batch
 * rejection in gfx_nv2a.cpp, the MP_matrix guard in gfx_retro_dc.c, the
 * ceremony texture byte-order fix in code_80281780.c. Those are fixes, and the
 * first is what stops the NV2A hard-locking. Only their printfs are gated. */

/* On-hardware dumps: geometry census, per-batch state, big-triangle report,
 * texture and TLUT dumps, voice census. WHITE arms a ONE-SHOT dump -- the next
 * frame prints and clears the latch. Capture with xbWatson. */
#ifndef MK64X_DEBUG_TOOLS
#define MK64X_DEBUG_TOOLS 0
#endif

/* BOTH TRIGGERS + BACK jumps straight to the award ceremony, from anywhere.
 * Off when shipping: it also lets a player land there from nowhere. */
#ifndef MK64X_CEREMONY_JUMP
#define MK64X_CEREMONY_JUMP 0
#endif

/* BLACK cycles the translucent model; see MK64X_TR_SORT in gfx_nv2a.cpp.
 * Separate from the dumps because comparing courses means playing several of
 * them, which the per-frame probes make impossible. */
#ifndef MK64X_TR_MODE_TOGGLE
#define MK64X_TR_MODE_TOGGLE 0
#endif

#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE
#ifdef __cplusplus
extern "C" {
#endif
#if MK64X_DEBUG_TOOLS
/* Armed by WHITE, cleared by whoever consumes it. Graphics and audio latch
 * separately: they are cleared from different threads and a shared flag raced. */
extern int xbox_gfx_log_once;
extern int xbox_snd_log_once;
#endif
/* Cycled by BLACK. Mode 1 is what ships; the others exist to isolate which
 * half of it is responsible when a course regresses:
 *   0 sort, ignore Z_UPD      1 submission order, honour Z_UPD
 *   2 sort, honour Z_UPD      3 submission order, ignore Z_UPD */
extern int xbox_gfx_tr_mode;
#ifdef __cplusplus
}
#endif
#endif

#endif /* XBOX_DEBUG_H */
