/**
 * @file rlimit.c
 * @brief Resource limits: CPU time, address space size, process count (see rlimit.h).
 */
#include <kernel/process/rlimit.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/memory/mm.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>
#include <kernel/lib/div64.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/drivers/timer/pit.h>

#define NEVER      RLIMIT_CPU_NEVER

static int slot(uint32_t resource) {
    switch (resource) {
    case RLIMIT_CPU:   return RL_CPU;
    case RLIMIT_NPROC: return RL_NPROC;
    case RLIMIT_AS:    return RL_AS;
    default:           return -1;
    }
}

static uint32_t limit_ms(uint32_t seconds) {
    return seconds == RLIM_INFINITY || seconds > NEVER / 1000 ? NEVER : seconds * 1000;
}

/* CPU milliseconds at which rlimit_charge() has to act next: the soft limit, then the hard limit. */
static uint32_t cpu_deadline(const pcb_t *p, uint32_t used_ms) {
    uint32_t soft = limit_ms(p->rlim[RL_CPU].cur), hard = limit_ms(p->rlim[RL_CPU].max);
    return used_ms < soft ? soft : hard;
}

void rlimit_init(pcb_t *p) {
    for (int i = 0; i < RL_COUNT; i++) p->rlim[i] = (struct rlim){ RLIM_INFINITY, RLIM_INFINITY };
    p->cpu_ms = p->cpu_ns_rem = 0;
    p->cpu_next = NEVER;
}

void rlimit_fork(pcb_t *child, const pcb_t *parent) {
    memcpy(child->rlim, parent->rlim, sizeof(child->rlim));
    child->cpu_ms = child->cpu_ns_rem = 0;
    child->cpu_next = cpu_deadline(child, 0);
}

int rlimit_set(pcb_t *p, uint32_t resource, uint32_t cur, uint32_t max) {
    int i = slot(resource);
    if (resource >= RLIMIT_NLIMITS || i < 0) return -EINVAL;
    if (cur > max) return -EINVAL;
    if (max > p->rlim[i].max) return -EPERM;           /* no privileges: limits only go down */
    p->rlim[i] = (struct rlim){ cur, max };
    if (i == RL_CPU) {
        p->cpu_next = cpu_deadline(p, p->cpu_ms);
        if (p->cpu_next != NEVER) clockevent_request(clock_ns() + rlimit_cpu_left_ns(p));
    }
    return 0;
}

int rlimit_get(const pcb_t *p, uint32_t resource, uint32_t *cur, uint32_t *max) {
    if (resource >= RLIMIT_NLIMITS) return -EINVAL;
    int i = slot(resource);
    *cur = i < 0 ? RLIM_INFINITY : p->rlim[i].cur;
    *max = i < 0 ? RLIM_INFINITY : p->rlim[i].max;
    return 0;
}

/* Charges delta_ns of CPU time to p and acts when a limit is reached. */
void rlimit_charge(pcb_t *p, uint64_t delta_ns) {
    uint64_t total = p->cpu_ns_rem + delta_ns;

    p->cpu_ms += (uint32_t)div64_32(total, 1000000);
    p->cpu_ns_rem = (uint32_t)(total - div64_32(total, 1000000) * 1000000);
    if (p->cpu_ms < p->cpu_next) return;
    uint32_t hard = limit_ms(p->rlim[RL_CPU].max);
    p->rl_hits |= RL_HIT(RL_CPU);
    if (p->cpu_ms >= hard) {
        signal_send(p, SIGKILL);
        p->cpu_next = NEVER;
    } else {
        signal_send(p, SIGXCPU);                     /* repeats every second until the hard limit */
        p->cpu_next = p->cpu_ms + 1000 < hard ? p->cpu_ms + 1000 : hard;
    }
}

/* Time the process can still run before rlimit_charge() has to act. */
uint64_t rlimit_cpu_left_ns(const pcb_t *p) {
    uint64_t left = p->cpu_next > p->cpu_ms ? (uint64_t)(p->cpu_next - p->cpu_ms) * 1000000 : 0;

    return left > p->cpu_ns_rem ? left - p->cpu_ns_rem : 0;
}

void rlimit_exit(pcb_t *p) {
    if (p->parent) p->parent->rl_hits |= p->rl_hits;
}

bool rlimit_fork_allowed(pcb_t *parent) {
    uint32_t limit = parent->rlim[RL_NPROC].cur;
    if (limit == RLIM_INFINITY) return true;
    uint32_t n = 0;
    for (pcb_t *p = process_all(); p; p = p->all_next)
        if (p->pid) n++;
    if (n < limit) return true;
    parent->rl_hits |= RL_HIT(RL_NPROC);
    return false;
}

bool rlimit_vm_allowed(pcb_t *p, size_t grow) {
    uint32_t limit = p->rlim[RL_AS].cur;
    if (limit == RLIM_INFINITY || !p->mm) return true;
    size_t total = mm_total_vm(p->mm);
    if (total <= limit && grow <= limit - total) return true;
    p->rl_hits |= RL_HIT(RL_AS);
    return false;
}

size_t rlimit_hits_text(uint32_t mask, char *out, size_t size) {
    static const char *const names[RL_COUNT] = { "cpu", "nproc", "as" };
    size_t n = 0;
    for (int i = 0; i < RL_COUNT; i++) {
        if (!(mask & RL_HIT(i))) continue;
        size_t len = strlen(names[i]);
        if (n + len + 2 > size) break;
        if (n) out[n++] = ',';
        memcpy(out + n, names[i], len);
        n += len;
    }
    if (!n && size > 4) { memcpy(out, "none", 4); n = 4; }
    if (n < size) out[n] = 0;
    return n;
}
