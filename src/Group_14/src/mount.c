#include "types.h"
#include "mount.h"
#include "vfs.h"
#include "kmalloc.h"
#include "terminal.h"
#include "string.h"

/*
 * mount_fs
 *  Looks up the filesystem driver via VFS, calls its mount() function,
 *  creates a new mount entry, and adds it to the mount table.
 */
int mount_fs(const char *mount_point, const char *device, const char *fs_name) {
    if (!mount_point || !device || !fs_name) {
        terminal_write("[Mount] Error: Invalid parameters.\n");
        return -1;
    }
    vfs_driver_t *driver = vfs_get_driver(fs_name);
    if (!driver) {
        terminal_write("[Mount] Error: No driver found for FS: ");
        terminal_write(fs_name);
        terminal_write("\n");
        return -1;
    }
    void *fs_context = driver->mount(device);
    if (!fs_context) {
        terminal_write("[Mount] Error: Mount failed on device: ");
        terminal_write(device);
        terminal_write("\n");
        return -1;
    }
    mount_t *mnt = (mount_t *)kmalloc(sizeof(mount_t));
    if (!mnt) {
        terminal_write("[Mount] Error: Out of memory for mount entry.\n");
        return -1;
    }
    mnt->mount_point = mount_point;
    mnt->fs_name = fs_name;
    mnt->fs_context = fs_context;
    mnt->next = NULL;
    /* Add to global mount table */
    extern int mount_table_add(mount_t *mnt);
    if (mount_table_add(mnt) != 0) {
        terminal_write("[Mount] Error: Failed to add mount entry.\n");
        return -1;
    }
    terminal_write("[Mount] Mounted device ");
    terminal_write(device);
    terminal_write(" at ");
    terminal_write(mount_point);
    terminal_write(" using FS ");
    terminal_write(fs_name);
    terminal_write("\n");
    return 0;
}

int unmount_fs(const char *mount_point) {
    if (!mount_point) {
        terminal_write("[Mount] Error: NULL mount point.\n");
        return -1;
    }
    mount_t *mnt = find_mount(mount_point);
    if (!mnt) {
        terminal_write("[Mount] Error: Mount point not found.\n");
        return -1;
    }
    vfs_driver_t *driver = vfs_get_driver(mnt->fs_name);
    if (driver && driver->unmount) {
        if (driver->unmount(mnt->fs_context) != 0) {
            terminal_write("[Mount] Error: Unmount failed for ");
            terminal_write(mount_point);
            terminal_write("\n");
            return -1;
        }
    }
    extern int mount_table_remove(const char *mount_point);
    if (mount_table_remove(mount_point) != 0) {
        terminal_write("[Mount] Error: Failed to remove mount entry.\n");
        return -1;
    }
    terminal_write("[Mount] Unmounted ");
    terminal_write(mount_point);
    terminal_write("\n");
    return 0;
}

mount_t *find_mount(const char *mount_point) {
    extern mount_t *mount_table_find(const char *mount_point);
    return mount_table_find(mount_point);
}

void list_mounts(void) {
    extern void mount_table_list(void);
    mount_table_list();
}
