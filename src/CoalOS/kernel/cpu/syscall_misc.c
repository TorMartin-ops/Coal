/**
 * @file syscall_misc.c
 * @brief Time, identity, arch_prctl, random, sysinfo, reboot and syslog system calls.
 */

#include "syscall_internal.h"
#include <kernel/cpu/percpu.h>
#include <kernel/memory/buddy.h>
#include <kernel/lib/random.h>
#include <kernel/drivers/timer/clock.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/drivers/timer/rtc.h>
#include <kernel/process/scheduler.h>
#include <kernel/drivers/wait.h>
#include <kernel/lib/port_io.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/core/log.h>

#define NSEC_PER_SEC 1000000000ull
#define SLEEP_CHUNK_S 1000000u          /* 10^9 ms still fits the tick compare */

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

//============================================================================
// Time: see clock.h
//============================================================================

long sc_time(uintptr_t tloc) {
    int64_t s = clock_realtime_ns() / NSEC_PER_SEC;

    if (tloc && copy_to_user((void *)tloc, &s, sizeof(s)) != 0) return -EFAULT;
    return s;
}

long sc_gettimeofday(uintptr_t tv, uintptr_t tz) {
    static const int32_t utc[2];

    if (tz && copy_to_user((void *)tz, utc, sizeof(utc)) != 0) return -EFAULT;      /* the zone is UTC */
    if (!tv) return 0;
    uint64_t ns = clock_realtime_ns();
    struct linux_timeval t = { ns / NSEC_PER_SEC, ns % NSEC_PER_SEC / 1000 };
    return copy_to_user((void *)tv, &t, sizeof(t)) ? -EFAULT : 0;
}

long sc_clock_gettime(uint32_t clk, uintptr_t uts) {
    uint64_t ns;

    if (clk == CLOCK_REALTIME) ns = clock_realtime_ns();
    else if (clk == CLOCK_MONOTONIC) ns = clock_monotonic_ns();
    else return -EINVAL;
    struct linux_timespec ts = { ns / NSEC_PER_SEC, ns % NSEC_PER_SEC };
    return copy_to_user((void *)uts, &ts, sizeof(ts)) ? -EFAULT : 0;
}

long sc_clock_getres(uint32_t clk, uintptr_t uts) {
    struct linux_timespec ts = { 0, 1 };

    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return -EINVAL;
    return uts && copy_to_user((void *)uts, &ts, sizeof(ts)) ? -EFAULT : 0;
}

static drv_waitq_t sleep_queue;

static long sleep_for(struct linux_timespec req, uintptr_t urem) {

    /* Sleep in chunks: the tick counter is 32 bits. */
    uint64_t sec = (uint64_t)req.tv_sec;
    uint32_t ms = ((uint32_t)req.tv_nsec + 999999u) / 1000000u;
    long rc = 0;
    if (!sec && !ms) yield();
    while (!rc && (sec || ms)) {
        uint32_t s = sec < SLEEP_CHUNK_S ? (uint32_t)sec : SLEEP_CHUNK_S;
        uintptr_t flags = local_irq_save();
        rc = drv_wait_timeout(&sleep_queue, s * 1000u + (s == sec ? ms : 0));   /* only a signal ends it early */
        local_irq_restore(flags);
        if (rc) break;                                  /* interrupted: report the whole chunk as left */
        if (s == sec) ms = 0;
        sec -= s;
    }
    if (urem) {
        struct linux_timespec left = { (int64_t)sec, (int64_t)ms * 1000000 };
        if (copy_to_user((void *)urem, &left, sizeof(left)) != 0) return -EFAULT;
    }
    return rc;
}

static long read_timespec(uintptr_t uts, struct linux_timespec *ts) {
    if (copy_from_user(ts, (const void *)uts, sizeof(*ts)) != 0) return -EFAULT;
    return ts->tv_sec < 0 || ts->tv_nsec < 0 || (uint64_t)ts->tv_nsec >= NSEC_PER_SEC ? -EINVAL : 0;
}

long sc_nanosleep(uintptr_t ureq, uintptr_t urem) {
    struct linux_timespec req;
    long rc = read_timespec(ureq, &req);

    return rc ? rc : sleep_for(req, urem);
}

#define TIMER_ABSTIME 1

