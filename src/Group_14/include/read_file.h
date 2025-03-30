#pragma once
#ifndef READ_FILE_H
#define READ_FILE_H

#include <stddef.h>         // For size_t, NULL
#include "libc/stddef.h"    // For additional definitions (if any)

/**
 * @brief Reads an entire file from disk into memory.
 *
 * In a production system, this function should interface with your filesystem driver
 * to load the file from disk into a dynamically allocated memory buffer.
 *
 * This stub implementation prints an error message and returns NULL.
 *
 * @param path      The path to the file.
 * @param file_size Output parameter to receive the file size in bytes.
 * @return Pointer to the file data, or NULL on failure.
 */
void *read_file(const char *path, size_t *file_size);

#endif // READ_FILE_H
