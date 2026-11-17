/**
 * @file rlimit.h
 * @brief Resource limits (setrlimit/prlimit64) for the resources Coal enforces.
 *
 * RLIMIT_CPU: SIGXCPU at the soft limit, SIGKILL at the hard limit, counted per process in
 * timer ticks (children start at zero, as on Linux). RLIMIT_AS: mmap and brk fail with ENOMEM
 * above the limit (the stack is not counted). RLIMIT_NPROC: fork fails with EAGAIN; Coal has a
 * single user, so the limit applies to every process in the system. Raising a hard limit is
 * EPERM: nothing in Coal holds CAP_SYS_RESOURCE. Other resources report "unlimited" and refuse
 * to be set (EINVAL).
 *
 * A limit that stopped a process is recorded in its rl_hits mask, merged into the parent when
 * the process exits, and shown in /proc/<pid>/status (RlimitHits).
 */
#ifndef KERNEL_PROCESS_RLIMIT_H
#define KERNEL_PROCESS_RLIMIT_H

#include <kernel/core/types.h>

struct pcb;

#define RLIM_INFINITY 0xFFFFFFFFu

#define RLIMIT_CPU    0
#define RLIMIT_NPROC  6
#define RLIMIT_AS     9
#define RLIMIT_NLIMITS 16

enum { RL_CPU, RL_NPROC, RL_AS, RL_COUNT };        /* index into pcb.rlim */
#define RL_HIT(r) (1u << (r))

struct rlim { uint32_t cur, max; };

/** Resets p's limits to unlimited. */
void rlimit_init(struct pcb *p);
/** Limits are inherited; the CPU clock of a new process starts at zero. */
void rlimit_fork(struct pcb *child, const struct pcb *parent);

/** Linux setrlimit semantics for one resource; 0 or -errno. */
int rlimit_set(struct pcb *p, uint32_t resource, uint32_t cur, uint32_t max);
/** Current limits of one resource (RLIM_INFINITY when not enforced); -EINVAL for a bad resource number. */
int rlimit_get(const struct pcb *p, uint32_t resource, uint32_t *cur, uint32_t *max);

/** Charges CPU time of the running process (from the scheduler) and signals at the limits. */
void rlimit_charge(struct pcb *p, uint64_t delta_ns);

/** Nanoseconds of CPU time left before the next limit acts (call only if cpu_next != RLIMIT_CPU_NEVER). */
uint64_t rlimit_cpu_left_ns(const struct pcb *p);

#define RLIMIT_CPU_NEVER 0xFFFFFFFFu

/** Merges the limit hits of an exiting process into its parent. */
void rlimit_exit(struct pcb *p);

/** fork: false (and the hit is recorded) if the process count would exceed RLIMIT_NPROC. */
bool rlimit_fork_allowed(struct pcb *parent);
/** mmap/brk: false (and the hit is recorded) if growing the address space by grow bytes would exceed RLIMIT_AS. */
bool rlimit_vm_allowed(struct pcb *p, size_t grow);

/** Comma-separated names of the hits in mask ("none" for 0), for /proc. */
size_t rlimit_hits_text(uint32_t mask, char *out, size_t size);

#endif
