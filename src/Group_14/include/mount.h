#pragma once
#ifndef MOUNT_H
#define MOUNT_H

#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif

/* Mount entry structure */
typedef struct mount {
    const char *mount_point;   // Mount point, e.g., "/"
    const char *fs_name;       // Filesystem driver name, e.g., "FAT16"
    void *fs_context;          // Filesystem-specific context (from driver->mount)
    struct mount *next;        // Next entry in mount table
} mount_t;

/* Mount API */
int mount_fs(const char *mount_point, const char *device, const char *fs_name);
int unmount_fs(const char *mount_point);
mount_t *find_mount(const char *mount_point);
void list_mounts(void);

#ifdef __cplusplus
}
#endif

#endif /* MOUNT_H */
