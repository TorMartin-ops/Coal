# Device Drivers

Coal OS implements device drivers following a clean, modular architecture with clear separation between hardware-specific code and kernel interfaces.

## Driver Architecture

```
┌─────────────────────────────────────────────────────┐
│              Kernel Subsystems                      │
│         (FS, Process, Network, etc.)               │
├─────────────────────────────────────────────────────┤
│          Generic Device Interface                   │
│      (Block devices, Char devices, etc.)           │
├─────────────────────────────────────────────────────┤
│         Hardware Abstraction Layer (HAL)            │
├─────────────────────────────────────────────────────┤
│              Device Drivers                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │ Keyboard │  │  Timer   │  │  Serial  │  ...   │
│  │  Driver  │  │  (PIT)   │  │   Port   │        │
│  └──────────┘  └──────────┘  └──────────┘        │
├─────────────────────────────────────────────────────┤
│              Hardware Devices                       │
└─────────────────────────────────────────────────────┘
```

## Driver Model

### Device Types

#### Character Devices
- Byte-stream oriented
- Direct I/O operations
- Examples: keyboard, serial port

#### Block Devices
- Block-oriented (typically 512 bytes)
- Buffered through cache
- Examples: hard disk, CD-ROM

### Driver Registration
```c
typedef struct device_driver {
    const char *name;
    device_type_t type;
    
    // Initialization
    int (*init)(void);
    int (*probe)(device_t *dev);
    void (*remove)(device_t *dev);
    
    // Power management
    int (*suspend)(device_t *dev);
    int (*resume)(device_t *dev);
    
    // Device operations
    union {
        char_dev_ops_t *char_ops;
        block_dev_ops_t *block_ops;
    };
} device_driver_t;

// Register a driver
int driver_register(device_driver_t *driver);
```

## Implemented Drivers

### 1. Keyboard Driver (PS/2)

Handles keyboard input with full scancode translation:

#### Features
- PS/2 protocol support
- Scancode set 1 translation
- Modifier key tracking
- LED control support
- Keyboard buffer management

#### Implementation
```c
// Keyboard controller ports
#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_CMD_PORT     0x64

// Status register bits
#define KBD_STATUS_OBF   0x01  // Output buffer full
#define KBD_STATUS_IBF   0x02  // Input buffer full

// Keyboard interrupt handler
void keyboard_irq_handler(isr_frame_t *frame) {
    // Read scancode
    uint8_t scancode = inb(KBD_DATA_PORT);
    
    // Handle special keys
    if (scancode == 0xE0) {
        extended_key = true;
        return;
    }
    
    // Translate to keycode
    keycode_t key = translate_scancode(scancode, extended_key);
    
    // Update modifier state
    update_modifiers(key, !(scancode & 0x80));
    
    // Add to keyboard buffer
    if (!(scancode & 0x80)) {  // Key press
        char ch = keycode_to_char(key, modifiers);
        keyboard_buffer_add(ch);
    }
}
```

#### Keymap Support
```c
// US QWERTY layout
static const char keymap_normal[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    // ... more keys
};

static const char keymap_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    // ... more keys
};
```

### 2. Timer Driver (PIT - Programmable Interval Timer)

Provides system timing and scheduling interrupts:

#### Features
- Configurable frequency (default 1000 Hz)
- High-resolution timing
- Preemptive multitasking support
- Sleep/delay functionality

#### Implementation
```c
// PIT ports
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL1  0x41
#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43

// Initialize PIT
void pit_init(uint32_t frequency) {
    // Calculate divisor
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    
    // Send command
    outb(PIT_COMMAND, 0x36);  // Channel 0, LSB/MSB, Mode 3
    
    // Send frequency divisor
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    
    // Register IRQ handler
    irq_register_handler(IRQ0, pit_irq_handler);
}

// Timer interrupt handler
void pit_irq_handler(isr_frame_t *frame) {
    tick_count++;
    
    // Update system time
    update_system_time();
    
    // Trigger scheduler
    if (scheduler_is_ready()) {
        scheduler_tick();
    }
}
```

#### Time Management
```c
// Get current time
uint64_t get_ticks(void) {
    return tick_count;
}

// Sleep implementation
void sleep_ms(uint32_t ms) {
    uint64_t target = tick_count + (ms * ticks_per_second / 1000);
    while (tick_count < target) {
        yield();
    }
}
```

### 3. Serial Port Driver

UART driver for debugging and communication:

#### Features
- 16550 UART support
- Configurable baud rate
- Interrupt and polling modes
- FIFO management
- Hardware flow control

