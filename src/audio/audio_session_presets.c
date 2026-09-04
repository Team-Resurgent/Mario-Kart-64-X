#ifndef AUDIO_SESSION_PRESETS_H
#define AUDIO_SESSION_PRESETS_H

#include "internal.h"
#include "data.h"
#include "../buffers/audio_heap.h"

struct ReverbSettingsEU sReverbSettings[] = {
    { 0x01, 0x28, 0x4fff },
    { 0x01, 0x14, 0x5fff },
};

/* Pool sizes enlarged for Xbox (64MB): persistent seq/bank pools now hold the
 * whole soundtrack and every instrument bank resident, so the persistent-full
 * soundAlloc failure -> temporary ping-pong fallback path never triggers. */
struct AudioSessionSettingsEU gAudioSessionPresets[] = {
    { 0x000068b0, 0x01, 0x18, 0x01, 0x00, &sReverbSettings[0], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },

    { 0x000068b0, 0x01, 0x14, 0x01, 0x00, &sReverbSettings[0], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },

    { 0x000068b0, 0x01, 0x1c, 0x01, 0x00, &sReverbSettings[0], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },

    { 0x000068b0, 0x01, 0x1c, 0x01, 0x00, &sReverbSettings[0], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },

    { 0x000068b0, 0x01, 0x10, 0x01, 0x00, &sReverbSettings[0], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },

    { 0x000068b0, 0x01, 0x10, 0x01, 0x00, &sReverbSettings[1], 0x7fff, 0x0000, 0x00028000, 0x00018000, 0x00000000,
      0x00010000, 0x00008000, 0x00000000 },
};

s8 gUnusedCount800EA5C8 = 0x1c;
s16 gTatumsPerBeat = TATUMS_PER_BEAT;
s32 gAudioHeapSize = AUDIO_HEAP_SIZE;
s32 gAudioInitPoolSize = AUDIO_HEAP_INIT_SIZE;
s32 D_800EA5D8 = 0;
volatile s32 gAudioLoadLock = 0;

#endif
