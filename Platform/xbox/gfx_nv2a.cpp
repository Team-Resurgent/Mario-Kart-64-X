/* gfx_nv2a.c — Direct3D 8 / NV2A backend for the MK64 Fast3D renderer.
 *
 * Counterpart to src/gfx/gfx_pvr.c: same struct GfxRenderingAPI, same direct
 * entry points the front-end calls outside the vtable, but targeting the
 * Xbox's NV2A through D3D8 instead of the Dreamcast's PowerVR2.
 *
 * Three structural differences from the PVR backend, all forced by the GPU:
 *
 *  1. NO LIVE LIST. PowerVR is tile-based deferred: one list is open at a time
 *     and opaque geometry streams straight into it through the store queue.
 *     NV2A is immediate-mode, so all three kinds (OP/PT/TR) buffer into
 *     buckets and are drawn in order at end_frame. This costs a little memory
 *     and buys the draw-order control point that (3) needs.
 *
 *  2. NO HARDWARE TRANSPARENCY SORT. PowerVR autosorts the TR list per pixel;
 *     translucency is order-independent for free. NV2A has no such thing, so
 *     the TR bucket is depth-sorted on the CPU before submission. This is the
 *     one place the Xbox is behind the Dreamcast.
 *
 *  3. NO VRAM HEAP. PowerVR textures live in 8MB of separate VRAM behind
 *     pvr_mem_malloc, which is why the PVR backend carries an LRU evictor, an
 *     allocation floor to fight fragmentation, and an OOM watchdog. The Xbox's
 *     64MB is unified: textures are ordinary allocations. The eviction path is
 *     kept but only arms under GFX_XBOX_TEX_BUDGET, since MK64's working set
 *     fits comfortably.
 *
 * The colour combiner is still evaluated per-vertex on the CPU by the
 * front-end (pvr_eval_combiner), exactly as on Dreamcast, and arrives baked
 * into each vertex's diffuse + specular. That is deliberate for this stage:
 * it reproduces the Dreamcast's known-good output on new hardware before the
 * register-combiner path replaces it. NV2A can evaluate (a-b)*c+d per pixel in
 * its 8 general combiner stages; wiring that up is the next step and will
 * delete the CPU evaluator entirely.
 */

#include <xtl.h>
#include <xgraphics.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <PR/gbi.h>
#include <kos.h>   /* Xbox shim: PVR_TXRFMT_* tags the front-end passes */

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"
#include "gl_fast_vert.h"
#include "xbox_debug.h"

/* Compiled as C++ so the D3D8 interfaces can be used in their native
 * pointer-to-method form, the way every RXDK graphics sample does. Everything
 * this file defines is consumed by the C front-end, so the whole body carries
 * C linkage. */
extern "C" {

/* ------------------------------------------------------------------ config */

#define XB_SCR_WIDTH   640
#define XB_SCR_HEIGHT  480

/* Bucket capacities. On Dreamcast OP streamed live and was unbounded; here it
 * has to be buffered, so it gets the largest share. 36 bytes/vertex:
 * 24576 + 8192 + 8192 verts == ~1.4MB total, against 64MB. */
#define OP_MAX_VERTS   24576
#define PT_MAX_VERTS    8192
#define TR_MAX_VERTS    8192
#define MAX_BATCH        768

/* Translucent model: submission order, honouring Z_UPD, as the N64 and the PC
 * port do. The Dreamcast reorders instead, but only because the PVR autosorts
 * translucency per pixel in hardware.
 *
 * Both are needed together. Sherbet Land's grey slab is opaque, so only the
 * depth buffer can hide it, and that needs the ice to write depth AND to have
 * drawn first. MK64X_TR_MODE_TOGGLE puts the four combinations on the BLACK
 * button for comparing on hardware. */
#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE
#define MK64X_TR_SORT  (xbox_gfx_tr_mode == 0 || xbox_gfx_tr_mode == 2)
#define MK64X_TR_ZUPD  (xbox_gfx_tr_mode == 1 || xbox_gfx_tr_mode == 2)
#else
#define MK64X_TR_SORT  0
#define MK64X_TR_ZUPD  1
#endif

/* Texture table size MUST match the front-end's cache (gfx_texture_cache
 * pool[]/hashmap[] in gfx_retro_dc.c, 1024). The front-end allocates one
 * backend id per cached texture and hard-stops when its pool fills, so the two
 * tables track 1:1. A larger table here only creates ids the front-end can
 * never manage. */
#define XB_TEX_MAX     1024

/* 0 disables eviction entirely (the common case: MK64's working set fits in
 * 64MB). Set to a byte budget to arm the LRU evictor. */
#define GFX_XBOX_TEX_BUDGET 0

#define XB_KIND_OP 0
#define XB_KIND_PT 1
#define XB_KIND_TR 2

/* Vertex layout: D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1.
 * dc_fast_t is reordered to match under TARGET_XBOX (see gl_fast_vert.h), so
 * the front-end's per-vertex writes land directly in D3D's expected order and
 * no conversion happens on submit. */
#define XB_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

/* ------------------------------------------------------------------ device */

static LPDIRECT3D8       sD3D    = NULL;
static LPDIRECT3DDEVICE8 sDev    = NULL;
static int               sInited = 0;

/* --------------------------------------------------------------- shaders -- */
/* The front-end treats ShaderProgram opaquely (only shader_get_info reads it),
 * so this layout is private to the backend. Kept identical to the PVR
 * backend's so the two stay diffable. */

enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    uint8_t          enabled;
    uint32_t         shader_id;
    struct CCFeatures cc;
    enum MixType     mix;
    uint8_t          texture_used[2];
    int              texture_ord[2];
    int              num_inputs;
};

static struct ShaderProgram  shader_program_pool[64];
static uint8_t               shader_program_pool_size = 0;
static struct ShaderProgram *cur_shader = NULL;

/* -------------------------------------------------------------- textures -- */

struct XbTex {
    LPDIRECT3DTEXTURE8 tex;         /* NULL if unallocated                    */
    uint32_t           alloc_size;  /* bytes; reused when a re-upload fits    */
    uint16_t           w, h;        /* padded POT dims                        */
    D3DFORMAT          fmt;
    uint8_t            filter;      /* 0 = point, 1 = bilinear                */
    uint8_t            clampu, clampv;
    uint8_t            flipu, flipv;
    float              u_scale, v_scale;  /* real/padded, read by get_*_scale */
    uint32_t           lru;
    uint32_t           use_frame;   /* last frame a queued batch referenced it */
};

/* Textures orphaned by a mid-frame re-upload. Batches queued earlier in the
 * frame still point at them, so they cannot be released until after the flush. */
static LPDIRECT3DTEXTURE8 sRetired[256];
static int                sNumRetired = 0;

static struct XbTex sTextures[XB_TEX_MAX];
static uint32_t     sTexCount   = 0;
/* Texture decode+upload calls in the CURRENT frame (reset each start_frame).
 * The real per-frame cost, as opposed to sTexCount's running slot total. */
static uint32_t     sUploadsThisFrame = 0;
static uint32_t     sLruClock   = 0;
static uint32_t     sFrameStartClock = 0;
static uint32_t     sTexBytes   = 0;
static uint32_t     sBoundTex[2] = { 0, 0 };
static uint32_t     sCurBound   = 0;
static uint32_t     sAllocFloor = 0;
static int          sCacheFlushPending = 0;

/* ----------------------------------------------------------- draw state --- */

/* One batch's worth of GPU state. Snapshotted when a batch opens; replayed
 * verbatim at flush. Small enough to memcmp for change detection. */
typedef struct {
    LPDIRECT3DTEXTURE8 tex;   /* resolved when the batch opens */
    uint32_t tex0;
    uint32_t fog_color;       /* captured per batch, not read at flush */
    uint8_t  filter;          /* sampler params likewise: a later tile load  */
    uint8_t  clampu, clampv;  /* must not retroactively re-address sprites   */
    uint8_t  flipu, flipv;    /* already queued this frame                   */
    uint8_t  depth_test;
    uint8_t  depth_write;
    uint8_t  zbias;        /* ZMODE_DEC — real D3DRS_ZBIAS, not a z nudge   */
    uint8_t  fog;
    uint8_t  tex_env;       /* enum gfx_tex_env               */
    uint8_t  blend_src;    /* enum gfx_blend_factor                         */
    uint8_t  blend_dst;
    /* The scissor belongs to the batch, not to the device at flush time.
     * Splitscreen sets a different rect per player view while the geometry
     * for every view is still queued, so applying it immediately left the
     * LAST view's rect clipping ALL four views (attract mode rendered two
     * quadrants and dropped two). Vertices carry absolute screen
     * coordinates, so this only ever clips. The VIEWPORT deliberately is
     * NOT here -- see xb_apply_state. */
    uint16_t sc_x, sc_y, sc_w, sc_h;
} XbState;

typedef struct {
    XbState  st;
    uint32_t start, count;
} XbBatch;

typedef struct {
    dc_fast_t *verts;
    XbBatch   *batch;
    uint32_t   nverts, max_verts;
    int        nbatch, max_batch, cur;
    uint8_t    dirty;
} XbBucket;

static dc_fast_t sOpVerts[OP_MAX_VERTS] __attribute__((aligned(32)));
static XbBatch   sOpBatch[MAX_BATCH];
static XbBucket  sOP = { sOpVerts, sOpBatch, 0, OP_MAX_VERTS, 0, MAX_BATCH, -1, 1 };

static dc_fast_t sPtVerts[PT_MAX_VERTS] __attribute__((aligned(32)));
static XbBatch   sPtBatch[MAX_BATCH];
static XbBucket  sPunch = { sPtVerts, sPtBatch, 0, PT_MAX_VERTS, 0, MAX_BATCH, -1, 1 };

static dc_fast_t sTrVerts[TR_MAX_VERTS] __attribute__((aligned(32)));
static XbBatch   sTrBatch[MAX_BATCH];
static XbBucket  sTR = { sTrVerts, sTrBatch, 0, TR_MAX_VERTS, 0, MAX_BATCH, -1, 1 };

