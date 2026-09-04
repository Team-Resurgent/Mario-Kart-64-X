/* kos_xbox.c — implementation of the KallistiOS shim declared in kos.h.
 *
 * The interesting part is maple: KOS's peripheral bus is backed by XInput
 * here, which is where controller support for this port actually lives. The
 * rest is thin glue over xapi.
 */

/* This is the one C translation unit that emits the D3DX math helpers. They
 * have external linkage in C (see gfx_xbox.c), so exactly one C file may
 * define them. */

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>   /* O_RDWR, O_CREAT — the modes fs_open translates */

#include "kos.h"
#include "xbox_debug.h"

/* ================================================================= timing == */

static LARGE_INTEGER sFreq;
static int           sClockReady = 0;

static void clock_init(void) {
    if (sClockReady) return;
    QueryPerformanceFrequency(&sFreq);
    if (sFreq.QuadPart == 0) sFreq.QuadPart = 733333333;
    sClockReady = 1;
}

uint64_t timer_us_gettime64(void) {
    LARGE_INTEGER now;
    clock_init();
    QueryPerformanceCounter(&now);
    return (uint64_t) ((now.QuadPart * 1000000ULL) / (uint64_t) sFreq.QuadPart);
}

uint64_t timer_ms_gettime64(void) { return timer_us_gettime64() / 1000ULL; }

uint64_t timer_ns_gettime64(void) {
    LARGE_INTEGER now;
    clock_init();
    QueryPerformanceCounter(&now);
    /* Scale before dividing would overflow at ~2.5 hours on a 733MHz counter,
     * so split the ratio: whole seconds first, then the remainder. */
    const uint64_t f = (uint64_t) sFreq.QuadPart;
    const uint64_t t = (uint64_t) now.QuadPart;
    return (t / f) * 1000000000ULL + ((t % f) * 1000000000ULL) / f;
}

/* ================================================================ threads == */

struct kthread { HANDLE h; };

typedef struct {
    void *(*routine)(void *);
    void  *param;
} thd_trampoline_t;

static DWORD WINAPI thd_trampoline(LPVOID p) {
    thd_trampoline_t *t = (thd_trampoline_t *) p;
    void *(*routine)(void *) = t->routine;
    void *param = t->param;
    free(t);
    routine(param);
    return 0;
}

kthread_t *thd_create_ex(const kthread_attr_t *attr, void *(*routine)(void *), void *param) {
    thd_trampoline_t *t = (thd_trampoline_t *) malloc(sizeof(*t));
    if (!t) return NULL;
    t->routine = routine;
    t->param   = param;

    struct kthread *k = (struct kthread *) malloc(sizeof(*k));
    if (!k) { free(t); return NULL; }

    DWORD stack = attr && attr->stack_size ? (DWORD) attr->stack_size : 32768;
    k->h = CreateThread(NULL, stack, thd_trampoline, t, 0, NULL);
    if (!k->h) { free(t); free(k); return NULL; }

    /* KOS priority is "lower number is higher"; the game only ever asks for a
     * slightly hotter audio thread, so map anything above default to
     * above-normal rather than inventing a full scale. */
    if (attr && attr->prio > 0)
        SetThreadPriority(k->h, THREAD_PRIORITY_ABOVE_NORMAL);

    /* create_detached: nothing joins these, so release our handle and let the
     * thread own its own lifetime. */
    if (attr && attr->create_detached) {
        CloseHandle(k->h);
        k->h = NULL;
    }
    return k;
}

void thd_sleep(int ms) { Sleep((DWORD) ms); }

void thd_pass(void) { Sleep(0); }

void thd_set_hz(int hz) { (void) hz; }

/* ================================================================ mutexes == */

int mutex_init(mutex_t *m, int type) {
    (void) type;   /* Xbox critical sections are always recursive */
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *) malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return -1;
    InitializeCriticalSection(cs);
    m->cs = cs;
    return 0;
}

