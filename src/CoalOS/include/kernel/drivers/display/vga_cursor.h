/**
 * @file vga_cursor.h
 * @brief VGA Hardware Cursor Management Interface
 * 
 * Provides functions for controlling the VGA text mode hardware cursor.
 */

#ifndef KERNEL_DRIVERS_DISPLAY_VGA_CURSOR_H
#define KERNEL_DRIVERS_DISPLAY_VGA_CURSOR_H

#include <libc/stdint.h>
#include <libc/stdbool.h>

/**
 * @brief Enable the hardware cursor with default shape
 */
void vga_cursor_enable(void);

/**
 * @brief Disable the hardware cursor
 */
void vga_cursor_disable(void);

/**
 * @brief Set cursor position
 * @param x Column position (0-79)
 * @param y Row position (0-24)
 */
void vga_cursor_set_position(int x, int y);

/**
 * @brief Get current cursor position
 * @param x Output parameter for column position
 * @param y Output parameter for row position
 */
void vga_cursor_get_position(int *x, int *y);

/**
 * @brief Check if cursor is enabled
 * @return true if cursor is enabled, false otherwise
 */
bool vga_cursor_is_enabled(void);

/**
 * @brief Set cursor shape/size
 * @param start_scanline Starting scan line (0-15)
 * @param end_scanline Ending scan line (0-15)
 */
void vga_cursor_set_shape(uint8_t start_scanline, uint8_t end_scanline);

#endif // KERNEL_DRIVERS_DISPLAY_VGA_CURSOR_H