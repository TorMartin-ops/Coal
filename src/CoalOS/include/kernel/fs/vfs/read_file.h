/** @file read_file.h */
#ifndef READ_FILE_H
#define READ_FILE_H

#include <kernel/core/types.h>

/**
 * Read a whole regular file into a kmalloc'ed buffer.
 * @param file_size set to the number of bytes read (0 on failure).
 * @return the buffer (release with kfree), or NULL if the file is missing,
 *         empty, not a regular file or could not be read completely.
 */
void *read_file(const char *path, size_t *file_size);

#endif
