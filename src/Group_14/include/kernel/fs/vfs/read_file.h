#pragma once
#ifndef READ_FILE_H
#define READ_FILE_H

#include <kernel/core/types.h>

/**
 * @brief Reads an entire file from the VFS into a newly allocated buffer.
 *
 * Opens the file specified by path, determines its size, allocates memory
 * using kmalloc, reads the full content, and closes the file.
 *
 * The caller is responsible for freeing the returned buffer using kfree()
 * with the size returned in file_size.
 *
 * @param path The null-terminated path to the file within the VFS.
 * @param file_size Output parameter pointer; will be filled with the file's size
 * in bytes on success. Crucially, if a short read occurs, this reflects
 * the actual bytes read into the buffer.
 * @return Pointer to the allocated buffer containing the file data, or NULL
 * on failure (e.g., file not found, out of memory, I/O error). Returns
 * NULL for zero-byte files in this implementation.
 */
void *read_file(const char *path, size_t *file_size);

#endif // READ_FILE_H