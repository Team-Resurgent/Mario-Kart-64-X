/* This decomp treats uintptr_t as a pointer type throughout, for N64 segmented
 * addressing. gcc (the Dreamcast toolchain) warns about the resulting implicit
 * conversions; clang 15+ makes them errors. RXDK's C build does not accept
 * extra compiler flags, so the diagnostic is restored to gcc's behaviour here
 * rather than by editing hundreds of call sites -- this code is known-good as
 * written on Dreamcast, and hand-inserting casts would risk changing it. */
#pragma clang diagnostic ignored "-Wint-conversion"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"

#include <kos.h>
#include "kos_undef.h"

#include <ultra64.h>
#include <macros.h>
#include <defines.h>
#include <segments.h>
#include <mk64.h>
#include <course.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code_80281780.h"
#include "racing/memory.h"
#include "camera.h"
#include "camera_junk.h"
#include "spawn_players.h"
#include "skybox_and_splitscreen.h"
#include "code_8006E9C0.h"
#include "podium_ceremony_actors.h"
#include "cpu_vehicles_camera_path.h"
#include "collision.h"
#include "code_80281C40.h"
#include "code_800029B0.h"
#include "menu_items.h"
#include "main.h"
#include "menus.h"
#include "render_courses.h"
#if defined(TARGET_XBOX)
#include "xbox_debug.h"
#else
#define MK64X_DEBUG_TOOLS 0
#endif

u8 defaultCharacterIds[] = { 1, 2, 3, 4, 5, 6, 7, 0 };

void debug_switch_character_ceremony_cutscene(void) {
    if (gEnableDebugMode) {
        if (gControllerOne->button & HOLD_ALL_DPAD_AND_C_BUTTONS) {
            // Allows to switch character in debug mode?
            if (gControllerOne->button & U_CBUTTONS) {
                gCharacterSelections[0] = LUIGI;
            } else if (gControllerOne->button & L_CBUTTONS) {
                gCharacterSelections[0] = YOSHI;
            } else if (gControllerOne->button & R_CBUTTONS) {
                gCharacterSelections[0] = TOAD;
            } else if (gControllerOne->button & D_CBUTTONS) {
                gCharacterSelections[0] = DK;
            } else if (gControllerOne->button & U_JPAD) {
                gCharacterSelections[0] = WARIO;
            } else if (gControllerOne->button & L_JPAD) {
                gCharacterSelections[0] = PEACH;
            } else if (gControllerOne->button & R_JPAD) {
                gCharacterSelections[0] = BOWSER;
            } else {
                gCharacterSelections[0] = MARIO;
            }
            //! @todo confirm this.
            // Resets gCharacterIdByGPOverallRank to default?
            bcopy(&defaultCharacterIds, &gCharacterIdByGPOverallRank, 8);
        }
    }
}

s32 func_80281880(s32 arg0) {
    s32 i;
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (gCharacterIdByGPOverallRank[i] == gCharacterSelections[arg0]) {
            break;
        }
    }
    return i;
}

void func_802818BC(void) {
    s32 temp_v0;
    UNUSED s32 pad;
    s32 sp1C;
    s32 temp_v0_2;

    if (gPlayerCount != TWO_PLAYERS_SELECTED) {
        D_802874D8.unk1D = func_80281880(0);
        D_802874D8.unk1E = gCharacterSelections[0];
        return;
    }
    // weird pattern but if it matches it matches
    temp_v0 = sp1C = func_80281880(0);
    temp_v0_2 = func_80281880(1);
    if (sp1C < temp_v0_2) {
        D_802874D8.unk1E = gCharacterSelections[0];
        D_802874D8.unk1D = temp_v0;
    } else {
        D_802874D8.unk1E = gCharacterSelections[1];
        D_802874D8.unk1D = temp_v0_2;
    }
}

