/* kos.h — Xbox shim for the slice of KallistiOS this port actually uses.
 *
 * This file SHADOWS the real KOS header. Platform/xbox comes first on the
 * include path for the Xbox target, so `#include <kos.h>` in shared sources
 * resolves here and the game compiles unmodified.
 *
 * Only symbols the game genuinely references are provided — deliberately not a
 * general KOS emulation. If a new one appears the build breaks loudly at the
 * use site, which is the intent. Implementations live in kos_xbox.c.
 */
#ifndef XBOX_KOS_SHIM_H
#define XBOX_KOS_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>     /* memalign — picolibc provides it */
#include <sys/types.h> /* ssize_t, used by the fs_* declarations below */
#include <fcntl.h>     /* O_RDONLY / O_RDWR / O_CREAT — the modes fs_open takes */
#include <stdio.h>     /* SEEK_SET and friends; real KOS pulls these in too */

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================== texture formats ==
 * The renderer front-end converts N64 texels to 16-bit and passes one of these
 * as upload_texture's `type`. PVR's ARGB1555/ARGB4444 bit layouts are
 * identical to D3D's A1R5G5B5/A4R4G4B4, so no pixel conversion is needed on
 * the Xbox side — only the format tag is translated (see gfx_nv2a.cpp). */
#define PVR_TXRFMT_ARGB1555     0x00000000u
#define PVR_TXRFMT_ARGB4444     0x00000010u
#define PVR_TXRFMT_NONTWIDDLED  0x04000000u

/* ============================================================ store queue ==
 * The SH-4 store queue is a 32-byte write-combining channel straight to the
 * tile accelerator. The Xbox has no equivalent in this position: geometry is
 * batched and handed to D3D at end_frame instead.
 *
 * pvr_sq_ship is NOT declared here — the renderer front-end defines its own
 * (SH-4 asm on Dreamcast, bypassed on Xbox), and a shim would collide. */
#define PVR_TA_INPUT ((void *) 0)

static inline void sq_fast_cpy(void *dst, const void *src, unsigned int n_lines32) {
    memcpy(dst, src, (size_t) n_lines32 * 32u);
}

/* ================================================================= timing == */
uint64_t timer_us_gettime64(void);
uint64_t timer_ms_gettime64(void);
uint64_t timer_ns_gettime64(void);

/* ================================================================ threads ==
 * KOS threads map onto Win32/xapi threads. The game creates exactly one
 * (the audio thread) and otherwise only sleeps and yields. */
typedef struct kthread_attr {
    int         create_detached;
    size_t      stack_size;
    void       *stack_ptr;
    int         prio;
    const char *label;
} kthread_attr_t;

typedef struct kthread kthread_t;

kthread_t *thd_create_ex(const kthread_attr_t *attr, void *(*routine)(void *), void *param);
void       thd_sleep(int ms);
void       thd_pass(void);
/* KOS scheduler tick rate. The Xbox scheduler is not tunable this way and the
 * game only ever raises it for audio responsiveness, so this is a no-op. */
void       thd_set_hz(int hz);

/* ============================================================== mutexes ==== */
typedef struct { void *cs; } mutex_t;

#define MUTEX_INITIALIZER { NULL }

int  mutex_init(mutex_t *m, int type);
int  mutex_lock(mutex_t *m);
int  mutex_unlock(mutex_t *m);
int  mutex_destroy(mutex_t *m);

#define MUTEX_TYPE_NORMAL    0
#define MUTEX_TYPE_RECURSIVE 2

/* KOS's scoped lock relies on a GCC cleanup attribute; clang supports the same
 * attribute, so the shape carries over directly. */
void shim__mutex_unlock_cleanup(mutex_t **m);
#define mutex_lock_scoped(m) \
    mutex_t *__scoped_##__LINE__ __attribute__((cleanup(shim__mutex_unlock_cleanup))) = \
        (mutex_lock(m), (m))

/* ================================================================== IRQs ==
 * Used only around a short critical section. Xbox titles own the machine, and
 * the one call site is commented out upstream, so these are inert. */