#### Implementation
```c
// Serial port registers
#define SERIAL_DATA         0  // Data register
#define SERIAL_IER          1  // Interrupt enable
#define SERIAL_IIR_FCR      2  // Interrupt ID / FIFO control
#define SERIAL_LCR          3  // Line control
#define SERIAL_MCR          4  // Modem control
#define SERIAL_LSR          5  // Line status
#define SERIAL_MSR          6  // Modem status

// Initialize serial port
void serial_init(uint16_t port, uint32_t baud) {
    // Disable interrupts
    outb(port + SERIAL_IER, 0x00);
    
    // Set baud rate
    outb(port + SERIAL_LCR, 0x80);  // Enable DLAB
    uint16_t divisor = 115200 / baud;
    outb(port + 0, divisor & 0xFF);
    outb(port + 1, divisor >> 8);
    
    // 8N1 configuration
    outb(port + SERIAL_LCR, 0x03);
    
    // Enable FIFO
    outb(port + SERIAL_IIR_FCR, 0xC7);
    
    // Enable interrupts
    outb(port + SERIAL_IER, 0x01);
}

// Write character
void serial_putc(uint16_t port, char c) {
    // Wait for transmit empty
    while (!(inb(port + SERIAL_LSR) & 0x20));
    
    outb(port + SERIAL_DATA, c);
}
```

### 4. Display Driver (VGA Text Mode)

Text mode display driver with cursor support:

#### Features
- 80x25 text mode
- 16 colors support
- Hardware cursor control
- Scrolling
- Clear screen

#### Implementation
```c
// VGA memory and ports
#define VGA_MEMORY  0xB8000
#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_CTRL    0x3D4
#define VGA_DATA    0x3D5

// Write character with attributes
void vga_putchar(int x, int y, char ch, uint8_t attr) {
    uint16_t *video = (uint16_t*)VGA_MEMORY;
    uint16_t entry = (attr << 8) | ch;
    video[y * VGA_WIDTH + x] = entry;
}

// Update cursor position
void vga_update_cursor(int x, int y) {
    uint16_t pos = y * VGA_WIDTH + x;
    
    outb(VGA_CTRL, 0x0F);
    outb(VGA_DATA, pos & 0xFF);
    outb(VGA_CTRL, 0x0E);
    outb(VGA_DATA, (pos >> 8) & 0xFF);
}
```

### 5. ATA/IDE Driver (In Development)

Basic ATA PIO mode driver:

#### Features
- PIO mode support
- Drive identification
- Read/write sectors
- Multiple drive support

#### Implementation
```c
// ATA registers
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_FEATURES    0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

// Read sectors
int ata_read_sectors(uint8_t drive, uint32_t lba, 
                     uint8_t count, void *buffer) {
    // Select drive and LBA mode
    outb(ATA_DRIVE, 0xE0 | (drive << 4) | ((lba >> 24) & 0x0F));
    
    // Set sector count and LBA
    outb(ATA_SECCOUNT, count);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    
    // Send read command
    outb(ATA_COMMAND, 0x20);
    
    // Read data
    uint16_t *buf = (uint16_t*)buffer;
    for (int i = 0; i < count; i++) {
        // Wait for data
        ata_wait_ready();
        
        // Read sector
        for (int j = 0; j < 256; j++) {
            buf[i * 256 + j] = inw(ATA_DATA);
        }
    }
    
    return 0;
}
```

## Interrupt Handling

### IRQ Management
```c
// IRQ handler registration
typedef void (*irq_handler_t)(isr_frame_t *frame);

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
    
    // Unmask IRQ in PIC
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t mask = inb(port);
    mask &= ~(1 << (irq & 7));
    outb(port, mask);
}

// Common IRQ handler
void irq_common_handler(isr_frame_t *frame) {
    uint8_t irq = frame->int_no - 32;
    
    // Call registered handler
    if (irq_handlers[irq]) {
        irq_handlers[irq](frame);
    }
    
    // Send EOI
    if (irq >= 8) {
        outb(PIC2_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}
```

## Driver Development Guidelines

### 1. Initialization
- Detect hardware presence
- Initialize hardware state
- Register interrupt handlers
- Allocate resources

### 2. Resource Management
- Use kernel allocators properly
- Free resources on failure
- Implement proper cleanup

### 3. Synchronization
- Use appropriate locking
- Minimize critical sections
- Avoid deadlocks

### 4. Error Handling
- Check all hardware operations
- Provide meaningful error codes
- Log errors for debugging

### 5. Performance
- Minimize port I/O operations
- Use DMA where available
- Implement interrupt coalescing

## Future Drivers

1. **USB Support**
   - UHCI/OHCI/EHCI controllers
   - HID devices
   - Mass storage

2. **Network Drivers**
   - RTL8139
   - E1000
   - Virtual drivers (virtio)

3. **Sound Drivers**
   - AC97
   - HD Audio
   - Sound Blaster 16

4. **Graphics Drivers**
   - VESA framebuffer
   - Basic 2D acceleration
   - Mode setting