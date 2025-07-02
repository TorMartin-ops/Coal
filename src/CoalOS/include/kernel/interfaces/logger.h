/**
 * @file logger.h
 * @brief Abstract logging interface for Coal OS
 * 
 * This interface follows the Dependency Inversion Principle by providing
 * an abstraction that high-level modules can depend on, rather than 
 * depending directly on concrete implementations like terminal.
 */

#ifndef COAL_INTERFACES_LOGGER_H
#define COAL_INTERFACES_LOGGER_H

#include <libc/stdarg.h>
#include <kernel/core/types.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>

/**
 * @brief Log levels for the logging system
 */
typedef enum {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_WARN  = 3,
    LOG_LEVEL_ERROR = 4,
    LOG_LEVEL_FATAL = 5
} log_level_t;

/**
 * @brief Abstract logger interface
 * 
 * This interface can be implemented by different concrete loggers
 * (terminal, serial, file, network, etc.) following the Open/Closed Principle
 */
typedef struct logger_interface {
    /**
     * @brief Log a message with variable arguments
     */
    void (*log)(log_level_t level, const char* module, const char* fmt, va_list args);
    
    /**
     * @brief Set minimum log level
     */
    void (*set_level)(log_level_t level);
    
    /**
     * @brief Get current log level
     */
    log_level_t (*get_level)(void);
    
    /**
     * @brief Initialize the logger
     */
    void (*init)(void);
    
    /**
     * @brief Cleanup logger resources
     */
    void (*cleanup)(void);
    
    /**
     * @brief Logger name/identifier
     */
    const char* name;
    
} logger_interface_t;

/**
 * @brief Global logger instance (dependency injection point)
 */
extern logger_interface_t* g_logger;

/**
 * @brief Set the global logger implementation
 * @param logger Logger implementation to use
 */
void logger_set_implementation(logger_interface_t* logger);

/**
 * @brief Convenience macros that use the injected logger
 */
#define LOGGER_TRACE(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        va_list args; va_start(args, fmt); \
        g_logger->log(LOG_LEVEL_TRACE, module, fmt, args); \
        va_end(args); \
    } } while(0)

#define LOGGER_DEBUG(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        terminal_printf("[DEBUG:%s] " fmt "\n", module, ##__VA_ARGS__); \
        serial_printf("[DEBUG:%s] " fmt "\n", module, ##__VA_ARGS__); \
    } } while(0)

#define LOGGER_INFO(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        terminal_printf("[INFO:%s] " fmt "\n", module, ##__VA_ARGS__); \
        serial_printf("[INFO:%s] " fmt "\n", module, ##__VA_ARGS__); \
    } } while(0)

#define LOGGER_WARN(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        terminal_printf("[WARN:%s] " fmt "\n", module, ##__VA_ARGS__); \
        serial_printf("[WARN:%s] " fmt "\n", module, ##__VA_ARGS__); \
    } } while(0)

#define LOGGER_ERROR(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        terminal_printf("[ERROR:%s] " fmt "\n", module, ##__VA_ARGS__); \
        serial_printf("[ERROR:%s] " fmt "\n", module, ##__VA_ARGS__); \
    } } while(0)

#define LOGGER_FATAL(module, fmt, ...) \
    do { if (g_logger && g_logger->log) { \
        terminal_printf("[FATAL:%s] " fmt "\n", module, ##__VA_ARGS__); \
        serial_printf("[FATAL:%s] " fmt "\n", module, ##__VA_ARGS__); \
    } } while(0)

/**
 * @brief Simple logging function that uses global logger
 */
static inline void klog_simple(log_level_t level, const char* module, const char* message) {
    if (g_logger && g_logger->log) {
        // For simple messages without format args
        va_list empty_args;
        g_logger->log(level, module, message, empty_args);
    }
}

#endif // COAL_INTERFACES_LOGGER_H