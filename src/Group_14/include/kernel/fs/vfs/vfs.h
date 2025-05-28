/* include/vfs.h */
#pragma once
#ifndef VFS_H
#define VFS_H

#include <libc/stddef.h>    // For size_t, NULL, etc.
#include <libc/stdint.h>    // For uint32_t and friends
#include <libc/stdbool.h>   // For bool
#include <kernel/core/types.h>
#include <kernel/sync/spinlock.h>       // <<< ADDED: Include for spinlock_t

#ifdef __cplusplus
extern "C" {
#endif

/* Define SEEK macros if not already defined */
#ifndef SEEK_SET
#define SEEK_SET    0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR    1
#endif
#ifndef SEEK_END
#define SEEK_END    2
#endif

/* Type for file offset */
typedef long off_t;

/* Forward declaration for vnode */
typedef struct vnode vnode_t;

/* VFS file handle structure */
typedef struct file {
    vnode_t    *vnode;    // Underlying vnode pointer
    uint32_t    flags;    // Open flags
    off_t       offset;   // Current file offset (protected by lock)
    spinlock_t  lock;     // <<< ADDED: Lock to protect file offset and concurrent driver access
} file_t;

/* VFS driver interface */
typedef struct vfs_driver {
    const char *fs_name;  // Filesystem name (e.g., "FAT32")
    /* Mount function: returns a filesystem-specific context pointer or NULL on failure. */
    void *(*mount)(const char *device);
    /* Unmount function: returns 0 on success, negative on error. */
    int (*unmount)(void *fs_context);
    /* Open: returns a pointer to a vnode or NULL on error. */
    vnode_t *(*open)(void *fs_context, const char *path, int flags);
    /* Read: returns the number of bytes read or negative error code. */
    int (*read)(file_t *file, void *buf, size_t len);
    /* Write: returns the number of bytes written or negative error code. */
    int (*write)(file_t *file, const void *buf, size_t len);
    /* Close: returns 0 on success, negative error code on failure. */
    int (*close)(file_t *file);
    /* Lseek: returns new file offset or negative error code. */
    off_t (*lseek)(file_t *file, off_t offset, int whence);
    int (*readdir)(file_t *dir_file, struct dirent *d_entry_out, size_t entry_index); // Add this
    int (*unlink)(void *fs_context, const char *path); // Add this
    struct vfs_driver *next;
} vfs_driver_t;;

/* Abstract vnode structure */
struct vnode {
    void *data;                // Filesystem‑specific data (driver-specific context)
    vfs_driver_t *fs_driver;   // Pointer to the driver handling this vnode
};

/* VFS API */
void vfs_init(void);
int vfs_register_driver(vfs_driver_t *driver);
int vfs_unregister_driver(vfs_driver_t *driver);
vfs_driver_t *vfs_get_driver(const char *fs_name);
int vfs_mount_root(const char *mount_point, const char *fs_name, const char *device);
int vfs_unmount_root(void);
int vfs_shutdown(void);
file_t *vfs_open(const char *path, int flags);
int vfs_close(file_t *file);
int vfs_read(file_t *file, void *buf, size_t len);
int vfs_write(file_t *file, const void *buf, size_t len);
off_t vfs_lseek(file_t *file, off_t offset, int whence);


#ifdef __cplusplus
}
#endif

#endif /* VFS_H */