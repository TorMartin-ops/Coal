/**
 * @file console_dev.c
 * @brief stdin/stdout/stderr for new processes: opens /dev/console.
 */
#include <kernel/drivers/display/console_dev.h>
#include <kernel/fs/vfs/vfs.h>

file_t *create_console_file(int mode) {
    file_t *f;
    return vfs_open("/dev/console", O_RDWR, 0, &f) == 0 ? f : NULL;
}

void console_dev_init(void) {}
