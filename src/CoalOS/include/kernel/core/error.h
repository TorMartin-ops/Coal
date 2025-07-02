/**
 * @file error.h
 * @brief Common error handling definitions for Coal OS
 */

#ifndef COAL_CORE_ERROR_H
#define COAL_CORE_ERROR_H

#include <libc/stdint.h>

/**
 * @brief Standard error codes used throughout the kernel
 */
typedef enum {
    E_SUCCESS       = 0,    // Operation succeeded
    E_INVAL         = -1,   // Invalid argument
    E_NOMEM         = -2,   // Out of memory
    E_IO            = -3,   // I/O error
    E_BUSY          = -4,   // Resource busy
    E_NOTFOUND      = -5,   // Resource not found
    E_PERM          = -6,   // Permission denied
    E_FAULT         = -7,   // Bad address
    E_NOSPC         = -8,   // No space left
    E_EXIST         = -9,   // Already exists
    E_NOTSUP        = -10,  // Not supported
    E_TIMEOUT       = -11,  // Operation timed out
    E_OVERFLOW      = -12,  // Buffer overflow
    E_CORRUPT       = -13,  // Data corruption detected
    E_ALIGN         = -14,  // Alignment error
    E_INTERNAL      = -15,  // Internal error
} error_t;

/**
 * @brief Convert error code to string
 */
const char* error_to_string(error_t err);

/**
 * @brief Check if error code indicates success
 */
#define IS_SUCCESS(err) ((err) == E_SUCCESS)

/**
 * @brief Check if error code indicates failure
 */
#define IS_ERROR(err) ((err) != E_SUCCESS)

/**
 * @brief Return early if error
 */
#define RETURN_IF_ERROR(expr) do { \
    error_t _err = (expr); \
    if (IS_ERROR(_err)) return _err; \
} while(0)

/**
 * @brief Jump to cleanup label if error
 */
#define GOTO_IF_ERROR(expr, label) do { \
    error_t _err = (expr); \
    if (IS_ERROR(_err)) { \
        err = _err; \
        goto label; \
    } \
} while(0)

#endif // COAL_CORE_ERROR_H