static XbBucket *const sBuckets[3] = { &sOP, &sPunch, &sTR };

static uint8_t  sListKind   = XB_KIND_OP;
static uint8_t  sDepthTest  = 1;
static uint8_t  sDepthWrite = 1;
static uint8_t  sZBias      = 0;
static uint8_t  sFogEnabled = 0;
static uint8_t  sTexEnv     = GFX_TEXENV_MODULATE;  /* enum gfx_tex_env */
static uint8_t  sBlendSrc   = GFX_BLENDF_SRCALPHA;
static uint8_t  sBlendDst   = GFX_BLENDF_INVSRCALPHA;
static D3DCOLOR sFogColor   = 0;

/* Current viewport/scissor, recorded by the setters and snapshotted per batch
 * (see XbState). Default to the whole screen so anything drawn before the game
 * sets a viewport is not clipped away. */
static uint16_t sVpX = 0, sVpY = 0, sVpW = XB_SCR_WIDTH, sVpH = XB_SCR_HEIGHT;
static uint16_t sScX = 0, sScY = 0, sScW = XB_SCR_WIDTH, sScH = XB_SCR_HEIGHT;

/* Exported so the front-end's inline OP path can test it without a call. */
uint8_t gfx_pvr_op_dirty = 1;
#define sOpDirty gfx_pvr_op_dirty

/* Front-end: clears its per-frame "palette already converted" memo. */
extern "C" void gfx_tlut_frame_reset(void);

static uint32_t sFrameNum = 0;
static int      sDropV = 0, sDropB = 0;

static void xb_mark_dirty(void) {
    sOpDirty = 1;
    sTR.dirty = 1;
    sPunch.dirty = 1;
}

/* --------------------------------------------------------------- buckets -- */

static void xb_snapshot(XbState *s) {
    /* Zero first: this struct is memcmp'd to detect state changes, so its tail
     * padding has to be deterministic too. */
    memset(s, 0, sizeof(*s));
    /* Port of gfx_pvr.c pvr_compile_header's `textured` predicate: a bound
     * texture only applies when the CURRENT SHADER samples it. Without the
     * cur_shader check, untextured draws (the vertex-coloured sky gradient,
     * the pause dim quad) were modulated by whatever texture was left bound --
     * sky x cloud-texture == black sky with white cloud shapes; dim quad x
     * '8th' glyph == the giant black 8th. */
    const int textured = cur_shader && cur_shader->texture_used[0] &&
                         sBoundTex[0] && sTextures[sBoundTex[0]].tex;
    if (textured) {
        struct XbTex *bt = &sTextures[sBoundTex[0]];
        bt->use_frame  = sFrameNum;
        s->tex         = bt->tex;
        s->tex0        = sBoundTex[0];
        s->filter      = bt->filter;
        s->clampu      = bt->clampu;
        s->clampv      = bt->clampv;
        s->flipu       = bt->flipu;
        s->flipv       = bt->flipv;
    }
    s->tex_env     = sTexEnv;
    s->fog_color   = sFogColor;
    s->depth_test  = sDepthTest;
    s->depth_write = sDepthWrite;
    s->zbias       = sZBias;
    s->fog         = sFogEnabled;
    s->blend_src   = sBlendSrc;
    s->blend_dst   = sBlendDst;
    s->sc_x = sScX; s->sc_y = sScY; s->sc_w = sScW; s->sc_h = sScH;
}

/* Reserve n contiguous vertices in `kind`'s bucket, opening a new batch if the
 * state changed. Returns NULL on overflow (logged, capped) — the front-end
 * drops the source triangle rather than corrupting the bucket. */
dc_fast_t *pvr_reserve(int kind, size_t n) {
    XbBucket *b = sBuckets[kind];

    if (b->nverts + n > b->max_verts) {
        if (++sDropV <= 16 || (sDropV & 255) == 0)
            printf("GFXNV2A vert overflow #%d kind=%d f=%u\n", sDropV, kind, (unsigned) sFrameNum);
        return NULL;
    }

    XbState want;
    xb_snapshot(&want);

    int need_new = b->dirty || b->cur < 0 ||
                   memcmp(&b->batch[b->cur].st, &want, sizeof(want)) != 0;

    if (need_new) {
        if (b->nbatch >= b->max_batch) {
            if (++sDropB <= 16 || (sDropB & 255) == 0)
                printf("GFXNV2A batch overflow #%d kind=%d f=%u\n", sDropB, kind, (unsigned) sFrameNum);
            return NULL;
        }
        b->cur = b->nbatch++;
        b->batch[b->cur].st    = want;
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }

    dc_fast_t *out = &b->verts[b->nverts];
    b->nverts             += (uint32_t) n;
    b->batch[b->cur].count += (uint32_t) n;
    return out;
}

/* OP submission. On Dreamcast this wrote straight to the tile accelerator;
 * here it appends to the OP bucket like everything else. */
void pvr_submit_op(const dc_fast_t *tris, size_t n) {
    dc_fast_t *dst = pvr_reserve(XB_KIND_OP, n);
    if (dst) memcpy(dst, tris, n * sizeof(dc_fast_t));
}

/* The PVR backend emitted a poly header here. NV2A state is applied at flush
 * time from the batch snapshot, so this only has to clear the flag. Kept
 * because the front-end's hot inline path calls it by name. */
void __attribute__((noinline)) pvr_emit_op_header_slow(void) { sOpDirty = 0; }

/* ----------------------------------------------------------- TR sorting ---
 *
 * Reachable only through MK64X_TR_MODE_TOGGLE; a shipping build draws TR in
 * submission order. Compiled out rather than left to the linker because the
 * scratch arrays below are ~40KB of static. */
#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE

/* Back-to-front by BATCH. z is inverse-w scaled into [0,1] with LARGER ==
 * NEARER (see the depth note in gfx_xbox_start_frame), so ascending key order
 * draws far-to-near. Insertion sort: the bucket is already close to sorted
 * most frames, so this is near-linear in practice. */

static void xb_sort_tr(void) {
    XbBucket *b = &sTR;
    if (b->nbatch < 2) return;

    static float key[MAX_BATCH];

    for (int i = 0; i < b->nbatch; i++) {
        const XbBatch *bb = &b->batch[i];
        if (!bb->count) { key[i] = 0.0f; continue; }
        /* Mean z over the batch: cheaper than min/max and stable enough for
         * ordering whole batches against each other. */
        float acc = 0.0f;
        for (uint32_t v = 0; v < bb->count; v++)
            acc += b->verts[bb->start + v].vert.z;
        key[i] = acc / (float) bb->count;
    }

    for (int i = 1; i < b->nbatch; i++) {
        XbBatch  tmpb = b->batch[i];
        float    tmpk = key[i];
        int j = i - 1;
        while (j >= 0 && key[j] > tmpk) {
            b->batch[j + 1] = b->batch[j];
            key[j + 1]      = key[j];
            j--;
        }
        b->batch[j + 1] = tmpb;
        key[j + 1]      = tmpk;
    }
}

/* Per-triangle ordering: strictly better than ranking whole batches by their
 * mean z, which lets two surfaces at overlapping depth swap as the camera
 * moves. Measured free on hardware (133 triangles into the same 56 draw
 * calls), but it does not on its own fix an OPAQUE surface in the translucent
 * bucket -- see MK64X_TR_SORT.
 *
 * Cost is draw calls, not the sort: once depth order stops coinciding with
 * state order the runs fragment. TR_REBUILD_CAP bounds that, falling back to
 * the batch sort rather than spending the frame time.
 *
 * Must be STABLE. Coplanar triangles -- 2D panes stepped 0.0005 apart, HUD
 * quads at one z -- carry no depth to order by, so submission order is the
 * only correct answer for them. */
#define TR_MAX_TRIS     (TR_MAX_VERTS / 3)
#define TR_REBUILD_CAP  256           /* <= MAX_BATCH; bounds TR draw calls */

static uint16_t sTriIdx[TR_MAX_TRIS];    /* sorted order (triangle indices) */
static uint16_t sTriTmp[TR_MAX_TRIS];    /* merge scratch                   */
static float    sTriKey[TR_MAX_TRIS];    /* mean z per triangle             */
static uint16_t sTriSrc[TR_MAX_TRIS];    /* owning batch, for its state     */
static XbBatch  sTriBatch[TR_REBUILD_CAP];

/* Census, for a dump: how many triangles were ordered, how many draw calls
 * that became, and -- if it bailed -- which guard tripped. */
static int sTrTris, sTrRebuilt, sTrFellBack;

/* Bottom-up merge sort of sTriIdx by sTriKey, ascending (far to near).
 * Taking from the left run unless the right is STRICTLY smaller is what makes
 * it stable. */
static void xb_tri_msort(int n) {
    uint16_t *a = sTriIdx, *t = sTriTmp;
    for (int w = 1; w < n; w <<= 1) {
        for (int lo = 0; lo < n; lo += (w << 1)) {
            int mid = lo + w;        if (mid > n) mid = n;
            int hi  = lo + (w << 1); if (hi  > n) hi  = n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                t[k++] = (sTriKey[a[j]] < sTriKey[a[i]]) ? a[j++] : a[i++];
            while (i < mid) t[k++] = a[i++];
            while (j < hi)  t[k++] = a[j++];
        }
        uint16_t *sw = a; a = t; t = sw;
    }
    if (a != sTriIdx) memcpy(sTriIdx, a, (size_t) n * sizeof(uint16_t));
}

#if MK64X_DEBUG_TOOLS
/* Alpha range actually present in an uploaded texture. This is what tells a
 * surface that is MEANT to be opaque apart from one whose alpha we are
 * dropping -- the two look identical on screen and cannot be told apart from
 * the vertex colour alone. Xbox textures are swizzled, which does not matter
 * here: the SET of texels is the same either way. */
