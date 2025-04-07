#include "vfs.h"
#include "kmalloc.h"
#include "terminal.h"
#include "string.h"
#include "types.h"
#include "sys_file.h"
#include "fs_errno.h" // Include for FS_ERR_* codes

/* Define SEEK macros if not already defined elsewhere (e.g., types.h or sys_file.h) */
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
    const char *mount_point;      // The path where the filesystem is mounted (e.g., "/")
    const char *fs_name;          // Name of the filesystem driver (e.g., "FAT32")
    void *fs_context;             // Filesystem-specific data returned by driver->mount()
    vfs_driver_t *driver;         // Pointer to the registered driver for this filesystem
    struct mount_entry *next;     // Next entry in the linked list of mounts
    size_t mount_point_len;       // Pre-calculated length of mount_point for efficiency
} mount_entry_t;

/* Global mount table head - Linked list of active mounts */
// TODO: Add locking (e.g., spinlock) for SMP safety if modifying concurrently
static mount_entry_t *mount_table = NULL;

/* Global driver list head - Linked list of registered filesystem drivers */
// TODO: Add locking (e.g., spinlock) for SMP safety if modifying concurrently
static vfs_driver_t *driver_list = NULL;

/*---------------------------------------------------------------------------
 * VFS Initialization and Driver Registration
 *---------------------------------------------------------------------------*/

/**
 * @brief Initializes the Virtual File System layer.
 * Sets up internal structures (clears driver and mount lists).
 */
void vfs_init(void) {
    // TODO: Acquire locks if SMP
    mount_table = NULL;
    driver_list = NULL;
    // TODO: Release locks if SMP
    terminal_write("[VFS] Initialized.\n");
}

/**
 * @brief Registers a filesystem driver with the VFS.
 * Adds the driver to the global linked list of available drivers.
 * @param driver Pointer to the vfs_driver_t structure to register.
 * @return 0 on success, negative error code on failure.
 */
int vfs_register_driver(vfs_driver_t *driver) {
    if (!driver || !driver->fs_name || !driver->mount || !driver->open) {
        terminal_write("[VFS] Error: Attempted to register invalid driver (NULL or missing functions).\n");
        return -FS_ERR_INVALID_PARAM;
    }

    // TODO: Acquire driver_list lock if SMP
    // Check if driver with the same name already exists
    vfs_driver_t *current = driver_list;
    while (current) {
        if (strcmp(current->fs_name, driver->fs_name) == 0) {
            terminal_printf("[VFS] Error: Driver '%s' already registered.\n", driver->fs_name);
            // TODO: Release lock
            return -FS_ERR_FILE_EXISTS; // Use a suitable error code
        }
        current = current->next;
    }

    // Add to the front of the list
    driver->next = driver_list;
    driver_list = driver;
    // TODO: Release lock

    terminal_printf("[VFS] Registered driver: %s\n", driver->fs_name);
    return FS_SUCCESS;
}

/**
 * @brief Unregisters a filesystem driver from the VFS.
 * @param driver Pointer to the driver to unregister.
 * @return 0 on success, negative error code on failure.
 */
int vfs_unregister_driver(vfs_driver_t *driver) {
    if (!driver) {
        return -FS_ERR_INVALID_PARAM;
    }

    // TODO: Acquire driver_list lock if SMP
    vfs_driver_t **prev = &driver_list;
    vfs_driver_t *curr = driver_list;
    while (curr) {
        if (curr == driver) {
            *prev = curr->next; // Remove from list
            // TODO: Release lock
            terminal_printf("[VFS] Unregistered driver: %s\n", driver->fs_name);
            return FS_SUCCESS;
        }
        prev = &curr->next;
        curr = curr->next;
    }
    // TODO: Release lock

    terminal_printf("[VFS] Error: Driver %s not found for unregistration.\n", driver->fs_name);
    return -FS_ERR_NOT_FOUND;
}

/**
 * @brief Retrieves a registered filesystem driver by name.
 * @param fs_name The name of the filesystem driver (e.g., "FAT32").
 * @return Pointer to the driver structure, or NULL if not found.
 */
