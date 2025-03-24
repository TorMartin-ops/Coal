#ifndef TERMINAL_H
#define TERMINAL_H

#include <libc/stdint.h>

/**
 * terminal_init
 *
 * Clears the screen, sets default color, enables hardware cursor, 
 * and resets cursor to (0,0).
 */
void terminal_init(void);

/**
 * terminal_set_color
 *
 * Sets the text color for subsequent prints. 
 * color is a byte: (bg << 4) | fg, e.g. 0x1E => white-on-blue.
 */
void terminal_set_color(uint8_t color);

/**
 * terminal_set_foreground
 *
 * Sets only the foreground color nibble, leaving background unchanged.
 */
void terminal_set_foreground(uint8_t fg);

/**
 * terminal_set_background
 *
 * Sets only the background color nibble, leaving foreground unchanged.
 */
void terminal_set_background(uint8_t bg);

/**
 * terminal_get_cursor_pos
 *
 * Returns current cursor (x,y) in 0-based coordinates.
 */
void terminal_get_cursor_pos(int* x, int* y);

/**
 * terminal_set_cursor_pos
 *
 * Moves the cursor to (x,y), clamping if needed.
 */
void terminal_set_cursor_pos(int x, int y);

/**
 * terminal_write_char
 *
 * Writes a single character. 
 * Handles '\n', '\r', '\b', '\t'.
 */
void terminal_write_char(char c);

/**
 * terminal_write
 *
 * Writes a null-terminated string. 
 * Interprets special chars the same as terminal_write_char.
 */
void terminal_write(const char* str);

/**
 * terminal_clear_row
 *
 * Clears the given row with spaces in the current color.
 */
void terminal_clear_row(int row);

/**
 * terminal_clear
 *
 * Clears the entire screen, resets cursor to (0,0).
 */
void terminal_clear(void);

#endif // TERMINAL_H
