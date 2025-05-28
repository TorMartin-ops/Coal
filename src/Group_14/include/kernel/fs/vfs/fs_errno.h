#pragma once
#ifndef FS_ERRNO_H
#define FS_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kernel/core/types.h>

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
    FS_ERR_INTERNAL         = -18, 
    FS_ERR_NAMETOOLONG      = -19,
    FS_ERR_OVERFLOW         = -20,
    FS_ERR_EOF              = -21,
    FS_ERR_NO_RESOURCES     = -22,  /* No resources available */
    FS_ERR_OUT_OF_BOUNDS    = -23,  /* Access out of bounds */
    FS_ERR_BAD_F           = -24,  /* Bad file descriptor */
    FS_ERR_BOUNDS_VIOLATION = -25,  /* Bounds violation */
    // Add additional error codes as needed.
} fs_error_t;


// --- POSIX Style Error Definitions ---
// Define standard names used in syscall.c and map them to appropriate *positive* values.
// System calls traditionally return the *negative* of these values on error.

#define EPERM        1  /* Operation not permitted */
#define ENOENT       2  /* No such file or directory */
#define ESRCH        3  /* No such process */
#define EINTR        4  /* Interrupted system call */
#define EIO          5  /* I/O error */
#define ENXIO        6  /* No such device or address */
#define E2BIG        7  /* Argument list too long */
#define ENOEXEC      8  /* Exec format error */
#define EBADF        9  /* Bad file number (maps reasonably to FS_ERR_BAD_F) */
#define ECHILD      10  /* No child processes */
#define EAGAIN      11  /* Try again */
#define ENOMEM      12  /* Out of memory (maps to FS_ERR_OUT_OF_MEMORY) */
#define EACCES      13  /* Permission denied (maps to FS_ERR_PERMISSION_DENIED) */
#define EFAULT      14  /* Bad address (maps reasonably to FS_ERR_BOUNDS_VIOLATION or FS_ERR_OUT_OF_BOUNDS) */
#define ENOTBLK     15  /* Block device required */
#define EBUSY       16  /* Device or resource busy (maps to FS_ERR_BUSY) */
#define EEXIST      17  /* File exists (maps to FS_ERR_FILE_EXISTS) */
#define EXDEV       18  /* Cross-device link */
#define ENODEV      19  /* No such device */
#define ENOTDIR     20  /* Not a directory (maps to FS_ERR_NOT_A_DIRECTORY) */
#define EISDIR      21  /* Is a directory (maps to FS_ERR_IS_A_DIRECTORY) */
#define EINVAL      22  /* Invalid argument (maps to FS_ERR_INVALID_PARAM) */
#define ENFILE      23  /* File table overflow */
#define EMFILE      24  /* Too many open files */
#define ENOTTY      25  /* Not a typewriter */
#define ETXTBSY     26  /* Text file busy */
#define EFBIG       27  /* File too large */
#define ENOSPC      28  /* No space left on device (maps to FS_ERR_NO_SPACE) */
#define ESPIPE      29  /* Illegal seek */
#define EROFS       30  /* Read-only file system (maps to FS_ERR_READ_ONLY) */
#define EMLINK      31  /* Too many links */
#define EPIPE       32  /* Broken pipe */
#define EDOM        33  /* Math argument out of domain of func */
#define ERANGE      34  /* Math result not representable (maps to FS_ERR_OVERFLOW) */
#define EDEADLK     35  /* Resource deadlock would occur */
#define ENAMETOOLONG 36 /* File name too long (maps to FS_ERR_NAMETOOLONG) */
#define ENOLCK      37  /* No record locks available */
#define ENOSYS      38  /* Function not implemented (maps to FS_ERR_NOT_SUPPORTED) */
#define ENOTEMPTY   39  /* Directory not empty */
#define ELOOP       40  /* Too many symbolic links encountered */


// Map specific errors needed by syscall.c if not directly covered above
// (Example: EFAULT is 14, ENOSYS is 38, EBADF is 9, EINVAL is 22, ENOMEM is 12)
// The above definitions should cover the errors reported in your build log.

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
        case FS_ERR_INTERNAL:           return "Internal error";
        case FS_ERR_NAMETOOLONG:        return "Name too long";
        case FS_ERR_OVERFLOW:           return "Overflow error";
        case FS_ERR_EOF:                return "End of file";
        case FS_ERR_NO_RESOURCES:     return "No resources available";
        case FS_ERR_OUT_OF_BOUNDS:      return "Out of bounds access";
        case FS_ERR_BAD_F:             return "Bad file descriptor";
        case FS_ERR_BOUNDS_VIOLATION:   return "Bounds violation";
        default:                        return "Unrecognized error";
    }
}

#ifdef __cplusplus
}
#endif

#endif // FS_ERRNO_H