vfs_driver_t *vfs_get_driver(const char *fs_name) {
    if (!fs_name) return NULL;

    // TODO: Acquire driver_list lock (read lock if available) if SMP
    vfs_driver_t *curr = driver_list;
    while (curr) {
        if (strcmp(curr->fs_name, fs_name) == 0) {
            // TODO: Release lock
            return curr;
        }
        curr = curr->next;
    }
    // TODO: Release lock

    terminal_printf("[VFS] Driver '%s' not found.\n", fs_name);
    return NULL;
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
    if (!mp || !fs || !ctx || !drv) return -FS_ERR_INVALID_PARAM;

    mount_entry_t *new_mount = (mount_entry_t *)kmalloc(sizeof(mount_entry_t));
    if (!new_mount) {
        terminal_write("[VFS] Error: Failed to allocate memory for mount entry.\n");
        return -FS_ERR_OUT_OF_MEMORY;
    }

    // *** FIX: Ensure mount_point is dynamically allocated or guaranteed to persist ***
    // Simple approach: assume mp is persistent (like string literal "/")
    // Better approach: Allocate and copy mp if it might be temporary.
    // char* mp_copy = kmalloc(strlen(mp) + 1); // Example if copy needed
    // if (!mp_copy) { kfree(new_mount, sizeof(mount_entry_t)); return -FS_ERR_OUT_OF_MEMORY; }
    // strcpy(mp_copy, mp);
    // new_mount->mount_point = mp_copy;

    new_mount->mount_point = mp; // Current code assumes mp persists
    new_mount->fs_name = fs;     // Assuming fs is persistent
    new_mount->fs_context = ctx;
    new_mount->driver = drv;
    new_mount->mount_point_len = strlen(mp); // Cache the length

    // TODO: Acquire mount_table lock if SMP
    // Add to front of mount list
    new_mount->next = mount_table;
    mount_table = new_mount;
    // TODO: Release lock

    terminal_printf("[VFS] Added mount: '%s' -> %s (context: 0x%x)\n", mp, fs, (uintptr_t)ctx);
    return FS_SUCCESS;
}

/**
 * @brief Finds the most specific mount entry corresponding to a given path.
 * Implements the longest-prefix match algorithm. "/" matches everything if it's the root mount.
 * @param path The absolute path to resolve.
 * @return Pointer to the best matching mount_entry_t, or NULL if no suitable mount point found.
 */
static mount_entry_t *find_mount_entry(const char *path) {
    if (!path || path[0] != '/') {
        terminal_printf("[VFS] find_mount_entry: Error - Path is NULL or not absolute: '%s'\n", path ? path : "NULL");
        return NULL; // VFS paths must be absolute
    }

    mount_entry_t *best_match = NULL;
    size_t best_len = 0; // Use 0 to allow root "/" (len 1) to be initial best match

    // TODO: Acquire mount_table lock (read lock if available) if SMP
    mount_entry_t *curr = mount_table;
    while (curr) {
        // Check if path starts with the mount point
        // Example: path="/home/user/file.txt", mount_point="/home"
        // Example: path="/kernel.bin", mount_point="/"
        if (strncmp(path, curr->mount_point, curr->mount_point_len) == 0) {

            // --- FIX: Logic to correctly match root and subdirectories ---
            bool is_exact_match = (path[curr->mount_point_len] == '\0');
            bool is_subdir_match = (path[curr->mount_point_len] == '/');
            bool is_root_mount = (curr->mount_point_len == 1 && curr->mount_point[0] == '/');

            // A valid match occurs if:
            // 1. The path exactly matches the mount point.
            // 2. The path starts with the mount point AND the next char is '/'.
            // 3. The mount point is "/" (special case, matches any absolute path).
            if (is_exact_match || is_subdir_match || is_root_mount) {
                 // This is a potential match. Is it better (longer prefix) than the current best?
                 // Special case: If current best match is root "/", and we find a longer match, prefer the longer one.
                 if (curr->mount_point_len >= best_len) {
                    // If lengths are equal, only overwrite if the new one isn't root OR the old one was NULL
                    // This prevents "/home" overwriting "/" if path is "/home"
                    if (curr->mount_point_len > best_len || !best_match) {
                         best_match = curr;
                         best_len = curr->mount_point_len;
                    } else if (best_match && best_len == 1 && best_match->mount_point[0] == '/') {
                         // If current best is root and we find another length 1 mount (shouldn't happen?), keep root.
                         // If we find a non-root length 1 mount (e.g. "/a") and path is "/a/b", prefer "/a".
                         if (!is_root_mount) { // Prefer non-root if lengths are both 1
                              best_match = curr;
                         }
                    }
                 }
            }
            // --- End FIX ---
        }
        curr = curr->next;
    }
    // TODO: Release lock

    if (best_match) {
        terminal_printf("[VFS DEBUG] find_mount_entry: Path '%s' best matched mount point '%s'\n", path, best_match->mount_point);
    } else {
        terminal_printf("[VFS DEBUG] find_mount_entry: No matching mount point found for path '%s'.\n", path);
    }
    return best_match;
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
    if (!mp || strcmp(mp, "/") != 0) {
        terminal_write("[VFS] Error: Root mount point must be '/'.\n");
        return -FS_ERR_INVALID_PARAM;
    }
    // *** FIX: Allow re-mounting root if necessary? Or maybe not for simplicity. ***
    // Let's keep the original check for now.
    // TODO: Acquire mount_table lock
    if (mount_table != NULL) {
        // Check if the existing root mount is actually "/". Should be, but check anyway.
        if(strcmp(mount_table->mount_point, "/") == 0) {
             terminal_write("[VFS] Warning: Root filesystem already mounted. Ignoring.\n");
            // TODO: Release lock
             return FS_SUCCESS; // Or an error? Let's treat as non-fatal for now.
        }
        // If mount_table is not NULL but isn't root, that's an inconsistent state.
        terminal_write("[VFS] Error: Mount table exists but root is not mounted? Inconsistent state.\n");
        // TODO: Release lock
        return -FS_ERR_UNKNOWN;
    }
    // TODO: Release mount_table lock if acquired

    vfs_driver_t *driver = vfs_get_driver(fs);
    if (!driver) {
        terminal_printf("[VFS] Error: Filesystem driver '%s' not found for root mount.\n", fs);
        return -FS_ERR_NOT_FOUND;
    }

    void *fs_context = driver->mount(dev);
    if (!fs_context) {
        terminal_printf("[VFS] Error: Driver '%s' failed to mount device '%s' for root.\n", fs, dev);
        return -FS_ERR_MOUNT;
    }

    // Use "/" directly as it's a literal and persists.
    return add_mount_entry("/", fs, fs_context, driver);
}

