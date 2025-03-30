#include "read_file.h"
#include "terminal.h"

/*
 * read_file: Stub implementation.
 *
 * In a production OS, implement file I/O here to load a file from disk.
 * This stub prints an error message and returns NULL.
 */
void *read_file(const char *path, size_t *file_size) {
    terminal_write("read_file: Filesystem interface not implemented for path: ");
    terminal_write(path);
    terminal_write("\n");
    
    if (file_size != NULL) {
        *file_size = 0;
    }
    
    return NULL;
}
