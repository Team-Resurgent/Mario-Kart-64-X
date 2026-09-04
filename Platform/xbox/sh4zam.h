/* sh4zam.h — x86 (Pentium III) replacement for the SH-4 sh4zam math library.
 *
 * This file deliberately SHADOWS include/sh4zam.h. Platform/xbox is placed
 * first on the include path for the Xbox target, so every `#include "sh4zam.h"`
 * in the shared sources picks this up instead of the SH-4 original. No call
 * site changes.
 *
 * Conventions replicated exactly from the SH-4 original — getting these wrong
 * silently produces a scrambled scene rather than a compile error:
 *
 *   Storage      elem[0..15] maps to XF0..XF15. elem2D[r][c] == elem[r*4+c].
 *                col[n] is elem[n*4 .. n*4+3]. This is COLUMN-MAJOR: the four
 *                floats elem[0..3] form the matrix's first column.
 *
 *   ftrv         out = XMTRX * v, i.e.
 *                  out.x = m[0]*x + m[4]*y + m[8] *z + m[12]*w
 *                  out.y = m[1]*x + m[5]*y + m[9] *z + m[13]*w
 *                  out.z = m[2]*x + m[6]*y + m[10]*z + m[14]*w
 *                  out.w = m[3]*x + m[7]*y + m[11]*z + m[15]*w
 *
 *   apply        POST-multiply: XMTRX = XMTRX * M. (The SH-4 version ftrv's
 *                each of M's four columns through XMTRX and stores them back
 *                as the result's columns, which is exactly XMTRX * M.)
 *
 *   fsca         16-bit binary angle: 65536 == 2*pi. shz_sincosf multiplies
 *                radians by 65536/(2*pi) and TRUNCATES TO uint16_t, so large
 *                angles wrap. That wrap is load-bearing for game code that
 *                feeds unnormalised angles — it is reproduced here.
 *
 * The SH-4 original's approximate reciprocal/rsqrt (fsrra) are replaced with
 * exact divides. The PIII's rcpps/rsqrtps are ~12-bit and would be *less*
 * accurate than fsrra, so exactness is both simpler and safer here.
 */
#ifndef SH4ZAM_H
#define SH4ZAM_H

#include <stdint.h>
#include <math.h>
#include <string.h>

#define SHZ_NOEXCEPT
#define SHZ_ALIGNAS(n)          __attribute__((aligned(n)))
#define SHZ_INLINE              inline static
#define SHZ_FORCE_INLINE        __attribute__((always_inline)) SHZ_INLINE
#define SHZ_NO_INLINE           __attribute__((noinline))
#define SHZ_ALIASING            __attribute__((__may_alias__))

#define SHZ_FSCA_RAD_FACTOR     10430.37835f

#define TRIG_ARG_SCALE 0.00009587f
#define SHZ_ANGLE(a) (((float)((uint16_t)a)) * TRIG_ARG_SCALE)

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- types -- */

typedef struct shz_sincos {
    float sin;
    float cos;
} shz_sincos_t;

typedef struct shz_vec2 {
    union {
        struct { float x; float y; };
        float e[2];
    };
} shz_vec2_t;

typedef struct shz_vec3 {
    union {
        struct {
            union {
                struct { float x; float y; };
                shz_vec2_t vec2;
            };
            float z;
        };
        float e[3];
    };
} shz_vec3_t;

typedef struct shz_vec4 {
    union {
        struct {
            union {
                struct {
                    union {
                        struct { float x; float y; };
                        shz_vec2_t vec2;
                    };
                    float z;
                };
                shz_vec3_t vec3;
            };
            float w;
        };
        float e[4];
    };
} shz_vec4_t;

typedef SHZ_ALIGNAS(8) union shz_matrix_2x2 {
    float       elem[4];
    float       elem2D[2][2];
    shz_vec2_t  col[2];
} shz_matrix_2x2_t;