/* Lazily initialise so a MUTEX_INITIALIZER static still works. Not race-free
 * against simultaneous first use from two threads, but the game's mutexes are
 * all created on the main thread before the audio thread starts. */
static CRITICAL_SECTION *mutex_cs(mutex_t *m) {
    if (!m->cs) mutex_init(m, MUTEX_TYPE_NORMAL);
    return (CRITICAL_SECTION *) m->cs;
}

int mutex_lock(mutex_t *m)   { EnterCriticalSection(mutex_cs(m)); return 0; }
int mutex_unlock(mutex_t *m) { LeaveCriticalSection(mutex_cs(m)); return 0; }

int mutex_destroy(mutex_t *m) {
    if (m->cs) {
        DeleteCriticalSection((CRITICAL_SECTION *) m->cs);
        free(m->cs);
        m->cs = NULL;
    }
    return 0;
}

void shim__mutex_unlock_cleanup(mutex_t **m) { if (m && *m) mutex_unlock(*m); }

/* ================================================================== misc == */

void *arch_get_ret_addr(void) { return __builtin_return_address(0); }

/* Top of usable RAM. 64MB retail; a devkit reports 128MB, and the game only
 * uses this to size its heap, so ask the kernel rather than hardcoding. */
uint8_t *_arch_mem_top = (uint8_t *) (64u * 1024u * 1024u);

void vid_border_color(int r, int g, int b) { (void) r; (void) g; (void) b; }

/* ================================================================== maple ==
 * XInput-backed. Ports are opened once on first use and kept open; XInputGetState
 * refreshes the device state itself, so polling here never blocks. */

#define MAPLE_PORTS 4

struct maple_device {
    int          port;
    HANDLE       h;
    cont_state_t state;
};

static struct maple_device sPads[MAPLE_PORTS];

#if MK64X_DEBUG_TOOLS
/* One-shot frame dump, armed by the WHITE button and cleared by each consumer.
 * Graphics and audio latch separately: they are cleared from different threads
 * and a single shared flag raced between them. */
int xbox_gfx_log_once = 0;
int xbox_snd_log_once = 0;
#endif
#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE
int xbox_gfx_tr_mode = 0;
#endif
#if MK64X_CEREMONY_JUMP
/* One-shot, armed by BOTH TRIGGERS + BACK and consumed by the game loop. */
int xbox_jump_ceremony = 0;
#endif
static int                 sMapleReady = 0;

static void maple_init(void);

/* Called first thing in main, before anything else runs.
 *
 * Xbox peripheral enumeration is ASYNCHRONOUS: XGetDevices reports nothing for
 * a while after XInitDevices. Doing this lazily on the first poll meant the
 * game's boot-time controller check ran before any pad had been enumerated, and
 * it printed "CONNECT A CONTROLLER TO SOCKET 1". Initialising here gives
 * enumeration the whole boot sequence -- seconds of file loading -- to settle
 * before the game asks. */
void xbox_input_init(void) {
    maple_init();
}

static void maple_init(void) {
    if (sMapleReady) return;
    XDEVICE_PREALLOC_TYPE prealloc[] = {
        { XDEVICE_TYPE_GAMEPAD, MAPLE_PORTS },
    };
    XInitDevices(sizeof(prealloc) / sizeof(prealloc[0]), prealloc);
    for (int i = 0; i < MAPLE_PORTS; i++) {
        sPads[i].port = i;
        sPads[i].h    = NULL;
    }
    sMapleReady = 1;
}

maple_device_t *maple_enum_type(int index, uint32_t function) {
    maple_init();

    /* No keyboard support on this port yet. Reporting absence is correct
     * rather than merely convenient: main.c only consults the keyboard to
     * offer a quit shortcut, which a console does not want anyway. */
    if (function != MAPLE_FUNC_CONTROLLER) return NULL;
    if (index < 0 || index >= MAPLE_PORTS)  return NULL;

    DWORD present = XGetDevices(XDEVICE_TYPE_GAMEPAD);
    if (!(present & (1u << index))) {
        if (sPads[index].h) {
            XInputClose(sPads[index].h);
            sPads[index].h = NULL;
        }
        return NULL;
    }

    if (!sPads[index].h) {
        sPads[index].h = XInputOpen(XDEVICE_TYPE_GAMEPAD, (DWORD) index,
                                    XDEVICE_NO_SLOT, NULL);
        if (!sPads[index].h) return NULL;
    }
    return &sPads[index];
}

