#pragma once
#ifndef VFS_H
#define VFS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * File Open Flags
 *---------------------------------------------------------------------------*/
#define O_RDONLY    0x0001
#define O_WRONLY    0x0002
#define O_RDWR      0x0004
#define O_CREAT     0x0008
#define O_TRUNC     0x0010

/*---------------------------------------------------------------------------
 * File offset type
 *---------------------------------------------------------------------------*/
typedef long off_t;

/*---------------------------------------------------------------------------
 * Forward Declarations
 *---------------------------------------------------------------------------*/
typedef struct vnode vnode_t;

/*---------------------------------------------------------------------------
 * File Handle
 *---------------------------------------------------------------------------*/
typedef struct file {
    vnode_t *vnode;      // Underlying file representation (vnode)
    int flags;           // Open flags (O_RDONLY, etc.)
    off_t offset;        // Current file offset
} file_t;

/*---------------------------------------------------------------------------
 * VFS Driver Interface
 *---------------------------------------------------------------------------*/
typedef struct vfs_driver {
    const char *fs_name;  // Filesystem name (e.g. "FAT32")
    /* Mount:
     *  - device: device identifier or path
     *  - Returns a filesystem-specific context pointer, or NULL on failure.
     */
    void *(*mount)(const char *device);
    /* Unmount:
     *  - Takes the filesystem context pointer.
     *  - Returns 0 on success, negative error code on failure.
     */
    int (*unmount)(void *fs_context);
    /* Open:
     *  - Opens a file by its path (relative to the mount point).
     *  - Returns a vnode pointer representing the file, or NULL on failure.
     */
    vnode_t *(*open)(void *fs_context, const char *path, int flags);
    /* Read:
     *  - Reads data from an open file.
     *  - Returns the number of bytes read or a negative error code.
     */
    int (*read)(file_t *file, void *buf, size_t len);
    /* Write:
     *  - Writes data to an open file.
     *  - Returns the number of bytes written or a negative error code.
     */
    int (*write)(file_t *file, const void *buf, size_t len);
    /* Close:
     *  - Closes an open file handle.
     *  - Returns 0 on success or a negative error code.
     */
    int (*close)(file_t *file);
    /* Lseek:
     *  - Adjusts the file offset.
     *  - Returns the new offset or a negative error code.
     */
    off_t (*lseek)(file_t *file, off_t offset, int whence);
    struct vfs_driver *next;  // For internal driver list linkage
} vfs_driver_t;

/*---------------------------------------------------------------------------
 * Abstract Vnode
 *---------------------------------------------------------------------------*/
struct vnode {
    void *data;              // Filesystem-specific file or directory data.
    vfs_driver_t *fs_driver; // Pointer to the VFS driver handling this vnode.
};

/*---------------------------------------------------------------------------
 * VFS API
 *---------------------------------------------------------------------------*/

/* Initialize the VFS layer */
void vfs_init(void);

/* Register and unregister a filesystem driver */
int vfs_register_driver(vfs_driver_t *driver);
int vfs_unregister_driver(vfs_driver_t *driver);

/* Retrieve a registered driver by filesystem name */
vfs_driver_t *vfs_get_driver(const char *fs_name);

/* Mount operations:
 *  - Mount the root filesystem. For example:
 *      vfs_mount_root("/", "FAT32", "hd0");
 *  - Unmount the root filesystem.
 */
int vfs_mount_root(const char *mount_point, const char *fs_name, const char *device);
int vfs_unmount_root(void);

/* Shutdown the entire VFS subsystem */
int vfs_shutdown(void);

/* File operations */
file_t *vfs_open(const char *path, int flags);
int vfs_close(file_t *file);
int vfs_read(file_t *file, void *buf, size_t len);
int vfs_write(file_t *file, const void *buf, size_t len);
off_t vfs_lseek(file_t *file, off_t offset, int whence);

#ifdef __cplusplus
}
#endif

#endif /* VFS_H */