typedef union shz_matrix_3x3 {
    float      elem[9];
    float      elem2D[3][3];
    shz_vec3_t col[3];
    struct {
        shz_vec3_t left;
        shz_vec3_t up;
        shz_vec3_t forward;
    };
} shz_matrix_3x3_t;

typedef union shz_matrix_3x4 {
    float      elem[12];
    float      elem2D[3][4];
    shz_vec3_t col[4];
    struct {
        shz_vec3_t left;
        shz_vec3_t up;
        shz_vec3_t forward;
        shz_vec3_t pos;
    };
} shz_matrix_3x4_t;

typedef SHZ_ALIGNAS(8) union shz_matrix_4x4 {
    float      elem[16];
    float      elem2D[4][4];
    shz_vec4_t col[4];
    struct {
        shz_vec4_t left;
        shz_vec4_t up;
        shz_vec4_t forward;
        shz_vec4_t pos;
    };
} shz_matrix_4x4_t;

/* ------------------------------------------------------------- scalars -- */

SHZ_FORCE_INLINE float shz_mag_sqr4f(float x, float y, float z, float w) {
    return x * x + y * y + z * z + w * w;
}

SHZ_FORCE_INLINE float shz_dot8f(float x1, float y1, float z1, float w1,
                                 float x2, float y2, float z2, float w2) {
    return x1 * x2 + y1 * y2 + z1 * z2 + w1 * w2;
}

SHZ_FORCE_INLINE float shz_inverse_sqrtf(float x) {
    return 1.0f / sqrtf(x);
}

SHZ_FORCE_INLINE float shz_inverse_posf(float x) {
    return 1.0f / x;
}

SHZ_FORCE_INLINE float shz_div_posf(float num, float denom) {
    return num / denom;
}

SHZ_FORCE_INLINE float shz_fast_invf(float x) {
    return 1.0f / x;
}

/* --------------------------------------------------------------- trig --- */

/* fsca: 16-bit binary angle, 65536 == 2*pi. The uint16_t parameter provides
 * the same wrap the SH-4 instruction has. */
SHZ_FORCE_INLINE shz_sincos_t shz_sincosu16(uint16_t radians16) {
    const float a = (float) radians16 * (6.2831853071795864769f / 65536.0f);
    shz_sincos_t r;
    r.sin = sinf(a);
    r.cos = cosf(a);
    return r;
}

SHZ_FORCE_INLINE shz_sincos_t shz_sincosf(float radians) {
    /* Matches the original: float -> uint16_t truncation, so the angle wraps. */
    return shz_sincosu16((uint16_t)(int32_t)(radians * SHZ_FSCA_RAD_FACTOR));
}

SHZ_FORCE_INLINE float shz_sincos_tanf(shz_sincos_t sincos) {
    return sincos.sin / sincos.cos;
}

SHZ_FORCE_INLINE float shz_sinf(float radians) { return shz_sincosf(radians).sin; }
SHZ_FORCE_INLINE float shz_cosf(float radians) { return shz_sincosf(radians).cos; }
SHZ_FORCE_INLINE float shz_tanf(float radians) { return shz_sincos_tanf(shz_sincosf(radians)); }

/* --------------------------------------------------------------- vec3 --- */

SHZ_FORCE_INLINE shz_vec3_t shz_vec3_scale(shz_vec3_t vec, float factor) SHZ_NOEXCEPT {
    shz_vec3_t r;
    r.x = vec.x * factor;
    r.y = vec.y * factor;
    r.z = vec.z * factor;
    return r;
}

SHZ_FORCE_INLINE float shz_vec3_magnitude_sqr(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_mag_sqr4f(vec.x, vec.y, vec.z, 0.0f);
}

SHZ_FORCE_INLINE float shz_vec3_magnitude_inv(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_inverse_sqrtf(shz_vec3_magnitude_sqr(vec));
}

