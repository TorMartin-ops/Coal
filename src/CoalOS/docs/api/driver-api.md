# Driver API

This document describes the APIs for developing device drivers in Coal OS.

## Overview

Coal OS provides a framework for device drivers with:
- **Device Registration**: Unified device model
- **Interrupt Handling**: IRQ management
- **Port I/O**: Hardware access primitives
- **DMA Operations**: Direct memory access (future)
- **Power Management**: Device power states (future)

## Device Registration

### Device Structure

```c
typedef struct device {
    const char *name;           // Device name
    uint32_t id;               // Unique device ID
    device_type_t type;        // Device type
    device_class_t *class;     // Device class
    void *driver_data;         // Driver private data
    struct device *parent;     // Parent device
    struct list_head children; // Child devices
    struct list_head node;     // Device list node
    device_ops_t *ops;         // Device operations
} device_t;

typedef enum {
    DEVICE_TYPE_CHAR,    // Character device
    DEVICE_TYPE_BLOCK,   // Block device
    DEVICE_TYPE_NET,     // Network device
    DEVICE_TYPE_BUS,     // Bus device
    DEVICE_TYPE_MISC     // Miscellaneous
} device_type_t;
```

### Device Operations

```c
typedef struct device_ops {
    int (*open)(device_t *dev, file_t *file);
    int (*close)(device_t *dev, file_t *file);
    ssize_t (*read)(device_t *dev, void *buf, size_t count, off_t *offset);
    ssize_t (*write)(device_t *dev, const void *buf, size_t count, off_t *offset);
    int (*ioctl)(device_t *dev, unsigned int cmd, unsigned long arg);
    int (*mmap)(device_t *dev, vm_area_t *vma);
} device_ops_t;
```

### Registration Functions

```c
/**
 * @brief Register a device
 * @param dev Device structure
 * @return 0 on success, negative error code on failure
 */
int device_register(device_t *dev);

/**
 * @brief Unregister a device
 * @param dev Device to unregister
 */
void device_unregister(device_t *dev);

/**
 * @brief Allocate a device structure
 * @param name Device name
 * @param type Device type
 * @return Allocated device or NULL
 */
device_t* device_alloc(const char *name, device_type_t type);

/**
 * @brief Free a device structure
 * @param dev Device to free
 */
void device_free(device_t *dev);

/**
 * @brief Set driver private data
 * @param dev Device
 * @param data Private data pointer
 */
static inline void device_set_drvdata(device_t *dev, void *data) {
    dev->driver_data = data;
}

/**
 * @brief Get driver private data
 * @param dev Device
 * @return Private data pointer
 */
static inline void* device_get_drvdata(device_t *dev) {
    return dev->driver_data;
}
```

## Interrupt Handling

### IRQ Registration

```c
/**
 * @brief Register interrupt handler
 * @param irq IRQ number
 * @param handler Handler function
 * @param flags Handler flags
 * @param name Handler name
 * @param dev_id Device ID for shared interrupts
 * @return 0 on success, negative error code on failure
 */
int request_irq(unsigned int irq, 
                irq_handler_t handler,
                unsigned long flags,
                const char *name,
                void *dev_id);

/**
 * @brief Free interrupt handler
 * @param irq IRQ number
 * @param dev_id Device ID
 */
void free_irq(unsigned int irq, void *dev_id);

/**
 * @brief Interrupt handler function type
 * @param irq IRQ number
 * @param dev_id Device ID
 * @return IRQ_HANDLED if handled, IRQ_NONE if not
 */
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

// IRQ handler return values
#define IRQ_NONE        0  // Interrupt not handled
#define IRQ_HANDLED     1  // Interrupt handled
#define IRQ_WAKE_THREAD 2  // Wake handler thread (future)

// IRQ flags
#define IRQF_SHARED     0x01  // Shared interrupt
#define IRQF_TRIGGER_RISING  0x02  // Rising edge triggered
#define IRQF_TRIGGER_FALLING 0x04  // Falling edge triggered
#define IRQF_TRIGGER_HIGH    0x08  // High level triggered
#define IRQF_TRIGGER_LOW     0x10  // Low level triggered
```

### IRQ Control

```c
/**
 * @brief Enable IRQ
 * @param irq IRQ number
 */
void enable_irq(unsigned int irq);

/**
 * @brief Disable IRQ
 * @param irq IRQ number
 */
void disable_irq(unsigned int irq);

/**
 * @brief Disable IRQ and wait for handlers
 * @param irq IRQ number
 */
void disable_irq_sync(unsigned int irq);

/**
 * @brief Check if in interrupt context
 * @return true if in interrupt, false otherwise
 */
bool in_interrupt(void);

/**
 * @brief Check if in IRQ context
 * @return true if in IRQ, false otherwise
 */
bool in_irq(void);
```

