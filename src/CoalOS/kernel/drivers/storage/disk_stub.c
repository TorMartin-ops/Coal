/**
 * @file disk_stub.c
 * @brief Temporary stub implementations for disk I/O functions
 * 
 * These are simplified wrappers around the actual disk functions.
 * In a real implementation, these would interface with the disk_t structure.
 */

#include <kernel/drivers/storage/disk.h>
#include <kernel/drivers/display/terminal.h>

// Global disk instance (should be properly managed in real code)
static disk_t* g_system_disk = NULL;

/**
 * @brief Set the system disk instance
 */
void disk_set_system_disk(disk_t* disk) {
    g_system_disk = disk;
}

/**
 * @brief Simplified disk read for ATA driver
 */
int disk_read_sectors(uint64_t lba, size_t count, void* buffer) {
    if (!g_system_disk) {
        terminal_printf("[disk_stub] ERROR: No system disk initialized\n");
        return -1;
    }
    
    return disk_read_raw_sectors(g_system_disk, lba, buffer, count);
}

/**
 * @brief Simplified disk write for ATA driver
 */
int disk_write_sectors(uint64_t lba, size_t count, const void* buffer) {
    if (!g_system_disk) {
        terminal_printf("[disk_stub] ERROR: No system disk initialized\n");
        return -1;
    }
    
    return disk_write_raw_sectors(g_system_disk, lba, buffer, count);
}