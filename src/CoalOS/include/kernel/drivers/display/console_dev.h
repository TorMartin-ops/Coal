/**
 * @file console_dev.h
 * @brief Console device driver header
 */

#ifndef CONSOLE_DEV_H
#define CONSOLE_DEV_H

#include <kernel/fs/vfs/vfs.h>
#include <libc/stddef.h>

// File mode constants for console
#define CONSOLE_STDIN_MODE  0x001  // Read-only
#define CONSOLE_STDOUT_MODE 0x002  // Write-only  
#define CONSOLE_STDERR_MODE 0x002  // Write-only

/**
 * @brief Create a console file structure
 * @param mode File mode (CONSOLE_STDIN_MODE, CONSOLE_STDOUT_MODE, etc.)
 * @return Pointer to file_t structure, or NULL on failure
 */
file_t *create_console_file(int mode);

/**
 * @brief Initialize console device driver
 * Called during system initialization
 */
void console_dev_init(void);

#endif // CONSOLE_DEV_H