SHZ_FORCE_INLINE shz_vec3_t shz_vec3_normalize(shz_vec3_t vec) SHZ_NOEXCEPT {
    return shz_vec3_scale(vec, shz_vec3_magnitude_inv(vec));
}

/* -------------------------------------------------------------- XMTRX --- */
/* The SH-4's XMTRX is a second bank of 16 FP registers. On x86 it is just a
 * file-static matrix; every load/store/apply below reads and writes it. */

static shz_matrix_4x4_t shz__xmtrx SHZ_ALIGNAS(16) = {
    .elem = { 1.0f, 0.0f, 0.0f, 0.0f,
              0.0f, 1.0f, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f,
              0.0f, 0.0f, 0.0f, 1.0f }
};

/* out = M * v, column-major (see the ftrv note at the top of this file). */
SHZ_INLINE void shz__mat_mul_vec4(const float *m, const float *v, float *out) {
    const float x = v[0], y = v[1], z = v[2], w = v[3];
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12] * w;
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13] * w;
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
    out[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
}

/* out = a * b. Each column of b is transformed by a — matching the SH-4
 * apply, which ftrv's b's four columns through XMTRX. out may alias a or b. */
SHZ_INLINE void shz__mat_mul(const float *a, const float *b, float *out) {
    float t[16];
    shz__mat_mul_vec4(a, b + 0,  t + 0);
    shz__mat_mul_vec4(a, b + 4,  t + 4);
    shz__mat_mul_vec4(a, b + 8,  t + 8);
    shz__mat_mul_vec4(a, b + 12, t + 12);
    memcpy(out, t, sizeof(t));
}

SHZ_FORCE_INLINE shz_vec4_t shz_xmtrx_trans_vec4(shz_vec4_t vec) {
    shz_vec4_t out;
    shz__mat_mul_vec4(shz__xmtrx.elem, vec.e, out.e);
    return out;
}

SHZ_FORCE_INLINE shz_vec3_t shz_xmtrx_trans_vec3(shz_vec3_t vec) {
    shz_vec4_t v;
    v.x = vec.x; v.y = vec.y; v.z = vec.z; v.w = 0.0f;
    return shz_xmtrx_trans_vec4(v).vec3;
}

SHZ_FORCE_INLINE shz_vec2_t shz_xmtrx_trans_vec2(shz_vec2_t vec) {
    shz_vec3_t v;
    v.x = vec.x; v.y = vec.y; v.z = 0.0f;
    return shz_xmtrx_trans_vec3(v).vec2;
}

/* --- load / store --- */

SHZ_INLINE void shz_xmtrx_load_4x4(const shz_matrix_4x4_t *matrix) {
    memcpy(shz__xmtrx.elem, matrix->elem, sizeof(float) * 16);
}

SHZ_INLINE void shz_xmtrx_load_4x4_unaligned(const float matrix[16]) {
    memcpy(shz__xmtrx.elem, matrix, sizeof(float) * 16);
}

SHZ_INLINE void shz_xmtrx_store_4x4(shz_matrix_4x4_t *matrix) {
    memcpy(matrix->elem, shz__xmtrx.elem, sizeof(float) * 16);
}

SHZ_INLINE void shz_xmtrx_store_4x4_unaligned(float matrix[16]) {
    memcpy(matrix, shz__xmtrx.elem, sizeof(float) * 16);
}

SHZ_INLINE void shz__transpose16(const float *in, float *out) {
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[c * 4 + r] = in[r * 4 + c];
    memcpy(out, t, sizeof(t));
}

