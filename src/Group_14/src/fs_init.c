#include "fs_init.h"
#include "vfs.h"          // VFS API (vfs_init, vfs_mount_root, vfs_shutdown, etc.)
#include "mount.h"        // Mount operations
#include "fat.h"          // FAT driver registration routines
#include "terminal.h"     // For logging/debug output
#include "kmalloc.h"      // For dynamic memory allocation
#include "fs_errno.h"     // Defines FS_ERR_* error codes
#include "types.h"        // Centralized type definitions

#include <string.h>       // For string functions

/* Global flag to track whether the file system layer has been initialized. */
static bool fs_initialized = false;

/* Default filesystem type for the root mount (using FAT32 in this example) */
static const char *default_fs = "FAT32";

/*
 * fs_init:
 * Initializes the file system layer by:
 *   - Initializing the VFS layer.
 *   - Registering available filesystem drivers.
 *   - Mounting the root filesystem.
 */
int fs_init(void)
{
    terminal_write("[FS_INIT] Starting file system initialization...\n");

    /* Initialize the VFS layer (vfs_init returns void) */
    vfs_init();

    /* Register filesystem drivers. */
    if (fat_register_driver() != 0) {
        terminal_write("[FS_INIT] Warning: FAT driver registration failed.\n");
    }

    /* Determine the root device.
     * In production, this would be obtained from boot configuration.
     */
    const char *root_device = "hd0";

    /* Mount the root filesystem.
     * vfs_mount_root expects: mount point, filesystem name, device identifier.
     */
    if (vfs_mount_root("/", default_fs, root_device) != 0) {
        terminal_write("[FS_INIT] Error: Root file system mount failed.\n");
        return FS_ERR_MOUNT;
    }

    fs_initialized = true;
    terminal_write("[FS_INIT] File system initialization complete.\n");
    return FS_SUCCESS;
}

/*
 * fs_shutdown:
 * Shuts down the file system layer by:
 *   - Unmounting the root filesystem.
 *   - Unregistering filesystem drivers.
 *   - Shutting down the VFS layer.
 */
int fs_shutdown(void)
{
    if (!fs_initialized) {
        terminal_write("[FS_SHUTDOWN] Error: File system not initialized.\n");
        return FS_ERR_NOT_INIT;
    }

    /* Unmount the root filesystem. */
    if (vfs_unmount_root() != 0) {
        terminal_write("[FS_SHUTDOWN] Warning: Root file system unmount failed.\n");
    }

    /* Unregister FAT driver */
    fat_unregister_driver();

    /* Shutdown the VFS layer. */
    vfs_shutdown();

    fs_initialized = false;
    terminal_write("[FS_SHUTDOWN] File system shutdown complete.\n");
    return FS_SUCCESS;
}
