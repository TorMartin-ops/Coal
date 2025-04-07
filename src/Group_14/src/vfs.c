#include "vfs.h"
#include "kmalloc.h"
#include "terminal.h"
#include "string.h"
#include "types.h"
#include "sys_file.h"
#include "fs_errno.h" // Include for FS_ERR_* codes

/* Define SEEK macros if not already defined elsewhere */
#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

/* Debug macro - define VFS_DEBUG to enable verbose logging */
#define VFS_DEBUG 1

#ifdef VFS_DEBUG
#define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
#define VFS_DEBUG_LOG(fmt, ...) terminal_printf("[VFS DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define VFS_LOG(fmt, ...) terminal_printf("[VFS] " fmt "\n", ##__VA_ARGS__)
#define VFS_DEBUG_LOG(fmt, ...) do {} while(0)
#endif

#define VFS_ERROR(fmt, ...) terminal_printf("[VFS ERROR] " fmt "\n", ##__VA_ARGS__)

/*---------------------------------------------------------------------------
 * Mount Table Structure
 *---------------------------------------------------------------------------*/
typedef struct mount_entry {
    const char *mount_point;      // The path where the filesystem is mounted (e.g., "/")
    const char *fs_name;          // Name of the filesystem driver (e.g., "FAT32")
    void *fs_context;             // Filesystem-specific data returned by driver->mount()
    vfs_driver_t *driver;         // Pointer to the registered driver for this filesystem
    struct mount_entry *next;     // Next entry in the linked list of mounts
    size_t mount_point_len;       // Pre-calculated length of mount_point for efficiency
} mount_entry_t;

/* Global mount table head - Linked list of active mounts */
static mount_entry_t *mount_table = NULL;

/* Global driver list head - Linked list of registered filesystem drivers */
static vfs_driver_t *driver_list = NULL;

/* Forward declarations for internal helper functions */
static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv);
static mount_entry_t *find_mount_entry(const char *path);
static const char *get_relative_path(const char *path, mount_entry_t *mnt);
static int check_driver_validity(vfs_driver_t *driver);

/*---------------------------------------------------------------------------
 * VFS Initialization and Driver Registration
 *---------------------------------------------------------------------------*/

/**
 * @brief Initializes the Virtual File System layer.
 * Sets up internal structures (clears driver and mount lists).
 */
void vfs_init(void) {
    mount_table = NULL;
    driver_list = NULL;
    
    VFS_LOG("Virtual File System initialized");
}

/**
 * @brief Checks if a driver structure is valid
 * @param driver Driver to check
 * @return 0 if valid, negative error code otherwise
 */
static int check_driver_validity(vfs_driver_t *driver) {
    if (!driver) {
        VFS_ERROR("Attempted to register NULL driver");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!driver->fs_name) {
        VFS_ERROR("Driver has NULL fs_name");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!driver->mount) {
        VFS_ERROR("Driver '%s' has NULL mount function", driver->fs_name);
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!driver->open) {
        VFS_ERROR("Driver '%s' has NULL open function", driver->fs_name);
        return -FS_ERR_INVALID_PARAM;
    }
    
    return FS_SUCCESS;
}

/**
 * @brief Registers a filesystem driver with the VFS.
 * Adds the driver to the global linked list of available drivers.
 * @param driver Pointer to the vfs_driver_t structure to register.
 * @return 0 on success, negative error code on failure.
 */
int vfs_register_driver(vfs_driver_t *driver) {
    int check_result = check_driver_validity(driver);
    if (check_result != FS_SUCCESS) {
        return check_result;
    }

    // Check if driver with the same name already exists
    vfs_driver_t *current = driver_list;
    while (current) {
        if (strcmp(current->fs_name, driver->fs_name) == 0) {
            VFS_ERROR("Driver '%s' already registered", driver->fs_name);
            return -FS_ERR_FILE_EXISTS;
        }
        current = current->next;
    }

    // Add to the front of the list
    driver->next = driver_list;
    driver_list = driver;

    VFS_LOG("Registered driver: %s", driver->fs_name);
    return FS_SUCCESS;
}

