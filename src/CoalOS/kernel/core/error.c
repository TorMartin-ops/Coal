/**
 * @file error.c
 * @brief Error handling implementation for Coal OS
 */

#include <kernel/core/error.h>

/**
 * @brief Convert error code to human-readable string
 */
const char* error_to_string(error_t err) {
    switch (err) {
        case E_SUCCESS:     return "Success";
        case E_INVAL:       return "Invalid argument";
        case E_NOMEM:       return "Out of memory";
        case E_IO:          return "I/O error";
        case E_BUSY:        return "Resource busy";
        case E_NOTFOUND:    return "Not found";
        case E_PERM:        return "Permission denied";
        case E_FAULT:       return "Bad address";
        case E_NOSPC:       return "No space left";
        case E_EXIST:       return "Already exists";
        case E_NOTSUP:      return "Not supported";
        case E_TIMEOUT:     return "Operation timed out";
        case E_OVERFLOW:    return "Buffer overflow";
        case E_CORRUPT:     return "Data corruption";
        case E_ALIGN:       return "Alignment error";
        case E_INTERNAL:    return "Internal error";
        default:            return "Unknown error";
    }
}