SHZ_INLINE void shz_xmtrx_load_4x4_transpose(const shz_matrix_4x4_t *matrix) {
    shz__transpose16(matrix->elem, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_store_4x4_transpose(shz_matrix_4x4_t *matrix) {
    shz__transpose16(shz__xmtrx.elem, matrix->elem);
}

SHZ_INLINE void shz_xmtrx_load_4x4_rows(const shz_vec4_t *r1, const shz_vec4_t *r2,
                                        const shz_vec4_t *r3, const shz_vec4_t *r4) {
    const shz_vec4_t *r[4] = { r1, r2, r3, r4 };
    for (int i = 0; i < 4; i++)
        for (int c = 0; c < 4; c++)
            shz__xmtrx.elem[c * 4 + i] = r[i]->e[c];
}

SHZ_INLINE void shz_xmtrx_load_4x4_cols(const shz_vec4_t *c1, const shz_vec4_t *c2,
                                        const shz_vec4_t *c3, const shz_vec4_t *c4) {
    const shz_vec4_t *c[4] = { c1, c2, c3, c4 };
    for (int i = 0; i < 4; i++)
        memcpy(&shz__xmtrx.elem[i * 4], c[i]->e, sizeof(float) * 4);
}

SHZ_INLINE void shz_matrix_4x4_copy(shz_matrix_4x4_t *dst, const shz_matrix_4x4_t *src) {
    memcpy(dst->elem, src->elem, sizeof(float) * 16);
}

/* --- 3x3 --- */

SHZ_INLINE void shz_xmtrx_load_3x3(const shz_matrix_3x3_t *matrix) {
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            shz__xmtrx.elem[c * 4 + r] = matrix->elem[c * 3 + r];
        shz__xmtrx.elem[c * 4 + 3] = 0.0f;
    }
    shz__xmtrx.elem[12] = shz__xmtrx.elem[13] = shz__xmtrx.elem[14] = 0.0f;
    shz__xmtrx.elem[15] = 1.0f;
}

SHZ_INLINE void shz_xmtrx_load_3x3_transpose(const float *matrix) {
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            shz__xmtrx.elem[c * 4 + r] = matrix[r * 3 + c];
        shz__xmtrx.elem[c * 4 + 3] = 0.0f;
    }
    shz__xmtrx.elem[12] = shz__xmtrx.elem[13] = shz__xmtrx.elem[14] = 0.0f;
    shz__xmtrx.elem[15] = 1.0f;
}

SHZ_INLINE void shz_xmtrx_store_3x3(shz_matrix_3x3_t *matrix) {
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            matrix->elem[c * 3 + r] = shz__xmtrx.elem[c * 4 + r];
}

SHZ_INLINE void shz_xmtrx_store_3x3_transpose(shz_matrix_3x3_t *matrix) {
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            matrix->elem[r * 3 + c] = shz__xmtrx.elem[c * 4 + r];
}

/* --- apply (post-multiply) --- */