/**
 * @brief Unregisters a filesystem driver from the VFS.
 * @param driver Pointer to the driver to unregister.
 * @return 0 on success, negative error code on failure.
 */
int vfs_unregister_driver(vfs_driver_t *driver) {
    if (!driver) {
        VFS_ERROR("Attempted to unregister NULL driver");
        return -FS_ERR_INVALID_PARAM;
    }

    vfs_driver_t **prev = &driver_list;
    vfs_driver_t *curr = driver_list;
    
    while (curr) {
        if (curr == driver) {
            *prev = curr->next; // Remove from list
            VFS_LOG("Unregistered driver: %s", driver->fs_name);
            return FS_SUCCESS;
        }
        prev = &curr->next;
        curr = curr->next;
    }

    VFS_ERROR("Driver '%s' not found for unregistration", driver->fs_name);
    return -FS_ERR_NOT_FOUND;
}

/**
 * @brief Retrieves a registered filesystem driver by name.
 * @param fs_name The name of the filesystem driver (e.g., "FAT32").
 * @return Pointer to the driver structure, or NULL if not found.
 */
vfs_driver_t *vfs_get_driver(const char *fs_name) {
    if (!fs_name) {
        VFS_ERROR("NULL fs_name passed to vfs_get_driver");
        return NULL;
    }

    vfs_driver_t *curr = driver_list;
    while (curr) {
        if (strcmp(curr->fs_name, fs_name) == 0) {
            return curr;
        }
        curr = curr->next;
    }

    VFS_ERROR("Driver '%s' not found", fs_name);
    return NULL;
}

/**
 * @brief Dumps information about all registered filesystem drivers to the terminal.
 * Useful for debugging.
 */
void vfs_list_drivers(void) {
    VFS_LOG("Registered filesystem drivers:");
    
    if (!driver_list) {
        VFS_LOG("  (none)");
        return;
    }
    
    vfs_driver_t *curr = driver_list;
    int count = 0;
    
    while (curr) {
        VFS_LOG("  %d: %s", ++count, curr->fs_name);
        curr = curr->next;
    }
    
    VFS_LOG("Total drivers: %d", count);
}

/*---------------------------------------------------------------------------
 * Mount Table Helpers
 *---------------------------------------------------------------------------*/

/**
 * @brief Adds a new mount entry to the global mount table.
 * @param mp Mount point path string.
 * @param fs Filesystem name string.
 * @param ctx Filesystem-specific context pointer.
 * @param drv Pointer to the filesystem driver.
 * @return 0 on success, negative error code on failure.
 */
static int add_mount_entry(const char *mp, const char *fs, void *ctx, vfs_driver_t *drv) {
    if (!mp || !fs || !ctx || !drv) {
        VFS_ERROR("Invalid parameters to add_mount_entry");
        return -FS_ERR_INVALID_PARAM;
    }

    // Create a persistent copy of the mount point path
    size_t mp_len = strlen(mp);
    char *mp_copy = (char *)kmalloc(mp_len + 1);
    if (!mp_copy) {
        VFS_ERROR("Failed to allocate memory for mount point path");
        return -FS_ERR_OUT_OF_MEMORY;
    }
    strcpy(mp_copy, mp);

    mount_entry_t *new_mount = (mount_entry_t *)kmalloc(sizeof(mount_entry_t));
    if (!new_mount) {
        VFS_ERROR("Failed to allocate memory for mount entry");
        kfree(mp_copy, mp_len + 1);
        return -FS_ERR_OUT_OF_MEMORY;
    }

    new_mount->mount_point = mp_copy;
    new_mount->fs_name = fs;      // Assuming fs_name is persistent (from driver)
    new_mount->fs_context = ctx;
    new_mount->driver = drv;
    new_mount->mount_point_len = mp_len;
    
    // Add to front of mount list
    new_mount->next = mount_table;
    mount_table = new_mount;

    VFS_LOG("Added mount: '%s' -> %s (context: 0x%x)", mp, fs, (uintptr_t)ctx);
    return FS_SUCCESS;
}

