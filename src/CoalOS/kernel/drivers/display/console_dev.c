/**
 * @file console_dev.c
 * @brief Console device driver for standard I/O
 * 
 * Provides console device operations that bridge the terminal driver
 * with the VFS layer for stdin, stdout, and stderr file descriptors.
 */

#include <kernel/drivers/display/terminal.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <libc/stddef.h>

// Console device data structure
typedef struct console_dev_data {
    int mode;  // CONSOLE_STDIN_MODE, CONSOLE_STDOUT_MODE, etc.
} console_dev_data_t;

// Console VFS driver operations
static void *console_vfs_mount(const char *device);
static int console_vfs_unmount(void *fs_context);
static vnode_t *console_vfs_open(void *fs_context, const char *path, int flags);
static int console_vfs_read(file_t *file, void *buffer, size_t count);
static int console_vfs_write(file_t *file, const void *buffer, size_t count);
static int console_vfs_close(file_t *file);
static off_t console_vfs_lseek(file_t *file, off_t offset, int whence);

// Console VFS driver structure
static vfs_driver_t console_driver = {
    .fs_name = "console",
    .mount = console_vfs_mount,
    .unmount = console_vfs_unmount,
    .open = console_vfs_open,
    .read = console_vfs_read,
    .write = console_vfs_write,
    .close = console_vfs_close,
    .lseek = console_vfs_lseek,
    .readdir = NULL,
    .unlink = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
    .read_inode = NULL,
    .write_inode = NULL,
    .stat_inode = NULL,
    .next = NULL
};

/**
 * @brief Mount console device (dummy function for VFS compatibility)
 * @param device Device name (ignored for console)
 * @return Dummy context pointer
 */
static void *console_vfs_mount(const char *device) {
    (void)device;
    // Console doesn't need real mounting, return dummy non-NULL value
    return (void*)0x1;
}

/**
 * @brief Unmount console device (dummy function for VFS compatibility)
 * @param fs_context Filesystem context (ignored for console)
 * @return 0 (always successful)
 */
static int console_vfs_unmount(void *fs_context) {
    (void)fs_context;
    return 0;
}

/**
 * @brief Open console vnode
 */
static vnode_t *console_vfs_open(void *fs_context, const char *path, int flags) {
    (void)fs_context;
    (void)path;
    
    vnode_t *vnode = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vnode) {
        return NULL;
    }
    
    console_dev_data_t *console_data = (console_dev_data_t *)kmalloc(sizeof(console_dev_data_t));
    if (!console_data) {
        kfree(vnode);
        return NULL;
    }
    
    // Determine mode based on flags
    console_data->mode = flags & 0x003;  // Extract access mode
    
    vnode->data = console_data;
    vnode->fs_driver = &console_driver;
    
    return vnode;
}

/**
 * @brief Read from console (stdin)
 * @param file File structure
 * @param buffer Buffer to read into
 * @param count Maximum bytes to read
 * @return Number of bytes read, or negative error code
 */
static int console_vfs_read(file_t *file, void *buffer, size_t count) {
    if (!file || !file->vnode || !buffer || count == 0) {
        return -EINVAL;
    }
    
    console_dev_data_t *console_data = (console_dev_data_t *)file->vnode->data;
    if (console_data->mode != 0) {  // Not read-only
        return -EACCES;
    }
    
    // Limit read size to reasonable maximum
    if (count > 1024) {
        count = 1024;
    }
    
    char *char_buffer = (char *)buffer;
    
    // Use terminal's line-based input for now
    // This will block until a complete line is available
    size_t bytes_read = terminal_read_line_blocking(char_buffer, count);
    
    return (int)bytes_read;
}

/**
 * @brief Write to console (stdout/stderr)
 * @param file File structure
 * @param buffer Buffer to write from
 * @param count Number of bytes to write
 * @return Number of bytes written, or negative error code
 */
static int console_vfs_write(file_t *file, const void *buffer, size_t count) {
    if (!file || !file->vnode || !buffer || count == 0) {
        return 0;
    }
    
    console_dev_data_t *console_data = (console_dev_data_t *)file->vnode->data;
    if (console_data->mode == 0) {  // Read-only
        return -EACCES;
    }
    
    const char *char_buffer = (const char *)buffer;
    
    // Write each character to the terminal
    for (size_t i = 0; i < count; i++) {
        terminal_putchar(char_buffer[i]);
    }
    
    return (int)count;
}

/**
 * @brief Close console file
 * @param file File structure
 * @return 0 (always succeeds)
 */
static int console_vfs_close(file_t *file) {
    if (!file || !file->vnode) {
        return 0;
    }
    
    // Free console data
    if (file->vnode->data) {
        kfree(file->vnode->data);
        file->vnode->data = NULL;
    }
    
    // Free vnode
    kfree(file->vnode);
    file->vnode = NULL;
    
    return 0;
}

/**
 * @brief Console lseek (not supported)
 * @param file File structure
 * @param offset Offset
 * @param whence Whence
 * @return -ESPIPE (not seekable)
 */
static off_t console_vfs_lseek(file_t *file, off_t offset, int whence) {
    (void)file;
    (void)offset;
    (void)whence;
    return -ESPIPE;  // Console is not seekable
}

/**
 * @brief Create a console file structure
 * @param mode File mode (O_RDONLY for stdin, O_WRONLY for stdout/stderr)
 * @return Pointer to file_t structure, or NULL on failure
 */
file_t *create_console_file(int mode) {
    // Use VFS to open console
    vnode_t *vnode = console_vfs_open(NULL, "console", mode);
    if (!vnode) {
        return NULL;
    }
    
    file_t *console_file = (file_t *)kmalloc(sizeof(file_t));
    if (!console_file) {
        console_vfs_close(&(file_t){.vnode = vnode});
        return NULL;
    }
    
    // Initialize file structure
    memset(console_file, 0, sizeof(file_t));
    console_file->vnode = vnode;
    console_file->flags = mode;
    console_file->offset = 0;
    spinlock_init(&console_file->lock);
    
    return console_file;
}

/**
 * @brief Initialize console device
 * Called during system initialization
 */
void console_dev_init(void) {
    // Register console driver with VFS
    int result = vfs_register_driver(&console_driver);
    if (result < 0) {
        serial_printf("[Console Device] Failed to register console driver: %d\n", result);
        return;
    }
    
    serial_printf("[Console Device] Console device driver initialized\n");
}