static void xb_tex_alpha_range(LPDIRECT3DTEXTURE8 t, int *lo, int *hi, unsigned *fmt) {
    *lo = -1; *hi = -1; *fmt = 0;
    if (!t) return;
    D3DSURFACE_DESC d;
    if (FAILED(t->GetLevelDesc(0, &d))) return;
    *fmt = (unsigned) d.Format;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, NULL, 0))) return;
    const unsigned n = (unsigned) d.Width * (unsigned) d.Height;
    const uint16_t *p = (const uint16_t *) lr.pBits;
    if (d.Format == D3DFMT_A4R4G4B4 || d.Format == D3DFMT_A1R5G5B5) {
        int mn = 256, mx = -1;
        for (unsigned i = 0; i < n; i++) {
            const int a = (d.Format == D3DFMT_A4R4G4B4) ? ((p[i] >> 12) * 17)
                                                        : ((p[i] >> 15) ? 255 : 0);
            if (a < mn) mn = a;
            if (a > mx) mx = a;
        }
        *lo = mn; *hi = mx;
    }
    t->UnlockRect(0);
}
#endif

/* Returns 0 if the caller should fall back to the whole-batch sort. */
static int xb_sort_tr_tris(void) {
    XbBucket *b = &sTR;
    sTrTris = sTrRebuilt = sTrFellBack = 0;
    if (b->nbatch < 2) return 1;

    /* Enumerate triangles. The batches must tile the vertex array in order for
     * an in-place permutation to mean anything -- they do, because a batch only
     * ever opens at the current end -- but verify it rather than trust it. */
    uint32_t off = 0;
    int n = 0;
    for (int i = 0; i < b->nbatch; i++) {
        const XbBatch *bb = &b->batch[i];
        if (bb->start != off || (bb->count % 3) != 0) { sTrFellBack = 1; return 0; }
        const uint32_t ntri = bb->count / 3;
        if (n + (int) ntri > TR_MAX_TRIS)             { sTrFellBack = 2; return 0; }
        const dc_fast_t *v = &b->verts[bb->start];
        for (uint32_t t = 0; t < ntri; t++) {
            sTriKey[n] = (v[t * 3 + 0].vert.z + v[t * 3 + 1].vert.z +
                          v[t * 3 + 2].vert.z) * (1.0f / 3.0f);
            sTriSrc[n] = (uint16_t) i;
            sTriIdx[n] = (uint16_t) n;
            n++;
        }
        off += bb->count;
    }
    if (off != b->nverts) { sTrFellBack = 3; return 0; }
    sTrTris = n;
    if (n < 2) return 1;

    xb_tri_msort(n);

    /* Price it before paying for it: a run of consecutive triangles sharing one
     * state is one draw call. */
    int runs = 1;
    for (int p = 1; p < n; p++) {
        const XbState *x = &b->batch[sTriSrc[sTriIdx[p - 1]]].st;
        const XbState *y = &b->batch[sTriSrc[sTriIdx[p]]].st;
        if (x != y && memcmp(x, y, sizeof(XbState)) != 0) runs++;
    }
    if (runs > TR_REBUILD_CAP) { sTrFellBack = 4; return 0; }

    /* Build the new batch list while the states are still addressable through
     * the OLD list, then install it. */
    int nb = 0;
    uint32_t start = 0, count = 0;
    for (int p = 0; p < n; p++) {
        const XbState *y = &b->batch[sTriSrc[sTriIdx[p]]].st;
        if (count == 0) {
            sTriBatch[nb].st = *y;
        } else if (memcmp(&sTriBatch[nb].st, y, sizeof(XbState)) != 0) {
            sTriBatch[nb].start = start;
            sTriBatch[nb].count = count;
            nb++;
            start += count;
            count  = 0;
            sTriBatch[nb].st = *y;
        }
        count += 3;
    }
    sTriBatch[nb].start = start;
    sTriBatch[nb].count = count;
    nb++;

    /* Permute in place by following cycles: O(n) with three verts of scratch
     * instead of a second copy of the bucket.
     *
     * The cycle walk is a SCATTER ("where element i goes") but the sort leaves
     * sTriIdx as a GATHER ("what position p wants"). Feeding one to the other
     * yields a valid permutation in the wrong order, which no bijection or
     * triangle-integrity check catches. Invert first; sTriTmp is free by now. */
    for (int p = 0; p < n; p++) sTriTmp[sTriIdx[p]] = (uint16_t) p;

    for (int p = 0; p < n; p++) {
        while (sTriTmp[p] != (uint16_t) p) {
            const int s = sTriTmp[p];
            for (int k = 0; k < 3; k++) {
                const dc_fast_t tmp   = b->verts[p * 3 + k];
                b->verts[p * 3 + k]   = b->verts[s * 3 + k];
                b->verts[s * 3 + k]   = tmp;
            }
            const uint16_t sw = sTriTmp[p];
            sTriTmp[p] = sTriTmp[s];
            sTriTmp[s] = sw;
        }
    }

    memcpy(b->batch, sTriBatch, (size_t) nb * sizeof(XbBatch));
    b->nbatch  = nb;
    sTrRebuilt = nb;
    return 1;
}
#endif /* TR ordering, toggle-only */

/* ------------------------------------------------------------ state apply - */

static DWORD xb_blend(uint8_t code) {
    switch (code) {
        case GFX_BLENDF_ZERO:        return D3DBLEND_ZERO;
        case GFX_BLENDF_ONE:         return D3DBLEND_ONE;
        case GFX_BLENDF_SRCALPHA:    return D3DBLEND_SRCALPHA;
        case GFX_BLENDF_INVSRCALPHA: return D3DBLEND_INVSRCALPHA;
        case GFX_BLENDF_DSTALPHA:    return D3DBLEND_DESTALPHA;
        case GFX_BLENDF_INVDSTALPHA: return D3DBLEND_INVDESTALPHA;
        default:                     return D3DBLEND_ONE;
    }
}

static DWORD xb_address(uint8_t clamp, uint8_t flip) {
    if (clamp) return D3DTADDRESS_CLAMP;
    if (flip)  return D3DTADDRESS_MIRROR;
    return D3DTADDRESS_WRAP;
}

