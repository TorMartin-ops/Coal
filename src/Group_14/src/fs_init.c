#include "fs_init.h"
#include "vfs.h"          // VFS API (vfs_init, vfs_mount_root, vfs_shutdown, etc.)
#include "mount.h"        // Mount operations (potentially used by vfs internals)
#include "fat.h"          // FAT driver registration routines
#include "terminal.h"     // For logging/debug output
#include "kmalloc.h"      // Potentially used by VFS internals
#include "fs_errno.h"     // Defines FS_ERR_* error codes
#include "types.h"        // Centralized type definitions

#include <string.h>       // For string functions if needed

/* Global flag to track whether the file system layer has been initialized. */
static bool fs_initialized = false;

/* Default filesystem type for the root mount */
static const char *default_fs = "FAT32"; // Or "FAT16" if your disk image is FAT16

/**
 * fs_init:
 * Initializes the file system layer by:
 * - Initializing the VFS layer.
 * - Registering available filesystem drivers.
 * - Mounting the root filesystem.
 */
int fs_init(void)
{
    if (fs_initialized) {
        terminal_write("[FS_INIT] Warning: File system already initialized.\n");
        return FS_ERR_UNKNOWN; // Or a specific error
    }

    terminal_write("[FS_INIT] Starting file system initialization...\n");

    /* Initialize the VFS layer */
    vfs_init();

    /* Register filesystem drivers. */
    // Assuming fat_register_driver determines type or defaults to FAT32/16 name
    if (fat_register_driver() != 0) {
        terminal_write("[FS_INIT] Error: FAT driver registration failed.\n");
        // Decide if this is fatal
        return FS_ERR_UNKNOWN;
    }
    // Register other drivers (e.g., ext2_register_driver()) here if needed.

    /* Determine the root device.
     * In production, this would be obtained from boot configuration.
     * *** CHANGE "hdd" to "hdb" based on QEMU setup ***
     */
    const char *root_device = "hdb"; // Was "hdd"

    terminal_printf("[FS_INIT] Attempting to mount root FS (%s) on device '%s' at '/'\n", default_fs, root_device);

    /* Mount the root filesystem. */
    if (vfs_mount_root("/", default_fs, root_device) != 0) {
        terminal_printf("[FS_INIT] Error: Root file system mount failed for device '%s'.\n", root_device);
        // Cleanup registered drivers?
        // vfs_shutdown(); // Call shutdown if init fails?
        return FS_ERR_MOUNT;
    }

    fs_initialized = true;
    terminal_write("[FS_INIT] File system initialization complete.\n");
    return FS_SUCCESS;
}

/**
 * fs_shutdown:
 * Shuts down the file system layer.
 */
int fs_shutdown(void)
{
    if (!fs_initialized) {
        terminal_write("[FS_SHUTDOWN] Warning: File system not initialized, nothing to shut down.\n");
        return FS_SUCCESS; // Or FS_ERR_NOT_INIT
    }
    terminal_write("[FS_SHUTDOWN] Shutting down file system...\n");

    /* Unmount the root filesystem. */
    if (vfs_unmount_root() != 0) {
        // This might fail if files are still open, etc.
        terminal_write("[FS_SHUTDOWN] Warning: Root file system unmount failed.\n");
    }

    /* Unregister FAT driver */
    fat_unregister_driver();
    // Unregister other drivers here...

    /* Shutdown the VFS layer. */
    vfs_shutdown();

    fs_initialized = false;
    terminal_write("[FS_SHUTDOWN] File system shutdown complete.\n");
    return FS_SUCCESS;
}