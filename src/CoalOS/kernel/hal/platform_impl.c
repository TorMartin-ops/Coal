/**
 * @file platform_impl.c
 * @brief Platform abstraction implementation with dependency injection
 */

#define LOG_MODULE "platform"

#include <kernel/hal/platform.h>
#include <kernel/interfaces/logger.h>

// Global platform operations (dependency injection)
platform_operations_t *g_platform_ops = NULL;

/**
 * @brief Set platform operations
 */
void platform_set_operations(platform_operations_t *ops) {
    g_platform_ops = ops;
    if (g_platform_ops) {
        LOGGER_INFO(LOG_MODULE, "Platform operations set to: %s", 
                   g_platform_ops->name ? g_platform_ops->name : "unknown");
    }
}