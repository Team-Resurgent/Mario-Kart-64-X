#include <actors.h>
#include <PR/gbi.h>

/**
 * @brief Render the red shell actor
 *
 * @param camera
 * @param matrix
 * @param shell
 */
void render_actor_red_shell(Camera* camera, Mat4 matrix, struct ShellActor* shell) {
    gDPLoadTLUT_pal256(gDisplayListHead++, &gTLUTRedShell); // set texture
    render_actor_shell(camera, matrix, shell);
}

/**
 * @brief Render the blue shell actor
 *
 * @param camera
 * @param matrix
 * @param shell
 */
void render_actor_blue_shell(Camera* camera, Mat4 matrix, struct ShellActor* shell) {
#if defined(TARGET_XBOX)
    // Segment copy (ROM-true BE); see render_actor_green_shell.
    gDPLoadTLUT_pal256(gDisplayListHead++, 0x0D005038);
#else
    gDPLoadTLUT_pal256(gDisplayListHead++, common_tlut_blue_shell); // set texture
#endif
    render_actor_shell(camera, matrix, shell);
}