/**
 * @brief Unmounts the root filesystem.
 * @return 0 on success, negative error code on failure.
 */
int vfs_unmount_root(void) {
    // TODO: Acquire mount_table lock if SMP
    mount_entry_t *root_mount = NULL;
    mount_entry_t *current = mount_table;
    mount_entry_t **prev_next = &mount_table;

    // Find the root mount entry specifically
    while(current) {
        if (strcmp(current->mount_point, "/") == 0) {
            root_mount = current;
            break;
        }
        prev_next = &current->next;
        current = current->next;
    }

    if (!root_mount) {
         terminal_write("[VFS] Error: Root filesystem not found or not mounted at '/'.\n");
         // TODO: Release lock
         return -FS_ERR_NOT_FOUND;
    }

    // Check if it's the only mount point
    // Count entries first to be sure
    int count = 0;
    current = mount_table;
    while(current) { count++; current = current->next; }

    if (count > 1) {
        terminal_write("[VFS] Error: Cannot unmount root while other filesystems are mounted.\n");
         // TODO: Release lock
        return -FS_ERR_UNKNOWN; // Or EBUSY
    }

    int result = FS_SUCCESS;
    if (root_mount->driver && root_mount->driver->unmount) {
        result = root_mount->driver->unmount(root_mount->fs_context);
        if (result != FS_SUCCESS) {
            terminal_printf("[VFS] Error: Driver '%s' failed to unmount root. Error: %d\n", root_mount->fs_name, result);
            // Don't free resources if driver unmount failed? Or proceed? Proceeding might leak fs_context.
        }
    }

    // Remove from table and free memory even if driver unmount failed? Decide policy.
    *prev_next = root_mount->next; // Remove from list (should set mount_table to NULL if it was the only one)
    // *** FIX: Need to free the mount_point string if it was dynamically allocated ***
    // if (root_mount->mount_point was allocated) kfree(root_mount->mount_point, ...);
    kfree(root_mount, sizeof(mount_entry_t)); // Free the mount_entry_t struct itself
    // TODO: Release lock

    terminal_write("[VFS] Unmounted root filesystem.\n");
    return result; // Return result from driver unmount
}


