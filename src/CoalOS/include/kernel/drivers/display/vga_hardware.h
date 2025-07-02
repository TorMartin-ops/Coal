/**
 * @file vga_hardware.h
 * @brief VGA hardware abstraction interface
 */

#ifndef KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H
#define KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>

// VGA constants
#define VGA_ADDRESS 0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25

// VGA colors
#define VGA_COLOR_BLACK         0
#define VGA_COLOR_BLUE          1
#define VGA_COLOR_GREEN         2
#define VGA_COLOR_CYAN          3
#define VGA_COLOR_RED           4
#define VGA_COLOR_MAGENTA       5
#define VGA_COLOR_BROWN         6
#define VGA_COLOR_LIGHT_GREY    7
#define VGA_COLOR_DARK_GREY     8
#define VGA_COLOR_LIGHT_BLUE    9
#define VGA_COLOR_LIGHT_GREEN   10
#define VGA_COLOR_LIGHT_CYAN    11
#define VGA_COLOR_LIGHT_RED     12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_LIGHT_BROWN   14
#define VGA_COLOR_WHITE         15

// Color helper macros
#define VGA_RGB(fg,bg)   (uint8_t)(((bg)&0x0F)<<4 | ((fg)&0x0F))
#define VGA_FG(attr)     ((attr)&0x0F)
#define VGA_BG(attr)     (((attr)>>4)&0x0F)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize VGA hardware
 * 
 * @return E_SUCCESS on success, error code on failure
 */
error_t vga_hardware_init(void);

/**
 * @brief Set cursor position on screen
 * 
 * @param x X coordinate (0-79)
 * @param y Y coordinate (0-24)
 */
void vga_set_cursor_position(int x, int y);

/**
 * @brief Set cursor visibility
 * 
 * @param visible True to show cursor, false to hide
 */
void vga_set_cursor_visible(bool visible);

/**
 * @brief Get cursor visibility state
 * 
 * @return True if cursor is visible, false otherwise
 */
bool vga_is_cursor_visible(void);

/**
 * @brief Write a character directly to VGA buffer
 * 
 * @param x X coordinate
 * @param y Y coordinate  
 * @param c Character to write
 * @param color Color attribute
 */
void vga_write_char_at(int x, int y, char c, uint8_t color);

/**
 * @brief Read a character from VGA buffer
 * 
 * @param x X coordinate
 * @param y Y coordinate
 * @return Character and color attribute as uint16_t
 */
uint16_t vga_read_char_at(int x, int y);

/**
 * @brief Clear the entire screen
 * 
 * @param color Background color to fill with
 */
void vga_clear_screen(uint8_t color);

/**
 * @brief Scroll screen up by one line
 * 
 * @param color Color for the new bottom line
 */
void vga_scroll_up(uint8_t color);

/**
 * @brief Cleanup VGA hardware
 */
void vga_hardware_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H