long sc_clock_nanosleep(uint32_t clk, uint32_t flags, uintptr_t ureq, uintptr_t urem) {
    struct linux_timespec req;
    long rc = read_timespec(ureq, &req);

    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC) return -EINVAL;
    if (rc) return rc;
    if (flags & TIMER_ABSTIME) {
        int64_t now = (int64_t)(clk == CLOCK_REALTIME ? clock_realtime_ns() : clock_monotonic_ns());
        int64_t left = req.tv_sec * (int64_t)NSEC_PER_SEC + req.tv_nsec - now;

        if (left <= 0) return 0;
        req = (struct linux_timespec){ left / (int64_t)NSEC_PER_SEC, left % (int64_t)NSEC_PER_SEC };
        urem = 0;                                       /* an absolute sleep reports no remainder */
    }
    return sleep_for(req, urem);
}

//============================================================================
// Identity
//============================================================================

long sc_uname(uintptr_t ubuf) {
    struct linux_utsname u;

    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "Coal");
    strcpy(u.nodename, "coal");
    strcpy(u.release, COAL_VERSION);
    strcpy(u.version, "#1");
    strcpy(u.machine, "x86_64");
    return copy_to_user((void *)ubuf, &u, sizeof(u)) ? -EFAULT : 0;
}

/* Credentials are kept and reported but not enforced: the file system has no owners or modes. */
#define NOID ((uint32_t)-1)

long sc_getuid(void)  { return get_current_process()->uid[0]; }
long sc_geteuid(void) { return get_current_process()->uid[1]; }
long sc_getgid(void)  { return get_current_process()->gid[0]; }
long sc_getegid(void) { return get_current_process()->gid[1]; }

static bool is_one_of(const uint32_t id[3], uint32_t x) { return x == id[0] || x == id[1] || x == id[2]; }

/* setuid/setgid: root sets all three, others only the effective id to the real or saved one. */
static long set_one(uint32_t id[3], uint32_t x, bool priv) {
    if (priv) id[0] = id[1] = id[2] = x;
    else if (x == id[0] || x == id[2]) id[1] = x;
    else return -EPERM;
    return 0;
}

static long set_re(uint32_t id[3], uint32_t r, uint32_t e, bool priv) {
    uint32_t old_r = id[0];

    if (!priv && ((r != NOID && r != id[0] && r != id[1]) || (e != NOID && !is_one_of(id, e)))) return -EPERM;
    if (r != NOID) id[0] = r;
    if (e != NOID) id[1] = e;
    if (r != NOID || (e != NOID && e != old_r)) id[2] = id[1];
    return 0;
}

static long set_res(uint32_t id[3], uint32_t r, uint32_t e, uint32_t s, bool priv) {
    uint32_t n[3] = {r, e, s};

    for (int i = 0; i < 3 && !priv; i++)
        if (n[i] != NOID && !is_one_of(id, n[i])) return -EPERM;
    for (int i = 0; i < 3; i++)
        if (n[i] != NOID) id[i] = n[i];
    return 0;
}

long sc_setuid(uint32_t u)  { pcb_t *p = get_current_process(); return set_one(p->uid, u, p->uid[1] == 0); }
long sc_setgid(uint32_t g)  { pcb_t *p = get_current_process(); return set_one(p->gid, g, p->uid[1] == 0); }
long sc_setreuid(uint32_t r, uint32_t e) { pcb_t *p = get_current_process(); return set_re(p->uid, r, e, p->uid[1] == 0); }
long sc_setregid(uint32_t r, uint32_t e) { pcb_t *p = get_current_process(); return set_re(p->gid, r, e, p->uid[1] == 0); }
long sc_setresuid(uint32_t r, uint32_t e, uint32_t s) { pcb_t *p = get_current_process(); return set_res(p->uid, r, e, s, p->uid[1] == 0); }
long sc_setresgid(uint32_t r, uint32_t e, uint32_t s) { pcb_t *p = get_current_process(); return set_res(p->gid, r, e, s, p->uid[1] == 0); }

static long get_res(const uint32_t id[3], uintptr_t r, uintptr_t e, uintptr_t s) {
    const uintptr_t dst[3] = {r, e, s};

    for (int i = 0; i < 3; i++)
        if (copy_to_user((void *)dst[i], &id[i], sizeof(uint32_t))) return -EFAULT;
    return 0;
}

long sc_getresuid(uintptr_t r, uintptr_t e, uintptr_t s) { return get_res(get_current_process()->uid, r, e, s); }
long sc_getresgid(uintptr_t r, uintptr_t e, uintptr_t s) { return get_res(get_current_process()->gid, r, e, s); }

//============================================================================
// Thread pointer, thread id, random bytes, memory totals
//============================================================================