/**
 * @brief Shuts down the VFS layer.
 * Currently, just unmounts the root. Should ideally unmount all filesystems.
 * @return 0 on success, negative error code if root unmount fails.
 */
int vfs_shutdown(void) {
    terminal_write("[VFS] Shutting down...\n");
    // TODO: Implement unmounting of all filesystems in reverse order?
    return vfs_unmount_root();
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
        terminal_write("[VFS] vfs_open: Error - NULL path provided.\n");
        return NULL;
    }
    terminal_printf("[VFS] vfs_open: Attempting to open path '%s' with flags 0x%x\n", path, flags);

    mount_entry_t *mnt = find_mount_entry(path);
    if (!mnt) {
        // Error already printed by find_mount_entry
        return NULL;
    }

    // Calculate the path relative to the mount point
    const char *relative_path = path + mnt->mount_point_len;

    // --- FIX: Ensure relative path starts correctly ---
    // If mount point is "/" (len 1), relative_path currently points to "kernel.bin"
    // If mount point is "/boot" (len 5) and path is "/boot/kernel.bin", relative_path points to "/kernel.bin"
    // If path is exactly the mount point (e.g. "/"), relative_path points to "" (null terminator)
    // Filesystems generally expect "/" as the root of *their* filesystem.
    if (*relative_path == '\0') {
        // Path is identical to mount point, pass "/" as relative path to FS driver
        relative_path = "/";
    } else if (*relative_path != '/' && !(mnt->mount_point_len == 1 && mnt->mount_point[0] == '/')) {
        // If mount point wasn't root, and relative path doesn't start with '/',
        // it means path was like "/mnt/dir" matching mount "/mnt". Relative should be "/dir".
        // This case seems less likely with the improved find_mount_entry logic but handle defensively.
        // The current pointer should already be correct: path="/mnt/dir", mnt="/mnt", rel="/dir"
        // Let's assume drivers handle both "/file" and "file" relative to their root.
        // No change needed here based on improved find_mount_entry.
    } else if (mnt->mount_point_len == 1 && mnt->mount_point[0] == '/') {
         // Mount point is root "/". Path is "/file". Relative path currently points to "file".
         // Let's pass the original path (which starts with '/') to the driver.
         relative_path = path;
    }
     // --- End FIX ---


    terminal_printf("[VFS] vfs_open: Using mount '%s', driver '%s', relative path '%s'\n",
                    mnt->mount_point, mnt->fs_name, relative_path);

    // Validate driver and open function pointer
    if (!mnt->driver || !mnt->driver->open) {
        terminal_printf("[VFS] vfs_open: Driver '%s' missing or no open function pointer.\n", mnt->fs_name);
        return NULL;
    }

    // Call the driver's open function
    vnode_t *node = mnt->driver->open(mnt->fs_context, relative_path, flags);
    if (!node) {
        terminal_printf("[VFS] vfs_open: Driver '%s' failed to open relative path '%s'.\n", mnt->fs_name, relative_path);
        return NULL; // Driver should have logged specific error
    }

    // Allocate the VFS file handle
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file) {
        terminal_write("[VFS] vfs_open: Failed to allocate memory for file_t.\n");
        // Need to close the vnode if the driver provided one but we can't create file_t
        if (mnt->driver->close) {
            // Create a temporary file_t just for closing the vnode
            file_t temp_file;
            temp_file.vnode = node;
            temp_file.flags = flags;
            temp_file.offset = 0;
            mnt->driver->close(&temp_file); // Ignore close error here?
        } else {
            // Cannot close vnode, potential resource leak in driver
            terminal_printf("[VFS] vfs_open: Warning - cannot close vnode after file_t alloc failure (no close function in driver %s).\n", mnt->fs_name);
        }
        return NULL;
    }

    // Populate the file handle
    file->vnode = node;
    file->flags = flags;
    file->offset = 0; // Files always start at offset 0

    terminal_printf("[VFS] vfs_open: Success for path '%s'. file=0x%x, vnode=0x%x\n", path, (uintptr_t)file, (uintptr_t)node);
    return file;
}

/**
 * @brief Closes an open file handle.
 * Calls the underlying filesystem driver's close function.
 * @param file Pointer to the file_t handle.
 * @return 0 on success, negative error code on failure.
 */
