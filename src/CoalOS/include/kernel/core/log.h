/**
 * @file log.h
 * @brief Kernel logging system for Coal OS
 */

#ifndef COAL_CORE_LOG_H
#define COAL_CORE_LOG_H

#include <libc/stdarg.h>

/**
 * @brief Log levels
 */
typedef enum {
    LOG_TRACE   = 0,
    LOG_DEBUG   = 1,
    LOG_INFO    = 2,
    LOG_WARN    = 3,
    LOG_ERROR   = 4,
    LOG_FATAL   = 5,
} log_level_t;

/**
 * @brief Initialize logging system
 */
void log_init(void);

/**
 * @brief Set minimum log level
 */
void log_set_level(log_level_t level);

/**
 * @brief Core logging function
 */
void klog(log_level_t level, const char* module, const char* fmt, ...);

/**
 * @brief Log with va_list
 */
void kvlog(log_level_t level, const char* module, const char* fmt, va_list args);

/**
 * @brief Convenience macros for logging
 */
#define KLOG_TRACE(module, fmt, ...) klog(LOG_TRACE, module, fmt, ##__VA_ARGS__)
#define KLOG_DEBUG(module, fmt, ...) klog(LOG_DEBUG, module, fmt, ##__VA_ARGS__)
#define KLOG_INFO(module, fmt, ...)  klog(LOG_INFO, module, fmt, ##__VA_ARGS__)
#define KLOG_WARN(module, fmt, ...)  klog(LOG_WARN, module, fmt, ##__VA_ARGS__)
#define KLOG_ERROR(module, fmt, ...) klog(LOG_ERROR, module, fmt, ##__VA_ARGS__)
#define KLOG_FATAL(module, fmt, ...) klog(LOG_FATAL, module, fmt, ##__VA_ARGS__)

/**
 * @brief Module-specific logging macros
 * Usage: Define LOG_MODULE before including this header
 */
#ifdef LOG_MODULE
#define LOG_TRACE(fmt, ...) KLOG_TRACE(LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) KLOG_DEBUG(LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  KLOG_INFO(LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  KLOG_WARN(LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) KLOG_ERROR(LOG_MODULE, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) KLOG_FATAL(LOG_MODULE, fmt, ##__VA_ARGS__)
#endif

/**
 * @brief Assertion with logging
 */
#define KLOG_ASSERT(cond, module, fmt, ...) do { \
    if (!(cond)) { \
        KLOG_FATAL(module, "Assertion failed: " #cond " - " fmt, ##__VA_ARGS__); \
        asm volatile("cli; hlt"); \
    } \
} while(0)

#endif // COAL_CORE_LOG_H