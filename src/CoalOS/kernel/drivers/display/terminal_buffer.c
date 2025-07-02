/**
 * @file terminal_buffer.c
 * @brief Terminal Buffer Management Implementation
 * 
 * Handles terminal buffer operations including character placement,
 * scrolling, clearing, and buffer manipulation.
 */

#include <kernel/drivers/display/terminal_buffer.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/lib/string.h>
#include <kernel/lib/assert.h>

// VGA buffer access
static volatile uint16_t * const vga_buffer = (volatile uint16_t *)VGA_ADDRESS;

/**
 * @brief Create a VGA entry combining character and color
 * @param ch Character to display
 * @param color Color attribute
 * @return Combined VGA entry
 */
static inline uint16_t vga_entry(char ch, uint8_t color) {
    return (uint16_t)ch | ((uint16_t)color << 8);
}

/**
 * @brief Put a character at specific position
 * @param c Character to place
 * @param color Color attribute
 * @param x Column position
 * @param y Row position
 */
void terminal_buffer_put_char_at(char c, uint8_t color, int x, int y) {
    KERNEL_ASSERT(x >= 0 && x < VGA_COLS, "terminal_buffer_put_char_at: x out of bounds");
    KERNEL_ASSERT(y >= 0 && y < VGA_ROWS, "terminal_buffer_put_char_at: y out of bounds");
    
    const size_t index = y * VGA_COLS + x;
    vga_buffer[index] = vga_entry(c, color);
}

/**
 * @brief Get character at specific position
 * @param x Column position
 * @param y Row position
 * @return Character at position (0 if out of bounds)
 */
char terminal_buffer_get_char_at(int x, int y) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return 0;
    }
    
    const size_t index = y * VGA_COLS + x;
    return vga_buffer[index] & 0xFF;
}

/**
 * @brief Get color at specific position
 * @param x Column position
 * @param y Row position
 * @return Color attribute at position (0 if out of bounds)
 */
uint8_t terminal_buffer_get_color_at(int x, int y) {
    if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
        return 0;
    }
    
    const size_t index = y * VGA_COLS + x;
    return (vga_buffer[index] >> 8) & 0xFF;
}

/**
 * @brief Clear a specific row
 * @param row Row to clear (0-24)
 * @param color Color to fill with
 */
void terminal_buffer_clear_row(int row, uint8_t color) {
    if (row < 0 || row >= VGA_ROWS) {
        return;
    }
    
    for (int x = 0; x < VGA_COLS; x++) {
        const size_t index = row * VGA_COLS + x;
        vga_buffer[index] = vga_entry(' ', color);
    }
}

/**
 * @brief Clear entire terminal buffer
 * @param color Color to fill with
 */
void terminal_buffer_clear(uint8_t color) {
    for (int y = 0; y < VGA_ROWS; y++) {
        terminal_buffer_clear_row(y, color);
    }
}

/**
 * @brief Scroll terminal up by one line
 * @param preserve_color If true, preserve existing colors; if false, use provided color
 * @param fill_color Color for new bottom line (used if preserve_color is false)
 */
void terminal_buffer_scroll_up(bool preserve_color, uint8_t fill_color) {
    // Move all lines up by one
    for (int y = 0; y < VGA_ROWS - 1; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            const size_t dest_index = y * VGA_COLS + x;
            const size_t src_index = (y + 1) * VGA_COLS + x;
            
            if (preserve_color) {
                vga_buffer[dest_index] = vga_buffer[src_index];
            } else {
                char ch = vga_buffer[src_index] & 0xFF;
                vga_buffer[dest_index] = vga_entry(ch, fill_color);
            }
        }
    }
    
    // Clear the bottom line
    terminal_buffer_clear_row(VGA_ROWS - 1, fill_color);
}

/**
 * @brief Scroll terminal down by one line
 * @param preserve_color If true, preserve existing colors; if false, use provided color
 * @param fill_color Color for new top line (used if preserve_color is false)
 */
void terminal_buffer_scroll_down(bool preserve_color, uint8_t fill_color) {
    // Move all lines down by one
    for (int y = VGA_ROWS - 1; y > 0; y--) {
        for (int x = 0; x < VGA_COLS; x++) {
            const size_t dest_index = y * VGA_COLS + x;
            const size_t src_index = (y - 1) * VGA_COLS + x;
            
            if (preserve_color) {
                vga_buffer[dest_index] = vga_buffer[src_index];
            } else {
                char ch = vga_buffer[src_index] & 0xFF;
                vga_buffer[dest_index] = vga_entry(ch, fill_color);
            }
        }
    }
    
    // Clear the top line
    terminal_buffer_clear_row(0, fill_color);
}

/**
 * @brief Copy a rectangular region of the buffer
 * @param src_x Source X coordinate
 * @param src_y Source Y coordinate
 * @param dst_x Destination X coordinate
 * @param dst_y Destination Y coordinate
 * @param width Width of region to copy
 * @param height Height of region to copy
 */
void terminal_buffer_copy_region(int src_x, int src_y, int dst_x, int dst_y, 
                                int width, int height) {
    // Validate parameters
    if (src_x < 0 || src_y < 0 || dst_x < 0 || dst_y < 0 ||
        width <= 0 || height <= 0 ||
        src_x + width > VGA_COLS || dst_x + width > VGA_COLS ||
        src_y + height > VGA_ROWS || dst_y + height > VGA_ROWS) {
        return; // Invalid parameters
    }
    
    // Determine copy direction to avoid overwriting
    if (dst_y < src_y || (dst_y == src_y && dst_x < src_x)) {
        // Copy forward
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t src_index = (src_y + y) * VGA_COLS + (src_x + x);
                size_t dst_index = (dst_y + y) * VGA_COLS + (dst_x + x);
                vga_buffer[dst_index] = vga_buffer[src_index];
            }
        }
    } else {
        // Copy backward
        for (int y = height - 1; y >= 0; y--) {
            for (int x = width - 1; x >= 0; x--) {
                size_t src_index = (src_y + y) * VGA_COLS + (src_x + x);
                size_t dst_index = (dst_y + y) * VGA_COLS + (dst_x + x);
                vga_buffer[dst_index] = vga_buffer[src_index];
            }
        }
    }
}

/**
 * @brief Fill a rectangular region with a character and color
 * @param x Starting X coordinate
 * @param y Starting Y coordinate
 * @param width Width of region
 * @param height Height of region
 * @param ch Character to fill with
 * @param color Color attribute
 */
void terminal_buffer_fill_region(int x, int y, int width, int height, 
                                char ch, uint8_t color) {
    // Validate and clamp parameters
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= VGA_COLS || y >= VGA_ROWS || width <= 0 || height <= 0) {
        return;
    }
    
    if (x + width > VGA_COLS) {
        width = VGA_COLS - x;
    }
    if (y + height > VGA_ROWS) {
        height = VGA_ROWS - y;
    }
    
    uint16_t entry = vga_entry(ch, color);
    
    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            size_t index = row * VGA_COLS + col;
            vga_buffer[index] = entry;
        }
    }
}