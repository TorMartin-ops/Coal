/**
 * @file log.c
 * @brief Kernel log: formats a message once and sends it to the screen, COM1
 *        and the ring buffer behind dmesg.
 */
#include <kernel/core/log.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/drivers/timer/pit.h>
#include <kernel/sync/spinlock.h>
#include <libc/stdarg.h>

#define LINE_MAX_LEN 256

static char     g_ring[KLOG_RING_SIZE];
static uint32_t g_total;                 /* bytes ever written */
static uint32_t g_cleared;               /* value of g_total at the last clear */
static spinlock_t g_lock;

static const char *const level_names[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL" };

/* s must be NUL terminated at s[n]. */
static int _snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static void emit(const char *s, size_t n) {
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);

    for (size_t i = 0; i < n; i++) g_ring[g_total++ % KLOG_RING_SIZE] = s[i];
    terminal_write_bytes(s, n);
    serial_write(s);
    spinlock_release_irqrestore(&g_lock, flags);
}

static void emit_va(char *buf, size_t used, const char *fmt, va_list ap) {
    int n = _vsnprintf(buf + used, LINE_MAX_LEN - used - 2, fmt, ap);

    if (n < 0) return;
    used += (size_t)n;
    if (used == 0 || buf[used - 1] != '\n') buf[used++] = '\n';
    buf[used] = '\0';
    emit(buf, used);
}

void klog(int level, const char *module, const char *fmt, ...) {
    char buf[LINE_MAX_LEN];
    va_list ap;
    size_t head;
    uint32_t ms = (uint32_t)timer_uptime_ms();
    const char *parts[] = { level_names[level], "] ", module, ": " };

    head = (size_t)_snprintf(buf, sizeof(buf) / 2, "[%5u.%03u] [", ms / 1000, ms % 1000);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
        for (const char *p = parts[i]; *p && head < sizeof(buf) / 2; p++) buf[head++] = *p;

    va_start(ap, fmt);
    emit_va(buf, head, fmt, ap);
    va_end(ap);
}

void kprintf(const char *fmt, ...) {
    char buf[LINE_MAX_LEN];
    va_list ap;

    va_start(ap, fmt);
    emit_va(buf, 0, fmt, ap);
    va_end(ap);
}

size_t klog_ring_size(void) {
    uint32_t n = g_total - g_cleared;
    return n < KLOG_RING_SIZE ? n : KLOG_RING_SIZE;
}

size_t klog_ring_copy(char *buf, size_t len) {
    uintptr_t flags = spinlock_acquire_irqsave(&g_lock);
    size_t n = klog_ring_size();

    if (len < n) n = len;
    for (size_t i = 0; i < n; i++) buf[i] = g_ring[(g_total - n + i) % KLOG_RING_SIZE];
    spinlock_release_irqrestore(&g_lock, flags);
    return n;
}

void klog_ring_clear(void) {
    g_cleared = g_total;
}