## Port I/O

### Basic I/O Operations

```c
/**
 * @brief Read byte from port
 * @param port Port address
 * @return Byte value
 */
static inline uint8_t inb(uint16_t port);

/**
 * @brief Write byte to port
 * @param port Port address
 * @param val Byte value
 */
static inline void outb(uint16_t port, uint8_t val);

/**
 * @brief Read word from port
 * @param port Port address
 * @return Word value
 */
static inline uint16_t inw(uint16_t port);

/**
 * @brief Write word to port
 * @param port Port address
 * @param val Word value
 */
static inline void outw(uint16_t port, uint16_t val);

/**
 * @brief Read dword from port
 * @param port Port address
 * @return Dword value
 */
static inline uint32_t inl(uint16_t port);

/**
 * @brief Write dword to port
 * @param port Port address
 * @param val Dword value
 */
static inline void outl(uint16_t port, uint32_t val);
```

### String I/O Operations

```c
/**
 * @brief Read string of bytes from port
 * @param port Port address
 * @param addr Destination buffer
 * @param count Number of bytes
 */
static inline void insb(uint16_t port, void *addr, uint32_t count);

/**
 * @brief Write string of bytes to port
 * @param port Port address
 * @param addr Source buffer
 * @param count Number of bytes
 */
static inline void outsb(uint16_t port, const void *addr, uint32_t count);

/**
 * @brief Read string of words from port
 * @param port Port address
 * @param addr Destination buffer
 * @param count Number of words
 */
static inline void insw(uint16_t port, void *addr, uint32_t count);

/**
 * @brief Write string of words to port
 * @param port Port address
 * @param addr Source buffer
 * @param count Number of words
 */
static inline void outsw(uint16_t port, const void *addr, uint32_t count);
```

### I/O Delays

```c
/**
 * @brief I/O delay
 * Provides ~1us delay for slow devices
 */
static inline void io_delay(void) {
    inb(0x80);  // Dummy read from unused port
}

/**
 * @brief Microsecond delay
 * @param us Microseconds to delay
 */
void udelay(unsigned long us);

/**
 * @brief Millisecond delay
 * @param ms Milliseconds to delay
 */
void mdelay(unsigned long ms);
```

## Memory-Mapped I/O

```c
/**
 * @brief Map device memory
 * @param phys_addr Physical address
 * @param size Size to map
 * @return Virtual address or NULL on failure
 */
void* ioremap(uintptr_t phys_addr, size_t size);

/**
 * @brief Unmap device memory
 * @param addr Virtual address
 * @param size Size to unmap
 */
void iounmap(void *addr, size_t size);

/**
 * @brief Read from memory-mapped register
 */
#define readb(addr) (*(volatile uint8_t *)(addr))
#define readw(addr) (*(volatile uint16_t *)(addr))
#define readl(addr) (*(volatile uint32_t *)(addr))

/**
 * @brief Write to memory-mapped register
 */
#define writeb(val, addr) (*(volatile uint8_t *)(addr) = (val))
#define writew(val, addr) (*(volatile uint16_t *)(addr) = (val))
#define writel(val, addr) (*(volatile uint32_t *)(addr) = (val))
```

## DMA Operations (Future)

```c
/**
 * @brief Allocate DMA buffer
 * @param size Buffer size
 * @param dma_addr Physical address return
 * @return Virtual address or NULL
 */
void* dma_alloc(size_t size, dma_addr_t *dma_addr);

/**
 * @brief Free DMA buffer
 * @param addr Virtual address
 * @param size Buffer size
 * @param dma_addr Physical address
 */
void dma_free(void *addr, size_t size, dma_addr_t dma_addr);

/**
 * @brief Map buffer for DMA
 * @param dev Device
 * @param buf Buffer address
 * @param size Buffer size
 * @param dir DMA direction
 * @return DMA address
 */
dma_addr_t dma_map_single(device_t *dev, void *buf, 
                         size_t size, dma_dir_t dir);

/**
 * @brief Unmap DMA buffer
 * @param dev Device
 * @param dma_addr DMA address
 * @param size Buffer size
 * @param dir DMA direction
 */
void dma_unmap_single(device_t *dev, dma_addr_t dma_addr,
                     size_t size, dma_dir_t dir);

// DMA directions
typedef enum {
    DMA_TO_DEVICE,      // Memory to device
    DMA_FROM_DEVICE,    // Device to memory
    DMA_BIDIRECTIONAL   // Both directions
} dma_dir_t;
```

## Timer Functions

