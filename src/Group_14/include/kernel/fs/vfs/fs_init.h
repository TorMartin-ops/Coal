#pragma once
#ifndef FS_INIT_H
#define FS_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/core/types.h> // Include for bool definition

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

/**
 * fs_is_initialized     // <<< ADDED PROTOTYPE
 *
 * Checks if the filesystem layer has been successfully initialized.
 *
 * @return true if initialized, false otherwise.
 */
bool fs_is_initialized(void);


#ifdef __cplusplus
}
#endif

#endif // FS_INIT_H