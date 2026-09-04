/* kos/oneshot_timer.h — Xbox shim.
 *
 * A deferred one-shot timer: created armed for N milliseconds, and re-armed
 * from the start by _reset. The save path uses one to coalesce EEPROM writes,
 * flushing two seconds after the last one rather than on every write.
 *
 * Backed by a thread waiting on an auto-reset event: a reset signals the
 * event, which restarts the wait; a wait that times out instead fires the
 * callback. That preserves the coalescing behaviour exactly.
 */
#ifndef XBOX_KOS_ONESHOT_TIMER_H
#define XBOX_KOS_ONESHOT_TIMER_H

#include "kos.h"

typedef struct oneshot_timer oneshot_timer_t;

oneshot_timer_t *oneshot_timer_create(void (*callback)(void *), void *data, int ms);
void             oneshot_timer_reset(oneshot_timer_t *t);
void             oneshot_timer_start(oneshot_timer_t *t);
void             oneshot_timer_stop(oneshot_timer_t *t);
void             oneshot_timer_destroy(oneshot_timer_t *t);

#endif
