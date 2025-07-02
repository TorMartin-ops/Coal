/**
 * @file filesystem.h
 * @brief Abstract filesystem interface for Coal OS
 * 
 * This interface follows the Interface Segregation Principle by breaking
 * down the monolithic VFS interface into smaller, focused interfaces.
 */

#ifndef COAL_INTERFACES_FILESYSTEM_H
#define COAL_INTERFACES_FILESYSTEM_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief File types
 */
typedef enum {
    FILE_TYPE_REGULAR = 0,
    FILE_TYPE_DIRECTORY,
    FILE_TYPE_SYMLINK,
    FILE_TYPE_BLOCK_DEVICE,
    FILE_TYPE_CHAR_DEVICE,
    FILE_TYPE_FIFO,
    FILE_TYPE_SOCKET,
} file_type_t;

/**
 * @brief File permissions
 */
typedef enum {
    PERM_READ    = (1 << 0),
    PERM_WRITE   = (1 << 1),
    PERM_EXECUTE = (1 << 2),
} file_permissions_t;

/**
 * @brief File information
 */
typedef struct file_info {
    file_type_t type;
    size_t size;
    uint32_t permissions;
    uint64_t created_time;
    uint64_t modified_time;
    uint64_t accessed_time;
    uint32_t inode;
    uint32_t links;
} file_info_t;

/**
 * @brief Directory entry
 */
typedef struct dir_entry {
    char name[256];
    file_info_t info;
    struct dir_entry* next;
} dir_entry_t;

/**
 * @brief File handle (opaque)
 */
typedef struct file_handle file_handle_t;

/**
 * @brief Basic file operations interface
 * 
 * Segregated interface for basic file I/O operations
 */
typedef struct file_operations {
    /**
     * @brief Open a file
     */
    error_t (*open)(const char* path, uint32_t flags, file_handle_t** handle);
    
    /**
     * @brief Close a file
     */
    error_t (*close)(file_handle_t* handle);
    
    /**
     * @brief Read from file
     */
    ssize_t (*read)(file_handle_t* handle, void* buffer, size_t size);
    
    /**
     * @brief Write to file
     */
    ssize_t (*write)(file_handle_t* handle, const void* buffer, size_t size);
    
    /**
     * @brief Seek in file
     */
    error_t (*seek)(file_handle_t* handle, off_t offset, int whence);
    
    /**
     * @brief Get current position
     */
    off_t (*tell)(file_handle_t* handle);
    
    /**
     * @brief Flush file buffers
     */
    error_t (*flush)(file_handle_t* handle);
    
} file_operations_t;

/**
 * @brief Directory operations interface
 * 
 * Segregated interface for directory operations
 */
typedef struct directory_operations {
    /**
     * @brief Create directory
     */
    error_t (*mkdir)(const char* path, uint32_t permissions);
    
    /**
     * @brief Remove directory
     */
    error_t (*rmdir)(const char* path);
    
    /**
     * @brief Open directory for reading
     */
    error_t (*opendir)(const char* path, file_handle_t** handle);
    
    /**
     * @brief Read directory entry
     */
    error_t (*readdir)(file_handle_t* handle, dir_entry_t* entry);
    
    /**
     * @brief Close directory
     */
    error_t (*closedir)(file_handle_t* handle);
    
} directory_operations_t;

/**
 * @brief File metadata operations interface
 * 
 * Segregated interface for file metadata operations
 */
typedef struct metadata_operations {
    /**
     * @brief Get file information
     */
    error_t (*stat)(const char* path, file_info_t* info);
    
    /**
     * @brief Change file permissions
     */
    error_t (*chmod)(const char* path, uint32_t permissions);
    
    /**
     * @brief Change file timestamps
     */
    error_t (*utime)(const char* path, uint64_t access_time, uint64_t modify_time);
    
    /**
     * @brief Remove file
     */
    error_t (*unlink)(const char* path);
    
    /**
     * @brief Rename/move file
     */
    error_t (*rename)(const char* old_path, const char* new_path);
    
} metadata_operations_t;

/**
 * @brief Filesystem management interface
 * 
 * Segregated interface for filesystem-level operations
 */