static void xb_apply_state(const XbState *s, int kind, int force_depth_test) {
    /* --- scissor ---
     * Only the SCISSOR is replayed per batch, which is what the N64 actually
     * uses to bound each splitscreen pane (RDP scissor). The RSP viewport
     * merely transforms vertices, and gfx_calc_and_set_viewport already bakes
     * it into the screen coordinates the front-end emits -- so the D3D
     * viewport must stay full-screen for the whole frame (set once in
     * start_frame). Setting it per batch DISPLACED the picture on hardware:
     * NV2A still runs pre-transformed vertices through the viewport
     * offset/scale registers, so the baked-in offset got applied twice.
     * Keeping it full-screen also permanently removes the degenerate-rect
     * GPU fault the fade animation used to trigger. */
    {
        D3DRECT r;
        r.x1 = s->sc_x;             r.y1 = s->sc_y;
        r.x2 = s->sc_x + s->sc_w;   r.y2 = s->sc_y + s->sc_h;
        sDev->SetScissors(1, FALSE, &r);
    }

    /* --- depth ---
     * z carries inverse-w with larger == nearer, matching the front-end's bake,
     * so the comparison is GREATEREQUAL against a zero-cleared buffer. */
    sDev->SetRenderState(D3DRS_ZENABLE,
                         (s->depth_test || force_depth_test) ? D3DZB_TRUE : D3DZB_FALSE);
    sDev->SetRenderState(D3DRS_ZFUNC, D3DCMP_GREATEREQUAL);
    sDev->SetRenderState(D3DRS_ZWRITEENABLE, s->depth_write ? TRUE : FALSE);
    /* Real depth bias — the Dreamcast had to nudge the vertex z instead. */
    sDev->SetRenderState(D3DRS_ZBIAS, s->zbias ? 1 : 0);

    /* --- fog ---
     * NV2A has a real fog unit, so the density does not need smuggling through
     * the offset colour's alpha the way PowerVR required. */
    const int fog_on = s->fog;
    sDev->SetRenderState(D3DRS_FOGENABLE, fog_on ? TRUE : FALSE);
    if (fog_on) {
        sDev->SetRenderState(D3DRS_FOGCOLOR, s->fog_color);
        /* Both modes NONE means "take the fog factor from the vertex". For
         * pre-transformed (XYZRHW) vertices D3D reads it from SPECULAR ALPHA,
         * which is exactly where the front-end already puts the N64 fog
         * coefficient for PowerVR -- inverted on the way in, see gfx_sp_tri1.
         * NV2A has no D3DRS_FOGVERTEXMODE; the Xbox header comments it out. */
        sDev->SetRenderState(D3DRS_FOGTABLEMODE,  D3DFOG_NONE);
        sDev->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    }

    /* --- per-kind raster mode --- */
    switch (kind) {
        case XB_KIND_OP:
            sDev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            sDev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            break;
        case XB_KIND_PT:
            /* Alpha-test cutouts: opaque pixels that still write depth.
             * 0xC0 mirrors the PVR backend's punch-through reference. */
            sDev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            sDev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
            sDev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
            /* Was 0xC0, which ate the antialiased edge texels of glyphs and
             * small icons (speckled menu icons). The PVR backend never sets a
             * PT ref -- KOS's default is far lower -- and the N64's own alpha
             * compare on these paths is a low threshold. */
            sDev->SetRenderState(D3DRS_ALPHAREF, 0x60);
            break;
        case XB_KIND_TR: {
            /* A depth-writing translucent surface must not write depth for
             * fully transparent texels: the N64 gates its z write on coverage,
             * and zero coverage writes none. Without this the invisible corners
             * of a shadow quad stamp a rectangular depth hole and reject the
             * water behind them (DK's Jungle Parkway). GREATER against 0 kills
             * only alpha==0, so blending is unaffected. */
            const int zwrite = (MK64X_TR_ZUPD && s->depth_write);
            sDev->SetRenderState(D3DRS_ALPHATESTENABLE, zwrite ? TRUE : FALSE);
            if (zwrite) {
                sDev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
                sDev->SetRenderState(D3DRS_ALPHAREF, 0x00);
            }
            sDev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            sDev->SetRenderState(D3DRS_ZWRITEENABLE, zwrite ? TRUE : FALSE);
            sDev->SetRenderState(D3DRS_SRCBLEND, xb_blend(s->blend_src));
            sDev->SetRenderState(D3DRS_DESTBLEND, xb_blend(s->blend_dst));
            break;
        }
    }

    /* --- texture + stage ---
     * Stage 0 is MODULATE(texture, diffuse) with the specular offset added
     * afterwards by the fixed-function pipe. That reproduces exactly what the
     * PVR poly header did with argb/oargb, which is what the front-end's CPU
     * combiner is still baking. Replaced by register combiners next. */
    if (s->tex0 && s->tex) {
        sDev->SetTexture(0, (LPDIRECT3DBASETEXTURE8) s->tex);
        sDev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        sDev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        sDev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        sDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        /* The four modes the front-end derives from the N64 combiner, in the
         * terms its own enum states them:
         *   REPLACE        px  = tex
         *   MODULATE       rgb = col*tex,               a = tex.a
         *   DECAL          rgb = lerp(col, tex, tex.a), a = col.a
         *   MODULATEALPHA  rgb = col*tex,               a = col.a*tex.a
         * BLENDTEXTUREALPHA computes Arg1*tex.a + Arg2*(1-tex.a), which is the
         * DECAL lerp with Arg1 = texture and Arg2 = diffuse. */
        switch (s->tex_env) {
        case GFX_TEXENV_REPLACE:
            sDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            sDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            break;
        case GFX_TEXENV_DECAL:
            sDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
            sDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            break;
        case GFX_TEXENV_MODULATEALPHA:
            sDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            sDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            break;
        case GFX_TEXENV_MODULATE:
        default:
            sDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            sDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            break;
        }
        sDev->SetTextureStageState(0, D3DTSS_ADDRESSU,  xb_address(s->clampu, s->flipu));
        sDev->SetTextureStageState(0, D3DTSS_ADDRESSV,  xb_address(s->clampv, s->flipv));
        sDev->SetTextureStageState(0, D3DTSS_MAGFILTER, s->filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        sDev->SetTextureStageState(0, D3DTSS_MINFILTER, s->filter ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        sDev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    } else {
        sDev->SetTexture(0, NULL);
        sDev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
        sDev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        sDev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2);
        sDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }
    sDev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    sDev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

/* Z-off geometry at far-pinned z is a BACKDROP: the front-end pins skybox
 * bands and cloud billboards to rz_far ~= 1e-5 (gfx_sp_tri1) so the PVR
 * autosort keeps them behind everything. With no autosort here they must be
 * drawn FIRST instead -- before OP -- with depth untouched, exactly the
 * painter order the N64 used. (Depth-testing them instead was tried and
 * rejects them wherever anything wrote depth into the sky region.) Near Z-off
 * overlays (fades, logo: z ~= 0.2 after the depth map) are orders of
 * magnitude above the threshold and stay in the normal TR pass. */
static int xb_is_backdrop(const XbBucket *b, const XbBatch *bb) {
    return !bb->st.depth_test && b->verts[bb->start].vert.z < 0.01f;
}

static int sDropNaN = 0;

/* Reject batches carrying non-finite or absurd coordinates. The race-finish /
 * attract-exit sequences occasionally emit NaN vertices (GFXZ once logged
 * z=INT_MIN with over=23 -- INT_MIN is a NaN cast to int). xemu's rasterizer
 * shrugs these off, but feeding NaN pre-transformed vertices to the real
 * NV2A's setup engine hangs the GPU dead: the exact hardware-only hardlock at
 * end-of-race transitions. A vertex is sane when every field compares with
 * itself (NaN fails x==x) and sits within a generous screen-space bound. */
static uint32_t sSaneFailIdx;    /* diagnostics: which vertex, which test */
static char     sSaneFailWhy;

static int xb_batch_sane(const XbBucket *b, const XbBatch *bb) {
    const dc_fast_t *v = &b->verts[bb->start];
    for (uint32_t i = 0; i < bb->count; i++) {
        const float x = v[i].vert.x, y = v[i].vert.y, z = v[i].vert.z, w = v[i].rhw;
        sSaneFailIdx = i;
        /* Reject ONLY non-finite values ((v - v) != 0 for NaN and +/-inf).
         * Earlier +/-8192 coordinate bounds were WRONG: the near-plane fan
         * clipper legally produces huge finite screen Y (y_clip / w=eps) for
         * triangles crossing the eye plane, and the NV2A scissors those fine
         * -- the bounds were silently eating real road and tree batches
         * (GFXNAN why=y, x on-screen, sane w). Only NaN hangs hardware. */
        if (!((x - x == 0.0f) && (y - y == 0.0f) &&
              (z - z == 0.0f) && (w - w == 0.0f))) { sSaneFailWhy = 'N'; return 0; }
    }
    return 1;
}

static void xb_flush_bucket(XbBucket *b, int kind, int backdrops) {
    for (int i = 0; i < b->nbatch; i++) {
        const XbBatch *bb = &b->batch[i];
        if (bb->count < 3) continue;
        if (xb_is_backdrop(b, bb) != backdrops) continue;
        if (!xb_batch_sane(b, bb)) {
            /* The REJECTION is not a diagnostic -- it is what stops the NV2A
             * hard-locking on a NaN vertex -- so only the report is gated. */
            sDropNaN++;
#if MK64X_DEBUG_TOOLS
            if (sDropNaN <= 8 || (sDropNaN & 255) == 0) {
                const dc_fast_t *vf = &b->verts[bb->start + sSaneFailIdx];
                printf("GFXNAN #%d kind=%d n=%u f=%u tex=%u why=%c vi=%u "
                       "v=(%d,%d,z%d,w%d)/1000\n",
                       sDropNaN, kind, (unsigned) bb->count, (unsigned) sFrameNum,
                       (unsigned) bb->st.tex0, sSaneFailWhy, (unsigned) sSaneFailIdx,
                       (int) (vf->vert.x * 1000.0f), (int) (vf->vert.y * 1000.0f),
                       (int) (vf->vert.z * 1000.0f), (int) (vf->rhw * 1000.0f));
            }
#endif
            continue;
        }
        xb_apply_state(&bb->st, kind, 0);
        sDev->DrawVerticesUP(D3DPT_TRIANGLELIST, bb->count,
                             &b->verts[bb->start], sizeof(dc_fast_t));
    }
}

/* ---------------------------------------------------------------- shaders - */

static uint8_t gfx_xbox_z_is_from_0_to_1(void) {
    /* D3D8 clip space is z in [0,1] — unlike PowerVR, which is 1/w only. */
    return 1;
}

static void gfx_xbox_unload_shader(UNUSED struct ShaderProgram *old_prg) {
    cur_shader = NULL;
    /* The snapshot's `textured` predicate reads cur_shader, so shader
     * transitions must open a new batch just like any other state change. */
    xb_mark_dirty();
}

static void gfx_xbox_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    xb_mark_dirty();
}

static struct ShaderProgram *gfx_xbox_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id       = shader_id;
    prg->cc              = ccf;
    prg->num_inputs      = ccf.num_inputs;
    prg->texture_used[0] = ccf.used_textures[0];
    prg->texture_used[1] = ccf.used_textures[1];

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        prg->texture_ord[0] = 0;
        prg->texture_ord[1] = 1;
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    } else {
        prg->mix = SH_MT_NONE;
    }

    prg->enabled = 0;
    gfx_xbox_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_xbox_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static void gfx_xbox_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs,
                                     uint8_t used_textures[2]) {
    *num_inputs      = prg->num_inputs;
    used_textures[0] = prg->texture_used[0];
    used_textures[1] = prg->texture_used[1];
}

/* --------------------------------------------------------------- textures - */

static uint32_t gfx_xbox_new_texture(void) {
    uint32_t id = ++sTexCount;
    if (id < XB_TEX_MAX) { sTextures[id].u_scale = 1.0f; sTextures[id].v_scale = 1.0f; }
    if (id >= XB_TEX_MAX) {
        /* Clamping means the id space is exhausted this reset cycle — every
         * further texture would alias slot 1023 and the scene would draw with
         * one texture everywhere. Force a flush instead. */
        sCacheFlushPending = 1;
        id = XB_TEX_MAX - 1;
    }
    return id;
}

static void gfx_xbox_select_texture(int tile, uint32_t texture_id) {
    sBoundTex[tile] = texture_id;
    sCurBound       = texture_id;
    sTextures[texture_id].lru = ++sLruClock;
    if (tile == 0) xb_mark_dirty();
}

int gfx_pvr_texture_resident(uint32_t id) {
    return id < XB_TEX_MAX && sTextures[id].tex != NULL;
}

void gfx_pvr_request_cache_flush(void) { sCacheFlushPending = 1; }

void gfx_pvr_set_alloc_floor(uint32_t full_w, uint32_t full_h) {
    sAllocFloor = full_w * full_h * 2u;
}

/* -------------------------------------------------- deferred destruction --
 * Xbox D3D is thin: Release() frees the memory IMMEDIATELY, but Present is
 * asynchronous -- the GPU can still be executing a pushbuffer that references
 * the texture. Freeing it then makes the NV2A read reused memory, which hangs
 * real hardware (xemu doesn't care). Transitions are exactly when we mass-free
 * (course cache flush right after the fade draws), matching the fade-to-black
 * -then-hardlock at race end / attract exit. So no texture is ever Released
 * directly: frees go into a ring and execute 3 rendered frames later, by which
 * point the GPU has provably consumed every referencing command. */
#define XB_FREE_RING     4
#define XB_FREE_RING_CAP 1024
static LPDIRECT3DTEXTURE8 sFreeRing[XB_FREE_RING][XB_FREE_RING_CAP];
static int                sFreeRingN[XB_FREE_RING];

