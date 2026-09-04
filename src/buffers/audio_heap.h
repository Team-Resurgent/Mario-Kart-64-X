#ifndef AUDIO_HEAP_H
#define AUDIO_HEAP_H

#include <PR/ultratypes.h>
#include <mk64.h>

/* N64 sizing was 0x48C00 for a 4MB console. With 64MB, give the audio
 * session room to keep every sequence and bank resident (see the enlarged
 * gAudioSessionPresets pools) so nothing evicts or falls back mid-race. */
#define AUDIO_HEAP_SIZE 0x100000
#define AUDIO_HEAP_INIT_SIZE 0x2600

#endif // AUDIO_HEAP_H
