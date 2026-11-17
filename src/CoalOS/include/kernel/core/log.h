/**
 * @file log.h
 * @brief Kernel logging: the single output path for kernel messages.
 *
 * Every message goes to the screen, COM1 and a ring buffer that dmesg reads
 * through syslog(2). Levels below KLOG_LEVEL are compiled out; the default is
 * INFO. LOG_* macros need LOG_MODULE defined before this header is included.
 */
#ifndef COAL_CORE_LOG_H
#define COAL_CORE_LOG_H

#include <libc/stddef.h>

enum { LVL_TRACE, LVL_DEBUG, LVL_INFO, LVL_WARN, LVL_ERROR, LVL_FATAL };

#ifndef KLOG_LEVEL
#define KLOG_LEVEL LVL_INFO
#endif

#define KLOG_RING_SIZE 16384

void klog(int level, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Untagged message (boot banner, panic reports). */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** Bytes currently held in the ring, copy of the most recent len bytes, discard all. */
/** From now on INFO and below go only to COM1 and the log ring, not the screen. */
void klog_console_quiet(void);

/** One more destination for every log line (virtio-console): called with interrupts off, must not log. */
void klog_set_sink(void (*sink)(const char *s, size_t n));

size_t klog_ring_size(void);
size_t klog_ring_copy(char *buf, size_t len);
void   klog_ring_clear(void);

#define KLOG(level, module, ...) \
    do { if ((level) >= KLOG_LEVEL) klog((level), (module), __VA_ARGS__); } while (0)

#define KLOG_TRACE(module, ...) KLOG(LVL_TRACE, module, __VA_ARGS__)
#define KLOG_DEBUG(module, ...) KLOG(LVL_DEBUG, module, __VA_ARGS__)
#define KLOG_INFO(module, ...)  KLOG(LVL_INFO,  module, __VA_ARGS__)
#define KLOG_WARN(module, ...)  KLOG(LVL_WARN,  module, __VA_ARGS__)
#define KLOG_ERROR(module, ...) KLOG(LVL_ERROR, module, __VA_ARGS__)
#define KLOG_FATAL(module, ...) KLOG(LVL_FATAL, module, __VA_ARGS__)

#ifdef LOG_MODULE
#define LOG_TRACE(...) KLOG_TRACE(LOG_MODULE, __VA_ARGS__)
#define LOG_DEBUG(...) KLOG_DEBUG(LOG_MODULE, __VA_ARGS__)
#define LOG_INFO(...)  KLOG_INFO(LOG_MODULE, __VA_ARGS__)
#define LOG_WARN(...)  KLOG_WARN(LOG_MODULE, __VA_ARGS__)
#define LOG_ERROR(...) KLOG_ERROR(LOG_MODULE, __VA_ARGS__)
#define LOG_FATAL(...) KLOG_FATAL(LOG_MODULE, __VA_ARGS__)
#endif

#endif
