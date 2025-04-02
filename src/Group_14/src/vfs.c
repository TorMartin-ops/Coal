#include "vfs.h"
#include "kmalloc.h"
#include "terminal.h"
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
typedef struct mount_entry {
    const char *mount_point;    // Mount point (e.g. "/")
    const char *fs_name;        // Filesystem name (e.g. "FAT32")
    void *fs_context;           // Filesystem-specific context (returned by mount)
    vfs_driver_t *driver;       // Pointer to the driver handling this mount
    struct mount_entry *next;   // Next mount entry
} mount_entry_t;

/* Global mount table head */
static mount_entry_t *mount_table = NULL;

/* Global driver list head */
static vfs_driver_t *driver_list = NULL;

/*---------------------------------------------------------------------------
 * VFS Initialization and Driver Registration
 *---------------------------------------------------------------------------*/
void vfs_init(void) {
    driver_list = NULL;
    mount_table = NULL;
    terminal_write("[VFS] Initialized.\n");
}

int vfs_register_driver(vfs_driver_t *driver) {
    if (!driver || !driver->fs_name) {
        terminal_write("[VFS] Registration failed: Invalid driver or fs_name.\n");
        return -1;
    }
    driver->next = driver_list;
    driver_list = driver;
    terminal_write("[VFS] Registered driver: ");
    terminal_write(driver->fs_name);
    terminal_write("\n");
    return 0;
}

