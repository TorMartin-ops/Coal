#pragma once
#ifndef FS_ERRNO_H
#define FS_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

/**
 * fs_error_t
 *
 * Enumeration of file system error codes.
 * Negative values indicate errors.
 */
typedef enum fs_error {
    FS_SUCCESS              = 0,    /* No error */
    FS_ERR_UNKNOWN          = -1,   /* Unknown error */
    FS_ERR_INVALID_PARAM    = -2,   /* Invalid parameter */
    FS_ERR_OUT_OF_MEMORY    = -3,   /* Out of memory */
    FS_ERR_IO               = -4,   /* I/O error */
    FS_ERR_NOT_FOUND        = -5,   /* File or directory not found */
    FS_ERR_PERMISSION_DENIED= -6,   /* Permission denied */
    FS_ERR_FILE_EXISTS      = -7,   /* File or directory already exists */
    FS_ERR_NOT_A_DIRECTORY  = -8,   /* Expected a directory, found file */
    FS_ERR_IS_A_DIRECTORY   = -9,   /* Expected a file, found directory */
    FS_ERR_NO_SPACE         = -10,  /* No space left on device */
    FS_ERR_READ_ONLY        = -11,  /* File system is read-only */
    FS_ERR_NOT_SUPPORTED    = -12,  /* Operation not supported */
    FS_ERR_INVALID_FORMAT   = -13,  /* Invalid file system format */
    FS_ERR_CORRUPT          = -14,  /* File system corrupt */
    FS_ERR_MOUNT            = -15,  /* Mount error */
    FS_ERR_NOT_INIT         = -16,  /* File system not initialized */
    FS_ERR_BUSY             = -17,  /* Resource is busy */
    // Add additional error codes as needed.
} fs_error_t;

/**
 * fs_strerror
 *
 * Returns a human-readable string describing the fs_error_t code.
 *
 * @param err The file system error code.
 * @return    A constant string describing the error.
 */
static inline const char *fs_strerror(fs_error_t err) {
    switch (err) {
        case FS_SUCCESS:                return "Success";
        case FS_ERR_UNKNOWN:            return "Unknown error";
        case FS_ERR_INVALID_PARAM:      return "Invalid parameter";
        case FS_ERR_OUT_OF_MEMORY:      return "Out of memory";
        case FS_ERR_IO:                 return "I/O error";
        case FS_ERR_NOT_FOUND:          return "Not found";
        case FS_ERR_PERMISSION_DENIED:  return "Permission denied";
        case FS_ERR_FILE_EXISTS:        return "File or directory already exists";
        case FS_ERR_NOT_A_DIRECTORY:    return "Not a directory";
        case FS_ERR_IS_A_DIRECTORY:     return "Is a directory";
        case FS_ERR_NO_SPACE:           return "No space left on device";
        case FS_ERR_READ_ONLY:          return "Read-only file system";
        case FS_ERR_NOT_SUPPORTED:      return "Operation not supported";
        case FS_ERR_INVALID_FORMAT:     return "Invalid file system format";
        case FS_ERR_CORRUPT:            return "File system corrupt";
        case FS_ERR_MOUNT:              return "Mount error";
        case FS_ERR_NOT_INIT:           return "File system not initialized";
        case FS_ERR_BUSY:               return "Resource is busy";
        default:                        return "Unrecognized error";
    }
}

#ifdef __cplusplus
}
#endif

#endif // FS_ERRNO_H
