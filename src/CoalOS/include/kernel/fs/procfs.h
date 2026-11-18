/**
 * @file procfs.h
 * @brief Minimal read-only /proc.
 *
 * Files: meminfo, uptime, version, self (= the calling process), <pid>/status
 * and <pid>/cmdline.
 *
 * procfs reads process data through the two accessors below. The process
 * layer may provide its own definitions; procfs.c carries weak fallbacks that
 * only know the calling process.
 */
#ifndef PROCFS_H
#define PROCFS_H

#include <kernel/core/types.h>

typedef struct proc_info {
    uint32_t pid;
    uint32_t ppid, pgid, sid;
    uint32_t uid[3], gid[3];        /* real, effective, saved */
    char     state;                 /* Linux letter: R running, S sleeping, Z zombie */
    char     name[16];
    char     cmdline[128];          /* arguments separated by NUL bytes */
    uint32_t cmdline_len;           /* bytes used in cmdline */
    uint32_t rl_hits;               /* resource limits that stopped it or a child (RL_HIT mask) */
} proc_info_t;

/** Fill `out` for `pid`. Returns 0 or -ESRCH. */
int proc_info_get(uint32_t pid, proc_info_t *out);
/** Store up to `max` live pids in `pids`; returns how many were stored. */
int proc_info_list(uint32_t *pids, int max);

/** Register the driver (mount it at /proc). */
int procfs_init(void);

#endif