/**
 * @brief Finds the most specific mount entry corresponding to a given path.
 * Implements the longest-prefix match algorithm.
 * @param path The absolute path to resolve.
 * @return Pointer to the best matching mount_entry_t, or NULL if no suitable mount point found.
 */
static mount_entry_t *find_mount_entry(const char *path) {
    if (!path) {
        VFS_ERROR("NULL path passed to find_mount_entry");
        return NULL;
    }
    
    if (path[0] != '/') {
        VFS_ERROR("Non-absolute path passed to find_mount_entry: '%s'", path);
        return NULL;
    }

    mount_entry_t *best_match = NULL;
    size_t best_len = 0;

    mount_entry_t *curr = mount_table;
    while (curr) {
        // Check if path starts with the mount point
        if (strncmp(path, curr->mount_point, curr->mount_point_len) == 0) {
            // Check for valid matches
            bool is_exact_match = (path[curr->mount_point_len] == '\0');
            bool is_subdir_match = (path[curr->mount_point_len] == '/');
            bool is_root_mount = (curr->mount_point_len == 1 && curr->mount_point[0] == '/');

            if (is_exact_match || is_subdir_match || is_root_mount) {
                // This is a valid match, determine if it's the best match so far
                if (curr->mount_point_len > best_len) {
                    best_match = curr;
                    best_len = curr->mount_point_len;
                }
                // If equal length, prefer non-root mounts
                else if (curr->mount_point_len == best_len && best_len == 1 && 
                        best_match && best_match->mount_point[0] == '/' && 
                        curr->mount_point[0] != '/') {
                    best_match = curr;
                }
            }
        }
        curr = curr->next;
    }

    if (best_match) {
        VFS_DEBUG_LOG("Path '%s' matched to mount point '%s'", path, best_match->mount_point);
    } else {
        VFS_ERROR("No mount point found for path '%s'", path);
    }
    
    return best_match;
}

/**
 * @brief Calculates the path relative to a mount point
 * @param path The original absolute path
 * @param mnt The mount entry
 * @return A pointer to the relative path (no allocation, just pointer arithmetic)
 */
static const char *get_relative_path(const char *path, mount_entry_t *mnt) {
    if (!path || !mnt) {
        return NULL;
    }
    
    const char *relative_path = path + mnt->mount_point_len;
    
    // Handle special cases
    if (*relative_path == '\0') {
        // Path is identical to mount point, use root directory
        return "/";
    } 
    else if (mnt->mount_point_len == 1 && mnt->mount_point[0] == '/') {
        // Root mount - use the original path
        return path;
    }
    
    // Otherwise return the path relative to mount point
    return relative_path;
}

/*---------------------------------------------------------------------------
 * Mount / Unmount Operations
 *---------------------------------------------------------------------------*/

/**
 * @brief Mounts the root filesystem. Special case for "/".
 * @param mp Mount point (must be "/").
 * @param fs Filesystem name.
 * @param dev Device identifier string.
 * @return 0 on success, negative error code on failure.
 */
int vfs_mount_root(const char *mp, const char *fs, const char *dev) {
    // Validate mount point
    if (!mp || strcmp(mp, "/") != 0) {
        VFS_ERROR("Root mount point must be '/', got: '%s'", mp ? mp : "NULL");
        return -FS_ERR_INVALID_PARAM;
    }
    
    // Check if root is already mounted
    if (mount_table != NULL) {
        mount_entry_t *current = mount_table;
        while (current) {
            if (strcmp(current->mount_point, "/") == 0) {
                VFS_LOG("Root filesystem already mounted as '%s', ignoring duplicate mount request", 
                        current->fs_name);
                return FS_SUCCESS;
            }
            current = current->next;
        }
        
        // Mount table exists but root isn't mounted - unusual state
        VFS_ERROR("Mount table has entries but root isn't mounted - inconsistent state");
        return -FS_ERR_UNKNOWN;
    }
    
    // Find the requested filesystem driver
    vfs_driver_t *driver = vfs_get_driver(fs);
    if (!driver) {
        VFS_ERROR("Filesystem driver '%s' not found for root mount", fs);
        return -FS_ERR_NOT_FOUND;
    }
    
    // Call the driver's mount function
    VFS_LOG("Attempting to mount device '%s' as %s at root", dev, fs);
    void *fs_context = driver->mount(dev);
    if (!fs_context) {
        VFS_ERROR("Driver '%s' failed to mount device '%s'", fs, dev);
        return -FS_ERR_MOUNT;
    }

    // Add the mount entry
    int result = add_mount_entry("/", fs, fs_context, driver);
    if (result != FS_SUCCESS) {
        // Clean up on failure
        if (driver->unmount) {
            driver->unmount(fs_context);
        }
        return result;
    }
    
    VFS_LOG("Root filesystem mounted successfully: '%s' on device '%s'", fs, dev);
    return FS_SUCCESS;
}