typedef struct filesystem_management {
    /**
     * @brief Mount filesystem
     */
    error_t (*mount)(const char* device, const char* mountpoint, const char* options);
    
    /**
     * @brief Unmount filesystem
     */
    error_t (*unmount)(const char* mountpoint);
    
    /**
     * @brief Get filesystem statistics
     */
    error_t (*statfs)(const char* path, void* stats);
    
    /**
     * @brief Sync filesystem
     */
    error_t (*sync)(void);
    
    /**
     * @brief Initialize filesystem
     */
    error_t (*init)(void);
    
    /**
     * @brief Cleanup filesystem
     */
    void (*cleanup)(void);
    
} filesystem_management_t;

/**
 * @brief Complete filesystem interface
 * 
 * Composition of all filesystem interfaces
 */
typedef struct filesystem_interface {
    const char* name;
    const file_operations_t* file_ops;
    const directory_operations_t* dir_ops;
    const metadata_operations_t* meta_ops;
    const filesystem_management_t* mgmt_ops;
    void* private_data;
} filesystem_interface_t;

/**
 * @brief Filesystem registry
 */
typedef struct filesystem_registry {
    /**
     * @brief Register filesystem
     */
    error_t (*register_fs)(filesystem_interface_t* fs);
    
    /**
     * @brief Unregister filesystem
     */
    error_t (*unregister_fs)(const char* name);
    
    /**
     * @brief Find filesystem by name
     */
    filesystem_interface_t* (*find_fs)(const char* name);
    
    /**
     * @brief Get default filesystem
     */
    filesystem_interface_t* (*get_default_fs)(void);
    
    /**
     * @brief Set default filesystem
     */
    error_t (*set_default_fs)(const char* name);
    
} filesystem_registry_t;

/**
 * @brief Global filesystem registry (dependency injection point)
 */
extern filesystem_registry_t* g_filesystem_registry;
extern filesystem_interface_t* g_root_filesystem;

/**
 * @brief Set filesystem registry
 */
void filesystem_set_registry(filesystem_registry_t* registry);
void filesystem_set_root(filesystem_interface_t* fs);

/**
 * @brief Convenience functions using injected filesystem
 */
static inline error_t fs_open(const char* path, uint32_t flags, file_handle_t** handle) {
    if (g_root_filesystem && g_root_filesystem->file_ops && g_root_filesystem->file_ops->open) {
        return g_root_filesystem->file_ops->open(path, flags, handle);
    }
    return E_NOTSUP;
}

static inline ssize_t fs_read(file_handle_t* handle, void* buffer, size_t size) {
    if (g_root_filesystem && g_root_filesystem->file_ops && g_root_filesystem->file_ops->read) {
        return g_root_filesystem->file_ops->read(handle, buffer, size);
    }
    return -E_NOTSUP;
}

static inline ssize_t fs_write(file_handle_t* handle, const void* buffer, size_t size) {
    if (g_root_filesystem && g_root_filesystem->file_ops && g_root_filesystem->file_ops->write) {
        return g_root_filesystem->file_ops->write(handle, buffer, size);
    }
    return -E_NOTSUP;
}

static inline error_t fs_close(file_handle_t* handle) {
    if (g_root_filesystem && g_root_filesystem->file_ops && g_root_filesystem->file_ops->close) {
        return g_root_filesystem->file_ops->close(handle);
    }
    return E_NOTSUP;
}

static inline error_t fs_mkdir(const char* path, uint32_t permissions) {
    if (g_root_filesystem && g_root_filesystem->dir_ops && g_root_filesystem->dir_ops->mkdir) {
        return g_root_filesystem->dir_ops->mkdir(path, permissions);
    }
    return E_NOTSUP;
}

static inline error_t fs_stat(const char* path, file_info_t* info) {
    if (g_root_filesystem && g_root_filesystem->meta_ops && g_root_filesystem->meta_ops->stat) {
        return g_root_filesystem->meta_ops->stat(path, info);
    }
    return E_NOTSUP;
}

#endif // COAL_INTERFACES_FILESYSTEM_H