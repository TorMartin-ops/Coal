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
#include <libc/stdint.h>    // Provides fixed-width integer types
#include <libc/stdbool.h>   // Provides bool, true, false

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

#ifdef __cplusplus
}
#endif

#endif /* TYPES_H */
