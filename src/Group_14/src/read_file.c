#include "read_file.h"
#include "vfs.h"         // For vfs_open, vfs_read, vfs_lseek, vfs_close
#include "kmalloc.h"     // For kmalloc, kfree
#include "terminal.h"    // For logging errors
#include "fs_errno.h"    // For error codes
#include "types.h"       // For size_t, NULL, etc.
#include "sys_file.h"    // For O_RDONLY (File open flags)

/**
 * @brief Reads an entire file from the VFS into a newly allocated buffer.
 *
 * Opens the file specified by path, determines its size, allocates memory,
 * reads the full content, and closes the file. The caller is responsible
 * for freeing the returned buffer using kfree().
 *
 * @param path The null-terminated path to the file within the VFS.
 * @param file_size Output parameter pointer; will be filled with the file's size in bytes on success.
 * @return Pointer to the allocated buffer containing the file data, or NULL on failure.
 */
void *read_file(const char *path, size_t *file_size) {
    if (!path || !file_size) {
        terminal_write("[read_file] Error: Invalid parameters (path or file_size is NULL).\n");
        return NULL;
    }

    *file_size = 0; // Initialize output parameter

    // 1. Open the file using VFS
    // We need read-only access. Assuming O_RDONLY is defined correctly.
    file_t *file = vfs_open(path, O_RDONLY);
    if (!file) {
        terminal_printf("[read_file] Error: Failed to open file '%s'.\n", path);
        return NULL; // vfs_open should have printed a more specific error
    }

    // 2. Determine file size
    off_t size_or_error = vfs_lseek(file, 0, SEEK_END);
    if (size_or_error < 0) {
        terminal_printf("[read_file] Error: Failed to seek to end of file '%s' (%ld).\n", path, size_or_error);
        vfs_close(file);
        return NULL;
    }
    size_t size = (size_t)size_or_error;

    // Return to the beginning of the file
    if (vfs_lseek(file, 0, SEEK_SET) != 0) {
         terminal_printf("[read_file] Error: Failed to seek back to beginning of file '%s'.\n", path);
         vfs_close(file);
         return NULL;
    }

    if (size == 0) {
         // Handle zero-byte files: return an allocated (but empty) buffer or NULL?
         // Let's return NULL for simplicity, as ELF loader likely expects non-empty files.
         terminal_printf("[read_file] Warning: File '%s' is empty.\n", path);
         vfs_close(file);
         // Optionally, allocate a 1-byte buffer if caller expects non-NULL for empty files
         // void *empty_buf = kmalloc(1); if (empty_buf) { ((char*)empty_buf)[0] = '\0'; return empty_buf; }
         return NULL;
    }

    // 3. Allocate buffer
    void *buffer = kmalloc(size);
    if (!buffer) {
        terminal_printf("[read_file] Error: Failed to allocate %d bytes for file '%s'.\n", size, path);
        vfs_close(file);
        return NULL;
    }

    // 4. Read file content
    int bytes_read = vfs_read(file, buffer, size);
    if (bytes_read < 0) {
        terminal_printf("[read_file] Error: Failed to read file '%s' (%d).\n", path, bytes_read);
        kfree(buffer); // Free allocated buffer on error
        vfs_close(file);
        return NULL;
    }
    if ((size_t)bytes_read != size) {
        // This shouldn't happen if lseek worked correctly, but check anyway
        terminal_printf("[read_file] Warning: Short read on file '%s' (expected %d, got %d).\n", path, size, bytes_read);
        // Proceeding with potentially truncated data, but update file_size
        size = (size_t)bytes_read;
        // Optionally: kfree(buffer, original_size); kmalloc smaller buffer; re-read? Or just fail?
        // For simplicity, we proceed but the caller should be aware via file_size.
    }

    // 5. Close file
    vfs_close(file); // Ignore close error for now?

    // 6. Update output parameter and return buffer
    *file_size = size;
    terminal_printf("[read_file] Successfully read %d bytes from '%s'.\n", size, path);
    return buffer;
}