typedef struct { uint32_t pc; } irq_context_t;
#define irq_disable_scoped() ((void) 0)

void *arch_get_ret_addr(void);

/* Top of usable RAM. The game uses it to size its heap. */
extern uint8_t *_arch_mem_top;

/* Dreamcast video border colour, used as a boot-progress indicator. The Xbox
 * has no border register; kept as a no-op so the progress calls still compile. */
void vid_border_color(int r, int g, int b);

/* ================================================================== maple ==
 * KOS's maple bus is the Dreamcast's peripheral bus. On Xbox this is backed by
 * XInput: maple_enum_type(port, FUNC) reports whether a device of that kind is
 * in that port, and maple_dev_status() fills a cont_state_t from the pad.
 *
 * cont_state_t keeps the Dreamcast field names and ranges so the game's
 * existing button/stick mapping code is unchanged:
 *   buttons  CONT_* bitmask
 *   joyx     -128..127, positive right
 *   joyy     -128..127, positive DOWN (the game inverts it)
 *   ltrig    0..255
 *   rtrig    0..255 */
#define MAPLE_FUNC_CONTROLLER 0x01000000
#define MAPLE_FUNC_KEYBOARD   0x40000000
#define MAPLE_FUNC_MEMCARD    0x02000000

#define CONT_C          (1u << 0)
#define CONT_B          (1u << 1)
#define CONT_A          (1u << 2)
#define CONT_START      (1u << 3)
#define CONT_DPAD_UP    (1u << 4)
#define CONT_DPAD_DOWN  (1u << 5)
#define CONT_DPAD_LEFT  (1u << 6)
#define CONT_DPAD_RIGHT (1u << 7)
#define CONT_Z          (1u << 8)
#define CONT_Y          (1u << 9)
#define CONT_X          (1u << 10)
#define CONT_D          (1u << 11)

typedef struct { int is_down; } kbd_key_state_t;

typedef struct cont_state {
    uint32_t        buttons;
    int             ltrig, rtrig;
    int             joyx, joyy;
    int             joy2x, joy2y;
    kbd_key_state_t key_states[256];
} cont_state_t;

typedef struct maple_device maple_device_t;

/* Start peripheral enumeration early -- it is asynchronous, so it has to be
 * kicked off well before the game's boot-time controller check. */
void            xbox_input_init(void);

maple_device_t *maple_enum_type(int index, uint32_t function);
void           *maple_dev_status(maple_device_t *dev);


/* ================================================================= fmath ==
 * KOS's vec3f_normalize normalises x, y and z in place (it uses fsrra on
 * SH-4). Same contract here, plain C. */
#define vec3f_normalize(x, y, z) do {                                      float _n = (x) * (x) + (y) * (y) + (z) * (z);                          if (_n > 0.0f) {                                                           float _i = 1.0f / __builtin_sqrtf(_n);                                 (x) *= _i; (y) *= _i; (z) *= _i;                                   }                                                                  } while (0)

/* ================================================================== vram ==
 * KOS exposes the framebuffer directly as vram_s (16-bit, 640 wide). The Xbox
 * back buffer is not CPU-addressable that way, so vram_s points at a mirror in
 * main memory and xbox_vram_snapshot() refreshes it from the back buffer.
 * Only the framebuffer-copy effect uses this, and it is rare, so the cost is
 * paid on demand rather than every frame. */
extern uint16_t *vram_s;
void xbox_vram_snapshot(void);
/* `step` is the sampling stride in BOTH axes: only pixels at px+n*step /
 * py+m*step are read from the back buffer. The jumbotron consumer samples the
 * framebuffer at every 2nd pixel and every 2nd row, so step=2 reads a QUARTER
 * of the region -- and back-buffer reads are the expensive part (uncached GPU
 * memory, ~17MB/s). Pixels in between are left untouched in the mirror. */
void xbox_vram_snapshot_rect(int px, int py, int pw, int ph, int step);


