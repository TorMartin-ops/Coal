/**
 * @file fs_init.c
 * @brief File system bring-up: root disk, /dev and /proc.
 */
#define LOG_MODULE "fs"
#include <kernel/core/log.h>
#include <kernel/process/process.h>
#include <kernel/process/scheduler.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>
#include <kernel/fs/vfs/fs_init.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/fat/fat.h>
#include <kernel/fs/devfs.h>
#include <kernel/fs/procfs.h>
#include <kernel/drivers/storage/disk.h>
#include <kernel/drivers/storage/buffer_cache.h>

#define ROOT_DEVICE "hda"

static disk_t root_disk;

static disk_t extra_disks[3];
static const char *const extra_names[3] = { "hdb", "hdc", "hdd" };   /* the disk keeps the name pointer */

int fs_attach_disk(const char *name) {
    if (strcmp(name, ROOT_DEVICE) == 0) return 0;
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, extra_names[i]) != 0) continue;
        if (!extra_disks[i].initialized &&
            (disk_init(&extra_disks[i], extra_names[i]) != 0 || buffer_register_disk(&extra_disks[i]) != 0))
            return -ENODEV;
        return 0;
    }
    return -ENODEV;
}

/* Writes cached changes to the disk every few seconds, so a crash loses only a short window. */
#define FLUSH_INTERVAL_MS 5000

static void flusher(void) {
    for (;;) {
        sleep_ms(FLUSH_INTERVAL_MS);
        vfs_sync();
    }
}

void fs_start_flusher(void) {
    if (!process_spawn_kernel("flush", flusher)) LOG_WARN("no flusher thread");
}

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