/**
 * @brief Unmounts the root filesystem.
 * @return 0 on success, negative error code on failure.
 */
int vfs_unmount_root(void) {
    // Find the root mount entry
    mount_entry_t *root_mount = NULL;
    mount_entry_t **prev_next = &mount_table;
    mount_entry_t *current = mount_table;
    
    while (current) {
        if (strcmp(current->mount_point, "/") == 0) {
            root_mount = current;
            break;
        }
        prev_next = &current->next;
        current = current->next;
    }

    if (!root_mount) {
        VFS_ERROR("Root filesystem not found or not mounted at '/'");
        return -FS_ERR_NOT_FOUND;
    }

    // Check if it's the only mount point
    int mount_count = 0;
    current = mount_table;
    while (current) {
        mount_count++;
        current = current->next;
    }

    if (mount_count > 1) {
        VFS_ERROR("Cannot unmount root while other filesystems are mounted (count: %d)", mount_count);
        return -FS_ERR_BUSY;
    }

    // Attempt to unmount using the driver
    int result = FS_SUCCESS;
    if (root_mount->driver && root_mount->driver->unmount) {
        result = root_mount->driver->unmount(root_mount->fs_context);
        if (result != FS_SUCCESS) {
            VFS_ERROR("Driver '%s' failed to unmount root (code: %d)", root_mount->fs_name, result);
        }
    }

    // Remove from mount table and free memory
    *prev_next = root_mount->next;
    
    // Free the mount point string that we allocated in add_mount_entry
    kfree((void*)root_mount->mount_point, root_mount->mount_point_len + 1);
    kfree(root_mount, sizeof(mount_entry_t));

    VFS_LOG("Root filesystem unmounted successfully");
    return result;
}

/**
 * @brief Lists all mounted filesystems to the terminal
 * Useful for debugging.
 */
void vfs_list_mounts(void) {
    VFS_LOG("Mounted filesystems:");
    
    if (!mount_table) {
        VFS_LOG("  (none)");
        return;
    }
    
    mount_entry_t *curr = mount_table;
    int count = 0;
    
    while (curr) {
        VFS_LOG("  %d: '%s' (%s) -> context: 0x%x", 
                ++count, curr->mount_point, curr->fs_name, (uintptr_t)curr->fs_context);
        curr = curr->next;
    }
    
    VFS_LOG("Total mount points: %d", count);
}

/**
 * @brief Shuts down the VFS layer.
 * Unmounts all mounted filesystems and frees resources.
 * @return 0 on success, negative error code on failure.
 */
int vfs_shutdown(void) {
    VFS_LOG("Shutting down VFS layer...");
    
    // Unmount all filesystems in reverse order of mounting
    // For now, just unmount root which is required to be the last mount point
    int result = vfs_unmount_root();
    
    // Reset global state
    driver_list = NULL;
    
    if (result == FS_SUCCESS) {
        VFS_LOG("VFS shutdown complete");
    } else {
        VFS_ERROR("VFS shutdown encountered errors (code: %d)", result);
    }
    
    return result;
}

/*---------------------------------------------------------------------------
 * File Operations
 *---------------------------------------------------------------------------*/

/**
 * @brief Opens a file or directory identified by path.
 * Resolves the mount point, determines the correct driver, and calls the driver's open function.
 * @param path The absolute path to the file/directory.
 * @param flags Open flags (e.g., O_RDONLY, O_CREAT - passed to driver).
 * @return Pointer to a file_t handle on success, NULL on error.
 */