extern Gfx d_course_royal_raceway_packed_dl_67E8[];
extern Gfx d_course_royal_raceway_packed_dl_AEF8[];
extern Gfx d_course_royal_raceway_packed_dl_A970[];
extern Gfx d_course_royal_raceway_packed_dl_AC30[];
extern Gfx d_course_royal_raceway_packed_dl_CE0[];
extern Gfx d_course_royal_raceway_packed_dl_E88[];
extern Gfx d_course_royal_raceway_packed_dl_A618[];
extern Gfx d_course_royal_raceway_packed_dl_A618[];
extern Gfx d_course_royal_raceway_packed_dl_23F8[];
extern Gfx d_course_royal_raceway_packed_dl_2478[];
#include "buffer_sizes.h"
extern uint8_t __attribute__((aligned(32))) CEREMONY_BUF[CEREMONY_BUF_SIZE];
extern uint8_t __attribute__((aligned(32))) COURSE_BUF[COURSE_BUF_SIZE];
extern u16 reflection_map_silver[1024];
extern u16 reflection_map_gold[1024];
extern u16 reflection_map_brass[1024];
extern CollisionTriangle __attribute__((aligned(32))) allColTris[allColTris_SIZE];

extern char *fnpre;

static char texfn[256];

extern u16 gTexturePodium1[];
extern u16 gTexturePodium2[];
extern u16 gTexturePodium3[];

void load_ceremony_data(void) {
    sprintf(texfn, "%s/dc_data/ceremony_data.bin", fnpre);
    FILE* file = fopen(texfn, "rb");
    if (!file) {
        perror("fopen");
        printf("\n");
        // while(1) {}
        exit(-1);
    }

    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fseek(file, 0, SEEK_SET);

    long toread = filesize;
    long didread = 0;

    while (didread < filesize) {
        long rv = fread(&CEREMONY_BUF[didread], 1, toread - didread, file);
        if (rv == -1) {
            perror("fread");
            printf("couldnt read into ceremony buf\n");
            // while(1) {}
            exit(-1);
        }
        toread -= rv;
        didread += rv;
    }
    fclose(file);
    file = NULL;
    set_segment_base_addr(0xB, (void*) CEREMONY_BUF);

    /* NOTE: no texture byteswap here. This function runs THREE times (from
     * load_ceremony_cutscene, from load_credits, and once during boot in
     * main.c), and a byteswap is its own inverse, so doing it here un-fixes
     * itself. ceremony_swap_segment_textures() is called once per scene load
     * by the caller instead. */
}

/* Byteswap the ceremony's 16-bit textures into ROM byte order.
 *
 * Addressed through the SEGMENT, deliberately. The display lists that sample
 * them live in this same blob, so coff_reloc has already rewritten their
 * texture pointers to 0x0Bxxxxxx -- they read CEREMONY_BUF. The identically
 * named C arrays are a SEPARATE copy in the executable, and on Xbox
 * segmented_to_virtual() correctly returns a real pointer unchanged, so
 * swapping gTexturePodium1 (as load_ceremony_data used to) byteswapped a copy
 * that nothing draws while the drawn one stayed wrong -- the rainbow 1/2/3.
 *
 * Offsets verified three ways: assets/ending_ceremony.json's block offsets
 * inside the ROM's MIO0 at 0x821D10, the asset bytes located inside
 * dc_data/ceremony_data.bin, and the array names in
 * assets/code/ceremony_data/ceremony_data.c, which carry their own offsets.
 * All six read back byteswapped against the ROM in the shipped blob.
 *
 * CALL THIS EXACTLY ONCE PER LOAD. A byteswap is its own inverse, so running
 * it twice restores the broken data -- which is what happened when it lived
 * inside load_ceremony_data(): that is called from THREE places (here, from
 * load_credits, and once more during boot in main.c), and the double swap
 * un-fixed a trophy that had been rendering correctly. */
