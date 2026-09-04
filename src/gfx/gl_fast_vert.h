#pragma once

#define VERTEX_EOL 0xf0000000
#define VERTEX 0xe0000000
#define PACK_ARGB8888(r, g, b, a) ((uint32_t)((uint8_t)(a) << 24) | ((uint8_t)(r) << 16) | ((uint8_t)(g) << 8) | (uint8_t)(b))
#define PACK_BGRA8888(r, g, b, a) ((uint32_t)((uint8_t)(b) << 24) | ((uint8_t)(g) << 16) | ((uint8_t)(r) << 8) | (uint8_t)(a))
#define VTX_COLOR_WHITE .color = {.packed = PACK_BGRA8888(255, 255, 255, 255)}

typedef struct __attribute__((packed, aligned(4))) vec3f_gl {
  float x, y, z;
} vec3f;

typedef struct __attribute__((packed, aligned(4))) uv_float {
  float u, v;
} uv_float;

typedef struct __attribute__((packed)) color_uc_struct {
  unsigned char b, g, r, a;
} color_uc_struct;

typedef union color_uc {
  color_uc_struct array;
  unsigned int packed;
} color_uc;

#if defined(TARGET_XBOX)
/* Depth fold factor. The renderer's z is PowerVR-style unbounded inverse-w;
 * D3D8 clips pre-transformed vertices against z in [0,1]. Multiplying by a
 * constant preserves every ordering (including the 2D scheme's +0.0005 per-rect
 * steps) while bringing near geometry and the 2D pane range inside the clip
 * volume. 0.25 leaves headroom above the observed ~2.0 maximum. */
#define XBOX_Z_SCALE 0.25f

/* NV2A/D3D8 vertex. Same MEMBER NAMES as the Dreamcast struct below, so every
 * front-end write site (bv->vert.x, bv->texture.u, bv->color.packed,
 * bv->pad0.vertindex) is unchanged — only the field ORDER differs, rearranged
 * to match D3DFVF_XYZRHW | DIFFUSE | SPECULAR | TEX1. That makes the buffer
 * directly drawable by DrawVerticesUP with no per-vertex conversion, which is
 * the same zero-copy property dc_fast_t had against pvr_vertex_t.
 *
 *   vert     x, y, z      screen space; z is inverse-w, larger == nearer
 *   rhw      1/w          perspective correction (PowerVR derived this from z;
 *                         D3D needs it explicitly or textures go affine)
 *   color    diffuse      baked combiner result   (was PVR argb)
 *   pad0     specular     baked combiner offset   (was PVR oargb)
 *   texture  u, v
 *   flags    unused here  kept so front-end stores still compile; excluded
 *                         from the FVF and skipped by the 36-byte stride.
 *
 * color_uc is {b,g,r,a} in memory order, so color.packed is already 0xAARRGGBB
 * — a D3DCOLOR as-is, no swizzle needed. */
typedef struct __attribute__((packed, aligned(4))) dc_fast_t {
  struct vec3f_gl vert;
  float rhw;
  color_uc color;  // bgra == D3DCOLOR ARGB
  union {
    float pad;
    unsigned int vertindex;
  } pad0;
  uv_float texture;
  uint32_t flags;
} dc_fast_t;
#else
typedef struct __attribute__((packed, aligned(4))) dc_fast_t {
  uint32_t flags;
  struct vec3f_gl vert;
  uv_float texture;
  color_uc color;  // bgra
  union {
    float pad;
    unsigned int vertindex;
  } pad0;
} dc_fast_t;
#endif

/* must be in order:  [weights (0-8)] [texture uv] [color] [normal] [vertex]
  aligned to 32bits (4bytes)
  we are going to use (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF  )
*/
typedef struct __attribute__((packed, aligned(4))) psp_fast_t {
  uv_float texture;
  color_uc color;  // bgra
  struct vec3f_gl vert;
} psp_fast_t;
