/**
 * @file syscall_misc.c
 * @brief Time, identity, signals and thread-area system calls.
 */

#include "syscall_internal.h"
#include <kernel/cpu/gdt.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/process/scheduler.h>
#include <kernel/process/signal.h>
#include <kernel/lib/string.h>

#define NSEC_PER_SEC 1000000000u
#define TLS_ENTRY    (GDT_TLS_SELECTOR >> 3)
#define LIMIT_IN_PAGES 0x10             /* user_desc.flags bit 4 */
#define KSIGSET_SIZE   8                /* bytes in the kernel sigset_t */

//============================================================================
// Time: wall clock = RTC time at boot + uptime
//============================================================================

static void uptime(uint32_t *sec, uint32_t *msec) {
    uint32_t ms = get_pit_ticks() / (TARGET_FREQUENCY / 1000);
    *sec = ms / 1000;
    *msec = ms % 1000;
}

static void wall_time(uint32_t *sec, uint32_t *nsec) {
    uint32_t s, ms;
    uptime(&s, &ms);
    *sec = rtc_boot_time() + s;
    *nsec = ms * 1000000u;
}

long sc_time(uint32_t tloc) {
    uint32_t s, ns;
    wall_time(&s, &ns);
    if (tloc && copy_to_user((void *)tloc, &s, sizeof(s)) != 0) return -EFAULT;
    return s;
}

long sc_gettimeofday(uint32_t tv, uint32_t tz) {
    uint32_t s, ns;
    (void)tz;                           /* obsolete; the zone is UTC */
    if (!tv) return 0;
    wall_time(&s, &ns);
    struct linux_timeval t = { (int32_t)s, (int32_t)(ns / 1000) };
    return copy_to_user((void *)tv, &t, sizeof(t)) ? -EFAULT : 0;
}

long sc_clock_gettime(uint32_t clk, uint32_t uts) {
    uint32_t s, ns;

    if (clk == CLOCK_REALTIME) {
        wall_time(&s, &ns);
    } else if (clk == CLOCK_MONOTONIC) {
        uptime(&s, &ns);
        ns *= 1000000u;
    } else {
        return -EINVAL;
    }
    struct linux_timespec ts = { (int32_t)s, (int32_t)ns };
    return copy_to_user((void *)uts, &ts, sizeof(ts)) ? -EFAULT : 0;
}

long sc_nanosleep(uint32_t ureq, uint32_t urem) {
    struct linux_timespec req;

    if (copy_from_user(&req, (const void *)ureq, sizeof(req)) != 0) return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || (uint32_t)req.tv_nsec >= NSEC_PER_SEC) return -EINVAL;

    uint32_t ms = (uint32_t)req.tv_sec * 1000u + ((uint32_t)req.tv_nsec + 999999u) / 1000000u;
    sleep_ms(ms);
    if (urem) {
        struct linux_timespec none = {0, 0};
        if (copy_to_user((void *)urem, &none, sizeof(none)) != 0) return -EFAULT;
    }
    return 0;
}

//============================================================================
// Identity
//============================================================================

long sc_uname(uint32_t ubuf) {
    struct linux_utsname u;

    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "Coal");
    strcpy(u.nodename, "coal");
    strcpy(u.release, "0.1.0");
    strcpy(u.version, "#1");
    strcpy(u.machine, "i386");
    return copy_to_user((void *)ubuf, &u, sizeof(u)) ? -EFAULT : 0;
}

long sc_zero(void) { return 0; }        /* uid, gid, euid, egid: single-user system, always root */

//============================================================================
// Thread area and signals (minimal, for static libc start-up)
//============================================================================

/* One TLS descriptor; libc loads GDT_TLS_SELECTOR into %gs. */
long sc_set_thread_area(uint32_t udesc) {
    struct linux_user_desc d;

    if (copy_from_user(&d, (const void *)udesc, sizeof(d)) != 0) return -EFAULT;
    if (d.entry_number != 0xFFFFFFFFu && d.entry_number != TLS_ENTRY) return -EINVAL;

    uint32_t pages = (d.flags & LIMIT_IN_PAGES) ? d.limit : (d.limit + PAGE_SIZE - 1) / PAGE_SIZE;
    gdt_set_tls_base(d.base_addr, pages);
    d.entry_number = TLS_ENTRY;
    return copy_to_user((void *)udesc, &d, sizeof(d)) ? -EFAULT : 0;
}

/* Single-threaded: there is no thread-exit futex to clear, so only the tid is reported. */
long sc_set_tid_address(uint32_t ptr) {
    (void)ptr;
    return get_current_process()->pid;
}

long sc_rt_sigaction(uint32_t sig, uint32_t uact, uint32_t uoact, uint32_t size) {
    pcb_t *p = get_current_process();
    struct linux_sigaction act;

    if (size != KSIGSET_SIZE || sig == 0 || sig >= SIGNAL_MAX) return -EINVAL;
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
        if (copy_from_user(&act, (const void *)uact, sizeof(act)) != 0) return -EFAULT;
    }
    if (uoact) {
        struct linux_sigaction old = { .handler = (uint32_t)(uintptr_t)p->signal_handlers[sig - 1] };
        if (copy_to_user((void *)uoact, &old, sizeof(old)) != 0) return -EFAULT;
    }
    if (uact)
        signal_register_handler(p, sig, (void *)(uintptr_t)act.handler, act.flags);
    return 0;
}

long sc_rt_sigprocmask(uint32_t how, uint32_t uset, uint32_t uoset, uint32_t size) {
    pcb_t *p = get_current_process();
    uint32_t set = 0, old;

    if (size != KSIGSET_SIZE) return -EINVAL;
    if (uset && copy_from_user(&set, (const void *)uset, sizeof(set)) != 0) return -EFAULT;
    if (uset) {
        int r = signal_mask_change(p, how, set, &old);
        if (r) return r;
    } else {
        old = p->signal_mask;
    }
    if (uoset) {
        uint32_t out[2] = { old, 0 };
        if (copy_to_user((void *)uoset, out, sizeof(out)) != 0) return -EFAULT;
    }
    return 0;
}
