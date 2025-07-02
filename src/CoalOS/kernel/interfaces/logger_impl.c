/**
 * @file logger_impl.c
 * @brief Logger interface implementation and dependency injection
 */

#include <kernel/interfaces/logger.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>

// Global logger instance (dependency injection)
logger_interface_t* g_logger = NULL;

// Terminal-based logger implementation
static spinlock_t terminal_logger_lock = {0};
static log_level_t terminal_logger_level = LOG_LEVEL_DEBUG;

static const char* level_strings[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
};

static void terminal_logger_log(log_level_t level, const char* module, const char* fmt, va_list args) {
    if (level < terminal_logger_level) {
        return;
    }

    uintptr_t flags = spinlock_acquire_irqsave(&terminal_logger_lock);

    // Simple format for now - we'll improve this when we have better printf support
    terminal_printf("[%s] %s: %s\n", level_strings[level], module, fmt);
    serial_printf("[%s] %s: %s\n", level_strings[level], module, fmt);

    spinlock_release_irqrestore(&terminal_logger_lock, flags);
}

static void terminal_logger_set_level(log_level_t level) {
    if (level >= LOG_LEVEL_TRACE && level <= LOG_LEVEL_FATAL) {
        terminal_logger_level = level;
    }
}

static log_level_t terminal_logger_get_level(void) {
    return terminal_logger_level;
}

static void terminal_logger_init(void) {
    spinlock_init(&terminal_logger_lock);
    terminal_logger_level = LOG_LEVEL_DEBUG;
}

static void terminal_logger_cleanup(void) {
    // Nothing to cleanup for terminal logger
}

// Terminal logger interface implementation
static logger_interface_t terminal_logger = {
    .log = terminal_logger_log,
    .set_level = terminal_logger_set_level,
    .get_level = terminal_logger_get_level,
    .init = terminal_logger_init,
    .cleanup = terminal_logger_cleanup,
    .name = "terminal_logger"
};

// Serial-only logger implementation for early boot
static void serial_logger_log(log_level_t level, const char* module, const char* fmt, va_list args) {
    // Simple serial-only logging for early boot when terminal isn't ready
    serial_printf("[%s] %s: %s\n", level_strings[level], module, fmt);
}

static void serial_logger_init(void) {
    // Serial is typically initialized very early
}

static void serial_logger_cleanup(void) {
    // Nothing to cleanup for serial logger
}

static logger_interface_t serial_logger = {
    .log = serial_logger_log,
    .set_level = terminal_logger_set_level,
    .get_level = terminal_logger_get_level,
    .init = serial_logger_init,
    .cleanup = serial_logger_cleanup,
    .name = "serial_logger"
};

void logger_set_implementation(logger_interface_t* logger) {
    g_logger = logger;
    if (g_logger && g_logger->init) {
        g_logger->init();
    }
}

// Convenience functions to get specific logger implementations
logger_interface_t* logger_get_terminal_impl(void) {
    return &terminal_logger;
}

logger_interface_t* logger_get_serial_impl(void) {
    return &serial_logger;
}

// Initialize with serial logger for early boot
void logger_init_early(void) {
    logger_set_implementation(&serial_logger);
}

// Switch to terminal logger after terminal is initialized
void logger_init_terminal(void) {
    logger_set_implementation(&terminal_logger);
}