file_t *vfs_open(const char *path, int flags) {
    if (!path) {
        VFS_ERROR("NULL path provided to vfs_open");
        return NULL;
    }
    
    VFS_DEBUG_LOG("Opening path '%s' with flags 0x%x", path, flags);

    // Find the mount point for this path
    mount_entry_t *mnt = find_mount_entry(path);
    if (!mnt) {
        VFS_ERROR("No mount point found for path '%s'", path);
        return NULL;
    }

    // Get the path relative to the mount point
    const char *relative_path = get_relative_path(path, mnt);
    if (!relative_path) {
        VFS_ERROR("Failed to calculate relative path for '%s'", path);
        return NULL;
    }

    VFS_DEBUG_LOG("Using mount '%s', driver '%s', relative path '%s'",
                  mnt->mount_point, mnt->fs_name, relative_path);

    // Validate driver's open function
    if (!mnt->driver || !mnt->driver->open) {
        VFS_ERROR("Driver '%s' missing or has no open function", mnt->fs_name);
        return NULL;
    }

    // Call the driver's open function
    vnode_t *node = mnt->driver->open(mnt->fs_context, relative_path, flags);
    if (!node) {
        VFS_ERROR("Driver '%s' failed to open path '%s'", mnt->fs_name, relative_path);
        return NULL;
    }

    // Allocate and initialize a file handle
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        VFS_ERROR("Failed to allocate memory for file handle");
        
        // Clean up the vnode to avoid resource leak
        if (mnt->driver->close) {
            file_t temp_file;
            temp_file.vnode = node;
            temp_file.flags = flags;
            temp_file.offset = 0;
            mnt->driver->close(&temp_file);
        } else {
            VFS_ERROR("Warning: Cannot clean up vnode after allocation failure (driver has no close function)");
        }
        
        return NULL;
    }

    // Initialize the file handle
    file->vnode = node;
    file->flags = flags;
    file->offset = 0;

    VFS_DEBUG_LOG("Successfully opened '%s' (file: 0x%x, vnode: 0x%x)",
                  path, (uintptr_t)file, (uintptr_t)node);
    return file;
}

/**
 * @brief Closes an open file handle.
 * Calls the underlying filesystem driver's close function.
 * @param file Pointer to the file_t handle.
 * @return 0 on success, negative error code on failure.
 */
int vfs_close(file_t *file) {
    if (!file) {
        VFS_ERROR("NULL file handle passed to vfs_close");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode) {
        VFS_ERROR("File handle has NULL vnode");
        kfree(file, sizeof(file_t));
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver) {
        VFS_ERROR("File vnode has NULL fs_driver");
        kfree(file, sizeof(file_t));
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver->close) {
        VFS_ERROR("Driver has no close function");
        kfree(file, sizeof(file_t));
        return -FS_ERR_NOT_SUPPORTED;
    }

    VFS_DEBUG_LOG("Closing file handle 0x%x (vnode: 0x%x)", (uintptr_t)file, (uintptr_t)file->vnode);
    
    // Call the driver's close function
    int result = file->vnode->fs_driver->close(file);
    
    // Free the file handle
    kfree(file, sizeof(file_t));
    
    if (result != FS_SUCCESS) {
        VFS_ERROR("Driver returned error %d from close operation", result);
    }
    
    return result;
}

/**
 * @brief Reads data from an open file handle.
 * Delegates the read operation to the appropriate filesystem driver.
 * @param file Pointer to the file_t handle.
 * @param buf Buffer to store the read data.
 * @param len Maximum number of bytes to read.
 * @return Number of bytes actually read, or negative error code on failure.
 */