int vfs_unregister_driver(vfs_driver_t *driver) {
    if (!driver) {
        return -1;
    }
    vfs_driver_t **prev = &driver_list;
    vfs_driver_t *curr = driver_list;
    while (curr) {
        if (curr == driver) {
            *prev = curr->next;
            terminal_write("[VFS] Unregistered driver: ");
            terminal_write(driver->fs_name);
            terminal_write("\n");
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    terminal_write("[VFS] Unregister failed: Driver not found.\n");
    return -1;
}

vfs_driver_t *vfs_get_driver(const char *fs_name) {
    vfs_driver_t *drv = driver_list;
    while (drv) {
        if (strcmp(drv->fs_name, fs_name) == 0)
            return drv;
        drv = drv->next;
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 * Mount Table Helpers
 *---------------------------------------------------------------------------*/
static int add_mount_entry(const char *mount_point, const char *fs_name, void *fs_context, vfs_driver_t *driver) {
    mount_entry_t *entry = (mount_entry_t *)kmalloc(sizeof(mount_entry_t));
    if (!entry) {
        terminal_write("[VFS] add_mount_entry: Out of memory.\n");
        return -1;
    }
    // In production, duplicate strings to avoid lifetime issues.
    entry->mount_point = mount_point;
    entry->fs_name = fs_name;
    entry->fs_context = fs_context;
    entry->driver = driver;
    entry->next = mount_table;
    mount_table = entry;
    return 0;
}

/*
 * find_mount_entry:
 * Searches the mount table for the entry whose mount point is the longest prefix
 * of the given path. This allows for multiple mount points.
 */
static mount_entry_t *find_mount_entry(const char *path) {
    mount_entry_t *best_match = NULL;
    mount_entry_t *curr = mount_table;
    while (curr) {
        size_t len = strlen(curr->mount_point);
        if (len > 0 && strncmp(path, curr->mount_point, len) == 0) {
            if (!best_match || len > strlen(best_match->mount_point)) {
                best_match = curr;
            }
        }
        curr = curr->next;
    }
    return best_match;
}

/*---------------------------------------------------------------------------
 * Mount / Unmount Operations
 *---------------------------------------------------------------------------*/
int vfs_mount_root(const char *mount_point, const char *fs_name, const char *device) {
    if (!mount_point || !fs_name || !device) {
        terminal_write("[VFS] mount_root: Invalid parameters.\n");
        return -1;
    }
    vfs_driver_t *driver = vfs_get_driver(fs_name);
    if (!driver) {
        terminal_write("[VFS] mount_root: Driver not found for FS: ");
        terminal_write(fs_name);
        terminal_write("\n");
        return -1;
    }
    void *fs_context = driver->mount(device);
    if (!fs_context) {
        terminal_write("[VFS] mount_root: Mount failed for device: ");
        terminal_write(device);
        terminal_write("\n");
        return -1;
    }
    if (add_mount_entry(mount_point, fs_name, fs_context, driver) != 0) {
        return -1;
    }
    terminal_write("[VFS] Mounted root FS: ");
    terminal_write(fs_name);
    terminal_write(" on device: ");
    terminal_write(device);
    terminal_write(" at mount point: ");
    terminal_write(mount_point);
    terminal_write("\n");
    return 0;
}

int vfs_unmount_root(void) {
    mount_entry_t **prev = &mount_table;
    mount_entry_t *curr = mount_table;
    while (curr) {
        if (strcmp(curr->mount_point, "/") == 0) {
            if (curr->driver && curr->driver->unmount) {
                if (curr->driver->unmount(curr->fs_context) != 0) {
                    terminal_write("[VFS] unmount_root: Unmount failed.\n");
                    return -1;
                }
            }
            *prev = curr->next;
            // Free the mount entry.
            kfree(curr, sizeof(mount_entry_t));
            terminal_write("[VFS] Root filesystem unmounted.\n");
            return 0;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    terminal_write("[VFS] unmount_root: Root filesystem not found.\n");
    return -1;
}

/*
 * vfs_shutdown:
 * Shuts down the VFS layer by unmounting all filesystems and clearing the driver list.
 */
int vfs_shutdown(void) {
    // Unmount all mounted filesystems
    while (mount_table) {
        if (mount_table->driver && mount_table->driver->unmount) {
            mount_table->driver->unmount(mount_table->fs_context);
        }
        mount_entry_t *temp = mount_table;
        mount_table = mount_table->next;
        kfree(temp, sizeof(mount_entry_t));
    }
    driver_list = NULL;
    terminal_write("[VFS] Shutdown complete.\n");
    return 0;
}

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/
file_t *vfs_open(const char *path, int flags) {
    if (!path) {
        terminal_write("[VFS] open: Invalid path.\n");
        return NULL;
    }
    mount_entry_t *mnt = find_mount_entry(path);
    if (!mnt) {
        terminal_write("[VFS] open: No mount entry found for path: ");
        terminal_write(path);
        terminal_write("\n");
        return NULL;
    }
    size_t mount_len = strlen(mnt->mount_point);
    const char *relative_path = path;
    if (mount_len > 1) {
        relative_path = path + mount_len;
        if (*relative_path == '\0')
            relative_path = "/";
    }
    vnode_t *node = mnt->driver->open(mnt->fs_context, relative_path, flags);
    if (!node) {
        terminal_write("[VFS] open: File not found: ");
        terminal_write(path);
        terminal_write("\n");
        return NULL;
    }
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        terminal_write("[VFS] open: Out of memory for file structure.\n");
        return NULL;
    }
    file->vnode = node;
    file->flags = flags;
    file->offset = 0;
    return file;
}

int vfs_close(file_t *file) {
    if (!file) {
        terminal_write("[VFS] close: Invalid file handle.\n");
        return -1;
    }
    if (!file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->close) {
        terminal_write("[VFS] close: Close operation not supported.\n");
        return -1;
    }
    int ret = file->vnode->fs_driver->close(file);
    kfree(file, sizeof(file_t));
    return ret;
}

int vfs_read(file_t *file, void *buf, size_t len) {
    if (!file || !buf) {
        terminal_write("[VFS] read: Invalid parameters.\n");
        return -1;
    }
    if (!file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->read) {
        terminal_write("[VFS] read: Read operation not supported.\n");
        return -1;
    }
    int bytes = file->vnode->fs_driver->read(file, buf, len);
    if (bytes > 0)
        file->offset += bytes;
    return bytes;
}

int vfs_write(file_t *file, const void *buf, size_t len) {
    if (!file || !buf) {
        terminal_write("[VFS] write: Invalid parameters.\n");
        return -1;
    }
    if (!file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->write) {
        terminal_write("[VFS] write: Write operation not supported.\n");
        return -1;
    }
    int bytes = file->vnode->fs_driver->write(file, buf, len);
    if (bytes > 0)
        file->offset += bytes;
    return bytes;
}

off_t vfs_lseek(file_t *file, off_t offset, int whence) {
    if (!file) {
        terminal_write("[VFS] lseek: Invalid file handle.\n");
        return -1;
    }
    if (file->vnode && file->vnode->fs_driver && file->vnode->fs_driver->lseek) {
        off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);
        file->offset = new_offset;
        return new_offset;
    } else {
        off_t new_offset;
        switch (whence) {
            case SEEK_SET:
                new_offset = offset;
                break;
            case SEEK_CUR:
                new_offset = file->offset + offset;
                break;
            case SEEK_END:
                terminal_write("[VFS] lseek: SEEK_END not supported in default implementation.\n");
                return -1;
            default:
                terminal_write("[VFS] lseek: Invalid whence parameter.\n");
                return -1;
        }
        if (new_offset < 0) {
            terminal_write("[VFS] lseek: New offset is negative.\n");
            return -1;
        }
        file->offset = new_offset;
        return new_offset;
    }
}