/* Scale a 16-bit signed thumb axis to the Dreamcast's -128..127. */
static int thumb_to_joy(SHORT v) {
    int j = v >> 8;
    if (j >  127) j =  127;
    if (j < -128) j = -128;
    return j;
}

void *maple_dev_status(maple_device_t *dev) {
    if (!dev || !dev->h) return NULL;

    XINPUT_STATE xs;
    memset(&xs, 0, sizeof(xs));
    if (XInputGetState(dev->h, &xs) != ERROR_SUCCESS) return NULL;

    const XINPUT_GAMEPAD *g = &xs.Gamepad;
    cont_state_t *s = &dev->state;
    memset(s, 0, sizeof(*s));

    /* Xbox face buttons are pressure-sensitive; treat anything past the
     * midpoint as pressed, which is what the digital DC buttons reported. */
    const BYTE TH = 0x40;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_A] > TH) s->buttons |= CONT_A;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_B] > TH) s->buttons |= CONT_B;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_X] > TH) s->buttons |= CONT_X;
    if (g->bAnalogButtons[XINPUT_GAMEPAD_Y] > TH) s->buttons |= CONT_Y;

    if (g->wButtons & XINPUT_GAMEPAD_START)      s->buttons |= CONT_START;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_UP)    s->buttons |= CONT_DPAD_UP;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)  s->buttons |= CONT_DPAD_DOWN;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)  s->buttons |= CONT_DPAD_LEFT;
    if (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) s->buttons |= CONT_DPAD_RIGHT;

#if MK64X_DEBUG_TOOLS
    /* WHITE arms a one-shot dump, on the rising edge only so holding the button
     * does not print every frame -- see xbox_debug.h on why that matters. */
    {
        static int prev_white = 0;
        int white = g->bAnalogButtons[XINPUT_GAMEPAD_WHITE] > TH;
        if (white && !prev_white) { xbox_gfx_log_once = 1; xbox_snd_log_once = 1; }
        prev_white = white;
    }
#endif
#if MK64X_DEBUG_TOOLS || MK64X_TR_MODE_TOGGLE
    /* BLACK cycles the translucent model, rising edge, same reason as WHITE. */
    {
        static int prev_black = 0;
        int black = g->bAnalogButtons[XINPUT_GAMEPAD_BLACK] > TH;
        if (black && !prev_black) {
            xbox_gfx_tr_mode = (xbox_gfx_tr_mode + 1) & 3;
            printf("GFXMODE %d (0=sort,noZUPD 1=submit,ZUPD[N64] "
                   "2=sort,ZUPD 3=submit,noZUPD)\n", xbox_gfx_tr_mode);
        }
        prev_black = black;
    }
#endif

#if MK64X_CEREMONY_JUMP
    /* BOTH TRIGGERS + BACK, rising edge. Deliberately awkward to press, and
     * nothing the game itself binds. */
    {
        static int prev_jump = 0;
        int jump = (g->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER]  > TH) &&
                   (g->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] > TH) &&
                   (g->wButtons & XINPUT_GAMEPAD_BACK);
        if (jump && !prev_jump) { xbox_jump_ceremony = 1; }
        prev_jump = jump;
    }
#endif

    s->ltrig = g->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER];
    s->rtrig = g->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER];

    /* joyx: positive right, same as the thumbstick.
     * joyy: the Dreamcast reports positive DOWN, and main.c inverts it with
     * 0xff - (uint8_t)joyy before scaling. Negating the (positive-up) Xbox
     * axis here reproduces that convention, so the game's mapping code needs
     * no change. */
    s->joyx  =  thumb_to_joy(g->sThumbLX);
    s->joyy  = -thumb_to_joy(g->sThumbLY);
    s->joy2x =  thumb_to_joy(g->sThumbRX);
    s->joy2y = -thumb_to_joy(g->sThumbRY);

    return s;
}

