#pragma once
#ifndef TYPES_H
#define TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Foundational Includes
 *--------------------------------------------------------------------------*/
#include <libc/stddef.h>    // Provides size_t, NULL, etc.
#include <libc/stdint.h>    // Provides fixed-width integer types (like uint32_t)
#include <libc/stdbool.h>   // Provides bool, true, false

/*---------------------------------------------------------------------------
 * 64-bit Integer Types (if not already in libc/stdint.h)
 *--------------------------------------------------------------------------*/
// Add this definition before struct vfs_driver in vfs.h (if not defined elsewhere)


#ifndef MAX_FILENAME_LEN // Define if not already present
#define MAX_FILENAME_LEN 255
#endif
struct dirent {
    char     d_name[MAX_FILENAME_LEN + 1];
    uint32_t d_ino;       // Use cluster number or another unique ID
    uint8_t  d_type;      // File type (DT_REG, DT_DIR, etc.)
    // Add other fields like d_reclen if needed by userspace
};


#ifndef _UINT64_T_DEFINED // Add guards if these might be defined elsewhere later
typedef unsigned long long uint64_t;
#define _UINT64_T_DEFINED
#endif

#ifndef _INT64_T_DEFINED
typedef long long int64_t;
#define _INT64_T_DEFINED
#endif


/*---------------------------------------------------------------------------
 * Basic Utility Macros
 *--------------------------------------------------------------------------*/

/* Ensure NULL is defined */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Macro to calculate the offset of a member within a structure */
#ifndef offsetof
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif

/* Macro to retrieve the pointer to the container structure from a member pointer */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/*---------------------------------------------------------------------------
 * Additional OS-Specific Types
 *--------------------------------------------------------------------------*/

/*
 * uintptr_t: Unsigned integer type capable of holding a pointer.
 * For i386 (32-bit), this is typically uint32_t.
 */
#ifndef _UINTPTR_T_DEFINED // Use a guard to prevent redefinition
typedef uint32_t uintptr_t;
#define _UINTPTR_T_DEFINED
#endif

/*
 * ssize_t: Signed integer type for byte counts and error codes.
 */
#ifndef _SSIZE_T_DEFINED
typedef int ssize_t;
#define _SSIZE_T_DEFINED
#endif

/*
 * off_t: File offset type.
 */
#ifndef _OFF_T_DEFINED
typedef long off_t;
#define _OFF_T_DEFINED
#endif

/*
 * mode_t: Type for file mode flags and permissions.
 */
#ifndef _MODE_T_DEFINED
typedef unsigned int mode_t;
#define _MODE_T_DEFINED
#endif

/*
 * dev_t: Device number type.
 */
#ifndef _DEV_T_DEFINED
typedef unsigned int dev_t;
#define _DEV_T_DEFINED
#endif

/*
 * ino_t: Inode number type.
 */
#ifndef _INO_T_DEFINED
typedef unsigned int ino_t;
#define _INO_T_DEFINED
#endif

/*
 * pid_t: Process identifier type.
 */
#ifndef _PID_T_DEFINED
typedef int pid_t;
#define _PID_T_DEFINED
#endif

/*
 * uid_t and gid_t: User and group identifier types.
 */
#ifndef _UID_T_DEFINED
typedef unsigned int uid_t;
#define _UID_T_DEFINED
#endif

#ifndef _GID_T_DEFINED
typedef unsigned int gid_t;
#define _GID_T_DEFINED
#endif

#ifndef DEFAULT_ALIGNMENT
#define DEFAULT_ALIGNMENT 8 // Define default memory alignment (e.g., 8 bytes)
#endif

// Utility macros
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifdef __cplusplus
}
#endif

#endif /* TYPES_H */