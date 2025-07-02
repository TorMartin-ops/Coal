/**
 * @file terminal_buffer.h
 * @brief Terminal Buffer Management Interface
 * 
 * Provides functions for manipulating the VGA text mode buffer including
 * character placement, scrolling, clearing, and region operations.
 */

#ifndef KERNEL_DRIVERS_DISPLAY_TERMINAL_BUFFER_H
#define KERNEL_DRIVERS_DISPLAY_TERMINAL_BUFFER_H

#include <libc/stdint.h>
#include <libc/stdbool.h>

/**
 * @brief Put a character at specific position
 * @param c Character to place
 * @param color Color attribute
 * @param x Column position (0-79)
 * @param y Row position (0-24)
 */
void terminal_buffer_put_char_at(char c, uint8_t color, int x, int y);

/**
 * @brief Get character at specific position
 * @param x Column position
 * @param y Row position
 * @return Character at position (0 if out of bounds)
 */
char terminal_buffer_get_char_at(int x, int y);

/**
 * @brief Get color at specific position
 * @param x Column position
 * @param y Row position
 * @return Color attribute at position (0 if out of bounds)
 */
uint8_t terminal_buffer_get_color_at(int x, int y);

/**
 * @brief Clear a specific row
 * @param row Row to clear (0-24)
 * @param color Color to fill with
 */
void terminal_buffer_clear_row(int row, uint8_t color);

/**
 * @brief Clear entire terminal buffer
 * @param color Color to fill with
 */
void terminal_buffer_clear(uint8_t color);

/**
 * @brief Scroll terminal up by one line
 * @param preserve_color If true, preserve existing colors; if false, use provided color
 * @param fill_color Color for new bottom line (used if preserve_color is false)
 */
void terminal_buffer_scroll_up(bool preserve_color, uint8_t fill_color);

/**
 * @brief Scroll terminal down by one line
 * @param preserve_color If true, preserve existing colors; if false, use provided color
 * @param fill_color Color for new top line (used if preserve_color is false)
 */
void terminal_buffer_scroll_down(bool preserve_color, uint8_t fill_color);

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
                                int width, int height);

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
                                char ch, uint8_t color);

#endif // KERNEL_DRIVERS_DISPLAY_TERMINAL_BUFFER_H