long sc_arch_prctl(int code, uintptr_t addr) {
    pcb_t *p = get_current_process();
    uint64_t fs, gs;

    cpu_get_user_bases(&fs, &gs);
    p->fs_base = fs;
    p->gs_base = gs;
    switch (code) {
    case ARCH_SET_FS:
    case ARCH_SET_GS:
        if (addr >= USER_SPACE_MAX) return -EPERM;                  /* only canonical user addresses */
        if (code == ARCH_SET_FS) p->fs_base = addr; else p->gs_base = addr;
        cpu_set_user_bases(p->fs_base, p->gs_base);
        return 0;
    case ARCH_GET_FS:
        return copy_to_user((void *)addr, &p->fs_base, sizeof(p->fs_base)) ? -EFAULT : 0;
    case ARCH_GET_GS:
        return copy_to_user((void *)addr, &p->gs_base, sizeof(p->gs_base)) ? -EFAULT : 0;
    default:
        return -EINVAL;
    }
}

/* Single-threaded: there is no thread-exit futex to clear, so only the tid is reported. */
long sc_set_tid_address(uintptr_t ptr) {
    (void)ptr;
    return get_current_process()->pid;
}

long sc_getrandom(uintptr_t ubuf, size_t len, uint32_t flags) {
    uint8_t kbuf[256];
    size_t done = 0;

    if (flags & ~3u) return -EINVAL;
    if (len > 0x7ffff000) len = 0x7ffff000;
    while (done < len) {
        size_t n = len - done < sizeof(kbuf) ? len - done : sizeof(kbuf);
        random_bytes(kbuf, n);
        if (copy_to_user((char *)ubuf + done, kbuf, n) != 0) return done ? (long)done : -EFAULT;
        done += n;
    }
    return done;
}

long sc_sysinfo(uintptr_t uinfo) {
    struct {
        int64_t uptime;
        uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        uint32_t pad2;
        uint64_t totalhigh, freehigh;
        uint32_t mem_unit;
        char pad3[20 - 2 * sizeof(long) - sizeof(int)];
    } si = { 0 };
    unsigned nproc = 0;

    for (pcb_t *p = process_all(); p; p = p->all_next) nproc++;
    si.uptime = clock_monotonic_ns() / NSEC_PER_SEC;
    si.totalram = (buddy_total_pages(ZONE_LOW) + buddy_total_pages(ZONE_HIGH)) * PAGE_SIZE;
    si.freeram = (buddy_free_pages_count(ZONE_LOW) + buddy_free_pages_count(ZONE_HIGH)) * PAGE_SIZE;
    si.procs = nproc;
    si.mem_unit = 1;
    return copy_to_user((void *)uinfo, &si, sizeof(si)) ? -EFAULT : 0;
}

//============================================================================
// reboot, syslog
//============================================================================

long sc_reboot(uint32_t magic1, uint32_t magic2, uint32_t cmd) {
    (void)magic2;
    if (magic1 != LINUX_REBOOT_MAGIC1) return -EINVAL;

    switch (cmd) {
    case LINUX_REBOOT_CMD_POWER_OFF:
    case LINUX_REBOOT_CMD_HALT:
        vfs_sync();
        outw(0x604, 0x2000);            /* QEMU ACPI PM1a */
        outw(0xB004, 0x2000);           /* Bochs / old QEMU */
        for (;;) __asm__ volatile("cli; hlt");
    case LINUX_REBOOT_CMD_RESTART:
        vfs_sync();
        outb(0x64, 0xFE);               /* pulse the CPU reset line through the keyboard controller */
        for (;;) __asm__ volatile("cli; hlt");
    default:
        return -EINVAL;
    }
}

long sc_syslog(int type, uintptr_t ubuf, size_t len) {
    if (type == SYSLOG_ACTION_SIZE_BUFFER) return KLOG_RING_SIZE;
    if (type == SYSLOG_ACTION_CLEAR) { klog_ring_clear(); return 0; }
    if (type != SYSLOG_ACTION_READ_ALL && type != SYSLOG_ACTION_READ_CLEAR) return -EINVAL;

    size_t size = klog_ring_size();
    if (len < size) size = len;
    char *kbuf = kmalloc(size ? size : 1);
    if (!kbuf) return -ENOMEM;

    long n = klog_ring_copy(kbuf, size);            /* the most recent bytes */
    if (copy_to_user((void *)ubuf, kbuf, n) != 0) n = -EFAULT;
    kfree(kbuf);
    if (n >= 0 && type == SYSLOG_ACTION_READ_CLEAR) klog_ring_clear();
    return n;
}