/* =========================================================== file system ==
 * KOS's VFS over Xbox file I/O. The save path is the only consumer; the VMU
 * filename it builds is used as-is under the title's own directory. */

#define FS_MAX_FILES 8

static HANDLE sFiles[FS_MAX_FILES];
static int    sFsReady = 0;

static void fs_init(void) {
    if (sFsReady) return;
    for (int i = 0; i < FS_MAX_FILES; i++) sFiles[i] = INVALID_HANDLE_VALUE;
    sFsReady = 1;
}

file_t fs_open(const char *fn, int mode) {
    fs_init();
    int slot = -1;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (sFiles[i] == INVALID_HANDLE_VALUE) { slot = i; break; }
    if (slot < 0) return -1;

    DWORD access = GENERIC_READ;
    DWORD disp   = OPEN_EXISTING;

    if (mode & O_RDWR)  access = GENERIC_READ | GENERIC_WRITE;
    if (mode & O_WRONLY) access = GENERIC_WRITE;
    if (mode & O_CREAT) disp   = OPEN_ALWAYS;
    if (mode & O_TRUNC) disp   = CREATE_ALWAYS;

    HANDLE h = CreateFile(fn, access, FILE_SHARE_READ, NULL, disp,
                          FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    sFiles[slot] = h;
    return slot;
}

static HANDLE fs_handle(file_t fd) {
    fs_init();
    if (fd < 0 || fd >= FS_MAX_FILES) return INVALID_HANDLE_VALUE;
    return sFiles[fd];
}

int fs_close(file_t fd) {
    HANDLE h = fs_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(h);
    sFiles[fd] = INVALID_HANDLE_VALUE;
    return 0;
}

ssize_t fs_read(file_t fd, void *buf, size_t nbytes) {
    HANDLE h = fs_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD) nbytes, &got, NULL)) return -1;
    return (ssize_t) got;
}

ssize_t fs_write(file_t fd, const void *buf, size_t nbytes) {
    HANDLE h = fs_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD) nbytes, &put, NULL)) return -1;
    return (ssize_t) put;
}

long fs_seek(file_t fd, long offset, int whence) {
    HANDLE h = fs_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD method = (whence == SEEK_CUR) ? FILE_CURRENT
                 : (whence == SEEK_END) ? FILE_END
                                        : FILE_BEGIN;
    DWORD r = SetFilePointer(h, offset, NULL, method);
    return (r == INVALID_SET_FILE_POINTER) ? -1L : (long) r;
}

ssize_t fs_total(file_t fd) {
    HANDLE h = fs_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD sz = GetFileSize(h, NULL);
    return (sz == INVALID_FILE_SIZE) ? -1 : (ssize_t) sz;
}

int fs_unlink(const char *fn) {
    return DeleteFile(fn) ? 0 : -1;
}

/* KOS debug font / console: inert (see kos.h). */
void bfont_draw_ex(void *buffer, int bufwidth, unsigned int fg, unsigned int bg,
                   int bpp, int opaque, unsigned int c, int iskana, int wide) {
    (void) buffer; (void) bufwidth; (void) fg; (void) bg;
    (void) bpp; (void) opaque; (void) c; (void) iskana; (void) wide;
}

void dbgio_enable(void) { }

/* ========================================================= oneshot timer ==
 * See kos/oneshot_timer.h. One thread per timer, waiting on an auto-reset
 * event with a timeout: signalling the event restarts the wait (that is
 * _reset), and a timeout fires the callback. Only the save path uses this,
 * so exactly one timer exists.
 */
#include "kos/oneshot_timer.h"

struct oneshot_timer {
    void  (*callback)(void *);
    void   *data;
    int     ms;
    HANDLE  ev;
    HANDLE  thread;
    volatile LONG running;
};