int vfs_read(file_t *file, void *buf, size_t len) {
    // Validate parameters
    if (!file) {
        VFS_ERROR("NULL file handle passed to vfs_read");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode) {
        VFS_ERROR("File handle has NULL vnode");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver) {
        VFS_ERROR("File vnode has NULL fs_driver");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver->read) {
        VFS_ERROR("Driver has no read function");
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    if (!buf && len > 0) {
        VFS_ERROR("NULL buffer passed to vfs_read with non-zero length");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (len == 0) {
        return 0; // Nothing to do
    }

    VFS_DEBUG_LOG("Reading from file 0x%x, offset=%ld, len=%u", (uintptr_t)file, file->offset, len);
    
    // Call the driver's read function
    int bytes_read = file->vnode->fs_driver->read(file, buf, len);

    // Update the file offset on successful read
    if (bytes_read > 0) {
        file->offset += bytes_read;
        VFS_DEBUG_LOG("Read %d bytes, new offset=%ld", bytes_read, file->offset);
    } else if (bytes_read < 0) {
        VFS_ERROR("Driver returned error %d from read operation", bytes_read);
    } else {
        VFS_DEBUG_LOG("Read 0 bytes (EOF)");
    }

    return bytes_read;
}

/**
 * @brief Writes data to an open file handle.
 * Delegates the write operation to the appropriate filesystem driver.
 * @param file Pointer to the file_t handle.
 * @param buf Buffer containing the data to write.
 * @param len Number of bytes to write.
 * @return Number of bytes actually written, or negative error code on failure.
 */
int vfs_write(file_t *file, const void *buf, size_t len) {
    // Validate parameters
    if (!file) {
        VFS_ERROR("NULL file handle passed to vfs_write");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode) {
        VFS_ERROR("File handle has NULL vnode");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver) {
        VFS_ERROR("File vnode has NULL fs_driver");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver->write) {
        VFS_ERROR("Driver has no write function");
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    if (!buf && len > 0) {
        VFS_ERROR("NULL buffer passed to vfs_write with non-zero length");
        return -FS_ERR_INVALID_PARAM;
    }
    
    if (len == 0) {
        return 0; // Nothing to do
    }

    // Check write permission
    if (!(file->flags & (O_WRONLY | O_RDWR))) {
        VFS_ERROR("File not opened for writing (flags: 0x%x)", file->flags);
        return -FS_ERR_PERMISSION_DENIED;
    }

    VFS_DEBUG_LOG("Writing to file 0x%x, offset=%ld, len=%u", (uintptr_t)file, file->offset, len);
    
    // Call the driver's write function
    int bytes_written = file->vnode->fs_driver->write(file, buf, len);

    // Update the file offset on successful write
    if (bytes_written > 0) {
        file->offset += bytes_written;
        VFS_DEBUG_LOG("Wrote %d bytes, new offset=%ld", bytes_written, file->offset);
    } else if (bytes_written < 0) {
        VFS_ERROR("Driver returned error %d from write operation", bytes_written);
    } else {
        VFS_DEBUG_LOG("Wrote 0 bytes (possibly disk full)");
    }

    return bytes_written;
}

/**
 * @brief Repositions the read/write offset of an open file handle.
 * Delegates the seek operation to the appropriate filesystem driver.
 * @param file Pointer to the file_t handle.
 * @param offset Offset value.
 * @param whence Seek mode (SEEK_SET, SEEK_CUR, SEEK_END).
 * @return The resulting offset location as measured in bytes from the beginning of the file on success,
 * or a negative error code on failure.
 */
off_t vfs_lseek(file_t *file, off_t offset, int whence) {
    // Validate parameters
    if (!file) {
        VFS_ERROR("NULL file handle passed to vfs_lseek");
        return (off_t)-FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode) {
        VFS_ERROR("File handle has NULL vnode");
        return (off_t)-FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver) {
        VFS_ERROR("File vnode has NULL fs_driver");
        return (off_t)-FS_ERR_INVALID_PARAM;
    }
    
    if (!file->vnode->fs_driver->lseek) {
        VFS_ERROR("Driver has no lseek function");
        return (off_t)-FS_ERR_NOT_SUPPORTED;
    }
    
    // Validate whence parameter
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        VFS_ERROR("Invalid whence value (%d) - must be SEEK_SET, SEEK_CUR, or SEEK_END", whence);
        return (off_t)-FS_ERR_INVALID_PARAM;
    }

    VFS_DEBUG_LOG("Seeking in file 0x%x, offset=%ld, whence=%d", (uintptr_t)file, offset, whence);
    
    // Call the driver's lseek function
    off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);

    // Update the file offset on successful seek
    if (new_offset >= 0) {
        file->offset = new_offset;
        VFS_DEBUG_LOG("Seek successful, new offset=%ld", new_offset);
    } else {
        VFS_ERROR("Driver returned error %ld from lseek operation", new_offset);
    }

    return new_offset;
}

/**
 * @brief Checks if the VFS has been properly initialized with at least one driver
 * and one mount

 * @return true if VFS is ready to use, false otherwise
*/
bool vfs_is_ready(void) {
    // Check if we have at least one driver registered
    if (!driver_list) {
        return false;
    }
    
    // Check if we have at least one filesystem mounted
    if (!mount_table) {
        return false;
    }
    
    // Check specifically for root mount
    mount_entry_t *current = mount_table;
    while (current) {
        if (strcmp(current->mount_point, "/") == 0) {
            return true; // Root is mounted, VFS is ready
        }
        current = current->next;
    }
    
    return false; // Root not mounted
 }
 
 /**
 * @brief Tests filesystem functionality by attempting simple operations
 * @return 0 on success, negative error code on failure
 */
 int vfs_self_test(void) {
    VFS_LOG("Running VFS self-test...");
    
    if (!vfs_is_ready()) {
        VFS_ERROR("VFS is not ready for testing (no drivers or mounts)");
        return -FS_ERR_NOT_INIT;
    }
    
    // Try to open a file that should always exist (e.g., root directory)
    file_t *root_dir = vfs_open("/", O_RDONLY);
    if (!root_dir) {
        VFS_ERROR("Failed to open root directory");
        return -FS_ERR_IO;
    }
    
    // Close the file
    int close_result = vfs_close(root_dir);
    if (close_result != FS_SUCCESS) {
        VFS_ERROR("Failed to close root directory (code: %d)", close_result);
        return close_result;
    }
    
    VFS_LOG("VFS self-test passed");
    return FS_SUCCESS;
 }
 
 /**
 * @brief Safely checks if a path exists in the filesystem
 * @param path The path to check
 * @return true if the path exists, false otherwise
 */
 bool vfs_path_exists(const char *path) {
    if (!path) {
        return false;
    }
    
    file_t *file = vfs_open(path, O_RDONLY);
    if (!file) {
        return false;
    }
    
    vfs_close(file);
    return true;
 }
 
 /**
 * @brief Helper function that shows the current mount structure
 * Both in a human-readable format and as a tree
 */
 void vfs_debug_dump(void) {
    VFS_LOG("========== VFS DEBUG INFORMATION ==========");
    
    // List all filesystem drivers
    vfs_list_drivers();
    
    // List all mount points
    vfs_list_mounts();
    
    // Show mount tree
    VFS_LOG("Mount tree:");
    
    if (!mount_table) {
        VFS_LOG("  (empty)");
    } else {
        // Build and display a simple tree
        mount_entry_t *root = NULL;
        
        // Find root mount point first
        mount_entry_t *current = mount_table;
        while (current) {
            if (strcmp(current->mount_point, "/") == 0) {
                root = current;
                break;
            }
            current = current->next;
        }
        
        if (!root) {
            VFS_LOG("  (no root mount)");
        } else {
            VFS_LOG("  / [%s]", root->fs_name);
            
            // Display other mount points as children
            current = mount_table;
            while (current) {
                if (current != root) {
                    // Calculate indentation based on path depth
                    size_t depth = 0;
                    for (size_t i = 0; i < current->mount_point_len; i++) {
                        if (current->mount_point[i] == '/') {
                            depth++;
                        }
                    }
                    
                    // Create indentation string
                    char indent[64] = "  ";
                    for (size_t i = 0; i < depth; i++) {
                        strcat(indent, "  ");
                    }
                    
                    VFS_LOG("%s%s [%s]", indent, current->mount_point, current->fs_name);
                }
                current = current->next;
            }
        }
    }
    
    VFS_LOG("==========================================");
 }