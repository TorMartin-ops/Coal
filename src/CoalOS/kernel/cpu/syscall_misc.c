/**
 * @file syscall_misc.c
 * @brief Time, identity, thread area, reboot and syslog system calls.
 */

#include "syscall_internal.h"
#include <kernel/cpu/gdt.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/process/scheduler.h>
#include <kernel/drivers/wait.h>
#include <kernel/lib/port_io.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/core/log.h>

#define NSEC_PER_SEC 1000000000u
#define SLEEP_CHUNK_S 1000000u          /* 10^9 ms still fits the tick compare */
#define TLS_ENTRY    (GDT_TLS_SELECTOR >> 3)
#define LIMIT_IN_PAGES 0x10             /* user_desc.flags bit 4 */

//============================================================================
// Time: wall clock = RTC time at boot + uptime
//============================================================================

/* CMOS real-time clock (UTC) as Unix time; read once, then kept as an offset from uptime. */
static uint32_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint32_t rtc_unix_time(void) {
    uint32_t sec, min, hour, day, mon, year, b;

    while (cmos(0x0A) & 0x80) ;                     /* update in progress */
    sec = cmos(0); min = cmos(2); hour = cmos(4);
    day = cmos(7); mon = cmos(8); year = cmos(9);
    b = cmos(0x0B);
    if (!(b & 4)) {                                 /* BCD */
        #define BCD(x) (((x) & 15) + ((x) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        hour = BCD(hour & 0x7F) | (hour & 0x80);
    }
    if (!(b & 2) && (hour & 0x80)) hour = ((hour & 0x7F) % 12) + 12;   /* 12-hour clock, PM */
    year += 2000;

    static const uint16_t days_before[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t days = 365 * (year - 1970) + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400
                  + days_before[(mon - 1) % 12] + day - 1;
    if (mon > 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) days++;
    return ((days * 24 + hour) * 60 + min) * 60 + sec;
}

static uint32_t rtc_boot_time(void) {
    static uint32_t boot;
    if (!boot) boot = rtc_unix_time() - (uint32_t)timer_uptime_ms() / 1000;
    return boot;
}

static void uptime(uint32_t *sec, uint32_t *msec) {
    uint32_t ms = (uint32_t)timer_uptime_ms();
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

static drv_waitq_t sleep_queue;

long sc_nanosleep(uint32_t ureq, uint32_t urem) {
    struct linux_timespec req;

    if (copy_from_user(&req, (const void *)ureq, sizeof(req)) != 0) return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || (uint32_t)req.tv_nsec >= NSEC_PER_SEC) return -EINVAL;

    /* Sleep in chunks: the tick counter is 32 bits. */
    uint32_t sec = (uint32_t)req.tv_sec, ms = ((uint32_t)req.tv_nsec + 999999u) / 1000000u;
    long rc = 0;
    if (!sec && !ms) yield();
    while (!rc && (sec || ms)) {
        uint32_t s = sec < SLEEP_CHUNK_S ? sec : SLEEP_CHUNK_S;
        uintptr_t flags = local_irq_save();
        rc = drv_wait_timeout(&sleep_queue, s * 1000u + (s == sec ? ms : 0));   /* only a signal ends it early */
        local_irq_restore(flags);
        if (!rc && s == sec) ms = 0;
        sec -= s;
    }
    if (urem) {
        struct linux_timespec left = { (int32_t)sec, (int32_t)ms * 1000000 };
        if (copy_to_user((void *)urem, &left, sizeof(left)) != 0) return -EFAULT;
    }
    return rc;
}

//============================================================================
// Identity
//============================================================================

long sc_uname(uint32_t ubuf) {
    struct linux_utsname u;

    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "Coal");
    strcpy(u.nodename, "coal");
    strcpy(u.release, COAL_VERSION);
    strcpy(u.version, "#1");
    strcpy(u.machine, "i386");
    return copy_to_user((void *)ubuf, &u, sizeof(u)) ? -EFAULT : 0;
}

long sc_zero(void) { return 0; }        /* uid, gid, euid, egid: single-user system, always root */

//============================================================================
// Thread area (minimal, for static libc start-up)
//============================================================================

/* One TLS descriptor per process (saved in the PCB, loaded on every switch); libc loads GDT_TLS_SELECTOR into %gs. */
long sc_set_thread_area(uint32_t udesc) {
    struct linux_user_desc d;

    if (copy_from_user(&d, (const void *)udesc, sizeof(d)) != 0) return -EFAULT;
    if (d.entry_number != 0xFFFFFFFFu && d.entry_number != TLS_ENTRY) return -EINVAL;

    uint32_t pages = (d.flags & LIMIT_IN_PAGES) ? d.limit : (d.limit + PAGE_SIZE - 1) / PAGE_SIZE;
    get_current_process()->tls_base = d.base_addr;
    get_current_process()->tls_pages = pages;
    gdt_set_tls_base(d.base_addr, pages);
    d.entry_number = TLS_ENTRY;
    return copy_to_user((void *)udesc, &d, sizeof(d)) ? -EFAULT : 0;
}

/* Single-threaded: there is no thread-exit futex to clear, so only the tid is reported. */
long sc_set_tid_address(uint32_t ptr) {
    (void)ptr;
    return get_current_process()->pid;
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

long sc_syslog(uint32_t type, uint32_t ubuf, uint32_t len) {
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
