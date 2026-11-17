/**
 * @file oom.h
 * @brief Out-of-memory killer.
 */
#ifndef OOM_H
#define OOM_H

#include <kernel/core/types.h>

#define OOM_RETRIES 30      /* x OOM_WAIT_MS: how long a fault waits for victims to give their memory back */
#define OOM_WAIT_MS 1000

/**
 * The current process found no free page. Kills the process using the most memory (never init), or waits
 * for one that is already dying, until some address space is released (at most OOM_WAIT_MS). True: try the
 * allocation again. False: nothing else can be killed or the caller itself is the biggest user, so it dies
 * (the caller forces SIGKILL).
 */
bool oom_make_room(void);

/** An address space is being torn down; its pages are not free until oom_teardown_end(), which wakes the faults waiting for memory. */
void oom_teardown_begin(void);
void oom_teardown_end(void);

#endif