static void xb_defer_release(LPDIRECT3DTEXTURE8 tex) {
    const int slot = (int) (sFrameNum & (XB_FREE_RING - 1));
    if (sFreeRingN[slot] < XB_FREE_RING_CAP)
        sFreeRing[slot][sFreeRingN[slot]++] = tex;
    else
        tex->Release();   /* ring full: should be unreachable (cap == table) */
}

/* Called at the top of start_frame: the slot about to be reused belongs to
 * frame N-3, whose GPU work completed at least one Present ago. */
static void xb_run_deferred_releases(void) {
    const int slot = (int) (sFrameNum & (XB_FREE_RING - 1));
    for (int i = 0; i < sFreeRingN[slot]; i++)
        sFreeRing[slot][i]->Release();
    sFreeRingN[slot] = 0;
}

static void xb_free_tex(struct XbTex *t) {
    if (!t->tex) return;
    xb_defer_release(t->tex);
    t->tex        = NULL;
    sTexBytes    -= t->alloc_size;
    t->alloc_size = 0;
}

void gfx_pvr_clear_all_textures(void) {
    /* Sweep the whole table, not `i <= sTexCount`: the front-end only calls
     * new_texture() the first time each pool node is used, so sTexCount stops
     * climbing and bounding the loop by it would leak every other texture. */
    for (uint32_t i = 0; i < XB_TEX_MAX; i++)
        xb_free_tex(&sTextures[i]);
    sTexCount    = 0;
    sBoundTex[0] = 0;
    sBoundTex[1] = 0;
    sCurBound    = 0;
    sTexBytes    = 0;
}

extern void reset_texcache(void);

void nuke_everything(void) {
    gfx_pvr_clear_all_textures();
    reset_texcache();
}

/* The front-end hands us data already in 16-bit PVR layout. ARGB1555 and
 * ARGB4444 have identical bit order to D3D's A1R5G5B5 and A4R4G4B4, so this is
 * a format tag translation, not a pixel conversion. */
/* Round up to a power of two, with the same 8-texel floor the PVR backend
 * used. Swizzled NV2A textures require power-of-two dimensions. */
static uint32_t xb_next_pot(uint32_t v) {
    uint32_t p = 8;
    while (p < v) p <<= 1;
    return p;
}

/* Edge-clamp pad into a POT rectangle -- never resample. Replicating the last
 * column and row rather than zero-filling keeps the padding from bleeding a
 * transparent seam in when the sampler filters near the edge. Same approach as
 * the PVR backend's pvr_pad16. */
#define XB_PAD_MAX (512u * 512u)
static uint16_t sPadScratch[XB_PAD_MAX] __attribute__((aligned(32)));

static void xb_pad16(const uint16_t *in, uint32_t iw, uint32_t ih,
                     uint16_t *out, uint32_t ow, uint32_t oh) {
    uint32_t y;
    for (y = 0; y < ih; y++) {
        uint16_t *o = out + (y * ow);
        memcpy(o, in + (y * iw), iw * 2u);
        const uint16_t edge = iw ? in[(y * iw) + iw - 1] : 0;
        for (uint32_t x = iw; x < ow; x++) o[x] = edge;
    }
    if (ih) {
        const uint16_t *last = out + ((ih - 1) * ow);
        for (; y < oh; y++) memcpy(out + (y * ow), last, ow * 2u);
    }
}

static void gfx_xbox_upload_texture(const uint8_t *buf16, int width, int height,
                                    unsigned int type) {
    sUploadsThisFrame++;
    struct XbTex *t = &sTextures[sCurBound];
    const D3DFORMAT fmt = ((type & ~PVR_TXRFMT_NONTWIDDLED) == PVR_TXRFMT_ARGB4444)
                              ? D3DFMT_A4R4G4B4 : D3DFMT_A1R5G5B5;

    /* N64 texture dimensions are frequently not powers of two -- 321x2, 27x17,
     * 125x8 all show up in MK64's first frames. The PVR backend padded them
     * here, in the BACKEND, and told the front-end about it through
     * get_u_scale/get_v_scale; the front-end does not pad. Do the same. */
    const uint32_t rw = (uint32_t) width, rh = (uint32_t) height;
    if (!rw || !rh) return;

    uint32_t w = rw, h = rh;
    const uint16_t *src = (const uint16_t *) buf16;
    float us = 1.0f, vs = 1.0f;

    if ((rw & (rw - 1)) || (rh & (rh - 1)) || rw < 8 || rh < 8) {
        const uint32_t pw = xb_next_pot(rw), ph = xb_next_pot(rh);
        if ((uint64_t) pw * ph <= XB_PAD_MAX) {
            xb_pad16(src, rw, rh, sPadScratch, pw, ph);
            src = sPadScratch;
            w = pw; h = ph;
            /* The used sub-region is the original extent; the front-end scales
               its UVs by this so the padding is never sampled. */
            us = (float) rw / (float) pw;
            vs = (float) rh / (float) ph;
        } else {
            /* Too large to pad into the scratch. Rare; upload as-is rather
               than drop the texture entirely. */
            printf("GFXNV2A oversized non-POT %ux%u\n", (unsigned) rw, (unsigned) rh);
        }
    }

    t->u_scale = us;
    t->v_scale = vs;

    const uint32_t size = w * h * 2u;
    const uint32_t want = (size > sAllocFloor) ? size : sAllocFloor;
    sAllocFloor = 0;   /* consumed per upload */

    /* Reuse the existing surface when the upload fits and the format matches:
     * menu squish effects re-upload the same texture at varying dims every
     * frame, and reallocating each time would churn. */
    /* The front-end re-uploads into an existing texture_id when its cache node
     * goes dirty (gfx_retro_dc.c: "re-upload into this same texture_id"). On
     * Dreamcast that was safe because geometry was submitted live to the tile
     * accelerator as it was built. Here every batch is deferred to end_frame,
     * so overwriting the surface in place retroactively changes the texture of
     * every batch already queued against this slot -- textures visibly swap
     * between objects. Orphan the old surface instead and build a new one;
     * the retired list is released after the flush. */
    if (t->tex && t->use_frame == sFrameNum) {
        if (sNumRetired < (int) (sizeof(sRetired) / sizeof(sRetired[0]))) {
            sRetired[sNumRetired++] = t->tex;
        } else {
            t->tex->Release();          /* list full: fall back to a stale draw */
        }
        sTexBytes -= t->alloc_size;
        t->tex = NULL;
        t->alloc_size = 0;
    }

    if (t->tex && (t->alloc_size < want || t->fmt != fmt || t->w != w || t->h != h))
        xb_free_tex(t);

    if (!t->tex) {
        if (FAILED(sDev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t->tex))) {
            printf("GFXNV2A CreateTexture %ux%u failed\n", (unsigned) w, (unsigned) h);
            sCacheFlushPending = 1;
            t->tex = NULL;
            return;
        }
        t->alloc_size = want;
        sTexBytes    += want;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(t->tex->LockRect(0, &lr, NULL, 0))) return;
    /* Swizzle into the NV2A's native layout. Unlike the Dreamcast — where
     * twiddling on every upload was measured as a major slowdown and had to be
     * abandoned — XGSwizzleRect is cheap enough to keep, and swizzled textures
     * sample faster and support wrap/mirror addressing without restriction. */
    XGSwizzleRect(src, 0, NULL, lr.pBits, w, h, NULL, 2);
    t->tex->UnlockRect(0);

    t->w   = (uint16_t) w;
    t->h   = (uint16_t) h;
    t->fmt = fmt;

#if MK64X_DEBUG_TOOLS
    /* Boot-time diagnostic: the spinning-logo reflection ramp is among the
     * first textures uploaded, and its decoded content is the prime suspect
     * for the logo's wrong colours. Print a few texels of the first uploads.
     * "First three slots" is per texture-cache generation, not per run, so
     * this fires again after every flush -- noisy, hence gated. */
    if (sCurBound <= 3) {
        printf("TEXUP id=%u %ux%u fmt=%s texels[0,8,16,24,31 diag]="
               "%04X %04X %04X %04X %04X\n",
               (unsigned) sCurBound, (unsigned) rw, (unsigned) rh,
               (fmt == D3DFMT_A4R4G4B4) ? "4444" : "1555",
               ((const uint16_t *) buf16)[0],
               (rw > 8  && rh > 8)  ? ((const uint16_t *) buf16)[8  * rw + 8]  : 0,
               (rw > 16 && rh > 16) ? ((const uint16_t *) buf16)[16 * rw + 16] : 0,
               (rw > 24 && rh > 24) ? ((const uint16_t *) buf16)[24 * rw + 24] : 0,
               ((const uint16_t *) buf16)[(rh - 1) * rw + (rw - 1)]);
    }
#endif
    xb_mark_dirty();
}

static void gfx_xbox_set_sampler_parameters(int tile, uint8_t linear_filter,
                                            uint32_t cms, uint32_t cmt) {
    struct XbTex *t = &sTextures[sBoundTex[tile]];
    t->filter = linear_filter ? 1 : 0;
    /* CLAMP wins, else MIRROR (mirror-repeat), else plain repeat — matching
     * the precedence the GLdc and PVR backends both use. */
    t->clampu = (cms & G_TX_CLAMP) ? 1 : 0;
    t->clampv = (cmt & G_TX_CLAMP) ? 1 : 0;
    t->flipu  = (!(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR)) ? 1 : 0;
    t->flipv  = (!(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR)) ? 1 : 0;
    xb_mark_dirty();
}

float gfx_pvr_get_u_scale(void) { return sTextures[sBoundTex[0]].u_scale; }
float gfx_pvr_get_v_scale(void) { return sTextures[sBoundTex[0]].v_scale; }

/* ------------------------------------------------------------ front-end -- */

static void gfx_xbox_set_depth_test(uint8_t depth_test) {
    if (sDepthTest != depth_test) { sDepthTest = depth_test; xb_mark_dirty(); }
}

