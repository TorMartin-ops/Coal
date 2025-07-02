// --- Improved read_file.c ---

#include <kernel/fs/vfs/read_file.h>
#include <kernel/fs/vfs/vfs.h>         // For vfs_* functions
#include <kernel/memory/kmalloc.h>     // For kmalloc, kfree
#include <kernel/drivers/display/terminal.h>    // For terminal_printf (used by logging macros)
#include <kernel/fs/vfs/fs_errno.h>    // For error codes (used in logging)
#include <kernel/core/types.h>       // For size_t, NULL, off_t
#include <kernel/fs/vfs/sys_file.h>    // For O_RDONLY
#include <libc/limits.h>      // For INT_MAX (used indirectly via types if size_t maps)

// --- Basic Logging Macros (Ideally move to a dedicated log.h) ---
// Define KLOG_LEVEL_DEBUG or similar during build to enable debug logs
#ifdef KLOG_LEVEL_DEBUG
#define LOG_DEBUG(fmt, ...) terminal_printf("[read_file:DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) do {} while(0) // Disabled if not KLOG_LEVEL_DEBUG
#endif
#define LOG_INFO(fmt, ...)  terminal_printf("[read_file:INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  terminal_printf("[read_file:WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) terminal_printf("[read_file:ERROR] " fmt "\n", ##__VA_ARGS__)
// --- End Logging Macros ---

// Define a reasonable maximum file size to prevent huge allocations (e.g., 128 MB)
// Adjust this based on your system's capabilities and expected use cases.
#define MAX_READ_FILE_SIZE (128 * 1024 * 1024)


/**
 * @brief Reads an entire file from the VFS into a newly allocated buffer.
 *
 * This function provides enhanced logging and more robust error handling compared
 * to a basic implementation. It handles opening, sizing, allocating, reading,
 * and closing the specified file.
 *
 * - The caller is responsible for freeing the returned buffer using kfree().
 * - For zero-byte files, this function returns a non-NULL pointer (minimal
 * 1-byte allocation) but sets *file_size to 0. Callers MUST check the
 * returned size.
 * - Returns NULL if any error occurs (invalid path, open failed, seek failed,
 * size exceeds limit, allocation failed, read failed, short read).
 *
 * @param path The null-terminated path to the file within the VFS. Must not be NULL.
 * @param file_size Output parameter pointer; filled with the file's size in bytes
 * on success (will be 0 for zero-byte files). Must not be NULL.
 * @return Pointer to the allocated buffer containing the file data,
 * or NULL on any failure.
 */
