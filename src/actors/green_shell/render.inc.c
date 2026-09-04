#include <actors.h>
#include <PR/gbi.h>
#include <main.h>
#include <assets/common_data.h>

/**
 * @brief Renders the green shell actor.
 *
 * @param camera
 * @param matrix
 * @param shell
 */
void render_actor_green_shell(Camera* camera, Mat4 matrix, struct ShellActor* shell) {
#if defined(TARGET_XBOX)
    // Load the palette from the SEGMENT copy (COMMON_BUF, extracted verbatim
    // from the ROM's MIO0 block -- raw BE, exactly what load_tlut expects;
    // 0x04E38 is common_tlut_green_shell's block_offset). The compiled C
    // array kept reaching load_tlut in value order despite the boot swap
    // (some third copy/aliasing we never fully pinned), which zeroed half the
    // palette's alpha bits: the translucent shell.
    gDPLoadTLUT_pal256(gDisplayListHead++, 0x0D004E38);
#else
    gDPLoadTLUT_pal256(gDisplayListHead++, common_tlut_green_shell); // set texture
#endif
    render_actor_shell(camera, matrix, shell);
}