/* Put one of the ceremony's 16-bit textures into the ROM's byte order,
 * whatever state it is currently in.
 *
 * THE EXECUTABLE'S ARRAYS ARE WHAT GET DRAWN, not the segment copies. The
 * podium model is `podium_dl3` and the trophies are `*_trophy_dl*`, all
 * assigned/invoked as plain C symbols (see func_8008629C), so the game runs
 * the EXE's display lists, whose texture pointers the linker resolved to the
 * EXE's arrays. Swapping the 0x0Bxxxxxx segment copies -- which is what this
 * function used to do -- edited data nothing samples.
 *
 * The two groups arrive in DIFFERENT conventions, which is why one swap could
 * never fix both:
 *   reflection_map_*  .inc.c is marked "xbox-u16-byteswapped", so the compiled
 *                     array's little-endian bytes ARE the ROM stream already.
 *   gTexturePodium*   .inc.c holds plain ROM values, so its bytes are reversed
 *                     and it needs the swap.
 * import_texture_rgba16 byteswaps on read (both of its branches), so ROM byte
 * order is what it wants in every case.
 *
 * Order-independence matters more than it looks: load_ceremony_data runs THREE
 * times, and menu_items.c separately byteswaps reflection_map_gold in place for
 * the spinning logo. That last one is why the GOLD trophy was wrong while the
 * bronze one -- brass, which nothing else touches -- looked correct. So this
 * checks a sentinel texel and only swaps when needed: idempotent, and immune to
 * however many times anything else got there first.
 *
 * `expect` is texel 48 as a native (little-endian) read once the bytes are in
 * ROM order, i.e. bswap16 of the ROM's own big-endian value at that texel. The
 * values come from the ROM's MIO0 block at 0x821D10. */
static void ensure_rom_byte_order(u16* px, u16 expect, const char* what) {
    (void) what;   /* only the gated reports name the texture */
    if (px[48] == expect) {
#if MK64X_DEBUG_TOOLS
        printf("CEREMTEX %-8s already ROM order (%04X)\n", what, px[48]);
#endif
        return;
    }
    for (s32 i = 0; i < 32 * 32; i++) {
        px[i] = __builtin_bswap16(px[i]);
    }
#if MK64X_DEBUG_TOOLS
    printf("CEREMTEX %-8s swapped -> %04X (wanted %04X)\n", what, px[48], expect);
#endif
}

static void ceremony_swap_segment_textures(void) {
    /* ROM values: brass E357, silver 4211, gold 5A8B,
     *             podium1 F98F, podium2 FADB, podium3 F9D3. */
    ensure_rom_byte_order(reflection_map_brass,  0x57E3, "brass");
    ensure_rom_byte_order(reflection_map_silver, 0x1142, "silver");
    ensure_rom_byte_order(reflection_map_gold,   0x8B5A, "gold");
    ensure_rom_byte_order(gTexturePodium1,       0x8FF9, "podium1");
    ensure_rom_byte_order(gTexturePodium2,       0xDBFA, "podium2");
    ensure_rom_byte_order(gTexturePodium3,       0xD3F9, "podium3");
}