```c
/**
 * @brief Get current system ticks
 * @return Current tick count
 */
uint64_t get_ticks(void);

/**
 * @brief Convert milliseconds to ticks
 * @param ms Milliseconds
 * @return Tick count
 */
uint64_t ms_to_ticks(uint32_t ms);

/**
 * @brief Convert ticks to milliseconds
 * @param ticks Tick count
 * @return Milliseconds
 */
uint32_t ticks_to_ms(uint64_t ticks);

/**
 * @brief Schedule delayed work
 * @param work Work structure
 * @param delay Delay in ticks
 */
void schedule_delayed_work(work_t *work, uint64_t delay);
```

## Driver Examples

### Character Device Driver

```c
// Simple character device driver
typedef struct {
    device_t *dev;
    char buffer[1024];
    size_t size;
    spinlock_t lock;
} mydev_t;

static mydev_t mydev;

static int mydev_open(device_t *dev, file_t *file) {
    kprintf("mydev: opened\n");
    return 0;
}

static ssize_t mydev_read(device_t *dev, void *buf, 
                         size_t count, off_t *offset) {
    mydev_t *md = device_get_drvdata(dev);
    unsigned long flags;
    
    spinlock_acquire_irqsave(&md->lock, &flags);
    
    if (*offset >= md->size) {
        spinlock_release_irqrestore(&md->lock, flags);
        return 0;  // EOF
    }
    
    size_t to_read = min(count, md->size - *offset);
    memcpy(buf, md->buffer + *offset, to_read);
    *offset += to_read;
    
    spinlock_release_irqrestore(&md->lock, flags);
    return to_read;
}

static device_ops_t mydev_ops = {
    .open = mydev_open,
    .read = mydev_read,
    // ... other operations
};

int mydev_init(void) {
    // Allocate device
    device_t *dev = device_alloc("mydev", DEVICE_TYPE_CHAR);
    if (!dev) return -ENOMEM;
    
    // Initialize driver data
    spinlock_init(&mydev.lock);
    strcpy(mydev.buffer, "Hello from mydev!\n");
    mydev.size = strlen(mydev.buffer);
    mydev.dev = dev;
    
    // Set up device
    dev->ops = &mydev_ops;
    device_set_drvdata(dev, &mydev);
    
    // Register device
    return device_register(dev);
}
```

### Interrupt-Driven Driver

```c
// Keyboard driver example
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_IRQ          1

static irqreturn_t kbd_interrupt(int irq, void *dev_id) {
    uint8_t status = inb(KBD_STATUS_PORT);
    
    if (!(status & 0x01)) {
        return IRQ_NONE;  // No data available
    }
    
    uint8_t scancode = inb(KBD_DATA_PORT);
    
    // Process scancode
    process_scancode(scancode);
    
    return IRQ_HANDLED;
}

int kbd_init(void) {
    // Request IRQ
    int ret = request_irq(KBD_IRQ, kbd_interrupt, 
                         IRQF_SHARED, "keyboard", NULL);
    if (ret < 0) {
        kprintf("kbd: Failed to request IRQ\n");
        return ret;
    }
    
    // Enable keyboard
    outb(KBD_STATUS_PORT, 0xAE);  // Enable keyboard
    
    return 0;
}
```

### PCI Device Driver (Future)

```c
// PCI device driver template
static int mypci_probe(pci_dev_t *pdev) {
    // Enable PCI device
    pci_enable_device(pdev);
    
    // Get BAR addresses
    uintptr_t bar0 = pci_resource_start(pdev, 0);
    size_t bar0_len = pci_resource_len(pdev, 0);
    
    // Map device memory
    void *mmio = ioremap(bar0, bar0_len);
    if (!mmio) {
        return -ENOMEM;
    }
    
    // Initialize device
    writel(0x1, mmio + DEVICE_ENABLE_REG);
    
    return 0;
}

static struct pci_device_id mypci_ids[] = {
    { PCI_DEVICE(0x1234, 0x5678) },  // Vendor, Device
    { 0 }
};

static pci_driver_t mypci_driver = {
    .name = "mypci",
    .id_table = mypci_ids,
    .probe = mypci_probe,
    // ... other callbacks
};
```

## Power Management (Future)

```c
/**
 * @brief Device power states
 */
typedef enum {
    DEVICE_POWER_ON,     // Full power
    DEVICE_POWER_IDLE,   // Idle, quick resume
    DEVICE_POWER_SLEEP,  // Sleep, slow resume
    DEVICE_POWER_OFF     // Powered off
} device_power_state_t;

/**
 * @brief Set device power state
 * @param dev Device
 * @param state New power state
 * @return 0 on success, negative error code on failure
 */
int device_set_power_state(device_t *dev, device_power_state_t state);
```

## Driver Guidelines

1. **Initialization Order**: Check dependencies
2. **Error Handling**: Clean up on failure
3. **Locking**: Protect shared data
4. **Interrupts**: Keep handlers fast
5. **Memory**: Use appropriate allocators
6. **Power**: Implement power management
7. **Debugging**: Add debug prints