/**
 * @file ata_driver.c
 * @brief ATA disk driver implementing the device driver interface
 * 
 * This driver follows the Open/Closed Principle by implementing
 * the abstract device driver interface, allowing it to be used
 * without modifying existing code.
 */

#include <kernel/interfaces/device_driver.h>
#include <kernel/interfaces/logger.h>
#include <kernel/drivers/storage/disk.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define LOG_MODULE "ata_driver"
#define SECTOR_SIZE 512

// ATA device private data
typedef struct ata_private_data {
    uint16_t base_port;
    uint16_t control_port;
    uint8_t drive_number;
    bool is_master;
    uint32_t sectors;
    uint16_t cylinders;
    uint8_t heads;
    uint8_t sectors_per_track;
} ata_private_data_t;

// Forward declarations
static error_t ata_init(device_driver_t* dev);
static void ata_cleanup(device_driver_t* dev);
static ssize_t ata_read(device_driver_t* dev, void* buffer, size_t size, off_t offset);
static ssize_t ata_write(device_driver_t* dev, const void* buffer, size_t size, off_t offset);
static error_t ata_ioctl(device_driver_t* dev, uint32_t cmd, void* arg);

// ATA device operations
static const device_operations_t ata_ops = {
    .init = ata_init,
    .cleanup = ata_cleanup,
    .read = ata_read,
    .write = ata_write,
    .ioctl = ata_ioctl,
    .interrupt_handler = NULL, // Using polling for now
    .suspend = NULL,
    .resume = NULL,
};

static error_t ata_init(device_driver_t* dev) {
    if (!dev || !dev->private_data) {
        LOGGER_ERROR(LOG_MODULE, "Invalid device or private data");
        return E_INVAL;
    }

    ata_private_data_t* ata_data = (ata_private_data_t*)dev->private_data;
    
    LOGGER_INFO(LOG_MODULE, "Initializing ATA device %s", dev->name);
    
    // ATA device initialization would happen here
    // For now, we just mark the device as ready
    // The actual disk initialization happens through disk_init(&disk, device_name)
    // when the filesystem layer initializes

    dev->state = DEVICE_STATE_READY;
    dev->capabilities = DEVICE_CAP_READ | DEVICE_CAP_WRITE | DEVICE_CAP_SEEK;
    
    LOGGER_INFO(LOG_MODULE, "ATA device %s initialized successfully", dev->name);
    return E_SUCCESS;
}

static void ata_cleanup(device_driver_t* dev) {
    if (!dev) {
        return;
    }

    LOGGER_INFO(LOG_MODULE, "Cleaning up ATA device %s", dev->name);
    
    dev->state = DEVICE_STATE_REMOVED;
    
    // Cleanup ATA-specific resources
    if (dev->private_data) {
        // Private data cleanup would go here
        // For now, we don't allocate any dynamic resources
    }
}

static ssize_t ata_read(device_driver_t* dev, void* buffer, size_t size, off_t offset) {
    if (!dev || !buffer || dev->state != DEVICE_STATE_READY) {
        return -E_INVAL;
    }

    // Convert offset to sector number
    uint32_t sector = offset / SECTOR_SIZE;
    uint32_t sectors_to_read = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    LOGGER_DEBUG(LOG_MODULE, "Reading %zu bytes from sector %u", size, sector);
    
    // Use existing disk read function
    int result = disk_read_sectors(sector, sectors_to_read, buffer);
    if (result != 0) {
        LOGGER_ERROR(LOG_MODULE, "Failed to read from ATA device");
        dev->error_count++;
        return -E_IO;
    }

    dev->read_count++;
    return size;
}

static ssize_t ata_write(device_driver_t* dev, const void* buffer, size_t size, off_t offset) {
    if (!dev || !buffer || dev->state != DEVICE_STATE_READY) {
        return -E_INVAL;
    }

    // Convert offset to sector number
    uint32_t sector = offset / SECTOR_SIZE;
    uint32_t sectors_to_write = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    
    LOGGER_DEBUG(LOG_MODULE, "Writing %zu bytes to sector %u", size, sector);
    
    // Use existing disk write function
    int result = disk_write_sectors(sector, sectors_to_write, buffer);
    if (result != 0) {
        LOGGER_ERROR(LOG_MODULE, "Failed to write to ATA device");
        dev->error_count++;
        return -E_IO;
    }

    dev->write_count++;
    return size;
}

static error_t ata_ioctl(device_driver_t* dev, uint32_t cmd, void* arg) {
    if (!dev || dev->state != DEVICE_STATE_READY) {
        return E_INVAL;
    }

    ata_private_data_t* ata_data = (ata_private_data_t*)dev->private_data;
    
    switch (cmd) {
        case 0x1000: // GET_SECTOR_COUNT
            if (arg) {
                *(uint32_t*)arg = ata_data->sectors;
                return E_SUCCESS;
            }
            return E_INVAL;
            
        case 0x1001: // GET_GEOMETRY
            if (arg) {
                struct {
                    uint16_t cylinders;
                    uint8_t heads;
                    uint8_t sectors_per_track;
                } *geometry = arg;
                
                geometry->cylinders = ata_data->cylinders;
                geometry->heads = ata_data->heads;
                geometry->sectors_per_track = ata_data->sectors_per_track;
                return E_SUCCESS;
            }
            return E_INVAL;
            
        default:
            LOGGER_WARN(LOG_MODULE, "Unsupported ioctl command: 0x%x", cmd);
            return E_NOTSUP;
    }
}

// Create ATA device driver instance
device_driver_t* ata_create_driver(uint16_t base_port, uint8_t drive_number) {
    // Allocate device driver structure
    device_driver_t* driver = kmalloc(sizeof(device_driver_t));
    if (!driver) {
        LOGGER_ERROR(LOG_MODULE, "Failed to allocate device driver");
        return NULL;
    }

    // Allocate private data
    ata_private_data_t* private_data = kmalloc(sizeof(ata_private_data_t));
    if (!private_data) {
        LOGGER_ERROR(LOG_MODULE, "Failed to allocate private data");
        kfree(driver);
        return NULL;
    }

    // Initialize private data
    private_data->base_port = base_port;
    private_data->control_port = base_port + 0x206;
    private_data->drive_number = drive_number;
    private_data->is_master = (drive_number == 0);
    
    // These would be detected during initialization
    private_data->sectors = 1024 * 1024; // Default 1M sectors
    private_data->cylinders = 1024;
    private_data->heads = 16;
    private_data->sectors_per_track = 63;

    // Initialize device driver structure
    memset(driver, 0, sizeof(device_driver_t));
    
    if (drive_number == 0) {
        driver->name = "ata0";
    } else {
        driver->name = "ata1";
    }
    
    driver->type = DEVICE_TYPE_BLOCK;
    driver->device_id = 0x1234; // Generic ATA ID
    driver->vendor_id = 0x8086; // Generic vendor
    driver->state = DEVICE_STATE_UNKNOWN;
    driver->capabilities = 0; // Will be set during init
    driver->ops = &ata_ops;
    driver->private_data = private_data;
    driver->base_address = base_port;
    driver->irq_line = (base_port == 0x1F0) ? 14 : 15; // Standard ATA IRQs
    driver->memory_size = 0; // ATA uses I/O ports, not memory

    return driver;
}

void ata_destroy_driver(device_driver_t* driver) {
    if (!driver) {
        return;
    }

    if (driver->private_data) {
        kfree(driver->private_data);
    }
    
    kfree(driver);
}