int vfs_close(file_t *file) {
    if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->close) {
        terminal_write("[VFS] vfs_close: Invalid file handle or driver/close function missing.\n");
        // If file is valid but driver stuff isn't, should we still free file_t? Yes.
        if (file) kfree(file, sizeof(file_t));
        return -FS_ERR_INVALID_PARAM;
    }

    terminal_printf("[VFS] vfs_close: Closing file handle 0x%x (vnode 0x%x)\n", (uintptr_t)file, (uintptr_t)file->vnode);
    int result = file->vnode->fs_driver->close(file); // Driver is responsible for freeing vnode->data and vnode itself

    // Free the VFS file handle structure itself AFTER calling driver close
    kfree(file, sizeof(file_t));

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
    if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->read || !buf) {
         terminal_write("[VFS] vfs_read: Invalid parameter (file, vnode, driver, read func, or buf is NULL).\n");
        return -FS_ERR_INVALID_PARAM;
    }
    if (len == 0) return 0;

    // Check if file was opened with read permission (implicit in O_RDONLY/O_RDWR)
    // VFS layer doesn't store original open flags reliably here, driver should check if needed.

    terminal_printf("[VFS DEBUG] vfs_read: file=0x%x, offset=%ld, len=%u\n", (uintptr_t)file, file->offset, len);
    int bytes_read = file->vnode->fs_driver->read(file, buf, len);

    // Update offset only if read was successful (bytes_read >= 0)
    if (bytes_read > 0) {
        file->offset += bytes_read;
        terminal_printf("[VFS DEBUG] vfs_read: Read %d bytes, new offset=%ld\n", bytes_read, file->offset);
    } else if (bytes_read < 0) {
        terminal_printf("[VFS] vfs_read: Driver returned error %d.\n", bytes_read);
    } else {
         terminal_printf("[VFS DEBUG] vfs_read: Read 0 bytes (EOF).\n");
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
     if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->write || !buf) {
        terminal_write("[VFS] vfs_write: Invalid parameter (file, vnode, driver, write func, or buf is NULL).\n");
        return -FS_ERR_INVALID_PARAM;
    }
     if (len == 0) return 0;

     // Check if file was opened with write permission (O_WRONLY or O_RDWR)
     if (!(file->flags & (O_WRONLY | O_RDWR))) {
        terminal_write("[VFS] vfs_write: File not opened for writing.\n");
        return -FS_ERR_PERMISSION_DENIED; // Or EBADF
     }

    terminal_printf("[VFS DEBUG] vfs_write: file=0x%x, offset=%ld, len=%u\n", (uintptr_t)file, file->offset, len);
    int bytes_written = file->vnode->fs_driver->write(file, buf, len);

     // Update offset only if write was successful (bytes_written > 0)
    if (bytes_written > 0) {
        file->offset += bytes_written;
        terminal_printf("[VFS DEBUG] vfs_write: Wrote %d bytes, new offset=%ld\n", bytes_written, file->offset);
    } else if (bytes_written < 0) {
         terminal_printf("[VFS] vfs_write: Driver returned error %d.\n", bytes_written);
    } else {
         // Writing 0 bytes might indicate an issue (e.g., no space), but isn't strictly an error return value.
         terminal_printf("[VFS DEBUG] vfs_write: Wrote 0 bytes.\n");
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
    if (!file || !file->vnode || !file->vnode->fs_driver || !file->vnode->fs_driver->lseek) {
        terminal_write("[VFS] vfs_lseek: Invalid parameter or driver/lseek function missing.\n");
        return (off_t)-FS_ERR_INVALID_PARAM; // Cast error code to off_t
    }

    // Validate whence
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        terminal_printf("[VFS] vfs_lseek: Invalid whence value (%d).\n", whence);
        return (off_t)-FS_ERR_INVALID_PARAM;
    }

    terminal_printf("[VFS DEBUG] vfs_lseek: file=0x%x, offset=%ld, whence=%d\n", (uintptr_t)file, offset, whence);
    off_t new_offset = file->vnode->fs_driver->lseek(file, offset, whence);

    if (new_offset >= 0) {
        file->offset = new_offset; // Update VFS file offset on success
        terminal_printf("[VFS DEBUG] vfs_lseek: Success, new offset=%ld\n", new_offset);
    } else {
        terminal_printf("[VFS] vfs_lseek: Driver returned error %ld.\n", new_offset);
    }

    return new_offset; // Return result from driver (could be error code)
}