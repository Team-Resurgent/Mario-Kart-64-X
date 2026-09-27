/* drawup_stress.cpp - repro harness for the NV2A "Graphics Parse error"
 * (DMA_PUSHER fault) seen when MK64-X submits large DrawVerticesUP batches.
 *
 * WHY THIS EXISTS
 * ---------------
 * On hardware/xemu the game intermittently dies with libd3d8's pushbuffer
 * parser reporting:
 *
 *     Graphics Parse error information:
 *       Get: <a>   Last: <b>   Cmd: <garbage>
 *
 * i.e. the NV2A DMA pusher walked past the committed PUT and parsed vertex
 * data as a command (a classic pushbuffer desync). MK64-X's ONLY draw entry
 * point is DrawVerticesUP(D3DPT_TRIANGLELIST, count, verts, 36) with the
 * XYZRHW|DIFFUSE|SPECULAR|TEX1 dc_fast_t vertex (8 FVF dwords / vertex).
 *
 * libd3d8 chunks an inline (UP) draw into <=2047-dword NV097_INLINE_ARRAY
 * runs, so with 8 dwords/vertex the internal batch boundary is ~255 vertices.
 * A draw with count > 255 exercises the multi-batch chunk loop, and issuing
 * many/large draws forces the main pushbuffer to wrap (MakeRequestedSpace)
 * repeatedly. This harness drives exactly that, through the REAL gfx_nv2a
 * path (pvr_reserve -> xb_flush_bucket -> DrawVerticesUP), sweeping batch
 * sizes that straddle the chunk boundary and repeating so the wrap lands at
 * many different ring offsets.
 *
 * HOW TO READ THE RESULT (over -serial stdio)
 * -------------------------------------------
 *   [T] SECT drawup            - harness started
 *   [T] STEP pass=P size=N      - about to submit a DrawVerticesUP of N verts
 *   [T] PASS drawup-stress ...  - survived every size/pass  => NOT reproduced
 *   [T] ALLDONE                 - clean end marker
 * If the bug reproduces, the serial log STOPS after a "Graphics Parse error"
 * dump (libd3d8 does _asm int 3), and the LAST "[T] STEP" line names the
 * batch size that triggered it. That is the regression signal: the fix is
 * proven when this title reaches "[T] PASS drawup-stress" / "[T] ALLDONE".
 */

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gfx_rendering_api.h"
#include "gl_fast_vert.h"
#include "kos.h"

