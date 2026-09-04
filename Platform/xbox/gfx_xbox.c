/* gfx_xbox.c — window-manager backend for the Xbox.
 *
 * Counterpart to src/gfx/gfx_dc.c. The Xbox has no windowing system at all, so
 * most of this interface is inert; what it really provides is the frame pump
 * and the clocks.
 *
 * Frame pacing follows the Dreamcast port's hard-won rule exactly: hold each
 * frame to a CONSTANT two-vblank beat, and NEVER pace on a measurement of the
 * frame being paced. The DC port's comment records what happens otherwise —
 * a one-frame hiccup ratchets the wait up permanently, and a skipped frame
 * bypasses the pacer entirely, producing two logic frames per beat and
 * double game speed at a correct-looking 30fps.
 */

/* xtl.h always pulls in d3dx8math, whose .inl defines every D3DX math helper
 * with EXTERNAL linkage in C -- the matching prototype in d3dx8math.h is not
 * static, so the definition cannot be made internal by redefining D3DXINLINE.
 * Two C translation units including xtl.h therefore collide on all 46 of them.
 *
 * The .inl has its own include guard, so claiming it here suppresses the
 * definitions in this file and leaves the declarations intact. kos_xbox.c is
 * the one C file that emits them. */
#define __D3DX8MATH_INL__

#include <xtl.h>
#include <stdint.h>
#include <stdio.h>

#include "kos.h"   /* timer_ms_gettime64 */
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "macros.h"

#define GFX_API_NAME "Xbox NV2A"
#define SCR_WIDTH  640
#define SCR_HEIGHT 480

/* Owned by game code (src/main.c): 0 = uncapped, 1 = vblank-locked 30.
 * update_gamestate() defaults it to 1 on every state change and
 * race_logic_loop re-asserts it per mode every frame. */
int force_30fps = 1;

/* 60Hz vblank count, DEFINED BY THE GAME in main.c (which also reads it to
 * derive gRun30hz). On Dreamcast a hardware vblank callback increments it;
 * here BlockUntilVerticalBlank is synchronous, so the pacer below maintains
 * it directly. */
extern volatile uint64_t vblticker;

/* ------------------------------------------------------------------ clocks
 * The clocks themselves live in kos_xbox.c (timer_us_gettime64 and friends);
 * these are the two names the game calls by their Dreamcast spelling. */

unsigned int GetSystemTimeLow(void) {
    return (unsigned int) timer_ms_gettime64();
}

void DelayThread(unsigned int ms) {
    Sleep(ms);
}

/* ------------------------------------------------------- window manager -- */

static void gfx_xbox_wm_init(UNUSED const char *game_name,
                             UNUSED uint8_t start_in_fullscreen) { }

static void gfx_xbox_wm_set_fullscreen_changed_callback(
        UNUSED void (*on_fullscreen_changed)(uint8_t is_now_fullscreen)) { }

static void gfx_xbox_wm_set_fullscreen(UNUSED uint8_t enable) { }

static void gfx_xbox_wm_set_keyboard_callbacks(
        UNUSED uint8_t (*on_key_down)(int scancode),
        UNUSED uint8_t (*on_key_up)(int scancode),
        UNUSED void (*on_all_keys_up)(void)) { }

static void gfx_xbox_wm_main_loop(void (*run_one_game_iter)(void)) {
    for (;;) {
        run_one_game_iter();
    }
}

static void gfx_xbox_wm_get_dimensions(uint32_t *width, uint32_t *height) {
    *width  = SCR_WIDTH;
    *height = SCR_HEIGHT;
}

static void gfx_xbox_wm_handle_events(void) { }

static uint8_t gfx_xbox_wm_start_frame(void) {
    /* Always render. The Dreamcast port removed its ms-based frame skip after
     * it was found to double game speed (see the note at the top of this
     * file); with vblank pacing a slow frame self-resyncs and skipping is
     * unnecessary. Same reasoning applies here. */
    return 1;
}

static void gfx_xbox_wm_swap_buffers_begin(void) { }

static uint64_t sLastBeat = 0;

static void gfx_xbox_wm_swap_buffers_end(void) {
    /* vblticker is now advanced ONLY by the vblank thread (kos_xbox.c
     * xb_vbl_thread -> vblfunc), matching the DC's hardware interrupt. The
     * increments that used to live here would double-count against it. */
    if (!force_30fps) return;
    /* Hold a constant 2-vblank beat by waiting out the ticker. Bounded: if the
     * vblank thread isn't up yet (very early boot), fall through rather than
     * spin forever. */
    const uint64_t target = sLastBeat + 2;
    int guard = 0;
    while (vblticker < target && guard++ < 8)
        genwait_wait((void *) &vblticker, NULL, 20, NULL);
    sLastBeat = (vblticker > target) ? vblticker : target;
}

static double gfx_xbox_wm_get_time(void) {
    return 0.0;
}

struct GfxWindowManagerAPI gfx_xbox = {
    gfx_xbox_wm_init,
    gfx_xbox_wm_set_keyboard_callbacks,
    gfx_xbox_wm_set_fullscreen_changed_callback,
    gfx_xbox_wm_set_fullscreen,
    gfx_xbox_wm_main_loop,
    gfx_xbox_wm_get_dimensions,
    gfx_xbox_wm_handle_events,
    gfx_xbox_wm_start_frame,
    gfx_xbox_wm_swap_buffers_begin,
    gfx_xbox_wm_swap_buffers_end,
    gfx_xbox_wm_get_time,
};
