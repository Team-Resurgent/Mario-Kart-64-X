/* Xbox save support for MK64.
 *
 * The game stores two things, on two different N64 devices:
 *
 *   EEPROM         gSaveData -- 512 bytes of time-trial records, Grand Prix
 *                  points, sound mode and checksums. src/save.c addresses it in
 *                  8-byte units, EEPROM_ADDR(ptr) being the byte offset into
 *                  that struct divided by 8, so the whole device maps onto a
 *                  flat 512-byte file at offset address*8.
 *   Controller Pak ghost replay data, through the osPfs* family.
 *
 * Both live in ONE dashboard save container here. Mirroring the N64's two
 * devices would give the player two entries to manage for 512 bytes and a
 * replay, which is worse for them and buys nothing -- there is no separate
 * physical card to move between consoles.
 *
 * The container comes from XCreateSaveGame on U:\, matching the SMW and Zelda
 * ports: OPEN_ALWAYS to create, OPEN_EXISTING to probe without leaving empty
 * folders behind. The dashboard shows it with saveimage.xbx, copied in on
 * creation.
 *
 * Files are opened, used and closed per operation rather than held open. The
 * Dreamcast build deferred its close behind a 2s timer because writing to a VMU
 * is slow and it wanted to batch; here the drive is fast, and a file left open
 * is exactly what gets truncated when someone pulls the power on a console.
 */
/* Deliberately NOT including <xtl.h>: it drags in d3dx8math.h, whose helpers
 * are `inline` without `static`, so a second C translation unit including it
 * emits duplicate D3DXVec* symbols. The one Xbox API needed here --
 * XCreateSaveGame -- is wrapped by xbox_save_dir() in kos_xbox.c, and the rest
 * of this file is portable stdio. */
#include <stdio.h>
#include <string.h>

/* Include order matters and is the same one src/os/ultra_reimpl.c uses, whose
 * declarations these replace. PR/os.h holds OSPfs / OSPfsState / PFS_* /
 * EEPROM_TYPE_4K but references OSIntMask, OSContStatus, OSPageMask and others
 * without including their headers, so ultra64.h has to land first; save.h then
 * pulls os.h in with everything it needs already defined. */
#include <PR/ultratypes.h>
#include <PR/os_message.h>
#include <PR/os_pi.h>
#include <PR/os_vi.h>
#include <PR/os_time.h>
#include <PR/libultra.h>
#include <ultra64.h>
#include <macros.h>           /* UNUSED */
#include "save.h"             /* -> PR/os.h */
#include "save_data.h"
#include "xbox_debug.h"

#define EEPROM_FILE     "mk64.eep"
#define GHOST_FILE      "mk64.gho"
#define EEPROM_BYTES    512              /* sizeof(SaveData); EEPROM_TYPE_4K */
#define GHOST_BYTES     32768            /* what the DC build reserved */
#define SAVE_PATH_MAX   260

extern SaveData gSaveData;
extern s32 D_800DC5AC;
void func_800B46D0(void);

/* kos_xbox.c: the save container's directory, ending in a backslash, or NULL
 * if it could not be created. */
extern const char *xbox_save_dir(void);

static int save_dir_ready(void) {
    return xbox_save_dir() != NULL;
}

static int save_path(const char *name, char *out, size_t n) {
    const char *dir = xbox_save_dir();
    if (!dir) {
        return 0;
    }
    snprintf(out, n, "%s%s", dir, name);
    return 1;
}

/* Open `name`, creating it zero-filled at `size` bytes if absent. Mode is a
 * stdio mode string. Returns NULL on failure. */
static FILE *save_open(const char *name, size_t size, const char *mode) {
    char path[SAVE_PATH_MAX];
    FILE *f;

    if (!save_path(name, path, sizeof(path))) {
        return NULL;
    }

    f = fopen(path, "r+b");
    if (!f) {
        /* First run: lay the file down at full size so every later read and
         * write can seek anywhere inside it without special cases. */
        f = fopen(path, "w+b");
        if (!f) {
            printf("SAVE cannot create %s\n", path);
            return NULL;
        }
        {
            static const unsigned char zero[256] = { 0 };
            size_t left = size;
            while (left) {
                size_t chunk = left < sizeof(zero) ? left : sizeof(zero);
                if (fwrite(zero, 1, chunk, f) != chunk) {
                    break;
                }
                left -= chunk;
            }
            fflush(f);
        }
#if MK64X_DEBUG_TOOLS
        printf("SAVE created %s (%u bytes)\n", path, (unsigned) size);
#endif
        if (mode) { /* keep the handle; caller reopens semantics not needed */ }
        return f;
    }
    (void) mode;
    return f;
}

