/**
 * @file text_renderer.h
 * @brief Text rendering and formatting interface
 */

#ifndef KERNEL_DRIVERS_DISPLAY_TEXT_RENDERER_H
#define KERNEL_DRIVERS_DISPLAY_TEXT_RENDERER_H

#include <kernel/core/types.h>
#include <kernel/core/error.h>
#include <kernel/drivers/display/vga_hardware.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize text renderer
 * 
 * @return E_SUCCESS on success, error code on failure
 */
error_t text_renderer_init(void);

/**
 * @brief Put a single character without ANSI processing
 * 
 * @param c Character to output
 */
void text_renderer_put_char(char c);

/**
 * @brief Write a string with ANSI escape sequence processing
 * 
 * @param str String to write
 */
void text_renderer_write_string(const char *str);

/**
 * @brief Write a string without ANSI processing
 * 
 * @param str String to write
 */
void text_renderer_write_string_raw(const char *str);

/**
 * @brief Set text color
 * 
 * @param color VGA color attribute
 */
void text_renderer_set_color(uint8_t color);

/**
 * @brief Get current text color
 * 
 * @return Current VGA color attribute
 */
uint8_t text_renderer_get_color(void);

/**
 * @brief Set cursor position
 * 
 * @param x X coordinate
 * @param y Y coordinate
 */
void text_renderer_set_cursor_position(int x, int y);

/**
 * @brief Get cursor position
 * 
 * @param x Output X coordinate (can be NULL)
 * @param y Output Y coordinate (can be NULL)
 */
void text_renderer_get_cursor_position(int *x, int *y);

/**
 * @brief Clear screen
 */
void text_renderer_clear_screen(void);

/**
 * @brief Printf-style formatted output
 * 
 * @param format Format string
 * @param ... Arguments
 * @return Number of characters written
 */
int text_renderer_printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Cleanup text renderer
 */
void text_renderer_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_DRIVERS_DISPLAY_TEXT_RENDERER_H