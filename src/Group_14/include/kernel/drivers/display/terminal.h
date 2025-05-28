#ifndef TERMINAL_H
#define TERMINAL_H

#include <kernel/core/types.h>
#include <libc/stddef.h> // For size_t
#include <kernel/drivers/input/keyboard.h> // For KeyEvent

/* --- Configuration --- */
#define VGA_ADDRESS 0xC00B8000 // Ensure this matches the mapped virtual address
#define VGA_COLS    80
#define VGA_ROWS    25

// Ensure this matches the value used in terminal.c or remove definition from .c file
#define MAX_INPUT_LENGTH 256 // Maximum characters per interactive input line (including null)


/* --- VGA Colors --- */
// ... (color definitions remain the same) ...
enum VGA_Color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14, // Often displays as yellow
    VGA_COLOR_WHITE = 15,
};

/* --- Public Functions --- */

/**
 * @brief Initializes the terminal, clears the screen, sets up cursor.
 */
void terminal_init(void);

/**
 * @brief Clears the entire terminal screen and resets cursor. Thread-safe.
 */
void terminal_clear(void);

/**
 * @brief Sets the VGA text color attribute.
 * @param color VGA color byte (Background << 4 | Foreground).
 */
void terminal_set_color(uint8_t color);

/**
 * @brief Moves the hardware cursor to the specified position. Thread-safe.
 * @param x Column (0-based).
 * @param y Row (0-based).
 */
void terminal_set_cursor_pos(int x, int y);

/**
 * @brief Gets the current cursor position. Thread-safe.
 * @param x Pointer to store column.
 * @param y Pointer to store row.
 */
void terminal_get_cursor_pos(int* x, int* y);

/**
 * @brief Sets the visibility of the hardware cursor. Thread-safe.
 * @param visible 0 to hide, non-zero to show.
 */
void terminal_set_cursor_visibility(uint8_t visible);


/**
 * @brief Writes a single character to the terminal (VGA and Serial).
 * Handles newline, backspace, tab, scrolling, and basic ANSI codes.
 * This function is thread-safe.
 * @param c Character to write.
 */
void terminal_putchar(char c);


/**
 * @brief Writes a null-terminated string to the terminal (VGA and Serial).
 * This function is thread-safe.
 * @param str The string to write.
 */
void terminal_write(const char* str);

/**
 * @brief Writes raw data of a specific length to the terminal.
 * Useful for writing data that might contain null bytes.
 * This function is thread-safe.
 * @param data Pointer to the data.
 * @param size Number of bytes to write.
 */
void terminal_write_len(const char* data, size_t size);


/**
 * @brief Formatted printing to the terminal (thread-safe).
 * Supports standard formats including %lld, %llu, %llx, %p, %c, %s, %d, %u, %x.
 * Supports basic width/padding like %08x.
 * @param format The format string.
 * @param ... Variable arguments.
 */
void terminal_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));


/* --- Interactive Input Functions --- */

/**
 * @brief Processes a key event for interactive input editing.
 * This function is called by the keyboard driver's callback.
 * It handles line buffering for the SYS_READ_TERMINAL syscall and
 * can also manage the more complex multi-line input_state if active.
 * @param event The keyboard event.
 */
void terminal_handle_key_event(const KeyEvent event);


/**
 * @brief Starts an interactive multi-line input session (for advanced editing). Thread-safe.
 * @param prompt Optional prompt string to display.
 */
void terminal_start_input(const char* prompt);

/**
 * @brief Concatenates all input lines from the multi-line editor into a provided buffer.
 * Lines are separated by newline characters. Thread-safe.
 * @param buffer Destination buffer.
 * @param size Size of the destination buffer.
 * @return Number of bytes written to the buffer (excluding null terminator),
 * or -1 if buffer is too small (output might be truncated).
 */
int terminal_get_input(char* buffer, size_t size);

/**
 * @brief Completes the interactive multi-line input session, moving cursor below input.
 * Thread-safe.
 */
void terminal_complete_input(void);


/* --- Deprecated/Internal Use (or specific callbacks) --- */
/**
 * @brief Legacy function name used by keyboard driver default callback.
 * Simply calls terminal_putchar.
 * @param c Character to write.
 */
void terminal_write_char(char c);

/**
 * @brief Handles a backspace operation on the terminal display.
 */
void terminal_backspace(void);

/**
 * @brief Writes a sequence of bytes to the terminal. (Used by syscall_write for STDOUT/STDERR)
 * @param data Pointer to the data.
 * @param size Number of bytes to write.
 */
void terminal_write_bytes(const char* data, size_t size);

/**
 * @brief Reads a line of input from the terminal, blocking until Enter is pressed.
 * This function is intended to be called by the SYS_READ_TERMINAL syscall handler.
 * It interacts with the internal line buffer populated by terminal_handle_key_event.
 * @param kbuf Kernel buffer to store the input line.
 * @param len Maximum number of bytes to read into kbuf (including null terminator).
 * @return The number of bytes read (excluding null terminator), or a negative error code.
 * Returns 0 for an empty line if Enter is pressed immediately.
 */
ssize_t terminal_read_line_blocking(char *kbuf, size_t len);


#endif // TERMINAL_H