/* --- keyboard ---
 * USB HID usage codes, which is what KOS's KBD_KEY_* are. No keyboard is
 * enumerated on this port (maple_enum_type returns NULL for it), so these
 * exist to keep main.c's keyboard path compiling; it is never entered. */
#define KBD_KEY_ESCAPE 0x29
#define KBD_KEY_ENTER  0x28
#define KBD_KEY_SPACE  0x2C
#define KBD_KEY_A 0x04
#define KBD_KEY_B 0x05
#define KBD_KEY_C 0x06
#define KBD_KEY_D 0x07
#define KBD_KEY_E 0x08
#define KBD_KEY_Q 0x14
#define KBD_KEY_S 0x16
#define KBD_KEY_W 0x1A
#define KBD_KEY_X 0x1B
#define KBD_KEY_Z 0x1D
#define KBD_KEY_RIGHT 0x4F
#define KBD_KEY_LEFT  0x50
#define KBD_KEY_DOWN  0x51
#define KBD_KEY_UP    0x52

typedef struct kbd_state {
    kbd_key_state_t key_states[256];
} kbd_state_t;

/* KOS's bitmap debug font and debug console. RXDK has its own debug channel,
 * and the boot-progress text these draw is Dreamcast-specific, so both are
 * inert here. */
void bfont_draw_ex(void *buffer, int bufwidth, unsigned int fg, unsigned int bg,
                   int bpp, int opaque, unsigned int c, int iskana, int wide);
void dbgio_enable(void);

/* =================================================================== VMU ==
 * The Dreamcast wraps saves in a VMU package: three 512-byte header blocks
 * (description, icon, animation) followed by the payload. The read path in
 * ultra_reimpl.c hard-codes that layout -- it requires a 2048-byte file and
 * seeks past 512*3 before reading -- so the Xbox build must reproduce the
 * SIZE and OFFSETS even though the header contents are meaningless here.
 * vmu_pkg_build therefore emits a zeroed 1536-byte header plus the payload. */
typedef struct vmu_pkg {
    char        desc_short[17];
    char        desc_long[33];
    char        app_id[17];
    int         icon_cnt;
    int         icon_anim_speed;
    int         eyecatch_type;
    int         data_len;
    const void *icon_data;
    const void *eyecatch_data;
    const void *data;
} vmu_pkg_t;

int  vmu_pkg_build(vmu_pkg_t *src, uint8_t **dst, ssize_t *dst_size);
int  vmu_pkg_load_icon(vmu_pkg_t *pkg, const char *icon_fn);

/* KOS vblank callback registration. The Xbox pacer drives the vblank counter
 * itself (BlockUntilVerticalBlank is synchronous), so there is no interrupt to
 * hook and this is inert. */
int  vblank_handler_add(void *hnd, void *data);

/* =============================================================== genwait ==
 * KOS's generic wait channel: threads sleep on an arbitrary address and are
 * woken together. Used here for the 60Hz vblank tick, which both the main
 * loop and the audio thread wait on -- hence wake_ALL rather than wake_one,
 * or one of the two starves.
 *
 * Backed by a manual-reset event pulsed on wake, which gives the same
 * release-everyone semantics. A timeout of 0 means wait indefinitely. */
int  genwait_wait(void *obj, const char *mesg, int timeout, void (*callback)(void *));
void genwait_wake_all(void *obj);

/* =========================================================== file system ==
 * KOS's VFS, used by the save-game path. Backed by Xbox file I/O; the VMU
 * becomes a file on the title's own storage. */
typedef int file_t;

#define O_META 0x0100

file_t  fs_open(const char *fn, int mode);
int     fs_close(file_t fd);
ssize_t fs_read(file_t fd, void *buf, size_t nbytes);
ssize_t fs_write(file_t fd, const void *buf, size_t nbytes);
long    fs_seek(file_t fd, long offset, int whence);
ssize_t fs_total(file_t fd);
int     fs_unlink(const char *fn);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_KOS_SHIM_H */
