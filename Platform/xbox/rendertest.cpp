/* rendertest.cpp — bootable smoke test for the NV2A backend.
 *
 * This drives the REAL gfx_nv2a_api vtable rather than reimplementing its
 * setup, so what it proves is what the game will actually rely on:
 *
 *   - the D3D8 device comes up at all on this hardware
 *   - dc_fast_t's reordered layout really is a valid FVF vertex, drawable by
 *     DrawVerticesUP at a 36-byte stride
 *   - the depth convention holds: z is inverse-w with LARGER == NEARER, tested
 *     GREATEREQUAL against a zero-cleared buffer. This is the single most
 *     likely thing to be wrong, since it is inverted from the Dreamcast's
 *   - texture upload works through XGSwizzleRect in A1R5G5B5
 *   - the OP / PT / TR bucket split flushes in the right order, and the TR
 *     sort actually orders back-to-front
 *   - controller input arrives through the maple->XInput shim
 *
 * Layout: four quadrants, each isolating one property, plus a pad readout.
 * Every check is arranged so that a FAILURE IS VISIBLE rather than subtle --
 * if depth is inverted the near/far pair swaps, if the TR sort is broken the
 * blend order visibly reverses.
 */

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gfx_rendering_api.h"
#include "gl_fast_vert.h"
#include "kos.h"

extern "C" {

/* --- externs gfx_nv2a.cpp expects from the renderer front-end ------------- */
float screen_2d_z      = 1.0f;
int   in_intro         = 0;
int   cur_frame_persp  = 0;
int   prev_frame_had_persp = 0;
int   has_done_3d      = 0;
void  reset_texcache(void) { }

extern struct GfxRenderingAPI gfx_nv2a_api;
extern dc_fast_t *pvr_reserve(int kind, size_t n);
extern void       gfx_pvr_set_blend(uint8_t kind);
extern void       gfx_pvr_set_blend_factors(uint8_t src, uint8_t dst);

#define KIND_OP 0
#define KIND_PT 1
#define KIND_TR 2

static void put(dc_fast_t *v, float x, float y, float z,
                uint32_t argb, float u, float vv) {
    v->vert.x = x; v->vert.y = y; v->vert.z = z;
    v->rhw = 1.0f;
    v->color.packed = argb;
    v->pad0.vertindex = 0;         /* specular / combiner offset */
    v->texture.u = u; v->texture.v = vv;
    v->flags = 0;
}

/* Axis-aligned quad as two triangles, flat z. */
static void quad(int kind, float x0, float y0, float x1, float y1,
                 float z, uint32_t argb, int textured) {
    dc_fast_t *v = pvr_reserve(kind, 6);
    if (!v) return;
    const float u1 = textured ? 1.0f : 0.0f;
    put(&v[0], x0, y0, z, argb, 0.0f, 0.0f);
    put(&v[1], x1, y0, z, argb, u1,   0.0f);
    put(&v[2], x1, y1, z, argb, u1,   u1);
    put(&v[3], x0, y0, z, argb, 0.0f, 0.0f);
    put(&v[4], x1, y1, z, argb, u1,   u1);
    put(&v[5], x0, y1, z, argb, 0.0f, u1);
}

/* A 64x64 A1R5G5B5 checker, in the exact format the front-end hands to
 * upload_texture (already 16-bit, alpha in the top bit). */
static uint16_t sChecker[64 * 64];

static void build_checker(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int c = ((x >> 3) ^ (y >> 3)) & 1;
            /* A1R5G5B5: alpha set, alternating orange / deep blue */
            sChecker[y * 64 + x] = c ? (uint16_t) (0x8000 | (31 << 10) | (20 << 5) | 4)
                                     : (uint16_t) (0x8000 | (3  << 10) | (6  << 5) | 26);
        }
}

