/**
 * @file vfs_page_io.c
 * @brief VFS Page I/O Support Functions for Page Cache
 * @author Coal OS Development Team
 * @version 1.0
 * 
 * @details Implements VFS-level read/write functions that work with
 * device and inode numbers for page cache integration.
 */

#include <kernel/fs/vfs/vfs.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/fs/vfs/mount_table.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>
#include <sys/stat.h>  // For struct stat

// Logging macros
#define VFS_PAGE_ERROR(fmt, ...) serial_printf("[VFS Page ERROR] %s:%d: " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#define VFS_PAGE_DEBUG(fmt, ...) serial_printf("[VFS Page DEBUG] " fmt "\n", ##__VA_ARGS__)

/**
 * @brief Read data at a specific offset using device and inode numbers
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param offset Offset within the file
 * @param buffer Buffer to read into
 * @param size Number of bytes to read
 * @return Number of bytes read or negative error code
 */
ssize_t vfs_read_at(uint32_t device_id, uint32_t inode_number,
                    uint64_t offset, void *buffer, size_t size) {
    if (!buffer || size == 0) {
        return -FS_ERR_INVALID_PARAM;
    }
    
    // Find the mount point for this device
    mount_t *mnt = mount_find_by_device_id(device_id);
    if (!mnt) {
        VFS_PAGE_ERROR("Device ID %u not mounted", device_id);
        return -FS_ERR_NOT_FOUND;
    }
    
    // Get the driver
    vfs_driver_t *driver = vfs_get_driver(mnt->fs_type);
    if (!driver) {
        VFS_PAGE_ERROR("No driver for filesystem type %s", mnt->fs_type);
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Check if driver supports inode-based operations
    if (!driver->read_inode) {
        VFS_PAGE_ERROR("Driver does not support inode-based read");
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Perform the read
    ssize_t result = driver->read_inode(mnt->fs_context, inode_number, 
                                       offset, buffer, size);
    
    if (result < 0) {
        VFS_PAGE_DEBUG("Read failed for inode %u: error %d", inode_number, result);
    }
    
    return result;
}

/**
 * @brief Write data at a specific offset using device and inode numbers
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param offset Offset within the file
 * @param buffer Buffer containing data to write
 * @param size Number of bytes to write
 * @return Number of bytes written or negative error code
 */
ssize_t vfs_write_at(uint32_t device_id, uint32_t inode_number,
                     uint64_t offset, const void *buffer, size_t size) {
    if (!buffer || size == 0) {
        return -FS_ERR_INVALID_PARAM;
    }
    
    // Find the mount point for this device
    mount_t *mnt = mount_find_by_device_id(device_id);
    if (!mnt) {
        VFS_PAGE_ERROR("Device ID %u not mounted", device_id);
        return -FS_ERR_NOT_FOUND;
    }
    
    // Get the driver
    vfs_driver_t *driver = vfs_get_driver(mnt->fs_type);
    if (!driver) {
        VFS_PAGE_ERROR("No driver for filesystem type %s", mnt->fs_type);
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Check if driver supports inode-based operations
    if (!driver->write_inode) {
        VFS_PAGE_ERROR("Driver does not support inode-based write");
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Perform the write
    ssize_t result = driver->write_inode(mnt->fs_context, inode_number,
                                        offset, buffer, size);
    
    if (result < 0) {
        VFS_PAGE_DEBUG("Write failed for inode %u: error %d", inode_number, result);
    }
    
    return result;
}

/**
 * @brief Get file size using device and inode numbers
 * @param device_id Device ID containing the file
 * @param inode_number Inode number of the file
 * @param size Pointer to store file size
 * @return 0 on success, negative error code on failure
 */
int vfs_get_file_size(uint32_t device_id, uint32_t inode_number, uint64_t *size) {
    if (!size) {
        return -FS_ERR_INVALID_PARAM;
    }
    
    // Find the mount point for this device
    mount_t *mnt = mount_find_by_device_id(device_id);
    if (!mnt) {
        VFS_PAGE_ERROR("Device ID %u not mounted", device_id);
        return -FS_ERR_NOT_FOUND;
    }
    
    // Get the driver
    vfs_driver_t *driver = vfs_get_driver(mnt->fs_type);
    if (!driver) {
        VFS_PAGE_ERROR("No driver for filesystem type %s", mnt->fs_type);
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Check if driver supports stat operations
    if (!driver->stat_inode) {
        VFS_PAGE_ERROR("Driver does not support inode stat");
        return -FS_ERR_NOT_SUPPORTED;
    }
    
    // Get file stats
    struct stat st;
    int result = driver->stat_inode(mnt->fs_context, inode_number, &st);
    if (result < 0) {
        return result;
    }
    
    *size = st.st_size;
    return 0;
}