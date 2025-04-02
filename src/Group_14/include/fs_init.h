#pragma once
#ifndef FS_INIT_H
#define FS_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "fs_errno.h"

/**
 * fs_init
 *
 * Initializes the file system layer.
 *
 * @return FS_SUCCESS on success, or a negative error code on failure.
 */
int fs_init(void);

/**
 * fs_shutdown
 *
 * Shuts down the file system layer.
 *
 * @return FS_SUCCESS on success, or a negative error code on failure.
 */
int fs_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // FS_INIT_H
