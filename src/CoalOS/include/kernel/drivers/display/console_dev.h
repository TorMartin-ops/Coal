/**
 * @file console_dev.h
 * @brief /dev/console: VFS glue between file descriptors and the tty/terminal.
 */
#ifndef CONSOLE_DEV_H
#define CONSOLE_DEV_H

#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/sys_file.h> /* O_RDONLY, O_WRONLY */

#define CONSOLE_STDIN_MODE  O_RDONLY
#define CONSOLE_STDOUT_MODE O_WRONLY
#define CONSOLE_STDERR_MODE O_WRONLY

/** Creates an open file on the console with the given O_* access mode. */
file_t *create_console_file(int flags);

/** Registers the console with the VFS and hooks keyboard input up to the tty. */
void console_dev_init(void);

#endif /* CONSOLE_DEV_H */