/* --------------------------------------------------------------- EEPROM --- */

s32 osEepromProbe(UNUSED OSMesgQueue *mq) {
    char path[SAVE_PATH_MAX];
    int fresh;
    FILE *f;

    if (!save_path(EEPROM_FILE, path, sizeof(path))) {
        return 0;   /* no container -> tell the game there is no save device */
    }

    f = fopen(path, "rb");
    fresh = (f == NULL);
    if (f) {
        fclose(f);
    }

    f = save_open(EEPROM_FILE, EEPROM_BYTES, "r+b");
    if (!f) {
        return 0;
    }
    fclose(f);

    if (fresh) {
        /* Brand new card. The Dreamcast build does the same two calls here:
         * seed the defaults the game normally builds in menus.c, so the first
         * boot starts from initialised records rather than zeroes. */
        func_800B46D0();
        D_800DC5AC = 0;
    }
    return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(UNUSED OSMesgQueue *mq, unsigned char address, unsigned char *buffer,
                     s32 length) {
    FILE *f;
    size_t got;
    long off = (long) address * 8;   /* addresses are in 8-byte units */

    if (length <= 0 || off < 0 || off + length > EEPROM_BYTES) {
        return 1;
    }
    f = save_open(EEPROM_FILE, EEPROM_BYTES, "rb");
    if (!f) {
        return 1;
    }
    if (fseek(f, off, SEEK_SET) != 0) {
        fclose(f);
        return 1;
    }
    got = fread(buffer, 1, (size_t) length, f);
    fclose(f);
    return (got == (size_t) length) ? 0 : 1;
}

s32 osEepromRead(OSMesgQueue *mq, u8 address, u8 *buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromLongWrite(UNUSED OSMesgQueue *mq, unsigned char address, unsigned char *buffer,
                      s32 length) {
    FILE *f;
    size_t put;
    long off = (long) address * 8;

    if (length <= 0 || off < 0 || off + length > EEPROM_BYTES) {
        return 1;
    }
    f = save_open(EEPROM_FILE, EEPROM_BYTES, "r+b");
    if (!f) {
        return 1;
    }
    if (fseek(f, off, SEEK_SET) != 0) {
        fclose(f);
        return 1;
    }
    put = fwrite(buffer, 1, (size_t) length, f);
    fflush(f);
    fclose(f);   /* closed immediately: a half-written open file is what dies
                  * when the console loses power mid-race */
    return (put == (size_t) length) ? 0 : 1;
}

s32 osEepromWrite(OSMesgQueue *mq, unsigned char address, unsigned char *buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

/* -------------------------------------------------- Controller Pak (ghosts) -
 * The game treats the pak as a tiny filesystem, but only ever stores one file.
 * So the whole family is backed by a single fixed-size file, and file_no is
 * always 0 -- enough for every call MK64 actually makes.
 */

static OSPfsState sGhostState;
static int        sGhostAllocated = 0;

s32 osPfsIsPlug(UNUSED OSMesgQueue *queue, u8 *pattern) {
    *pattern = save_dir_ready() ? 1 : 0;
    return 0;
}

s32 osPfsInit(UNUSED OSMesgQueue *queue, OSPfs *pfs, int channel) {
    if (channel != 0 || !save_dir_ready()) {
        return PFS_NO_PAK_INSERTED;
    }
    pfs->queue   = queue;
    pfs->channel = channel;
    pfs->status  = PFS_INITIALIZED;
    return PFS_NO_ERROR;
}

s32 osPfsNumFiles(UNUSED OSPfs *pfs, s32 *max_files, s32 *files_used) {
    *max_files  = 1;
    *files_used = sGhostAllocated ? 1 : 0;
    return PFS_NO_ERROR;
}

s32 osPfsFileState(UNUSED OSPfs *pfs, UNUSED s32 file_no, OSPfsState *state) {
    if (state) {
        *state = sGhostState;
    }
    return PFS_NO_ERROR;
}

s32 osPfsFreeBlocks(UNUSED OSPfs *pfs, s32 *bytes_not_used) {
    /* Plenty: this is a hard drive. The value only gates the game's "not
     * enough space" message. */
    *bytes_not_used = GHOST_BYTES * 4;
    return PFS_NO_ERROR;
}

s32 osPfsAllocateFile(UNUSED OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name,
                      u8 *ext_name, UNUSED int file_size_in_bytes, s32 *file_no) {
    FILE *f = save_open(GHOST_FILE, GHOST_BYTES, "r+b");
    if (!f) {
        return PFS_NO_PAK_INSERTED;
    }
    fclose(f);

    memset(&sGhostState, 0, sizeof(sGhostState));
    sGhostState.company_code = company_code;
    sGhostState.game_code    = game_code;
    if (game_name) {
        strncpy(sGhostState.game_name, (const char *) game_name, sizeof(sGhostState.game_name) - 1);
    }
    if (ext_name) {
        strncpy(sGhostState.ext_name, (const char *) ext_name, sizeof(sGhostState.ext_name) - 1);
    }
    sGhostAllocated = 1;
    *file_no = 0;
    return PFS_NO_ERROR;
}

s32 osPfsFindFile(UNUSED OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name,
                  u8 *ext_name, s32 *file_no) {
    char path[SAVE_PATH_MAX];
    FILE *f;

    if (!save_path(GHOST_FILE, path, sizeof(path))) {
        return PFS_NO_PAK_INSERTED;
    }
    f = fopen(path, "rb");
    if (!f) {
        return PFS_ERR_INVALID;   /* no ghost saved yet */
    }
    fclose(f);

    /* Remember the identity so a later osPfsFileState answers consistently;
     * with one file there is nothing to match against. */
    sGhostState.company_code = company_code;
    sGhostState.game_code    = game_code;
    if (game_name) {
        strncpy(sGhostState.game_name, (const char *) game_name, sizeof(sGhostState.game_name) - 1);
    }
    if (ext_name) {
        strncpy(sGhostState.ext_name, (const char *) ext_name, sizeof(sGhostState.ext_name) - 1);
    }
    sGhostAllocated = 1;
    *file_no = 0;
    return PFS_NO_ERROR;
}

s32 osPfsReadWriteFile(UNUSED OSPfs *pfs, UNUSED s32 file_no, u8 flag, int offset,
                       int size_in_bytes, u8 *data_buffer) {
    FILE *f;
    size_t done;

    if (size_in_bytes <= 0 || offset < 0 || offset + size_in_bytes > GHOST_BYTES) {
        return PFS_ERR_INVALID;
    }
    f = save_open(GHOST_FILE, GHOST_BYTES, (flag == PFS_READ) ? "rb" : "r+b");
    if (!f) {
        return PFS_ERR_NOPACK;
    }
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return PFS_ERR_INVALID;
    }
    if (flag == PFS_READ) {
        done = fread(data_buffer, 1, (size_t) size_in_bytes, f);
        fclose(f);
        return (done == (size_t) size_in_bytes) ? PFS_NO_ERROR : PFS_ERR_BAD_DATA;
    }
    done = fwrite(data_buffer, 1, (size_t) size_in_bytes, f);
    fflush(f);
    fclose(f);
    return (done == (size_t) size_in_bytes) ? PFS_NO_ERROR : PFS_CORRUPTED;
}

s32 osPfsDeleteFile(UNUSED OSPfs *pfs, UNUSED u16 company_code, UNUSED u32 game_code,
                    UNUSED u8 *game_name, UNUSED u8 *ext_name) {
    char path[SAVE_PATH_MAX];

    if (!save_path(GHOST_FILE, path, sizeof(path))) {
        return PFS_ERR_NOPACK;
    }
    sGhostAllocated = 0;
    memset(&sGhostState, 0, sizeof(sGhostState));
    return (remove(path) == 0) ? PFS_NO_ERROR : PFS_ERR_INVALID;
}

void __osPfsCloseAllFiles(void) {
    /* Nothing is held open. */
}