void *read_file(const char *path, size_t *file_size) {
    LOG_DEBUG("Enter: path='%s', file_size_ptr=%p", path ? path : "NULL", file_size);

    void *buffer = NULL;
    file_t *file = NULL;
    off_t size_check = -1;
    size_t size = 0;
    int ret = 0; // Generic return code variable

    // 1. Validate parameters
    if (!path || !file_size) {
        LOG_ERROR("Invalid parameters: path=%p, file_size_ptr=%p", path, file_size);
        // No resources to clean up yet
        return NULL;
    }
    *file_size = 0; // Initialize output parameter defensively

    // 2. Open the file via VFS
    LOG_DEBUG("Attempting to open '%s' with O_RDONLY", path);
    file = vfs_open(path, O_RDONLY);
    if (!file) {
        // vfs_open should log the specific error, but we add context
        LOG_ERROR("vfs_open failed for '%s'.", path);
        goto cleanup; // file is already NULL
    }
    LOG_DEBUG("vfs_open succeeded for '%s'. file=%p", path, file);

    // 3. Determine file size by seeking to the end
    LOG_DEBUG("Seeking to end of file '%s' (file=%p)", path, file);
    size_check = vfs_lseek(file, 0, SEEK_END);
    if (size_check < 0) {
        LOG_ERROR("vfs_lseek(SEEK_END) failed for '%s'. Error code: %ld", path, size_check);
        goto cleanup;
    }
    size = (size_t)size_check;
    LOG_INFO("Determined size for '%s' is %d bytes.", path, size);

    // 4. Check against maximum allowed size
    if (size > MAX_READ_FILE_SIZE) {
         LOG_ERROR("File '%s' size (%d bytes) exceeds MAX_READ_FILE_SIZE (%d bytes).",
                   path, size, MAX_READ_FILE_SIZE);
         goto cleanup;
    }

    // 5. Seek back to the beginning
    LOG_DEBUG("Seeking to beginning of file '%s' (file=%p)", path, file);
    ret = vfs_lseek(file, 0, SEEK_SET);
    if (ret != 0) {
        LOG_ERROR("vfs_lseek(SEEK_SET) failed for '%s'. Error code: %d", path, ret);
        goto cleanup;
    }
    LOG_DEBUG("Seek to beginning successful for '%s'.", path);

    // 6. Handle zero-byte file case
    if (size == 0) {
        LOG_WARN("File '%s' has size 0. Returning minimal buffer.", path);
        // Allocate 1 byte to return a non-NULL pointer, clearly indicating
        // "empty file found" rather than an error condition.
        // The caller MUST check *file_size.
        buffer = kmalloc(1); // Allocate minimal buffer
        if (!buffer) {
             LOG_ERROR("Failed to allocate minimal 1-byte buffer for zero-sized file '%s'.", path);
             // Although size is 0, kmalloc still failed, treat as error.
             goto cleanup;
        }
        // Size is already 0, *file_size will be set at the end.
        goto cleanup; // Skip the read, proceed to close and return.
    }

    // 7. Allocate buffer for file content
    LOG_DEBUG("Allocating %d bytes for '%s'.", size, path);
    buffer = kmalloc(size);
    if (!buffer) {
        LOG_ERROR("kmalloc failed to allocate %d bytes for file '%s'.", size, path);
        goto cleanup;
    }
    LOG_DEBUG("Allocation successful for '%s', buffer=%p.", path, buffer);

    // 8. Read the entire file content
    LOG_DEBUG("Reading %d bytes from '%s' into buffer %p.", size, path, buffer);
    ret = vfs_read(file, buffer, size);
    if (ret < 0) {
        LOG_ERROR("vfs_read failed for '%s'. Error code: %d", path, ret);
        goto cleanup; // Buffer will be freed in cleanup
    }
    LOG_DEBUG("vfs_read returned %d.", ret);

    // 9. Check if the number of bytes read matches the expected size
    if ((size_t)ret != size) {
        LOG_ERROR("Short read detected for '%s'! Expected %d bytes, but vfs_read returned %d.",
                  path, size, ret);
        // Treat short read as an error in this context
        size = 0; // Ensure caller sees size 0 on error
        goto cleanup; // Buffer will be freed
    }
    LOG_DEBUG("Successfully read %d bytes from '%s'.", size, path);


// --- Cleanup Section ---
cleanup:
    // This section is reached on success *or* failure after file opening.
    // Resources are cleaned up based on whether they were successfully acquired.

    if (file) {
        LOG_DEBUG("Closing file '%s' (file=%p).", path, file);
        // We could check the return value of vfs_close, but often ignored unless
        // tracking specific descriptor leaks or write-back caching issues.
        vfs_close(file);
    }

    // Check if we are returning successfully (buffer allocated or zero-size handled)
    // and no intermediate error occurred that jumped to cleanup *before* success.
    // Success conditions:
    // 1) Size was 0, minimal buffer was allocated.
    // 2) Size > 0, buffer allocated, read successful and matched size.
    // Failure conditions that reach here will have buffer == NULL or size == 0 (from error).
    bool success = (size == 0 && buffer != NULL) || // Case 1: Successful read of zero-byte file
                   (size > 0 && buffer != NULL && (size_t)ret == size); // Case 2: Successful read of non-zero file

    if (success) {
        *file_size = size; // Set the final size for the caller
        LOG_INFO("Successfully prepared buffer for '%s'. size=%d, buffer=%p", path, size, buffer);
        LOG_DEBUG("Exit: Returning buffer %p", buffer);
        return buffer;
    } else {
        // An error occurred, ensure buffer is freed if it was allocated
        if (buffer) {
            LOG_DEBUG("Freeing buffer %p due to error.", buffer);
            kfree(buffer);
            buffer = NULL; // Ensure NULL is returned
        }
        *file_size = 0; // Ensure size is 0 on error exit
        LOG_WARN("Exiting with error for path '%s'. Returning NULL.", path);
        LOG_DEBUG("Exit: Returning NULL");
        return NULL;
    }
}

// --- End of read_file.c ---