static void gfx_xbox_set_depth_mask(uint8_t z_upd) {
    if (sDepthWrite != z_upd) { sDepthWrite = z_upd; xb_mark_dirty(); }
}

static void gfx_xbox_set_zmode_decal(uint8_t zmode_decal) {
    /* On Dreamcast this was necessarily a no-op: PowerVR has no depth bias, so
     * decals were resolved by nudging the vertex z in the front-end. NV2A has
     * D3DRS_ZBIAS, so this becomes real state. */
    uint8_t v = zmode_decal ? 1 : 0;
    if (sZBias != v) { sZBias = v; xb_mark_dirty(); }
}

static void gfx_xbox_set_tex_env(uint32_t mode) {
    /* The front-end reduces the N64 combiner to one of four texel<->vertex
     * combines and tells the backend which. Treating them all as MODULATE --
     * which this did -- multiplies a REPLACE texture by the vertex colour, so
     * anything whose baked colour is black renders black. That is what the
     * character portraits were doing. */
    if (sTexEnv != (uint8_t) mode) {
        sTexEnv = (uint8_t) mode;
        xb_mark_dirty();
    }
}

static void gfx_xbox_set_viewport(int x, int y, int width, int height) {
    /* PowerVR clips per tile, so the PVR backend discarded these entirely.
     * NV2A needs them — four-player splitscreen depends on it.
     *
     * GUARD (the end-of-race / attract-exit hardlock): the fade animates the
     * viewport narrower each frame and lands on Width == 0, which reaches the
     * NV2A as SET_VIEWPORT_CLIP_HORIZONTAL min=320 max=319 -- an inverted clip
     * rect the real GPU faults on (Class 97 / Method 02c0 / Data 013f0140 in
     * the crash record) while xemu shrugs. Clamp every field to a legal,
     * on-screen, >=1px rect; on the fade's final frame a 1px viewport of
     * solid black is invisible. */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x > XB_SCR_WIDTH - 1)  x = XB_SCR_WIDTH - 1;
    if (y > XB_SCR_HEIGHT - 1) y = XB_SCR_HEIGHT - 1;
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    if (x + width  > XB_SCR_WIDTH)  width  = XB_SCR_WIDTH - x;
    if (y + height > XB_SCR_HEIGHT) height = XB_SCR_HEIGHT - y;

    /* Recorded for diagnostics only. The device viewport stays full-screen all
     * frame -- see the scissor block in xb_apply_state for why. */
    sVpX = (uint16_t) x;      sVpY = (uint16_t) y;
    sVpW = (uint16_t) width;  sVpH = (uint16_t) height;
}

static void gfx_xbox_set_scissor(int x, int y, int width, int height) {
    /* FLIP TO D3D SPACE FIRST. The front-end builds this rect in OpenGL/GLdc
     * window space, whose origin is the BOTTOM-left -- gfx_dp_set_scissor
     * computes y = (SCREEN_HEIGHT - lry) * RATIO_Y. D3D's scissor origin is
     * the TOP-left, and the vertex screen map already delivers geometry in
     * top-left space (sm_ybias = fb_h - vpf_y - vh/2), so an unflipped rect
     * clips the MIRRORED region: each splitscreen pane was clipped by the
     * other pane's rect and vanished. Verified against a hardware GFXB dump --
     * a pane drawing at y 0..240 carried sc=(0,240 640x240). */
    y = XB_SCR_HEIGHT - (y + height);

    /* Same degenerate-rect guard as set_viewport: an empty or inverted rect
     * in the pushbuffer faults real hardware. */
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x > XB_SCR_WIDTH - 1)  x = XB_SCR_WIDTH - 1;
    if (y > XB_SCR_HEIGHT - 1) y = XB_SCR_HEIGHT - 1;
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    if (x + width  > XB_SCR_WIDTH)  width  = XB_SCR_WIDTH - x;
    if (y + height > XB_SCR_HEIGHT) height = XB_SCR_HEIGHT - y;

    /* Recorded, not applied -- see gfx_xbox_set_viewport. */
    if (sScX != x || sScY != y || sScW != width || sScH != height) {
        sScX = (uint16_t) x;      sScY = (uint16_t) y;
        sScW = (uint16_t) width;  sScH = (uint16_t) height;
        xb_mark_dirty();
    }
}

static void gfx_xbox_set_use_alpha(UNUSED uint8_t use_alpha) {
    /* OP/PT/TR routing arrives via gfx_pvr_set_blend. */
}

/* Front-end list classifier: 0 = OP, 1 = PT, 2 = TR. Routing only. */
void gfx_pvr_set_blend(uint8_t kind) { sListKind = kind; }

void gfx_pvr_set_blend_factors(uint8_t src_code, uint8_t dst_code) {
    if (src_code != sBlendSrc || dst_code != sBlendDst) {
        sBlendSrc = src_code;
        sBlendDst = dst_code;
        sTR.dirty = 1;   /* blend state only affects the TR bucket */
    }
}

void gfx_pvr_set_fog(uint8_t enabled) {
    uint8_t v = enabled ? 1 : 0;
    if (sFogEnabled != v) { sFogEnabled = v; xb_mark_dirty(); }
}

void gfx_pvr_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    /* Must open a new batch: geometry is deferred to end_frame, so without
     * this every batch queued before the change would be flushed with the
     * colour that happened to be current at flush time. */
    D3DCOLOR c = D3DCOLOR_ARGB(a, r, g, b);
    if (sFogColor != c) { sFogColor = c; xb_mark_dirty(); }
}

static void gfx_xbox_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len,
                                    size_t buf_vbo_num_tris) {
    const dc_fast_t *tris = (const dc_fast_t *) buf_vbo;
    const size_t n = buf_vbo_num_tris * 3;
    dc_fast_t *dst = pvr_reserve(sListKind, n);
    if (dst) memcpy(dst, tris, n * sizeof(dc_fast_t));
}

extern int in_intro;

void gfx_pvr_draw_triangles_2d(void *buf_vbo, UNUSED size_t buf_vbo_len,
                               UNUSED size_t use_texture) {
    dc_fast_t *v = (dc_fast_t *) buf_vbo;

    /* Authored override carried over from the PVR backend: for this one
     * shader_id the combiner genuinely evaluates to white, but the N64 look
     * wants black showing through the transparent texels. Not derivable from
     * the mux. */
    if (cur_shader && cur_shader->shader_id == 0x01200A00 && !in_intro)
        for (size_t i = 0; i < 6; i++)
            v[i].color.packed = 0xFF000000;

    dc_fast_t *dst = pvr_reserve(sListKind, 6);
    if (dst) memcpy(dst, v, 6 * sizeof(dc_fast_t));
}

/* -------------------------------------------------------------- lifecycle - */

static void gfx_xbox_init(void) {
    if (sInited) return;

    sD3D = Direct3DCreate8(D3D_SDK_VERSION);

    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = XB_SCR_WIDTH;
    pp.BackBufferHeight       = XB_SCR_HEIGHT;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    /* 30fps is the game's target; pacing is handled by the window manager. */
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    sD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING,
                       &pp, &sDev);

    sDev->SetVertexShader(XB_FVF);

    /* The N64 backfacing convention is handled entirely in the front-end's
     * triangle setup, so the hardware must not cull as well. */
    sDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    sDev->SetRenderState(D3DRS_LIGHTING, FALSE);
    /* Specular is how the front-end's baked combiner offset (oargb on
     * Dreamcast) reaches the output. */
    sDev->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
    sDev->SetRenderState(D3DRS_DITHERENABLE, TRUE);

    sInited = 1;
}

static void gfx_xbox_on_resize(void) { }

extern float screen_2d_z;

static void gfx_xbox_start_frame(void) {
    sFrameNum++;

    /* Drop the front-end's "this palette is already converted" memo. The game
     * animates palettes in place between frames (spinning kart wheels), so the
     * skip is only valid within one frame. */
    gfx_tlut_frame_reset();
    sUploadsThisFrame = 0;

    /* Free the textures deferred 3 frames ago -- the GPU is done with them. */
    xb_run_deferred_releases();

    if (sCacheFlushPending) {
#if MK64X_DEBUG_TOOLS
        printf("TRANS texcache flush f=%u\n", (unsigned) sFrameNum);
#endif
        gfx_pvr_clear_all_textures();
        reset_texcache();
        sCacheFlushPending = 0;
    }

    /* D3D8's Clear is clipped to the CURRENT viewport, and the game leaves an
     * inset one active (the N64 picture is 316 wide with a 2px border, and
     * splitscreen panes are smaller still). Clearing through that leaves the
     * border strips holding stale swap-chain content -- the title screen was
     * visibly flickering in the edges. Reset to the full frame first; the
     * game's viewport re-applies as it renders. */
    {
        D3DVIEWPORT8 full = { 0, 0, XB_SCR_WIDTH, XB_SCR_HEIGHT, 0.0f, 1.0f };
        sDev->SetViewport(&full);
        /* The scissor bounds the clear as well, and the last batch of the
         * previous frame leaves a splitscreen pane's rect behind -- without
         * this the clear only covers that pane and the other three keep stale
         * content. Batches re-apply their own rects at flush. */
        D3DRECT fullr = { 0, 0, XB_SCR_WIDTH, XB_SCR_HEIGHT };
        sDev->SetScissors(1, FALSE, &fullr);
    }
    sDev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                D3DCOLOR_XRGB(0, 0, 0), /* z clear */ 0.0f, 0);
    sDev->BeginScene();

    /* 2D depth base. The front-end increments this per rect to encode 2D paint
     * order into z; base 1.0 keeps those small increments well inside the
     * depth buffer's resolution. */
    screen_2d_z = 1.0f;

    sOP.nverts = 0;    sOP.nbatch = 0;    sOP.cur = -1;    sOP.dirty = 1;
    sPunch.nverts = 0; sPunch.nbatch = 0; sPunch.cur = -1; sPunch.dirty = 1;
    sTR.nverts = 0;    sTR.nbatch = 0;    sTR.cur = -1;    sTR.dirty = 1;
    sOpDirty = 1;

    sFrameStartClock = sLruClock;

    {
        extern int cur_frame_persp, prev_frame_had_persp, has_done_3d;
        prev_frame_had_persp = cur_frame_persp;
        cur_frame_persp = 0;
        has_done_3d = 0;
    }
}

