/**
 * @file oom.h
 * @brief Out-of-memory killer.
 */
#ifndef OOM_H
#define OOM_H

#include <kernel/core/types.h>

#define OOM_RETRIES 200     /* x 10 ms: how long a fault waits for a victim to give its memory back */

/**
 * The current process found no free page. Kills the process using the most memory (never init), or waits
 * for one that is already dying, and gives it 10 ms to exit. True: try the allocation again. False: nothing
 * else can be killed or the caller itself is the biggest user, so it dies (the caller forces SIGKILL).
 */
bool oom_make_room(void);

#endif
