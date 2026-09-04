#include <ultra64.h>
#include <macros.h>
#include <mk64.h>
#include "math_util.h"
#include "animation.h"
#include "racing/memory.h"
#include <main.h>
#include <PR/gbi.h>
#include "code_80057C60.h"
#include "sh4zam.h"

Vec3s sOriginalPosAnimation;
s16 isNotTheFirst;
s16 sMatrixShouldNotPop;
s16 sMatrixStackSize;

void convert_to_fixed_point_matrix_animation(Mtx* dest, Mat4 src) {
    /* ALWAYS guMtxF2L (s32 shift/mask packing, endian-safe). The old #else
     * fallback wrote the s16.16 halves through s16 pointers, which lays the
     * integer part in the LOW half of each packed word on little-endian --
     * the reader (gfx_sp_matrix) expects it in the HIGH half. This TU never
     * saw AVOID_UB (only render_player.h pulls in buffers.h), so on Xbox
     * every armature matrix -- seagulls, penguins, ghosts, moles: the ONLY
     * users of this function -- was int/frac-scrambled; six nested limb
     * multiplies of such matrices overflow to inf then NaN: the water-course
     * "screen explodes white near the critters" bug. */
    guMtxF2L(src, dest);
}

void mtxf_translate_rotate2(Mat4 dest, Vec3f pos, Vec3s angle) {
#if 1
    
    register f32 sx = sins(angle[0]);
    register f32 cx = coss(angle[0]);

    register f32 sy = sins(angle[1]);
    register f32 cy = coss(angle[1]);

    register f32 sz = sins(angle[2]);
    register f32 cz = coss(angle[2]);

    dest[0][0] = cy * cz;
    dest[0][1] = cy * sz;
    dest[0][2] = -sy;
    dest[0][3] = 0.0f;

    dest[1][0] = sx * sy * cz - cx * sz;
    dest[1][1] = sx * sy * sz + cx * cz;
    dest[1][2] = sx * cy;
    dest[1][3] = 0.0f;

    dest[2][0] = cx * sy * cz + sx * sz;
    dest[2][1] = cx * sy * sz - sx * cz;
    dest[2][2] = cx * cy;
    dest[2][3] = 0.0f;

    dest[3][0] = pos[0];
    dest[3][1] = pos[1];
    dest[3][2] = pos[2];
    dest[3][3] = 1.0f;
#else
    shz_xmtrx_init_rotation(SHZ_ANGLE(angle[0]), SHZ_ANGLE(angle[1]), SHZ_ANGLE(angle[2]));
    shz_xmtrx_set_translation(pos[0], pos[1], pos[2]);
    shz_xmtrx_store_4x4(dest);
#endif
}

void render_limb_or_add_mtx(Armature* arg0, s16* arg1, AnimationLimbVector arg2, s32 timeCycle) {
    Vec3f pos;
    Vec3s angle;
    Mat4 modelMatrix;
    s32 i;
    s32 some_offset;
    Gfx* model;
    Gfx* virtualModel;
    virtualModel = arg0->model;
    if (isNotTheFirst == 0) {
        for (i = 0; i < 3; i++) {
            pos[i] = sOriginalPosAnimation[i] + arg0->pos[i];
        }
        isNotTheFirst += 1;
    } else {
        for (i = 0; i < 3; i++) {
            pos[i] = arg0->pos[i];
        }
    }
    for (i = 0; i < 3; i++) {
        if (timeCycle < arg2[i].animation_length) {
            some_offset = timeCycle;
        } else {
            some_offset = 0;
        }
        angle[i] = arg1[arg2[i].indexCycle + some_offset];
    }

    mtxf_translate_rotate2(modelMatrix, pos, angle);
    {
        s32 slot = mtx_hud_alloc();
        convert_to_fixed_point_matrix_animation(&gGfxPool->mtxHud[slot], modelMatrix);
        sMatrixStackSize += 1;
        /* Always emit the push even on pool exhaustion (slot reuse) so the
         * armature's POP_MATRIX ops stay balanced. */
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL2(&gGfxPool->mtxHud[slot]),
                  G_MTX_PUSH | G_MTX_MUL | G_MTX_MODELVIEW);
    }
    if (virtualModel != NULL) {
        model = segmented_to_virtual(virtualModel);
        gSPDisplayList(gDisplayListHead++, model);
    }
}

void render_armature(Armature* animation, Animation* arg1, s16 timeCycle) {
    s16* angle_array;
    s32 some_offset;
    AnimationLimbVector* animation_cycle_list;
    s32 animation_type;
    s32 someIndex;

    angle_array = segmented_to_virtual(arg1->angle_array);
    animation_cycle_list = segmented_to_virtual(arg1->animation_cycle_spec_vector);
    sMatrixStackSize = 0;
    isNotTheFirst = 0;
    for (someIndex = 0; someIndex < 3; someIndex++) {
        if (timeCycle < (*animation_cycle_list)[someIndex].animation_length) {
            some_offset = timeCycle;
        } else {
            some_offset = 0;
        }
        sOriginalPosAnimation[someIndex] = angle_array[(*animation_cycle_list)[someIndex].indexCycle + some_offset];
    }
    animation_cycle_list++;
    sMatrixShouldNotPop = 0;
    do {
        animation_type = animation->type;
        switch (animation_type) { /* irregular */
            case STOP_ANIMATION:
                break;
            case DISABLE_AUTOMATIC_POP_MATRIX:
                sMatrixShouldNotPop = 1;
                break;
            case POP_MATRIX:
                gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
                sMatrixStackSize -= 1;
                break;
            case RENDER_MODEL_OR_ADD_POS:
                if (sMatrixShouldNotPop == 0) {
                    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);
                    sMatrixStackSize -= 1;
                }
                render_limb_or_add_mtx(animation, angle_array, *animation_cycle_list, (s32) timeCycle);
                sMatrixShouldNotPop = 0;
                animation_cycle_list++;
                break;
        }
        animation = (Armature*) ((u32*) animation + animation->size);
    } while (animation_type != STOP_ANIMATION);
}

s16 render_animated_model(Armature* virtualArmature, Animation** virtualListAnimation, s16 animationIndex,
                          s16 timeCycle) {
    Armature* armature;
    Animation* animation;
    Animation** listAnimation;

    armature = segmented_to_virtual(virtualArmature);
    listAnimation = segmented_to_virtual(virtualListAnimation);      // Convert the array's address
    animation = segmented_to_virtual(listAnimation[animationIndex]); // Convert an array element's address
    if (timeCycle >= animation->animation_length) {
        timeCycle = 0;
    }
    render_armature(armature, animation, timeCycle);
    timeCycle++;
    if (timeCycle >= animation->animation_length) {
        timeCycle = 0;
    }
    return timeCycle;
}

s16 get_animation_length(Animation** addr, s16 offset) {
    Animation** item = segmented_to_virtual(addr);
    Animation* temp = (Animation*) segmented_to_virtual((void*) item[offset]);

    return temp->animation_length - 1;
}