#if MK64X_DEBUG_TOOLS
/* One-shot geometry census. The decisive question when nothing appears is
 * whether triangles are reaching the backend at all: if they are, the problem
 * is state (depth, blend, viewport); if they are not, the problem is upstream
 * in the display-list walk. */
static void xb_report_geometry(void) {
    if (!xbox_gfx_log_once) return;

    /* Per-batch listing (first 14 across the buckets): small geometry like the
     * spinning logo and menu text never crosses the big-triangle probe's area
     * threshold, so dump each batch's full state plus its first vertex's
     * colour and UVs. GFXB kind batch nverts tex(WxH) env blend dt/zw argb
     * uv/100 xy. */
    {
        int shown = 0;
        static const char *kn[3] = { "OP", "PT", "TR" };
        for (int k = 0; k < 3 && shown < 34; k++) {
            const XbBucket *b = sBuckets[k];
            /* Menus draw ~150 background strips FIRST, so the interesting
             * batches (text, cursor, highlight) are at the END of TR. List the
             * last 20 TR batches; OP/PT from the front as before. */
            int i0 = (k == 2 && b->nbatch > 20) ? b->nbatch - 20 : 0;
            for (int i = i0; i < b->nbatch && shown < 34; i++, shown++) {
                const XbBatch *bt = &b->batch[i];
                const dc_fast_t *v0 = &b->verts[bt->start];
                const struct XbTex *xt = &sTextures[bt->st.tex0];
                /* sc= the batch's own scissor rect. Splitscreen panes and any
                 * full-screen overlay are told apart by this alone. */
                printf("GFXB %s b=%d n=%u tex=%u(%ux%u) env=%u bl=%u/%u dt=%u zw=%u "
                       "argb=%08X uv=(%d,%d) xy=(%d,%d) sc=(%u,%u %ux%u)\n",
                       kn[k], i, (unsigned) bt->count,
                       (unsigned) bt->st.tex0, (unsigned) xt->w, (unsigned) xt->h,
                       (unsigned) bt->st.tex_env,
                       (unsigned) bt->st.blend_src, (unsigned) bt->st.blend_dst,
                       (unsigned) bt->st.depth_test, (unsigned) bt->st.depth_write,
                       (unsigned) v0->color.packed,
                       (int) (v0->texture.u * 100.0f), (int) (v0->texture.v * 100.0f),
                       (int) v0->vert.x, (int) v0->vert.y,
                       (unsigned) bt->st.sc_x, (unsigned) bt->st.sc_y,
                       (unsigned) bt->st.sc_w, (unsigned) bt->st.sc_h);
            }
        }
    }

    /* Split the batches by what they will actually sample. An untextured batch
     * draws flat diffuse, and a batch pointing at a slot with no D3D texture
     * draws whatever the fallback holds -- either can look like a solid white
     * polygon, which is what the track is doing. */
    int untex = 0, notex = 0, textured = 0;
    for (int k = 0; k < 3; k++) {
        const XbBucket *b = sBuckets[k];
        for (int i = 0; i < b->nbatch; i++) {
            const uint32_t id = b->batch[i].st.tex0;
            if (id == 0)                     untex++;
            else if (!b->batch[i].st.tex)    notex++;
            else                             textured++;
        }
    }
    /* z is inverse-w * XBOX_Z_SCALE. D3D8 clips pre-transformed vertices to
     * z in [0,1]; anything outside is silently cut, which looks like straight
     * slices taken out of the scene. Measure it rather than assume. */
    float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
    int zhi = 0, zlo = 0, nv = 0;
    for (int k = 0; k < 3; k++) {
        const XbBucket *b = sBuckets[k];
        for (uint32_t v = 0; v < b->nverts; v++) {
            const float z = b->verts[v].vert.z, w = b->verts[v].rhw;
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
            if (w < wmin) wmin = w;
            if (w > wmax) wmax = w;
            if (z > 1.0f) zhi++;
            if (z < 0.0f) zlo++;
            nv++;
        }
    }
    printf("GFXZ f=%u n=%d z=[%d..%d]/1000 over=%d under=%d rhw=[%d..%d]/1000\n",
           (unsigned) sFrameNum, nv, (int)(zmin*1000.0f), (int)(zmax*1000.0f),
           zhi, zlo, (int)(wmin*1000.0f), (int)(wmax*1000.0f));

    /* texslots = distinct texture slots allocated since the last cache flush
     * (this was previously mislabelled "uploaded", which reads as a per-frame
     * cost and is not — it never decreases within a course).
     * uploads = actual texture decode+upload calls THIS frame, which is the
     * number that matters for a frame-time investigation. */
    printf("GFX f=%u OP=%u/%d PT=%u/%d TR=%u/%d  tex=%d untex=%d empty=%d "
           "texslots=%u uploads=%u dropV=%d dropB=%d\n",
           (unsigned) sFrameNum,
           (unsigned) sOP.nverts,    sOP.nbatch,
           (unsigned) sPunch.nverts, sPunch.nbatch,
           (unsigned) sTR.nverts,    sTR.nbatch,
           textured, untex, notex, (unsigned) sTexCount,
           (unsigned) sUploadsThisFrame,
           sDropV, sDropB);
    printf("GFXCOW f=%u retired_this_frame=%d\n", (unsigned) sFrameNum, sNumRetired);
}


/* The artifacts are screen-crossing wedges, so they are by definition the
 * largest triangles on screen. Rather than guess what geometry they belong to,
 * find every triangle covering more than an eighth of the frame and print what
 * it actually is: which bucket, its texture, combiner mode, blend factors, and
 * its three screen positions. */
static void xb_report_big_tris(void) {
    if (!xbox_gfx_log_once) return;
    const float area_limit = (float) (XB_SCR_WIDTH * XB_SCR_HEIGHT) / 12.0f;
    static const char *kindname[3] = { "OP", "PT", "TR" };
    int shown = 0;

    for (int k = 0; k < 3 && shown < 10; k++) {
        const XbBucket *b = sBuckets[k];
        for (int i = 0; i < b->nbatch && shown < 10; i++) {
            const XbBatch *bt = &b->batch[i];
            for (uint32_t v = 0; v + 2 < bt->count && shown < 10; v += 3) {
                const dc_fast_t *p0 = &b->verts[bt->start + v];
                const dc_fast_t *p1 = &b->verts[bt->start + v + 1];
                const dc_fast_t *p2 = &b->verts[bt->start + v + 2];
                const float ax = p1->vert.x - p0->vert.x, ay = p1->vert.y - p0->vert.y;
                const float bx = p2->vert.x - p0->vert.x, by = p2->vert.y - p0->vert.y;
                float area = (ax * by - ay * bx) * 0.5f;
                if (area < 0.0f) area = -area;
                if (area < area_limit) continue;
                printf("GFXBIG f=%u %s b=%d area=%d tex=%u env=%u blend=%u/%u "
                       "zw=%u dt=%u argb=%08X oargb=%08X "
                       "v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)\n",
                       (unsigned) sFrameNum, kindname[k], i, (int) area,
                       (unsigned) bt->st.tex0, (unsigned) bt->st.tex_env,
                       (unsigned) bt->st.blend_src, (unsigned) bt->st.blend_dst,
                       (unsigned) bt->st.depth_write, (unsigned) bt->st.depth_test,
                       (unsigned) p0->color.packed, (unsigned) p0->pad0.vertindex,
                       (int) p0->vert.x, (int) p0->vert.y,
                       (int) p1->vert.x, (int) p1->vert.y,
                       (int) p2->vert.x, (int) p2->vert.y);
                shown++;
            }
        }
    }
}
#endif /* MK64X_DEBUG_TOOLS */

