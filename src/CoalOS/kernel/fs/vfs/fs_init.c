/**
 * @file fs_init.c
 * @brief File system bring-up: root disk, /dev and /proc.
 */
#define LOG_MODULE "fs"
#include <kernel/core/log.h>
#include <kernel/core/cmdline.h>
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

#define DEFAULT_ROOT "hda"
#define MAX_DISKS    6
#define NAME_LEN     8

/* Attached disks; a disk keeps the pointer to its name, so the names live here. */
static struct { disk_t disk; char name[NAME_LEN]; } g_disks[MAX_DISKS];

static bool attach(const char *name) {
    int free_slot = -1;

    if (strlen(name) >= NAME_LEN) return false;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_disks[i].disk.initialized && strcmp(g_disks[i].name, name) == 0) return true;
        if (!g_disks[i].disk.initialized && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return false;
    strcpy(g_disks[free_slot].name, name);
    return disk_init(&g_disks[free_slot].disk, g_disks[free_slot].name) == 0 &&
           buffer_register_disk(&g_disks[free_slot].disk) == 0;
}

int fs_attach_disk(const char *name) {
    return attach(name) ? 0 : -ENODEV;
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

    char root[NAME_LEN + 5] = DEFAULT_ROOT;                 /* root=/dev/vda or root=vda */
    if (cmdline_get("root", 0, root, sizeof(root)) && strncmp(root, "/dev/", 5) == 0) memmove(root, root + 5, strlen(root) - 4);
    if (!attach(root)) rc = -ENODEV;
    if (rc == 0) rc = vfs_mount("/", "fat", root);
    if (rc < 0) { LOG_ERROR("cannot mount root from %s: %d", root, rc); return rc; }

    if (vfs_mount("/dev", "devfs", NULL) < 0) LOG_WARN("/dev not mounted");
    if (vfs_mount("/proc", "procfs", NULL) < 0) LOG_WARN("/proc not mounted");
    return 0;
}

