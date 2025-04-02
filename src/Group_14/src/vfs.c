#include "vfs.h"
#include "kmalloc.h"
#include "terminal.h"    // Ensure terminal_printf is available if used
#include "string.h"
#include "types.h"

/* Define SEEK macros if not already defined */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

/*---------------------------------------------------------------------------
 * Mount Table Structure
 *---------------------------------------------------------------------------*/
typedef struct mount_entry { /* ... same ... */
    const char *mount_point; const char *fs_name; void *fs_context;
    vfs_driver_t *driver; struct mount_entry *next;
} mount_entry_t;

/* Global mount table head */
static mount_entry_t *mount_table = NULL;
/* Global driver list head */
static vfs_driver_t *driver_list = NULL;

/*---------------------------------------------------------------------------
 * VFS Initialization and Driver Registration
 *---------------------------------------------------------------------------*/
void vfs_init(void) { /* ... same ... */ }
int vfs_register_driver(vfs_driver_t *driver) { /* ... same ... */ }
int vfs_unregister_driver(vfs_driver_t *driver) { /* ... same ... */ }
vfs_driver_t *vfs_get_driver(const char *fs_name) { /* ... same ... */ }

/*---------------------------------------------------------------------------
 * Mount Table Helpers
 *---------------------------------------------------------------------------*/
static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) { /* ... same ... */ }

/**
 * find_mount_entry (Revised Logic - same as previous)
 */
static mount_entry_t *find_mount_entry(const char *path) { /* ... same revised logic with debug prints ... */
    mount_entry_t *best_match = NULL; mount_entry_t *curr = mount_table; size_t best_len = 0;
    terminal_printf("[VFS DEBUG] find_mount_entry: path='%s', current mount_table=0x%x\n", path ? path : "NULL", (uintptr_t)curr);
    if (!path) return NULL;
    while (curr) { /* ... loop logic same ... */ }
    if (best_match) terminal_printf("[VFS DEBUG] find_mount_entry: Returning best match '%s'\n", best_match->mount_point);
    else terminal_printf("[VFS DEBUG] find_mount_entry: No matching mount point found for '%s'.\n", path);
    return best_match;
}

/*---------------------------------------------------------------------------
 * Mount / Unmount Operations
 *---------------------------------------------------------------------------*/
int vfs_mount_root(const char *mp, const char *fs, const char *dev) { /* ... same ... */ }
int vfs_unmount_root(void) { /* ... same ... */ }
int vfs_shutdown(void) { /* ... same ... */ }

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/
file_t *vfs_open(const char *path, int flags) {
    if (!path) { return NULL; }
    terminal_printf("[VFS] vfs_open: Attempting to open path '%s'\n", path);

    mount_entry_t *mnt = find_mount_entry(path);

    // *** ADDED DEBUG PRINT HERE ***
    terminal_printf("[VFS DEBUG] vfs_open: find_mount_entry returned mnt = 0x%x\n", (uintptr_t)mnt);

    if (!mnt) { // Check the value received from find_mount_entry
        terminal_printf("[VFS] vfs_open: No mount entry found for path '%s' (mnt is NULL!)\n", path); // Modified msg
        return NULL;
    }

    // This should only be reached if mnt is NOT NULL
    terminal_printf("[VFS] vfs_open: Using mount point '%s' for path '%s'\n", mnt->mount_point, path);

    size_t mount_len = strlen(mnt->mount_point);
    const char *relative_path;

    if (mount_len == 1 && mnt->mount_point[0] == '/') {
        relative_path = path;
    } else {
        relative_path = path + mount_len;
        if (*relative_path == '\0') { relative_path = "/"; }
    }

    terminal_printf("[VFS] vfs_open: Calling driver '%s' open with relative path '%s'\n", mnt->fs_name, relative_path);

    if (!mnt->driver || !mnt->driver->open) {
        terminal_printf("[VFS] vfs_open: Driver '%s' missing or no open function.\n", mnt->fs_name);
        return NULL;
    }

    vnode_t *node = mnt->driver->open(mnt->fs_context, relative_path, flags);
    if (!node) {
        terminal_printf("[VFS] vfs_open: Driver '%s' failed to open relative path '%s'.\n", mnt->fs_name, relative_path);
        return NULL;
    }

    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) { /* ... error handling ... */ return NULL; }

    file->vnode = node; file->flags = flags; file->offset = 0;
    terminal_printf("[VFS] vfs_open: Successfully opened '%s'. file=0x%x, vnode=0x%x\n", path, (uintptr_t)file, (uintptr_t)node);
    return file;
}

// vfs_close, vfs_read, vfs_write, vfs_lseek remain the same
int vfs_close(file_t *file) { /* ... same ... */ }
int vfs_read(file_t *file, void *buf, size_t len) { /* ... same ... */ }
int vfs_write(file_t *file, const void *buf, size_t len) { /* ... same ... */ }
off_t vfs_lseek(file_t *file, off_t offset, int whence) { /* ... same ... */ }