static void gfx_xbox_end_frame(void) {
#if MK64X_DEBUG_TOOLS
    if (xbox_gfx_log_once) {
        /* The game's LAST viewport this frame, which is what the device used
         * to be given for every batch. Printed alongside the per-batch sc=
         * rects so a splitscreen/attract layout can be reconstructed. */
        printf("GFXVP last viewport=(%u,%u %ux%u) last scissor=(%u,%u %ux%u)\n",
               (unsigned) sVpX, (unsigned) sVpY, (unsigned) sVpW, (unsigned) sVpH,
               (unsigned) sScX, (unsigned) sScY, (unsigned) sScW, (unsigned) sScH);
    }
    xb_report_geometry();
    xb_report_big_tris();
    /* NOT cleared here: xb_report_tr_depth runs after the z remap and the
     * split, which is the only point where the TR bucket looks the way the
     * flush will see it. */
#endif
    /* Order matters and is the same as the Dreamcast's, for the same reason:
     * OP lays down opaque depth, PT's alpha-tested cutouts are also opaque and
     * also write depth, and TR composites over both. The difference is that TR
     * must be sorted first, because nothing sorts it for us. */
    /* z arrives as inverse-w * XBOX_Z_SCALE. That linear scale was sized from
     * the 2D panes' "observed ~2.0 maximum", but 3D inverse-w has no upper
     * bound -- it grows without limit as geometry nears the camera -- so near
     * polygons can exceed 1.0 and be cut by D3D8's [0,1] clip on
     * pre-transformed vertices. A clipped triangle is sliced along a straight
     * line, which is what the screen-crossing wedges look like.
     *
     * z/(z+1) is monotonic on [0,inf), so it preserves every depth ordering
     * exactly (including the 2D panes' 0.0005 steps, where it is near-linear)
     * while never reaching 1.0. Nothing can be clipped. */
    for (int k = 0; k < 3; k++) {
        XbBucket *b = sBuckets[k];
        for (uint32_t v = 0; v < b->nverts; v++) {
            float z = b->verts[v].vert.z;
            b->verts[v].vert.z = (z > 0.0f) ? (z / (z + 1.0f)) : 0.0f;
        }
    }

    /* TR keeps the game's submission order; see MK64X_TR_SORT. */
#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE
    if (MK64X_TR_SORT) {
        if (!xb_sort_tr_tris()) xb_sort_tr();
    }
#endif
#if MK64X_DEBUG_TOOLS
    /* The TR bucket exactly as the flush will walk it: post-remap, post-sort --
     * the only point where it looks the way the flush sees it. */
    if (xbox_gfx_log_once) {
        printf("TRSORT tris=%d batches=%d->%d fellback=%d\n",
               sTrTris, sTR.nbatch, sTrRebuilt, sTrFellBack);
        for (int i = 0; i < sTR.nbatch; i++) {
            const XbBatch *bt = &sTR.batch[i];
            if (bt->count < 3) continue;
            float lo = 3.0e38f, hi = -3.0e38f;
            for (uint32_t v = 0; v < bt->count; v++) {
                const float z = sTR.verts[bt->start + v].vert.z;
                if (z < lo) lo = z;
                if (z > hi) hi = z;
            }
            int alo, ahi; unsigned afmt;
            xb_tex_alpha_range(bt->st.tex, &alo, &ahi, &afmt);
            printf("TRZ %2d tris=%4u tex=%u env=%u bl=%u/%u dt=%u zw=%u "
                   "z=[%d..%d]/1000 argb=%08X ta=[%d..%d] tfmt=%u cl=%u%u\n",
                   i, (unsigned) bt->count / 3, (unsigned) bt->st.tex0,
                   (unsigned) bt->st.tex_env,
                   (unsigned) bt->st.blend_src, (unsigned) bt->st.blend_dst,
                   (unsigned) bt->st.depth_test, (unsigned) bt->st.depth_write,
                   (int) (lo * 1000.0f), (int) (hi * 1000.0f),
                   (unsigned) sTR.verts[bt->start].color.packed,
                   alo, ahi, afmt,
                   (unsigned) bt->st.clampu, (unsigned) bt->st.clampv);
        }
    }
    xbox_gfx_log_once = 0;
#endif
    /* Backdrops go down first (no depth write, no test): the painter base. */
    xb_flush_bucket(&sTR, XB_KIND_TR, 1);
#if MK64X_DEBUG_TOOLS
#endif
    xb_flush_bucket(&sOP, XB_KIND_OP, 0);
    xb_flush_bucket(&sPunch, XB_KIND_PT, 0);
    xb_flush_bucket(&sTR, XB_KIND_TR, 0);

    /* Surfaces orphaned by a mid-frame re-upload: their draws were SUBMITTED
     * this frame but the GPU hasn't executed them yet (Present is async), so
     * releasing here handed the NV2A freed memory. Defer like every other
     * texture free. */
    sDev->SetTexture(0, NULL);
    for (int i = 0; i < sNumRetired; i++)
        xb_defer_release(sRetired[i]);
    sNumRetired = 0;
}

/* ---------------------------------------------------------- vram mirror --
 * KOS hands the game a raw 16-bit framebuffer pointer (vram_s). The Xbox back
 * buffer is a swizzled/tiled D3D surface rather than a linear CPU-addressable
 * one, so vram_s points at a main-memory mirror that is filled on demand.
 *
 * The one consumer (the framebuffer-copy effect in skybox_and_splitscreen.c)
 * indexes it as 640-wide RGB565, so that is what is produced here regardless
 * of the back buffer's own format. Refreshing every frame would cost a 600KB
 * readback for something used rarely, hence on demand. */
static uint16_t sVramMirror[XB_SCR_WIDTH * XB_SCR_HEIGHT];
uint16_t *vram_s = sVramMirror;

/* Rect-limited mirror refresh, in back-buffer pixel coords, run EVERY frame:
 * the jumbotron/tunnel screens sample a small window, and converting just that
 * window costs well under a millisecond -- full-rate live feed, unlike the
 * throttled full-frame path below (kept for any other vram_s consumer). */
void xbox_vram_snapshot_rect(int px, int py, int pw, int ph, int step) {
    if (!sDev) return;
    if (step < 1) step = 1;
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px + pw > XB_SCR_WIDTH)  pw = XB_SCR_WIDTH - px;
    if (py + ph > XB_SCR_HEIGHT) ph = XB_SCR_HEIGHT - py;
    if (pw <= 0 || ph <= 0) return;

    /* -1 = FRONT buffer: last frame's image, already displayed. But on Xbox,
     * LockRect on ANY swap surface still synchronizes with the GPU -- the
     * lock, not the read, was the remaining 30->20fps stall at the wall
     * screen. The N64/DC treat the framebuffer as plain memory; so do we:
     * lock each swap surface ONCE EVER to learn its fixed CPU pointer and
     * pitch (Xbox surfaces never move), then read directly with no lock and
     * no sync on every later call. A same-frame torn line in a 4Hz-feel
     * jumbotron feed is imperceptible. */
    LPDIRECT3DSURFACE8 back = NULL;
    if (FAILED(sDev->GetBackBuffer(-1, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;

    static struct { LPDIRECT3DSURFACE8 surf; const uint8_t *bits; INT pitch; } sMap[4];
    const uint8_t *src = NULL;
    INT pitch = 0;
    for (int i = 0; i < 4; i++) {
        if (sMap[i].surf == back) { src = sMap[i].bits; pitch = sMap[i].pitch; break; }
    }
    if (!src) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(back->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
            back->UnlockRect();
            for (int i = 0; i < 4; i++) {
                if (!sMap[i].surf) {
                    sMap[i].surf  = back;
                    sMap[i].bits  = (const uint8_t *) lr.pBits;
                    sMap[i].pitch = lr.Pitch;
                    break;
                }
            }
            src = (const uint8_t *) lr.pBits;
            pitch = lr.Pitch;
        }
    }
    if (src) {
        /* Only the sampled lattice is read. Each of these loads is a separate
         * uncached transaction against GPU memory, so touching pixels the
         * consumer never looks at is the dominant cost of the whole effect --
         * step=2 is a straight 4x reduction with no visual difference. */
        for (int y = py; y < py + ph; y += step) {
            const uint32_t *row = (const uint32_t *) (src + (size_t) y * pitch);
            uint16_t *dst = &sVramMirror[(size_t) y * XB_SCR_WIDTH];
            for (int x = px; x < px + pw; x += step) {
                const uint32_t p = row[x];
                dst[x] = (uint16_t) (((p >> 8) & 0xF800) |
                                     ((p >> 5) & 0x07E0) |
                                     ((p >> 3) & 0x001F));
            }
        }
    }
    back->Release();
}

void xbox_vram_snapshot(void) {
    if (!sDev) return;

    /* Reading the back buffer from the CPU is uncached GPU-memory access --
     * ~70ms for the full frame -- and the tunnel/jumbotron screens request it
     * EVERY frame they are visible, which is exactly the sections that ran at
     * 10 FPS (Luigi Raceway's archway screen, Wario Stadium). The N64 feed is
     * visibly low-refresh anyway, so refresh the mirror at most every 8th
     * frame and serve the cached copy in between: ~9ms amortised. */
    static uint32_t sLastSnapFrame = 0;
    static int      sHaveSnap = 0;
    if (sHaveSnap && (sFrameNum - sLastSnapFrame) < 8) return;
    sLastSnapFrame = sFrameNum;
    sHaveSnap = 1;

    LPDIRECT3DSURFACE8 back = NULL;
    if (FAILED(/* -1 = FRONT buffer: last frame's image, already displayed, GPU done
     * with it. Locking the back buffer instead stalled the CPU on the
     * in-flight frame every time the wall screen was visible (30->20fps
     * with stutter). One frame of feed latency is invisible. */
    sDev->GetBackBuffer(-1, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(back->LockRect(&lr, NULL, D3DLOCK_READONLY))) {
        const uint8_t *src = (const uint8_t *) lr.pBits;
        for (int y = 0; y < XB_SCR_HEIGHT; y++) {
            const uint32_t *row = (const uint32_t *) (src + (size_t) y * lr.Pitch);
            uint16_t *dst = &sVramMirror[(size_t) y * XB_SCR_WIDTH];
            for (int x = 0; x < XB_SCR_WIDTH; x++) {
                const uint32_t p = row[x];                  /* X8R8G8B8 */
                dst[x] = (uint16_t) (((p >> 8) & 0xF800) |  /* R5 */
                                     ((p >> 5) & 0x07E0) |  /* G6 */
                                     ((p >> 3) & 0x001F));  /* B5 */
            }
        }
        back->UnlockRect();
    }
    back->Release();
}

static void gfx_xbox_finish_render(void) {
    sDev->EndScene();
    sDev->Present(NULL, NULL, NULL, NULL);
}

struct GfxRenderingAPI gfx_nv2a_api = {
    gfx_xbox_z_is_from_0_to_1,
    gfx_xbox_unload_shader,
    gfx_xbox_load_shader,
    gfx_xbox_create_and_load_new_shader,
    gfx_xbox_lookup_shader,
    gfx_xbox_shader_get_info,
    gfx_xbox_new_texture,
    gfx_xbox_select_texture,
    gfx_xbox_upload_texture,
    gfx_xbox_set_sampler_parameters,
    gfx_xbox_set_depth_test,
    gfx_xbox_set_depth_mask,
    gfx_xbox_set_zmode_decal,
    gfx_xbox_set_tex_env,
    gfx_xbox_set_viewport,
    gfx_xbox_set_scissor,
    gfx_xbox_set_use_alpha,
    gfx_xbox_draw_triangles,
    gfx_xbox_init,
    gfx_xbox_on_resize,
    gfx_xbox_start_frame,
    gfx_xbox_end_frame,
    gfx_xbox_finish_render,
};

}  /* extern "C" */