static DWORD WINAPI oneshot_thread(LPVOID p) {
    oneshot_timer_t *t = (oneshot_timer_t *) p;
    while (t->running) {
        DWORD r = WaitForSingleObject(t->ev, (DWORD) t->ms);
        if (r == WAIT_TIMEOUT && t->running && t->callback)
            t->callback(t->data);
        /* WAIT_OBJECT_0 means _reset was called: fall through and wait the
         * full period again from now. */
    }
    return 0;
}

oneshot_timer_t *oneshot_timer_create(void (*callback)(void *), void *data, int ms) {
    oneshot_timer_t *t = (oneshot_timer_t *) malloc(sizeof(*t));
    if (!t) return NULL;
    t->callback = callback;
    t->data     = data;
    t->ms       = ms;
    t->running  = 1;
    t->ev       = CreateEvent(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    t->thread   = CreateThread(NULL, 16384, oneshot_thread, t, 0, NULL);
    if (!t->ev || !t->thread) { free(t); return NULL; }
    return t;
}

void oneshot_timer_reset(oneshot_timer_t *t)  { if (t && t->ev) SetEvent(t->ev); }
void oneshot_timer_start(oneshot_timer_t *t)  { if (t) t->running = 1; }
void oneshot_timer_stop(oneshot_timer_t *t)   { if (t) { t->running = 0; if (t->ev) SetEvent(t->ev); } }

void oneshot_timer_destroy(oneshot_timer_t *t) {
    if (!t) return;
    oneshot_timer_stop(t);
    if (t->thread) { WaitForSingleObject(t->thread, 1000); CloseHandle(t->thread); }
    if (t->ev) CloseHandle(t->ev);
    free(t);
}

/* =============================================================== genwait ==
 * One channel is enough: the only object waited on is the vblank counter.
 * A manual-reset event is pulsed so every waiter is released together, which
 * is the wake_all semantics the vblank path depends on -- both the main loop
 * and the audio thread sleep here, and waking only one starves the other.
 */
static HANDLE sGenwaitEv = NULL;

static HANDLE genwait_ev(void) {
    if (!sGenwaitEv) sGenwaitEv = CreateEvent(NULL, TRUE, FALSE, NULL);  /* manual reset */
    return sGenwaitEv;
}

int genwait_wait(void *obj, const char *mesg, int timeout, void (*callback)(void *)) {
    (void) obj; (void) mesg; (void) callback;
    HANDLE e = genwait_ev();
    if (!e) return -1;
    /* KOS treats 0 as "wait forever". Cap it anyway: a missed wake here would
     * otherwise hang the frame loop outright rather than merely stutter. */
    DWORD ms = (timeout > 0) ? (DWORD) timeout : 100;
    return (WaitForSingleObject(e, ms) == WAIT_OBJECT_0) ? 0 : -1;
}

void genwait_wake_all(void *obj) {
    (void) obj;
    HANDLE e = genwait_ev();
    if (!e) return;
    SetEvent(e);     /* release everyone waiting... */
    ResetEvent(e);   /* ...then re-arm for the next tick */
}

/* =================================================================== VMU ==
 * See kos.h. The header block is zeroed rather than forged: nothing on Xbox
 * reads it, and the only thing that matters is that the payload lands at the
 * offset the reader seeks to.
 */
#define VMU_HDR_BYTES (512 * 3)

int vmu_pkg_load_icon(vmu_pkg_t *pkg, const char *icon_fn) {
    (void) pkg; (void) icon_fn;
    return 0;   /* no VMU icon on this console */
}

int vmu_pkg_build(vmu_pkg_t *src, uint8_t **dst, ssize_t *dst_size) {
    if (!src || !dst || !dst_size) return -1;
    const size_t total = VMU_HDR_BYTES + (size_t) src->data_len;
    uint8_t *out = (uint8_t *) malloc(total);
    if (!out) { *dst = NULL; *dst_size = 0; return -1; }
    memset(out, 0, VMU_HDR_BYTES);
    if (src->data && src->data_len > 0)
        memcpy(out + VMU_HDR_BYTES, src->data, (size_t) src->data_len);
    *dst = out;
    *dst_size = (ssize_t) total;
    return 0;
}

/* Save path. On Dreamcast this names a file on a specific VMU in a specific
 * port; here the title's own save directory is the equivalent. */
char *get_vmu_fn(maple_device_t *vmudev, char *fn) {
    static char path[64];
    (void) vmudev;
    /* NOTE the escape: "T:\\%s". Written as "T:\%s" this produced "T:mk64.rec"
     * -- a path relative to whatever the current directory on T: happens to
     * be -- because \% is not a valid escape and collapses to a bare %. */
    snprintf(path, sizeof(path), "T:\\%s", fn ? fn : "save.rec");
    return path;
}

/* Resolve the title's save-game container, creating it on first use.
 *
 * This lives here rather than in xbox_savegame.c only because <xtl.h> can be
 * included by exactly ONE C translation unit in this project: it drags in
 * d3dx8math.h, whose helpers are `inline` without `static`, so a second C file
 * including it emits duplicate D3DXVec* symbols at link time. Everything else
 * about saving is plain stdio and lives in xbox_savegame.c.
 *
 * Returns a path ending in a backslash, or NULL if the container could not be
 * created (in which case the game is told there is no save device at all,
 * rather than being allowed to think a save succeeded). */
const char *xbox_save_dir(void) {
    static char dir[MAX_PATH];
    static int  state = 0;          /* 0 untried, 1 ready, -1 failed */

    if (state) {
        return state > 0 ? dir : NULL;
    }

    DWORD rv = XCreateSaveGame("U:\\", L"Mario Kart 64", OPEN_ALWAYS, 0,
                               dir, sizeof(dir));
    if (rv != ERROR_SUCCESS) {
        printf("SAVE XCreateSaveGame failed (%lu) -- saving disabled\n",
               (unsigned long) rv);
        state = -1;
        return NULL;
    }

    size_t n = strlen(dir);
    if (n && dir[n - 1] != '\\' && n + 1 < sizeof(dir)) {
        dir[n] = '\\';
        dir[n + 1] = '\0';
    }

    /* The dashboard reads the thumbnail from inside the container. Missing is
     * harmless -- it just falls back to a generic icon. */
    {
        char img[MAX_PATH];
        snprintf(img, sizeof(img), "%ssaveimage.xbx", dir);
        CopyFile("D:\\dc_data\\saveimage.xbx", img, FALSE);
    }

#if MK64X_DEBUG_TOOLS
    printf("SAVE container: %s\n", dir);
#endif
    state = 1;
    return dir;
}

/* Real 60Hz vblank delivery. On Dreamcast vblfunc is a hardware vblank
 * interrupt; it increments vblticker, advances the game's vblank timers, and
 * genwait_wake_all's BOTH the main loop and the audio thread. The old no-op
 * stub here meant genwait_wake_all was never called, so the audio thread only
 * ever TIMED OUT of its wait (100ms cap): the sequencer ran at ~10Hz instead
 * of 60 and everything played at a sixth of its tempo. A dedicated thread
 * blocking on the hardware vertical blank restores the DC semantics exactly --
 * including ticking during loads, when the render loop isn't presenting. */
typedef void (*xb_vbl_fn)(uint32_t, void *);
static xb_vbl_fn sVblFn   = NULL;
static void     *sVblData = NULL;

static DWORD WINAPI xb_vbl_thread(LPVOID p) {
    (void) p;
    for (;;) {
        D3DDevice_BlockUntilVerticalBlank();
        if (sVblFn) sVblFn(0, sVblData);
    }
    return 0;
}

int vblank_handler_add(void *hnd, void *data) {
    static int started = 0;
    sVblFn   = (xb_vbl_fn) hnd;
    sVblData = data;
    if (!started) {
        started = 1;
        HANDLE h = CreateThread(NULL, 16384, xb_vbl_thread, NULL, 0, NULL);
        if (h) { SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL); CloseHandle(h); }
    }
    return 0;
}