SHZ_INLINE void shz_xmtrx_apply_4x4_unaligned(const float matrix[16]) {
    shz__mat_mul(shz__xmtrx.elem, matrix, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_4x4(const shz_matrix_4x4_t *matrix) {
    shz__mat_mul(shz__xmtrx.elem, matrix->elem, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_3x3(const shz_matrix_3x3_t *matrix) {
    float m[16];
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            m[c * 4 + r] = matrix->elem[c * 3 + r];
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = m[13] = m[14] = 0.0f;
    m[15] = 1.0f;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_3x3_transpose(const shz_matrix_3x3_t *matrix) {
    float m[16];
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++)
            m[c * 4 + r] = matrix->elem[r * 3 + c];
        m[c * 4 + 3] = 0.0f;
    }
    m[12] = m[13] = m[14] = 0.0f;
    m[15] = 1.0f;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_load_4x4_apply(const shz_matrix_4x4_t *matrix1,
                                         const shz_matrix_4x4_t *matrix2) {
    shz__mat_mul(matrix1->elem, matrix2->elem, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_load_4x4_apply_store(shz_matrix_4x4_t *out,
                                               const shz_matrix_4x4_t *matrix1,
                                               const shz_matrix_4x4_t *matrix2) {
    shz__mat_mul(matrix1->elem, matrix2->elem, shz__xmtrx.elem);
    memcpy(out->elem, shz__xmtrx.elem, sizeof(float) * 16);
}

/* --- init / compose --- */

SHZ_INLINE void shz_xmtrx_init_identity(void) {
    static const float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    memcpy(shz__xmtrx.elem, I, sizeof(I));
}

SHZ_INLINE void shz_xmtrx_init_diagonal(float x, float y, float z, float w) {
    memset(shz__xmtrx.elem, 0, sizeof(float) * 16);
    shz__xmtrx.elem[0]  = x;
    shz__xmtrx.elem[5]  = y;
    shz__xmtrx.elem[10] = z;
    shz__xmtrx.elem[15] = w;
}

SHZ_INLINE void shz_xmtrx_init_scale(float x, float y, float z) {
    shz_xmtrx_init_diagonal(x, y, z, 1.0f);
}

SHZ_INLINE void shz_xmtrx_init_translation(float x, float y, float z) {
    shz_xmtrx_init_identity();
    shz__xmtrx.elem[12] = x;
    shz__xmtrx.elem[13] = y;
    shz__xmtrx.elem[14] = z;
}

/* Translation lives in the fourth COLUMN (elem[12..14]) under this layout. */
SHZ_INLINE void shz_xmtrx_set_translation(float x, float y, float z) {
    shz__xmtrx.elem[12] = x;
    shz__xmtrx.elem[13] = y;
    shz__xmtrx.elem[14] = z;
}

SHZ_INLINE void shz_xmtrx_init_rotation_x(float x) {
    const shz_sincos_t sc = shz_sincosf(x);
    shz_xmtrx_init_identity();
    shz__xmtrx.elem[5]  =  sc.cos;
    shz__xmtrx.elem[6]  =  sc.sin;
    shz__xmtrx.elem[9]  = -sc.sin;
    shz__xmtrx.elem[10] =  sc.cos;
}

SHZ_INLINE void shz_xmtrx_init_rotation_y(float y) {
    const shz_sincos_t sc = shz_sincosf(y);
    shz_xmtrx_init_identity();
    shz__xmtrx.elem[0]  =  sc.cos;
    shz__xmtrx.elem[2]  = -sc.sin;
    shz__xmtrx.elem[8]  =  sc.sin;
    shz__xmtrx.elem[10] =  sc.cos;
}

SHZ_INLINE void shz_xmtrx_init_rotation_z(float z) {
    const shz_sincos_t sc = shz_sincosf(z);
    shz_xmtrx_init_identity();
    shz__xmtrx.elem[0] =  sc.cos;
    shz__xmtrx.elem[1] =  sc.sin;
    shz__xmtrx.elem[4] = -sc.sin;
    shz__xmtrx.elem[5] =  sc.cos;
}

SHZ_INLINE void shz_xmtrx_apply_rotation_x(float x) {
    const shz_sincos_t sc = shz_sincosf(x);
    float m[16] = { 1,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    m[5]  =  sc.cos; m[6]  =  sc.sin;
    m[9]  = -sc.sin; m[10] =  sc.cos;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_rotation_y(float y) {
    const shz_sincos_t sc = shz_sincosf(y);
    float m[16] = { 0,0,0,0, 0,1,0,0, 0,0,0,0, 0,0,0,1 };
    m[0] =  sc.cos; m[2]  = -sc.sin;
    m[8] =  sc.sin; m[10] =  sc.cos;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_rotation_z(float z) {
    const shz_sincos_t sc = shz_sincosf(z);
    float m[16] = { 0,0,0,0, 0,0,0,0, 0,0,1,0, 0,0,0,1 };
    m[0] =  sc.cos; m[1] =  sc.sin;
    m[4] = -sc.sin; m[5] =  sc.cos;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

/* Args are the N64's (xAngle, yAngle, zAngle) -- mtxf_pos_rotation_xyz passes
 * orientation[0..2] straight through. The N64 reference matrix (kept under
 * #if 0 there) is v' = v * Rz * Rx * Ry in its row-vector convention, which in
 * this file's column-major post-multiply terms is init_y, apply_x, apply_z --
 * verified element-by-element against that reference. The previous version
 * fed the X angle into a Z rotation (and composed in the wrong order), which
 * is why every composed-rotation actor -- the jungle paddle boat, the
 * turnpike train -- rendered tilted. */
SHZ_INLINE void shz_xmtrx_init_rotation(float x, float y, float z) {
    shz_xmtrx_init_rotation_y(y);
    shz_xmtrx_apply_rotation_x(x);
    shz_xmtrx_apply_rotation_z(z);
}

SHZ_INLINE void shz_xmtrx_apply_rotation(float x, float y, float z) {
    shz_xmtrx_apply_rotation_y(y);
    shz_xmtrx_apply_rotation_x(x);
    shz_xmtrx_apply_rotation_z(z);
}

SHZ_INLINE void shz_xmtrx_apply_translation(float x, float y, float z) {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    m[12] = x; m[13] = y; m[14] = z;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_apply_scale(float x, float y, float z) {
    float m[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    m[0] = x; m[5] = y; m[10] = z;
    shz__mat_mul(shz__xmtrx.elem, m, shz__xmtrx.elem);
}

SHZ_INLINE void shz_xmtrx_transpose(void) {
    shz__transpose16(shz__xmtrx.elem, shz__xmtrx.elem);
}

/* --- explicit-matrix transforms (do not touch XMTRX) --- */

SHZ_INLINE shz_vec3_t shz_matrix4x4_trans_vec3(const shz_matrix_4x4_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = m->elem[0] * v.x + m->elem[4] * v.y + m->elem[8]  * v.z;
    out.y = m->elem[1] * v.x + m->elem[5] * v.y + m->elem[9]  * v.z;
    out.z = m->elem[2] * v.x + m->elem[6] * v.y + m->elem[10] * v.z;
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix4x4_trans_vec3_transpose(const shz_matrix_4x4_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = m->elem[0] * v.x + m->elem[1] * v.y + m->elem[2]  * v.z;
    out.y = m->elem[4] * v.x + m->elem[5] * v.y + m->elem[6]  * v.z;
    out.z = m->elem[8] * v.x + m->elem[9] * v.y + m->elem[10] * v.z;
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix3x3_trans_vec3(const shz_matrix_3x3_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = m->elem[0] * v.x + m->elem[3] * v.y + m->elem[6] * v.z;
    out.y = m->elem[1] * v.x + m->elem[4] * v.y + m->elem[7] * v.z;
    out.z = m->elem[2] * v.x + m->elem[5] * v.y + m->elem[8] * v.z;
    return out;
}

SHZ_INLINE shz_vec3_t shz_matrix3x3_trans_vec3_transpose(const shz_matrix_3x3_t *m, shz_vec3_t v) {
    shz_vec3_t out;
    out.x = m->elem[0] * v.x + m->elem[1] * v.y + m->elem[2] * v.z;
    out.y = m->elem[3] * v.x + m->elem[4] * v.y + m->elem[5] * v.z;
    out.z = m->elem[6] * v.x + m->elem[7] * v.y + m->elem[8] * v.z;
    return out;
}

/* --- memory --- */

/* SH-4 cache-line allocate. x86 has no equivalent that helps here; the PIII's
 * prefetch would only pull the line in, which is the opposite of the intent
 * (allocate-without-fetch for a line about to be fully overwritten). No-op. */
SHZ_FORCE_INLINE void shz_dcache_alloc_line(void *src) { (void) src; }

SHZ_INLINE void shz_memcpy4_16(void *dst, const void *src) {
    memcpy(dst, src, 16);
}

#ifdef __cplusplus
}
#endif

#endif /* SH4ZAM_H */
