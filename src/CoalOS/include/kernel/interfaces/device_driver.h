/**
 * @file device_driver.h
 * @brief Abstract device driver interface for Coal OS
 * 
 * This interface follows the Open/Closed Principle by providing
 * extensible device driver framework where new drivers can be added
 * without modifying existing code.
 */

#ifndef COAL_INTERFACES_DEVICE_DRIVER_H
#define COAL_INTERFACES_DEVICE_DRIVER_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

/**
 * @brief Device types
 */
typedef enum {
    DEVICE_TYPE_UNKNOWN = 0,
    DEVICE_TYPE_BLOCK,      // Block devices (disk, etc.)
    DEVICE_TYPE_CHAR,       // Character devices (keyboard, serial, etc.)
    DEVICE_TYPE_NETWORK,    // Network devices
    DEVICE_TYPE_DISPLAY,    // Display devices
    DEVICE_TYPE_AUDIO,      // Audio devices
    DEVICE_TYPE_INPUT,      // Input devices
    DEVICE_TYPE_TIMER,      // Timer devices
} device_type_t;

/**
 * @brief Device states
 */
typedef enum {
    DEVICE_STATE_UNKNOWN = 0,
    DEVICE_STATE_INITIALIZING,
    DEVICE_STATE_READY,
    DEVICE_STATE_BUSY,
    DEVICE_STATE_ERROR,
    DEVICE_STATE_SUSPENDED,
    DEVICE_STATE_REMOVED,
} device_state_t;

/**
 * @brief Device capabilities/flags
 */
typedef enum {
    DEVICE_CAP_READ      = (1 << 0),
    DEVICE_CAP_WRITE     = (1 << 1),
    DEVICE_CAP_SEEK      = (1 << 2),
    DEVICE_CAP_INTERRUPT = (1 << 3),
    DEVICE_CAP_DMA       = (1 << 4),
    DEVICE_CAP_REMOVABLE = (1 << 5),
} device_capabilities_t;

/**
 * @brief Forward declaration
 */
struct device_driver;

/**
 * @brief Device operations interface
 * 
 * Different device types implement appropriate subsets of these operations
 */
typedef struct device_operations {
    /**
     * @brief Initialize the device
     */
    error_t (*init)(struct device_driver* dev);
    
    /**
     * @brief Cleanup the device
     */
    void (*cleanup)(struct device_driver* dev);
    
    /**
     * @brief Read from device
     */
    ssize_t (*read)(struct device_driver* dev, void* buffer, size_t size, off_t offset);
    
    /**
     * @brief Write to device
     */
    ssize_t (*write)(struct device_driver* dev, const void* buffer, size_t size, off_t offset);
    
    /**
     * @brief Device-specific control operations
     */
    error_t (*ioctl)(struct device_driver* dev, uint32_t cmd, void* arg);
    
    /**
     * @brief Interrupt handler
     */
    void (*interrupt_handler)(struct device_driver* dev);
    
    /**
     * @brief Power management
     */
    error_t (*suspend)(struct device_driver* dev);
    error_t (*resume)(struct device_driver* dev);
    
} device_operations_t;

/**
 * @brief Abstract device driver
 */
typedef struct device_driver {
    /**
     * @brief Device identification
     */
    const char* name;
    device_type_t type;
    uint32_t device_id;
    uint32_t vendor_id;
    
    /**
     * @brief Device state
     */
    device_state_t state;
    device_capabilities_t capabilities;
    
    /**
     * @brief Operations
     */
    const device_operations_t* ops;
    
    /**
     * @brief Device-specific data
     */
    void* private_data;
    
    /**
     * @brief Resource information
     */
    uintptr_t base_address;
    uint32_t irq_line;
    size_t memory_size;
    
    /**
     * @brief Statistics
     */
    uint64_t read_count;
    uint64_t write_count;
    uint64_t error_count;
    
    /**
     * @brief Link for device registry
     */
    struct device_driver* next;
    
} device_driver_t;

/**
 * @brief Device registry interface
 */
typedef struct device_registry {
    /**
     * @brief Register a device driver
     */
    error_t (*register_device)(device_driver_t* driver);
    
    /**
     * @brief Unregister a device driver
     */
    error_t (*unregister_device)(device_driver_t* driver);
    
    /**
     * @brief Find device by name
     */
    device_driver_t* (*find_device)(const char* name);
    
    /**
     * @brief Find devices by type
     */
    device_driver_t* (*find_devices_by_type)(device_type_t type);
    
    /**
     * @brief Initialize all registered devices
     */
    error_t (*init_all_devices)(void);
    
    /**
     * @brief Enumerate all devices
     */
    void (*enumerate_devices)(void (*callback)(device_driver_t* dev, void* context), void* context);
    
} device_registry_t;

/**
 * @brief Global device registry (dependency injection point)
 */
extern device_registry_t* g_device_registry;

/**
 * @brief Set device registry implementation
 */
void device_set_registry(device_registry_t* registry);

/**
 * @brief Convenience functions
 */
static inline error_t device_register(device_driver_t* driver) {
    if (g_device_registry && g_device_registry->register_device) {
        return g_device_registry->register_device(driver);
    }
    return E_NOTSUP;
}

static inline device_driver_t* device_find(const char* name) {
    if (g_device_registry && g_device_registry->find_device) {
        return g_device_registry->find_device(name);
    }
    return NULL;
}

static inline error_t device_init_all(void) {
    if (g_device_registry && g_device_registry->init_all_devices) {
        return g_device_registry->init_all_devices();
    }
    return E_NOTSUP;
}

/**
 * @brief Helper macros for device operations
 */
#define DEVICE_READ(dev, buf, size, offset) \
    ((dev) && (dev)->ops && (dev)->ops->read ? \
     (dev)->ops->read(dev, buf, size, offset) : -E_NOTSUP)

#define DEVICE_WRITE(dev, buf, size, offset) \
    ((dev) && (dev)->ops && (dev)->ops->write ? \
     (dev)->ops->write(dev, buf, size, offset) : -E_NOTSUP)

#define DEVICE_IOCTL(dev, cmd, arg) \
    ((dev) && (dev)->ops && (dev)->ops->ioctl ? \
     (dev)->ops->ioctl(dev, cmd, arg) : E_NOTSUP)

#endif // COAL_INTERFACES_DEVICE_DRIVER_H