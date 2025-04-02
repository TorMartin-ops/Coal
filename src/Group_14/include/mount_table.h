#pragma once
#ifndef MOUNT_TABLE_H
#define MOUNT_TABLE_H

#include "types.h"
#include <string.h>

#include "mount.h"  // This header defines mount_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * mount_table_add
 *  Adds a mount entry to the global mount table.
 *
 * @param mnt Pointer to a mount_t structure.
 * @return 0 on success, -1 on failure.
 */
int mount_table_add(mount_t *mnt);

/**
 * mount_table_remove
 *  Removes a mount entry identified by the given mount point.
 *
 * @param mount_point The mount point string.
 * @return 0 on success, -1 if not found.
 */
int mount_table_remove(const char *mount_point);

/**
 * mount_table_find
 *  Searches for a mount entry by its mount point.
 *
 * @param mount_point The mount point string.
 * @return Pointer to the mount_t entry if found, or NULL otherwise.
 */
mount_t *mount_table_find(const char *mount_point);

/**
 * mount_table_list
 *  Prints the current mount table for debugging.
 */
void mount_table_list(void);

#ifdef __cplusplus
}
#endif

#endif /* MOUNT_TABLE_H */
