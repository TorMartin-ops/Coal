/**
 * @file vga_hardware.c
 * @brief VGA hardware abstraction following Single Responsibility Principle
 * 
 * This module is responsible ONLY for:
 * - Direct VGA hardware register access
 * - Cursor position management
 * - Hardware-specific VGA operations
 */

#define LOG_MODULE "vga_hardware"

#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/interfaces/logger.h>
#include <kernel/lib/port_io.h>
#include <kernel/sync/spinlock.h>

// VGA hardware constants
#define VGA_CMD_PORT        0x3D4
#define VGA_DATA_PORT       0x3D5
#define VGA_REG_CURSOR_HI   0x0E
#define VGA_REG_CURSOR_LO   0x0F
#define VGA_REG_CURSOR_START 0x0A
#define VGA_REG_CURSOR_END   0x0B
#define CURSOR_SCANLINE_START 14
#define CURSOR_SCANLINE_END   15

// VGA state
static spinlock_t vga_hw_lock = {0};
static bool vga_initialized = false;
static uint8_t cursor_visible = 1;

/**
 * @brief Initialize VGA hardware
 */
error_t vga_hardware_init(void) {
    spinlock_init(&vga_hw_lock);
    
    // Enable cursor by default
    vga_set_cursor_visible(true);
    
    vga_initialized = true;
    LOGGER_INFO(LOG_MODULE, "VGA hardware initialized");
    return E_SUCCESS;
}

/**
 * @brief Set cursor position on screen
 */
void vga_set_cursor_position(int x, int y) {
    if (!vga_initialized) {
        return;
    }
    
    // Validate coordinates
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        LOGGER_WARN(LOG_MODULE, "Invalid cursor position: (%d, %d)", x, y);
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    
    uint16_t pos = y * VGA_COLS + x;
    
    // Set cursor position in VGA registers
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO);
    outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));
    outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI);
    outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
    
    spinlock_release_irqrestore(&vga_hw_lock, flags);
}

/**
 * @brief Set cursor visibility
 */
void vga_set_cursor_visible(bool visible) {
    if (!vga_initialized) {
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    
    cursor_visible = visible ? 1 : 0;
    
    if (visible) {
        // Enable cursor
        outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
        outb(VGA_DATA_PORT, CURSOR_SCANLINE_START);
        outb(VGA_CMD_PORT, VGA_REG_CURSOR_END);
        outb(VGA_DATA_PORT, CURSOR_SCANLINE_END);
    } else {
        // Disable cursor by setting start scanline bit 5
        outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
        outb(VGA_DATA_PORT, 0x20);
    }
    
    spinlock_release_irqrestore(&vga_hw_lock, flags);
}

/**
 * @brief Get cursor visibility state
 */
bool vga_is_cursor_visible(void) {
    return cursor_visible != 0;
}

/**
 * @brief Write a character directly to VGA buffer
 */
void vga_write_char_at(int x, int y, char c, uint8_t color) {
    if (!vga_initialized) {
        return;
    }
    
    // Validate coordinates
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return;
    }
    
    volatile uint16_t *vga_buffer = (volatile uint16_t *)VGA_ADDRESS;
    size_t index = y * VGA_COLS + x;
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    vga_buffer[index] = (uint16_t)c | ((uint16_t)color << 8);
    spinlock_release_irqrestore(&vga_hw_lock, flags);
}

/**
 * @brief Read a character from VGA buffer
 */
uint16_t vga_read_char_at(int x, int y) {
    if (!vga_initialized) {
        return 0;
    }
    
    // Validate coordinates
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return 0;
    }
    
    volatile uint16_t *vga_buffer = (volatile uint16_t *)VGA_ADDRESS;
    size_t index = y * VGA_COLS + x;
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    uint16_t result = vga_buffer[index];
    spinlock_release_irqrestore(&vga_hw_lock, flags);
    
    return result;
}

/**
 * @brief Clear the entire screen
 */
void vga_clear_screen(uint8_t color) {
    if (!vga_initialized) {
        return;
    }
    
    volatile uint16_t *vga_buffer = (volatile uint16_t *)VGA_ADDRESS;
    uint16_t blank = (uint16_t)' ' | ((uint16_t)color << 8);
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    
    for (size_t i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_buffer[i] = blank;
    }
    
    spinlock_release_irqrestore(&vga_hw_lock, flags);
}

/**
 * @brief Scroll screen up by one line
 */
void vga_scroll_up(uint8_t color) {
    if (!vga_initialized) {
        return;
    }
    
    volatile uint16_t *vga_buffer = (volatile uint16_t *)VGA_ADDRESS;
    uint16_t blank = (uint16_t)' ' | ((uint16_t)color << 8);
    
    uintptr_t flags = spinlock_acquire_irqsave(&vga_hw_lock);
    
    // Move all lines up
    for (int y = 0; y < VGA_ROWS - 1; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            size_t dest_index = y * VGA_COLS + x;
            size_t src_index = (y + 1) * VGA_COLS + x;
            vga_buffer[dest_index] = vga_buffer[src_index];
        }
    }
    
    // Clear the last line
    for (int x = 0; x < VGA_COLS; x++) {
        size_t index = (VGA_ROWS - 1) * VGA_COLS + x;
        vga_buffer[index] = blank;
    }
    
    spinlock_release_irqrestore(&vga_hw_lock, flags);
}

/**
 * @brief Cleanup VGA hardware
 */
void vga_hardware_cleanup(void) {
    vga_initialized = false;
    LOGGER_INFO(LOG_MODULE, "VGA hardware cleaned up");
}