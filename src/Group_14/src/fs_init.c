#include "fs_init.h"
#include "vfs.h"          // VFS API (vfs_init, vfs_mount_root, vfs_shutdown, etc.)
#include "mount.h"        // Mount operations (potentially used by vfs internals)
#include "fat.h"          // FAT driver registration routines
#include "terminal.h"     // For logging/debug output
#include "kmalloc.h"      // Potentially used by VFS internals
#include "fs_errno.h"     // Defines FS_ERR_* error codes
#include "types.h"        // Centralized type definitions
#include "sys_file.h"

#include <string.h>       // For string functions if needed

/* Global flag to track whether the file system layer has been initialized. */
static bool fs_initialized = false;

/* Default filesystem type for the root mount */
static const char *default_fs = "FAT"; // Or "FAT16" if your disk image is FAT16

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
        return FS_SUCCESS; // Return success instead of error if already initialized
    }

    terminal_write("[FS_INIT] Starting file system initialization...\n");

    /* Initialize the VFS layer */
    terminal_write("[FS_INIT] Initializing VFS layer...\n");
    vfs_init();

    /* Register filesystem drivers. */
    terminal_write("[FS_INIT] Registering FAT filesystem driver...\n");
    int reg_result = fat_register_driver();
    if (reg_result != 0) {
        terminal_printf("[FS_INIT] Error: FAT driver registration failed with code %d.\n", reg_result);
        // This is fatal - if we can't register filesystem drivers, we can't proceed
        return FS_ERR_UNKNOWN;
    }
    terminal_write("[FS_INIT] FAT driver registered successfully.\n");
    
    // Register other drivers (e.g., ext2_register_driver()) here if needed.

    /* Determine the root device.
     * In production, this would be obtained from boot configuration.
     */
    const char *root_device = "hdb"; // Primary slave as connected via -hdb in QEMU

    terminal_printf("[FS_INIT] Attempting to mount root FS (%s) on device '%s' at '/'\n", default_fs, root_device);

    /* Mount the root filesystem. */
    int mount_result = vfs_mount_root("/", default_fs, root_device);
    if (mount_result != 0) {
        terminal_printf("[FS_INIT] Error: Root file system mount failed for device '%s' with code %d.\n", 
                      root_device, mount_result);
        
        // Try to perform a clean shutdown of the VFS layer
        terminal_write("[FS_INIT] Attempting to clean up after mount failure...\n");
        vfs_shutdown();
        
        return FS_ERR_MOUNT;
    }

    fs_initialized = true;
    terminal_write("[FS_INIT] File system initialization complete.\n");
    
    // Print mount table for debugging
    terminal_write("[FS_INIT] Current mount points:\n");
    list_mounts();
    
    return FS_SUCCESS;
}

/**
 * fs_is_initialized:
 * Returns whether the filesystem layer has been initialized.
 */
bool fs_is_initialized(void)
{
    return fs_initialized;
}

/**
 * fs_shutdown:
 * Shuts down the file system layer.
 */
int fs_shutdown(void)
{
    if (!fs_initialized) {
        terminal_write("[FS_SHUTDOWN] Warning: File system not initialized, nothing to shut down.\n");
        return FS_SUCCESS;
    }
    
    terminal_write("[FS_SHUTDOWN] Shutting down file system...\n");

    /* Unmount the root filesystem. */
    int unmount_result = vfs_unmount_root();
    if (unmount_result != 0) {
        // This might fail if files are still open, etc.
        terminal_printf("[FS_SHUTDOWN] Warning: Root file system unmount failed with code %d.\n", 
                       unmount_result);
        // Continue with shutdown anyway
    }

    /* Unregister FAT driver */
    terminal_write("[FS_SHUTDOWN] Unregistering FAT driver...\n");
    fat_unregister_driver();
    // Unregister other drivers here...

    /* Shutdown the VFS layer. */
    terminal_write("[FS_SHUTDOWN] Shutting down VFS layer...\n");
    int vfs_result = vfs_shutdown();
    if (vfs_result != 0) {
        terminal_printf("[FS_SHUTDOWN] Warning: VFS shutdown returned code %d.\n", vfs_result);
    }

    fs_initialized = false;
    terminal_write("[FS_SHUTDOWN] File system shutdown complete.\n");
    return FS_SUCCESS;
}

/**
 * fs_test_file_access:
 * Simple helper function to test file access.
 * Attempts to open, read, and close a file to verify filesystem functionality.
 *
 * @param path Path to the file to test
 * @return 0 on success, negative error code on failure
 */
int fs_test_file_access(const char *path)
{
    if (!fs_initialized) {
        terminal_write("[FS_TEST] Error: File system not initialized.\n");
        return -FS_ERR_NOT_INIT;
    }

    terminal_printf("[FS_TEST] Testing file access: '%s'\n", path);
    
    // Try to open the file for reading
    file_t *file = vfs_open(path, O_RDONLY);
    if (!file) {
        terminal_printf("[FS_TEST] Error: Could not open file '%s'\n", path);
        return -FS_ERR_NOT_FOUND;
    }
    
    // Try to read some data
    char buffer[128];
    int bytes_read = vfs_read(file, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
        terminal_printf("[FS_TEST] Error: Failed to read from file '%s' (code %d)\n", 
                       path, bytes_read);
        vfs_close(file);
        return bytes_read;
    }
    
    // Null-terminate and display the data
    buffer[bytes_read] = '\0';
    terminal_printf("[FS_TEST] Successfully read %d bytes from '%s':\n", bytes_read, path);
    terminal_write(buffer);
    terminal_write("\n");
    
    // Close the file
    int close_result = vfs_close(file);
    if (close_result != 0) {
        terminal_printf("[FS_TEST] Warning: Failed to close file '%s' (code %d)\n", 
                       path, close_result);
        return close_result;
    }
    
    terminal_printf("[FS_TEST] File access test successful for '%s'\n", path);
    return FS_SUCCESS;
}