void __cdecl main(void) {
    struct GfxRenderingAPI *r = &gfx_nv2a_api;

    r->init();
    build_checker();

    /* Upload the checker through the real texture path. */
    const uint32_t tex = r->new_texture();
    r->select_texture(0, tex);
    r->upload_texture((const uint8_t *) sChecker, 64, 64, PVR_TXRFMT_ARGB1555);
    r->set_sampler_parameters(0, /*linear*/ 1, 0, 0);

    unsigned frame = 0;

    for (;;) {
        r->start_frame();

        /* ---- quadrant 1 (top-left): DEPTH ----------------------------------
         * Two overlapping opaque quads. The GREEN one has the LARGER z, so
         * with "larger == nearer" it must appear IN FRONT of the red one.
         * If depth is inverted, red wins and the failure is obvious. */
        r->set_depth_test(1);
        r->set_depth_mask(1);
        gfx_pvr_set_blend(KIND_OP);
        r->select_texture(0, 0);
        quad(KIND_OP,  40.0f,  40.0f, 200.0f, 160.0f, 0.20f, 0xFFCC3322, 0);
        quad(KIND_OP, 120.0f,  90.0f, 280.0f, 210.0f, 0.60f, 0xFF33CC44, 0);

        /* ---- quadrant 2 (top-right): TEXTURE + FILTER ---------------------
         * Checker through XGSwizzleRect. Wrong swizzle shows as scrambled
         * blocks rather than a clean grid. */
        r->select_texture(0, tex);
        quad(KIND_OP, 360.0f, 40.0f, 600.0f, 210.0f, 0.40f, 0xFFFFFFFF, 1);

        /* ---- quadrant 3 (bottom-left): TRANSLUCENCY SORT -------------------
         * Three blended quads submitted FAR-Z-LAST on purpose. A correct
         * back-to-front sort re-orders them so the nearest (largest z, the
         * blue one) composites on top. Submitting in the wrong order and
         * getting the right picture is the whole point of the test. */
        r->select_texture(0, 0);
        gfx_pvr_set_blend(KIND_TR);
        gfx_pvr_set_blend_factors(GFX_BLENDF_SRCALPHA, GFX_BLENDF_INVSRCALPHA);
        quad(KIND_TR,  90.0f, 280.0f, 230.0f, 420.0f, 0.70f, 0x803344EE, 0); /* near, submitted 1st */
        quad(KIND_TR,  40.0f, 250.0f, 180.0f, 390.0f, 0.30f, 0x80EE4433, 0); /* far,  submitted 2nd */
        quad(KIND_TR,  65.0f, 265.0f, 205.0f, 405.0f, 0.50f, 0x8044EE33, 0); /* mid,  submitted 3rd */

        /* ---- quadrant 4 (bottom-right): ALPHA TEST (punch-through) --------
         * Same checker, but through the PT bucket, which enables alpha test.
         * Opaque texels only; it must still write depth. */
        r->select_texture(0, tex);
        gfx_pvr_set_blend(KIND_PT);
        quad(KIND_PT, 360.0f, 260.0f, 600.0f, 430.0f, 0.45f, 0xFFFFFFFF, 1);

        r->end_frame();
        r->finish_render();

        /* ---- controller readout, over the debug channel --------------------
         * Proves the maple->XInput shim end to end without needing a font. */
        if ((frame % 60) == 0) {
            char line[160];
            int  n = snprintf(line, sizeof(line), "[f%u] pads:", frame);
            for (int p = 0; p < 4; p++) {
                maple_device_t *d = maple_enum_type(p, MAPLE_FUNC_CONTROLLER);
                if (!d) { n += snprintf(line + n, sizeof(line) - n, " %d:-", p); continue; }
                cont_state_t *s = (cont_state_t *) maple_dev_status(d);
                if (!s) { n += snprintf(line + n, sizeof(line) - n, " %d:?", p); continue; }
                n += snprintf(line + n, sizeof(line) - n,
                              " %d:[b%04X x%+4d y%+4d L%3d R%3d]",
                              p, (unsigned) s->buttons, s->joyx, s->joyy, s->ltrig, s->rtrig);
            }
            printf("%s\n", line);
        }
        frame++;
    }
}

}  /* extern "C" */