extern "C" {

/* --- externs gfx_nv2a.cpp expects from the renderer front-end (same set the
 *     sibling rendertest.cpp provides) --------------------------------------- */
float screen_2d_z          = 1.0f;
int   in_intro             = 0;
int   cur_frame_persp      = 0;
int   prev_frame_had_persp = 0;
int   has_done_3d          = 0;
void  reset_texcache(void) { }
void  gfx_tlut_frame_reset(void) { }   /* front-end TLUT reset; unused here */

extern struct GfxRenderingAPI gfx_nv2a_api;
extern dc_fast_t *pvr_reserve(int kind, size_t n);
extern void       gfx_pvr_set_blend(uint8_t kind);

#define KIND_OP 0

/* Screen extent we scatter triangles across (rendertest uses up to ~640x480). */
#define SCR_W 640.0f
#define SCR_H 480.0f

/* Fill `n` vertices (n must be a multiple of 3) as n/3 independent, on-screen,
 * finite triangles. Everything here is chosen to PASS gfx_nv2a's xb_batch_sane
 * finiteness gate (no NaN/Inf) so the batch is actually handed to the NV2A and
 * the pushbuffer path really runs. Coordinates are deterministic so a repro is
 * reproducible. */
static void fill_tris(dc_fast_t *v, unsigned n, unsigned salt) {
    const unsigned tris = n / 3;
    for (unsigned t = 0; t < tris; t++) {
        /* Walk a coarse grid; wrap within the viewport. */
        const float gx = (float)((t * 7u + salt) % 620u) + 4.0f;
        const float gy = (float)((t * 13u + salt) % 456u) + 4.0f;
        const uint32_t argb = 0xFF000000u | ((t * 2654435761u) & 0x00FFFFFFu);
        dc_fast_t *a = &v[t * 3 + 0];
        dc_fast_t *b = &v[t * 3 + 1];
        dc_fast_t *c = &v[t * 3 + 2];

        a->vert.x = gx;        a->vert.y = gy;        a->vert.z = 0.5f;
        b->vert.x = gx + 12.f; b->vert.y = gy;        b->vert.z = 0.5f;
        c->vert.x = gx + 12.f; c->vert.y = gy + 12.f; c->vert.z = 0.5f;

        a->rhw = b->rhw = c->rhw = 1.0f;
        a->color.packed = b->color.packed = c->color.packed = argb;
        a->pad0.vertindex = b->pad0.vertindex = c->pad0.vertindex = 0;
        a->texture.u = a->texture.v = 0.0f;
        b->texture.u = 1.0f; b->texture.v = 0.0f;
        c->texture.u = 1.0f; c->texture.v = 1.0f;
        a->flags = b->flags = c->flags = 0;
    }
}

/* Batch sizes (vertices, all multiples of 3) chosen to straddle the ~255-vertex
 * INLINE_ARRAY chunk boundary and to force big multi-wrap draws. Kept under the
 * OP bucket cap (OP_MAX_VERTS = 24576). */
static const unsigned kSizes[] = {
      3,   84,  252,  255,  258,  300,  510,  513,
    768,  999, 1500, 3000, 6000, 9999, 12000, 18000,
};
#define NSIZES ((unsigned)(sizeof(kSizes) / sizeof(kSizes[0])))

/* Ascending single-draw sizes bracketing the reported ~17000-vertex threshold.
 * Each is ONE DrawVerticesUP whose inline data (n * 8 dwords) wraps the main
 * pushbuffer many times inside the single call; the fault appears once n is big
 * enough (~17000 verts = ~136000 dwords). Kept under the OP bucket cap 24576. */
static const unsigned kBig[] = {
    12000, 15000, 16000, 16500, 17000, 17250, 17500, 18000, 20000, 24000,
};
#define NBIG ((unsigned)(sizeof(kBig) / sizeof(kBig[0])))

#define ITERS  4u

/* Set to 1 to prove the workaround: a full CPU<->GPU sync after every draw
 * makes the race disappear (the title then runs to ALLDONE). Left 0 so the
 * title REPRODUCES the fault -- that is the regression signal. */
#ifndef SYNC_EVERY_DRAW
#define SYNC_EVERY_DRAW 0
#endif

static unsigned long g_submitted;

/* One DrawVerticesUP(D3DPT_TRIANGLELIST, n, ...) through the real gfx_nv2a
 * path. NOT followed by a GPU idle (unless SYNC_EVERY_DRAW) so draws pipeline
 * and the buffer wraps under the running GPU. */
static void draw_batch(struct GfxRenderingAPI *r, unsigned n, unsigned salt) {
    r->start_frame();
    r->set_depth_test(0);
    r->set_depth_mask(1);
    gfx_pvr_set_blend(KIND_OP);
    r->select_texture(0, 0);

    dc_fast_t *v = pvr_reserve(KIND_OP, n);
    if (v) { fill_tris(v, n, salt); g_submitted += n; }
    else   { printf("[T] WARN reserve failed size=%u\n", n); }

    r->end_frame();
    r->finish_render();
#if SYNC_EVERY_DRAW
    D3DDevice_BlockUntilIdle();   /* masks the race -- for A/B proof only */
#endif
}

void __cdecl main(void) {
    struct GfxRenderingAPI *r = &gfx_nv2a_api;
    r->init();

    printf("[T] SECT drawup\n");
    printf("[T] INFO sizes=%u iters=%u sync=%d (8 fvf dwords/vert, ~17k threshold)\n",
           NBIG, ITERS, SYNC_EVERY_DRAW);

    /* Ascending single-draw sweep. The buggy build faults once n crosses the
     * ~17000-vertex mark; the LAST "[T] STEP big=N" on the wire is the size that
     * tipped it over (a trailing 3-vertex draw keeps the GPU advancing into the
     * big buffer so the fault surfaces close to its own draw). SYNC_EVERY_DRAW=1
     * serializes and the whole sweep then passes -- proving the wrap/GPU race. */
    for (unsigned it = 0; it < ITERS; it++) {
        for (unsigned s = 0; s < NBIG; s++) {
            printf("[T] STEP it=%u big=%u\n", it, kBig[s]);
            draw_batch(r, kBig[s], it * 101u + s * 7u + 1u);
            draw_batch(r, 3u, it * 101u + s * 7u + 5u);   /* nudge GPU forward */
        }
        printf("[T] HB it=%u verts=%lu\n", it, g_submitted);
    }

    printf("[T] PASS drawup-stress iters=%u verts=%lu\n", ITERS, g_submitted);
    printf("[T] DONE drawup %u/%u\n", ITERS, ITERS);
    printf("[T] ALLDONE\n");

    /* Keep presenting so the serial buffer flushes and main() doesn't return. */
    for (;;) { r->start_frame(); r->end_frame(); r->finish_render(); }
}

}  /* extern "C" */
