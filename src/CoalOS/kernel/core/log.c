/**
 * @file log.c
 * @brief Kernel logging system implementation - simplified version
 */

#include <kernel/core/log.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>

static log_level_t g_min_level = LOG_DEBUG;
static spinlock_t g_log_lock = {0};

static const char* level_strings[] = {
    "TRACE",
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR",
    "FATAL"
};

void log_init(void) {
    spinlock_init(&g_log_lock);
    g_min_level = LOG_DEBUG;
}

void log_set_level(log_level_t level) {
    if (level >= LOG_TRACE && level <= LOG_FATAL) {
        g_min_level = level;
    }
}

void kvlog(log_level_t level, const char* module, const char* fmt, va_list args) {
    if (level < g_min_level) {
        return;
    }

    uintptr_t flags = spinlock_acquire_irqsave(&g_log_lock);

    // Print to terminal
    terminal_printf("[%s] %s: ", level_strings[level], module);
    terminal_vprintf(fmt, args);
    terminal_write("\n");

    // Also log to serial with proper formatting
    serial_printf("[%s] %s: ", level_strings[level], module);
    serial_vprintf(fmt, args);
    serial_write("\n");

    spinlock_release_irqrestore(&g_log_lock, flags);
}

void klog(log_level_t level, const char* module, const char* fmt, ...) {
    if (level < g_min_level) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    kvlog(level, module, fmt, args);
    va_end(args);
}