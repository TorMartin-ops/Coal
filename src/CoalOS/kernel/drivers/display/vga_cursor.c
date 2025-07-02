/**
 * @file vga_cursor.c
 * @brief VGA Hardware Cursor Management Implementation
 * 
 * Handles hardware cursor control for VGA text mode including
 * position updates, enabling/disabling, and shape configuration.
 */

#include <kernel/drivers/display/vga_cursor.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/lib/port_io.h>
#include <kernel/sync/spinlock.h>

// VGA register constants
#define VGA_CRTC_ADDR    0x3D4
#define VGA_CRTC_DATA    0x3D5
#define VGA_CURSOR_START 0x0A
#define VGA_CURSOR_END   0x0B
#define VGA_CURSOR_HIGH  0x0E
#define VGA_CURSOR_LOW   0x0F

// Cursor state
static bool cursor_enabled = true;
static spinlock_t cursor_lock = {0};

/**
 * @brief Enable the hardware cursor with default shape
 */
void vga_cursor_enable(void) {
    uintptr_t flags = spinlock_acquire_irqsave(&cursor_lock);
    
    outb(VGA_CRTC_ADDR, VGA_CURSOR_START);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xC0) | 13); // Start scan line 13
    
    outb(VGA_CRTC_ADDR, VGA_CURSOR_END);
    outb(VGA_CRTC_DATA, (inb(VGA_CRTC_DATA) & 0xE0) | 14); // End scan line 14
    
    cursor_enabled = true;
    spinlock_release_irqrestore(&cursor_lock, flags);
}

/**
 * @brief Disable the hardware cursor
 */
void vga_cursor_disable(void) {
    uintptr_t flags = spinlock_acquire_irqsave(&cursor_lock);
    
    outb(VGA_CRTC_ADDR, VGA_CURSOR_START);
    outb(VGA_CRTC_DATA, 0x20); // Bit 5 set = cursor disabled
    
    cursor_enabled = false;
    spinlock_release_irqrestore(&cursor_lock, flags);
}

/**
 * @brief Set cursor position
 * @param x Column position (0-79)
 * @param y Row position (0-24)
 */
void vga_cursor_set_position(int x, int y) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return; // Invalid position
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&cursor_lock);
    
    uint16_t position = y * VGA_COLS + x;
    
    // Send high byte
    outb(VGA_CRTC_ADDR, VGA_CURSOR_HIGH);
    outb(VGA_CRTC_DATA, (position >> 8) & 0xFF);
    
    // Send low byte
    outb(VGA_CRTC_ADDR, VGA_CURSOR_LOW);
    outb(VGA_CRTC_DATA, position & 0xFF);
    
    spinlock_release_irqrestore(&cursor_lock, flags);
}

/**
 * @brief Get current cursor position
 * @param x Output parameter for column position
 * @param y Output parameter for row position
 */
void vga_cursor_get_position(int *x, int *y) {
    if (!x || !y) {
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&cursor_lock);
    
    uint16_t position = 0;
    
    // Read high byte
    outb(VGA_CRTC_ADDR, VGA_CURSOR_HIGH);
    position = inb(VGA_CRTC_DATA) << 8;
    
    // Read low byte
    outb(VGA_CRTC_ADDR, VGA_CURSOR_LOW);
    position |= inb(VGA_CRTC_DATA);
    
    spinlock_release_irqrestore(&cursor_lock, flags);
    
    *x = position % VGA_COLS;
    *y = position / VGA_COLS;
}

/**
 * @brief Check if cursor is enabled
 * @return true if cursor is enabled, false otherwise
 */
bool vga_cursor_is_enabled(void) {
    return cursor_enabled;
}

/**
 * @brief Set cursor shape/size
 * @param start_scanline Starting scan line (0-15)
 * @param end_scanline Ending scan line (0-15)
 */
void vga_cursor_set_shape(uint8_t start_scanline, uint8_t end_scanline) {
    if (start_scanline > 15 || end_scanline > 15) {
        return; // Invalid parameters
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&cursor_lock);
    
    // Set cursor start scan line
    outb(VGA_CRTC_ADDR, VGA_CURSOR_START);
    uint8_t current = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_DATA, (current & 0xC0) | (start_scanline & 0x1F));
    
    // Set cursor end scan line
    outb(VGA_CRTC_ADDR, VGA_CURSOR_END);
    current = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_DATA, (current & 0xE0) | (end_scanline & 0x1F));
    
    spinlock_release_irqrestore(&cursor_lock, flags);
}