void load_ceremony_cutscene(void) {
    Camera* camera = &cameras[0];
    memset(&D_802874D8, 0, sizeof(D_802874D8));
    /* sPodiumActorList is NULL until podium_ceremony_actors allocates it from
     * CEREMONY_ACTOR_BUF; on the first ceremony entry this memset wrote to
     * address 0. The SH4 (MMU off) silently swallows writes to that region,
     * so the DC port never saw it -- the Xbox access-violates. */
    if (sPodiumActorList != NULL) {
        memset(sPodiumActorList, 0, (sizeof(CeremonyActor) * 200));
    }
    sPodiumActorList = NULL;

    gCurrentCourseId = COURSE_ROYAL_RACEWAY;
    D_800DC5B4 = (u16) 1;
    gIsMirrorMode = 0;
    gGotoMenu = 0xFFFF;
    D_80287554 = 0;
    set_perspective_and_aspect_ratio();
    func_802A74BC();
    camera->unk_B4 = 60.0f;
    gCameraZoom[0] = 60.0f;
    D_800DC5EC->screenWidth = SCREEN_WIDTH;
    D_800DC5EC->screenHeight = SCREEN_HEIGHT;
    D_800DC5EC->screenStartX = 160;
    D_800DC5EC->screenStartY = 120;
    gScreenModeSelection = SCREEN_MODE_1P;
    gActiveScreenMode = SCREEN_MODE_1P;
    gModeSelection = GRAND_PRIX;
    load_course(gCurrentCourseId);
    load_ceremony_data();
    sprintf(texfn, "%s/dc_data/banshee_boardwalk_data.bin", fnpre);

    FILE* file = fopen(texfn, "rb");
    if (!file) {
        perror("fopen");
        printf("\n");
        // while(1) {}
        exit(-1);
    }

    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fseek(file, 0, SEEK_SET);

    long toread = filesize;
    long didread = 0;

    while (didread < filesize) {
        long rv = fread(&COURSE_BUF[didread], 1, toread - didread, file);
        if (rv == -1) {
            perror("fread");
            printf("couldnt read into course buf\n");
            // while(1) {}
            exit(-1);
        }
        toread -= rv;
        didread += rv;
    }
    fclose(file);
    file = NULL;

    set_segment_base_addr(6, (void*) COURSE_BUF);

    D_8015F8E4 = -2000.0f;

    gCourseMinX = -0x15A1;
    gCourseMinY = -0x15A1;
    gCourseMinZ = -0x15A1;

    gCourseMaxX = 0x15A1;
    gCourseMaxY = 0x15A1;
    gCourseMaxZ = 0x15A1;

    D_8015F59C = 0;
    D_8015F5A0 = 0;
    D_8015F58C = 0;
    gCollisionMeshCount = (u16) 0;
    D_800DC5BC = (u16) 0;
    D_800DC5C8 = (u16) 0;
    gCollisionMesh = (CollisionTriangle*) allColTris;
    //! @bug these segmented addresses need to be symbols for mobility
    // d_course_royal_raceway_packed_dl_67E8
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_67E8, -1);
    // d_course_royal_raceway_packed_dl_AEF8
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_AEF8, -1);
    // d_course_royal_raceway_packed_dl_A970
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_A970, 8);
    // d_course_royal_raceway_packed_dl_AC30
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_AC30, 8);
    // d_course_royal_raceway_packed_dl_CE0
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_CE0, 0x10);
    // d_course_royal_raceway_packed_dl_E88
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_E88, 0x10);
    // d_course_royal_raceway_packed_dl_A618
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_A618, -1);
    // d_course_royal_raceway_packed_dl_A618
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_A618, -1);
    // d_course_royal_raceway_packed_dl_23F8
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_23F8, 1);
    // d_course_royal_raceway_packed_dl_2478
    generate_collision_mesh_with_default_section_id((Gfx*) d_course_royal_raceway_packed_dl_2478, 1);
    func_80295C6C();
    debug_switch_character_ceremony_cutscene();
    func_802818BC();
    func_8003D080();
    init_hud();
    func_8001C05C();
    balloons_and_fireworks_init();
    init_camera_podium_ceremony();
    func_80093E60();

    /* Once per ceremony load, after load_ceremony_data has refilled
     * CEREMONY_BUF from the file. (The swaps this replaces originally ran at
     * segment offsets 0x2F18 / 0x3718 / 0x3F18, which are not textures at all
     * -- the array names in ceremony_data.c place those inside
     * ceremony_data_seg11_vtx_2D50 / _3710 / _3E80, so the old code scrambled
     * 6KB of the trophy's own vertices every ceremony.) */
    ceremony_swap_segment_textures();
}
