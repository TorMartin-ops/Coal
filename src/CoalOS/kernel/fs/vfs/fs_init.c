/**
 * @file fs_init.c
 * @brief File system bring-up: root disk, /dev and /proc.
 */
#define LOG_MODULE "fs"
#include <kernel/core/log.h>
#include <kernel/fs/vfs/fs_init.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/fat/fat.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/procfs.h>
#include <kernel/drivers/storage/disk.h>
#include <kernel/drivers/storage/buffer_cache.h>

#define ROOT_DEVICE "hda"

static disk_t root_disk;

int fs_init(void) {
    vfs_init();
    int rc = fat_register_driver();
    if (rc == 0) rc = devfs_init();
    if (rc == 0) rc = procfs_init();
    if (rc < 0) { LOG_ERROR("driver registration failed: %d", rc); return rc; }

    rc = disk_init(&root_disk, ROOT_DEVICE);
    if (rc == 0) rc = buffer_register_disk(&root_disk);
    if (rc == 0) rc = vfs_mount("/", "fat", ROOT_DEVICE);
    if (rc < 0) { LOG_ERROR("cannot mount root from %s: %d", ROOT_DEVICE, rc); return rc; }

    if (vfs_mount("/dev", "devfs", NULL) < 0) LOG_WARN("/dev not mounted");
    if (vfs_mount("/proc", "procfs", NULL) < 0) LOG_WARN("/proc not mounted");
    return 0;
}

