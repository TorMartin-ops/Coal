#pragma once
#ifndef VFS_H
#define VFS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * File Open Flags
 *
 * NOTE: These definitions were moved to sys_file.h to resolve conflicts.
 * Ensure sys_file.h provides the standard O_* definitions (O_RDONLY=0, etc.)
 * if needed directly by VFS API users, or rely on the values passed
 * from the system call layer.
 *---------------------------------------------------------------------------*/
// #define O_RDONLY    0x0001 // REMOVED
// #define O_WRONLY    0x0002 // REMOVED
// #define O_RDWR      0x0004 // REMOVED
// #define O_CREAT     0x0008 // REMOVED
// #define O_TRUNC     0x0010 // REMOVED

/*---------------------------------------------------------------------------
 * File offset type and SEEK definitions (can stay or be moved to types.h)
 *---------------------------------------------------------------------------*/
// typedef long off_t; // Already in types.h

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
 * Forward Declarations
 *---------------------------------------------------------------------------*/
typedef struct vnode vnode_t;
typedef struct vfs_driver vfs_driver_t; // Forward declare driver struct

/*---------------------------------------------------------------------------
 * File Handle
 *---------------------------------------------------------------------------*/
typedef struct file {
    vnode_t *vnode;      // Underlying file representation (vnode)
    int flags;           // Open flags (O_RDONLY, etc. passed from syscall)
    off_t offset;        // Current file offset
    // Add lock/mutex here if multiple threads can access the same file_t
} file_t;

/*---------------------------------------------------------------------------
 * VFS Driver Interface
 *---------------------------------------------------------------------------*/
struct vfs_driver { // Changed from typedef struct vfs_driver
    const char *fs_name;  // Filesystem name (e.g. "FAT32")
    void *(*mount)(const char *device);
    int (*unmount)(void *fs_context);
    vnode_t *(*open)(void *fs_context, const char *path, int flags);
    int (*read)(file_t *file, void *buf, size_t len);
    int (*write)(file_t *file, const void *buf, size_t len);
    int (*close)(file_t *file);
    off_t (*lseek)(file_t *file, off_t offset, int whence);
    vfs_driver_t *next;  // Use struct vfs_driver here too
}; // No typedef needed now

/*---------------------------------------------------------------------------
 * Abstract Vnode
 *---------------------------------------------------------------------------*/
struct vnode {
    void *data;              // Filesystem-specific file or directory data.
    vfs_driver_t *fs_driver; // Pointer to the VFS driver handling this vnode.
    // Add common vnode info like type (file/dir), size, permissions, refcount?
};

/*---------------------------------------------------------------------------
 * VFS API
 *---------------------------------------------------------------------------*/
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