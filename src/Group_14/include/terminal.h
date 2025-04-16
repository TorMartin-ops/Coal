#ifndef TERMINAL_H
#define TERMINAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "keyboard.h"   // Needed for KeyEvent type


// <<< ADD THESE DEFINES HERE >>>
#define VGA_ADDRESS 0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25
// <<< END OF ADDED DEFINES >>>

#define MAX_INPUT_LENGTH 256

/**
 * @brief Initializes the VGA text-mode terminal.
 *
 * Clears the screen, sets the default text color (light gray on black),
 * and enables the hardware cursor.
 */
void terminal_init(void);

/**
 * @brief Clears the entire terminal screen and resets the cursor position.
 */
void terminal_clear(void);

/**
 * @brief Clears a specific row of the terminal.
 *
 * @param row The 0-based row index to clear.
 */
void terminal_clear_row(int row);

/**
 * @brief Sets the overall text color.
 *
 * The color is represented as a byte where the lower 4 bits are the foreground
 * and the upper 4 bits are the background color.
 *
 * @param color Color value.
 */
void terminal_set_color(uint8_t color);

/**
 * @brief Sets only the foreground (text) color.
 *
 * @param fg Foreground color (0–15).
 */
void terminal_set_foreground(uint8_t fg);

/**
 * @brief Sets only the background color.
 *
 * @param bg Background color (0–15).
 */
void terminal_set_background(uint8_t bg);

/**
 * @brief Retrieves the current cursor position.
 *
 * @param x Pointer to store the horizontal position.
 * @param y Pointer to store the vertical position.
 */
void terminal_get_cursor_pos(int* x, int* y);

/**
 * @brief Moves the cursor to a specified position.
 *
 * Coordinates are clamped to terminal dimensions.
 *
 * @param x New horizontal coordinate.
 * @param y New vertical coordinate.
 */
void terminal_set_cursor_pos(int x, int y);

/**
 * @brief Enables or disables the hardware cursor.
 *
 * @param visible 1 to show the cursor; 0 to hide it.
 */
void terminal_set_cursor_visibility(uint8_t visible);

/**
 * @brief Outputs a single character to the terminal.
 *
 * Interprets control characters such as newline (\n), carriage return (\r),
 * backspace (\b), and tab (\t). This function is used when interactive input
 * is not active.
 *
 * @param c Character to output.
 */
void terminal_write_char(char c);

/**
 * @brief Outputs a null-terminated string to the terminal.
 *
 * @param str String to output.
 */
void terminal_write(const char* str);

/**
 * @brief Prints a formatted representation of a keyboard event.
 *
 * Displays the key code, action (PRESS, RELEASE, or REPEAT), modifiers (in hex),
 * and timestamp.
 *
 * @param event Pointer to the KeyEvent structure.
 */
void terminal_print_key_event(const void* event);

/**
 * @brief A basic formatted print function.
 *
 * Supports %s (string), %d (decimal), %x (hexadecimal), and %% (literal '%').
 *
 * @param format Format string.
 * @param ... Additional arguments.
 */
void terminal_printf(const char* format, ...);

/**
 * @brief Processes keyboard events for interactive multi-line editing.
 *
 * Handles editing operations such as inserting and deleting characters,
 * splitting lines (Enter), merging lines (Backspace/Delete), and navigation using
 * arrow keys. In addition to left/right/home/end, UP and DOWN keys move between lines
 * with smooth column preservation.
 *
 * @param event The KeyEvent representing the key action.
 */
void terminal_handle_key_event(const KeyEvent event);

/**
 * @brief Begins an interactive multi-line input session with an optional prompt.
 *
 * After calling this function, the terminal accepts interactive input with
 * full multi-line editing support (including smooth vertical navigation).
 *
 * @param prompt Optional prompt string.
 */
void terminal_start_input(const char* prompt);

/**
 * @brief Returns the current interactive input as a single concatenated string.
 *
 * The multi-line input is combined with newline characters separating lines.
 *
 * @return Pointer to the concatenated input buffer.
 */
const char* terminal_get_input(void);

/**
 * @brief Completes the current interactive input session.
 *
 * Finalizes input, echoes a newline, and advances the output cursor to below the input.
 */
void terminal_complete_input(void);

/**
 * @brief Outputs a single character (non-interactive).
 *
 * This function is provided as an alias for terminal_write_char and is
 * used when interactive input is not active.
 */
void terminal_putchar(char c);


#ifndef KERNEL_PANIC_HALT // Prevent multiple definitions
#define KERNEL_PANIC_HALT(msg) do { \
    terminal_printf("\n[KERNEL PANIC] %s at %s:%d. System Halted.\n", msg, __FILE__, __LINE__); \
    while(1) { asm volatile("cli; hlt"); } \
} while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif // TERMINAL_H
