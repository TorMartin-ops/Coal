/**
 * terminal.c
 * VGA text-mode driver for a 32-bit x86 OS.
 *
 * This module provides functions to initialize and control the VGA text-mode
 * terminal. It supports standard text output, scrolling, and interactive multi-line
 * input editing. The interactive input area always starts below the kernel's printed text,
 * and the user is prevented from moving the input cursor into the kernel output area.
 *
 * Production–quality design emphasizes modularity, maintainability, and enhanced UI/UX.
 */

 #include "terminal.h"
 #include "port_io.h"      // For outb, inb
 #include "keyboard.h"     // For KeyEvent, KeyCode, keyboard_get_modifiers, apply_modifiers_extended, etc.
 #include "types.h"
 #include <libc/stdarg.h>       // For va_list, va_start, va_end
 #include <string.h>            // For memset, memcpy, memmove, strlen, strcat
 #include "serial.h"
 #include <libc/stdint.h> // Ensure uint64_t, int64_t are defined 

 /* Fixed tab width (number of spaces per tab) */
 #define TAB_WIDTH   4

 /* --- Multi-Line Interactive Input Definitions --- */
 #define MAX_INPUT_LINES 256  // Allow up to 256 lines

 // Each input line is stored in a fixed-size buffer.
 static char input_lines[MAX_INPUT_LINES][MAX_INPUT_LENGTH];
 // Stores the current length of each line.
 static int line_lengths[MAX_INPUT_LINES];
 // The index (0-based) of the current line being edited.
 static int current_line = 0;
 // Total number of input lines currently in the buffer.
 static int total_lines = 1;
 // Horizontal cursor position within the current line.
 static int input_cursor = 0;
 // The starting row (in terminal coordinates) where interactive input is displayed.
 // This is set when interactive input begins, and cannot be moved upward.
 static int input_start_row = 35; // Note: This was hardcoded, consider setting dynamically based on cursor_y
 // The desired column used for vertical navigation.
 static int desired_column = 0;

 /* --- Forward Declarations --- */
 static void update_hardware_cursor(void);
 static void enable_hardware_cursor(void);
 static void put_char_at(char c, int x, int y);
 static void scroll_one_line(void);
 static void redraw_input(void);
 static void update_desired_column(void);
 static void erase_character(void);
 static void insert_character(char c);
 static int mini_vsnprintf(char *str, size_t size, const char *format, va_list args);
 static int vsnprintf(char *str, size_t size, const char *format, va_list args);
 static void _reverse(char* str, int len);
 static int _utoa(uint32_t num, char* str, int base, bool uppercase_hex);
 static int _itoa(int num, char* str);
 static int _ulltoa_hex(uint64_t num, char* str, bool uppercase_hex);

 /* Internal function that writes a character without affecting interactive input */
 static void terminal_write_char_internal(char c);

 /*
  * Public function to output a character.
  * This symbol is used by the keyboard driver (default callback) so it must be defined.
  */
 void terminal_write_char(char c);


/* --- Static Helper Function Definitions --- */

// Helper to reverse a string in place
static void _reverse(char* str, int len) {
    int i = 0, j = len - 1;
    while (i < j) {
        char temp = str[i];
        str[i] = str[j];
        str[j] = temp;
        i++; j--;
    }
}

// Helper for 32-bit unsigned integer to ASCII (supports different bases)
// Returns the number of characters written (excluding null terminator)
static int _utoa(uint32_t num, char* str, int base, bool uppercase_hex) {
    int i = 0;
    const char* digits = uppercase_hex ? "0123456789ABCDEF" : "0123456789abcdef";

    if (base < 2 || base > 16) {
        str[0] = '\0';
        return 0;
    }

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return 1;
    }

    while (num != 0) {
        int rem = num % base; // Standard 32-bit division/modulo
        str[i++] = digits[rem];
        num = num / base;
    }

    str[i] = '\0';
    _reverse(str, i);
    return i;
}
// Helper for 32-bit signed integer to ASCII (base 10 only)
// Returns the number of characters written (excluding null terminator)
static int _itoa(int num, char* str) {
    int i = 0;
    bool is_negative = false;
    uint32_t unsigned_num;

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return 1;
    }

    if (num < 0) {
        is_negative = true;
        // Correctly handle INT_MIN for 32-bit
        unsigned_num = (num == (-2147483647 - 1)) ? 2147483648U : (uint32_t)(-num);
    } else {
        unsigned_num = (uint32_t)num;
    }

    // Convert the unsigned number to base 10
    while (unsigned_num != 0) {
        int rem = unsigned_num % 10;
        str[i++] = rem + '0';
        unsigned_num = unsigned_num / 10;
    }

    if (is_negative) {
        str[i++] = '-';
    }

    str[i] = '\0';
    _reverse(str, i);
    return i;
}

// Helper for 64-bit unsigned integer to Hex ASCII
// Returns the number of characters written (excluding null terminator)
// **NOTE:** Relies on the compiler providing 64-bit integer division/modulo support.
static int _ulltoa_hex(uint64_t num, char* str, bool uppercase_hex) {
    int i = 0;
    const char* digits = uppercase_hex ? "0123456789ABCDEF" : "0123456789abcdef";

    if (num == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return 1;
    }

    // Extract hex digits from low to high
    while (num != 0) {
        // Use standard C % and / which should invoke compiler helpers for uint64_t
        int rem = (int)(num % 16);
        str[i++] = digits[rem];
        num = num / 16;
    }

    str[i] = '\0';
    _reverse(str, i);
    return i;
}

static int vsnprintf(char *str, size_t size, const char *format, va_list args) {
    return mini_vsnprintf(str, size, format, args);
}


 /* --- Minimal vsnprintf Implementation --- */
/* --- Revised Minimal vsnprintf Implementation (with %llx) --- */
// Supports: %s, %d, %u, %x, %X, %p, %%, %llx, %llX
// Does NOT support width, precision, other flags, other length modifiers.
// NOTE: %lld / %llu are NOT fully supported due to 64-bit decimal complexity.
static int mini_vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (!str || size == 0) return 0;

    size_t written = 0;
    char temp_buf[65]; // Buffer for number conversions (64 bits hex + null)
    bool is_long_long = false; // Track 'll' modifier

    while (*format && written < size - 1) {
        if (*format != '%') {
            str[written++] = *format++;
            continue;
        }

        // --- Process flags after '%' ---
        format++; // Skip '%'
        is_long_long = false;

        // Check for 'll' modifier (basic check)
        if (format[0] == 'l' && format[1] == 'l') {
            is_long_long = true;
            format += 2;
        }
        // Add checks for other modifiers (l, h, hh, etc.) here if needed

        // --- Process specifier ---
        switch (*format) {
            case 's': {
                // Ignore 'll' for strings
                const char *s_arg = va_arg(args, const char *);
                if (!s_arg) s_arg = "(null)";
                while (*s_arg && written < size - 1) {
                    str[written++] = *s_arg++;
                }
                break;
            }
            case 'd': {
                if (is_long_long) {
                    // --- 64-bit Signed Decimal (%lld) ---
                    // NOT IMPLEMENTED - Complex 64-bit division/modulo required.
                    // Fallback: Print as hex or placeholder
                    long long lld_arg = va_arg(args, long long); // Get 64-bit arg
                    const char* msg = "[lld NI]"; // Not Implemented placeholder
                    for(int k=0; msg[k] && written < size -1; ++k) str[written++] = msg[k];
                    // Alternative: Print as hex
                    // int len = _ulltoa_hex((uint64_t)lld_arg, temp_buf, false); // Print as hex
                    // if (lld_arg < 0 && written < size -1) str[written++] = '-'; // Basic sign handling
                    // for (int k = 0; k < len && written < size - 1; k++) str[written++] = temp_buf[k];
                } else {
                    // 32-bit Signed Decimal (%d)
                    int d_arg = va_arg(args, int);
                    int len = _itoa(d_arg, temp_buf);
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                }
                break;
            }
            case 'u': {
                if (is_long_long) {
                    // --- 64-bit Unsigned Decimal (%llu) ---
                    // NOT IMPLEMENTED - Complex 64-bit division/modulo required.
                    // Fallback: Print as hex or placeholder
                    unsigned long long llu_arg = va_arg(args, unsigned long long); // Get 64-bit arg
                    const char* msg = "[llu NI]"; // Not Implemented placeholder
                    for(int k=0; msg[k] && written < size -1; ++k) str[written++] = msg[k];
                    // Alternative: Print as hex
                    // int len = _ulltoa_hex(llu_arg, temp_buf, false);
                    // for (int k = 0; k < len && written < size - 1; k++) str[written++] = temp_buf[k];
                } else {
                    // 32-bit Unsigned Decimal (%u)
                    unsigned int u_arg = va_arg(args, unsigned int);
                    int len = _utoa(u_arg, temp_buf, 10, false);
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                }
                break;
            }
            case 'x': // Lowercase hex
            case 'X': { // Uppercase hex
                bool uppercase = (*format == 'X');
                if (is_long_long) {
                    // --- 64-bit Hex (%llx, %llX) ---
                    unsigned long long llx_arg = va_arg(args, unsigned long long);
                    int len = _ulltoa_hex(llx_arg, temp_buf, uppercase);
                    // Optional "0x" prefix
                    // if (written < size - 3) { str[written++] = '0'; str[written++] = 'x'; }
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                } else {
                    // 32-bit Hex (%x, %X)
                    unsigned int x_arg = va_arg(args, unsigned int);
                    int len = _utoa(x_arg, temp_buf, 16, uppercase);
                    // Optional "0x" prefix
                    // if (written < size - 3) { str[written++] = '0'; str[written++] = 'x'; }
                    for (int k = 0; k < len && written < size - 1; k++) {
                        str[written++] = temp_buf[k];
                    }
                }
                break;
            }
            case 'p': { // Pointer (%p) - Ignore 'll' modifier
                void* p_arg = va_arg(args, void*);
                uintptr_t ptr_val = (uintptr_t)p_arg;
                if (written < size - 3) { str[written++] = '0'; str[written++] = 'x'; }
                int len = _utoa(ptr_val, temp_buf, 16, false);
                int padding = 8 - len; // Pad 32-bit pointers to 8 digits
                for (int p = 0; p < padding && written < size - 1; ++p) str[written++] = '0';
                for (int k = 0; k < len && written < size - 1; k++) str[written++] = temp_buf[k];
                break;
            }
            case '%': { // Literal '%%'
                str[written++] = '%';
                break;
            }
            default: // Unknown format specifier
                str[written++] = '%';
                // Print modifier if present
                if (is_long_long) {
                     if (written < size - 1) str[written++] = 'l';
                     if (written < size - 1) str[written++] = 'l';
                }
                // Print the unknown specifier char itself
                if (*format && written < size - 1) {
                    str[written++] = *format;
                }
                break;
        } // End switch(*format)

        // Ensure we advance format pointer past the specifier
        if (*format) {
             format++;
        }

    } // End while loop

    str[written] = '\0'; // Null-terminate
    return written;
}

 /* --- Terminal Output Global State --- */
 static uint8_t terminal_color = 0x07;    // Light gray on black.
 static uint16_t* vga_buffer = (uint16_t*)VGA_ADDRESS;
 static int cursor_x = 0;
 static int cursor_y = 0;
 static uint8_t cursor_visible = 1;         // 1: visible; 0: hidden.

 /* --- Terminal Functions --- */

 /**
  * enable_hardware_cursor - Configures the VGA hardware cursor shape.
  */
 static void enable_hardware_cursor(void) {
     if (!cursor_visible)
         return;
     outb(0x3D4, 0x0A);
     outb(0x3D5, (inb(0x3D5) & 0xC0) | 14); // Start scanline 14
     outb(0x3D4, 0x0B);
     outb(0x3D5, (inb(0x3D5) & 0xE0) | 15); // End scanline 15 (makes a block cursor)
 }

 /**
  * put_char_at - Writes a character at a specific (x, y) location.
  */
 static void put_char_at(char c, int x, int y) {
     if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) return; // Bounds check
     int index = y * VGA_COLS + x;
     vga_buffer[index] = (uint16_t)(c | (terminal_color << 8));
 }

 /**
  * terminal_init - Initializes the terminal.
  */
 void terminal_init(void) {
     terminal_clear();
     enable_hardware_cursor();
     update_hardware_cursor();
 }

 /**
  * terminal_clear - Clears the entire screen and resets the cursor.
  *
  * Also resets the multi-line interactive input state.
  */
 void terminal_clear(void) {
     for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
         vga_buffer[i] = (uint16_t)(' ' | (terminal_color << 8));
     }
     cursor_x = 0;
     cursor_y = 0;
     /* Reset multi-line input state */
     current_line = 0;
     total_lines = 1;
     input_cursor = 0;
     input_start_row = cursor_y; // Start input dynamically where cursor is after clear
     desired_column = 0;
     if (MAX_INPUT_LINES > 0) { // Check bounds before accessing
         line_lengths[0] = 0;
         input_lines[0][0] = '\0';
     }
     update_hardware_cursor();
 }

 /**
  * terminal_clear_row - Clears a specific row on the screen.
  */
 void terminal_clear_row(int row) {
     if (row < 0 || row >= VGA_ROWS)
         return;
     int start = row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; col++) {
         vga_buffer[start + col] = (uint16_t)(' ' | (terminal_color << 8));
     }
 }

 /**
  * scroll_one_line - Scrolls the screen up by one line.
  */
 static void scroll_one_line(void) {
     memmove(vga_buffer, vga_buffer + VGA_COLS, (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
     terminal_clear_row(VGA_ROWS - 1);
 }

 /**
  * update_hardware_cursor - Updates the hardware cursor to (cursor_x, cursor_y).
  */
 static void update_hardware_cursor(void) {
     if (!cursor_visible) {
         // Hide cursor by setting start scanline > end scanline
         outb(0x3D4, 0x0A);
         outb(0x3D5, 0x20);
         return;
     }
     // Ensure cursor position is valid before calculating hardware position
     if (cursor_x < 0) cursor_x = 0;
     if (cursor_y < 0) cursor_y = 0;
     if (cursor_x >= VGA_COLS) cursor_x = VGA_COLS - 1;
     if (cursor_y >= VGA_ROWS) cursor_y = VGA_ROWS - 1;

     uint16_t pos = cursor_y * VGA_COLS + cursor_x;
     outb(0x3D4, 0x0F); // Cursor Location Low Register
     outb(0x3D5, (uint8_t)(pos & 0xFF));
     outb(0x3D4, 0x0E); // Cursor Location High Register
     outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
 }

 /**
  * terminal_set_cursor_pos - Moves the cursor to (x, y).
  */
 void terminal_set_cursor_pos(int x, int y) {
     // Clamp coordinates to valid range
     if (x < 0) x = 0;
     if (y < 0) y = 0;
     if (x >= VGA_COLS) x = VGA_COLS - 1;
     if (y >= VGA_ROWS) y = VGA_ROWS - 1;
     cursor_x = x;
     cursor_y = y;
     update_hardware_cursor();
 }

 /**
  * terminal_get_cursor_pos - Retrieves the current cursor position.
  */
 void terminal_get_cursor_pos(int* x, int* y) {
     if (x) *x = cursor_x;
     if (y) *y = cursor_y;
 }

 /**
  * terminal_set_cursor_visibility - Sets hardware cursor visibility.
  */
 void terminal_set_cursor_visibility(uint8_t visible) {
     cursor_visible = (visible != 0); // Ensure it's 0 or 1
     if (cursor_visible) {
         enable_hardware_cursor(); // Make sure cursor shape is set if enabling
     }
     update_hardware_cursor(); // Update visibility/position
 }

 /**
  * terminal_set_color - Sets the overall text color.
  */
 void terminal_set_color(uint8_t color) {
     terminal_color = color;
 }

 /**
  * terminal_set_foreground - Sets the foreground color.
  */
 void terminal_set_foreground(uint8_t fg) {
     terminal_color = (terminal_color & 0xF0) | (fg & 0x0F);
 }

 /**
  * terminal_set_background - Sets the background color.
  */
 void terminal_set_background(uint8_t bg) {
     terminal_color = ((bg & 0x0F) << 4) | (terminal_color & 0x0F);
 }

 /**
  * terminal_write_char_internal - Internal function for non-interactive output.
  * Now also calls serial_putchar.
  */
 static void terminal_write_char_internal(char c) {
     // --- VGA Output Logic ---
     switch (c) {
         case '\n':
             cursor_x = 0;
             cursor_y++;
             break;
         case '\r':
             cursor_x = 0;
             break;
         case '\b': // Backspace - Move cursor left, overwrite with space
              if (cursor_x > 0) {
                   cursor_x--;
                   put_char_at(' ', cursor_x, cursor_y);
              } else if (cursor_y > 0) {
                   // Move to end of previous line (optional behavior)
                   // cursor_y--;
                   // cursor_x = VGA_COLS - 1;
                   // put_char_at(' ', cursor_x, cursor_y);
              }
              break;
         case 0x7F: // Delete (often treated like backspace)
              // Ignore delete for non-interactive output, or implement forward delete?
              break;
         case '\t': {
             int next_tab_stop = ((cursor_x / TAB_WIDTH) + 1) * TAB_WIDTH;
             int spaces = next_tab_stop - cursor_x;
             // Prevent tab from wrapping lines excessively
             if (next_tab_stop >= VGA_COLS) {
                  spaces = VGA_COLS - 1 - cursor_x;
             }
             for (int i = 0; i < spaces && cursor_x < VGA_COLS -1; i++) {
                 put_char_at(' ', cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
         }
         default:
             // Only print printable ASCII characters
             if (c >= ' ' && c <= '~') {
                 put_char_at(c, cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
     }

     // Handle line wrap
     if (cursor_x >= VGA_COLS) {
         cursor_x = 0;
         cursor_y++;
     }

     // Handle scrolling
     if (cursor_y >= VGA_ROWS) {
         scroll_one_line();
         cursor_y = VGA_ROWS - 1;
         // Adjust input_start_row if scrolling pushes it off screen?
         if (input_start_row > 0) {
            input_start_row--;
         }
     }

     update_hardware_cursor();

     // --- Serial Output Logic ---
     serial_putchar(c); // Send the same character to the serial port
 }

 /**
  * terminal_write_char - Public function for non-interactive output.
  */
 void terminal_write_char(char c) {
     terminal_write_char_internal(c);
 }

 /**
  * terminal_write - Outputs a null-terminated string.
  */
 void terminal_write(const char* str) {
     if (!str) return;
     for (size_t i = 0; str[i] != '\0'; i++) {
         terminal_write_char(str[i]); // This will now output to both VGA and Serial
     }
 }

 /**
  * terminal_printf - Formatted printing.
  */
  void terminal_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[128]; // Static buffer size might still be limiting
    // Use the internal vsnprintf wrapper which calls mini_vsnprintf
    vsnprintf(buf, sizeof(buf), format, args);
    terminal_write(buf); // terminal_write handles dual output (VGA + Serial)
    va_end(args);
}

 /**
  * terminal_print_key_event - Prints a formatted key event (for debugging).
  */
 void terminal_print_key_event(const void* event) {
     if (!event) return;
     const KeyEvent* ke = (const KeyEvent*)event;
     char hex_code[9];
     char hex_mods[3];

     // Minimal hex conversion helper
     static const char hex_chars[] = "0123456789abcdef";
     hex_code[0] = '0'; hex_code[1] = 'x';
     hex_code[2] = hex_chars[(ke->code >> 12) & 0xF];
     hex_code[3] = hex_chars[(ke->code >> 8) & 0xF];
     hex_code[4] = hex_chars[(ke->code >> 4) & 0xF];
     hex_code[5] = hex_chars[ke->code & 0xF];
     hex_code[6] = '\0'; // Shorter hex for typical codes

     hex_mods[0] = hex_chars[(ke->modifiers >> 4) & 0xF];
     hex_mods[1] = hex_chars[ke->modifiers & 0xF];
     hex_mods[2] = '\0';

     // Print using terminal_printf which handles dual output
     terminal_printf("KeyEvent: code=%s (%c), action=%s, mods=0x%s, time=%d\n",
                     hex_code,
                     (ke->code >= ' ' && ke->code <= '~') ? (char)ke->code : '?',
                     (ke->action == KEY_PRESS) ? "PRESS" : ((ke->action == KEY_RELEASE) ? "RELEASE" : "REPEAT"),
                     hex_mods,
                     ke->timestamp);
 }

 /* --- Interactive Multi-Line Input Functions --- */

 /**
  * redraw_input - Redraws the visible portion of the interactive input.
  */
 static void redraw_input(void) {
     // Ensure input_start_row is valid
     if (input_start_row >= VGA_ROWS) {
         input_start_row = VGA_ROWS - 1;
     }
     if (input_start_row < 0 ) {
         input_start_row = 0; // Cannot be negative
     }

     int available_rows = VGA_ROWS - input_start_row;
     if (available_rows <= 0) return; // No space to draw input

     int first_visible_line = 0;
     if (total_lines > available_rows) {
         first_visible_line = total_lines - available_rows;
     }
     // Ensure current line is visible
     if (current_line < first_visible_line) {
          first_visible_line = current_line;
     } else if (current_line >= first_visible_line + available_rows) {
          first_visible_line = current_line - available_rows + 1;
     }


     // Draw the visible lines
     for (int i = 0; i < available_rows; ++i) {
          int line_index = first_visible_line + i;
          int screen_row = input_start_row + i;
          terminal_clear_row(screen_row); // Clear the screen row first

          if (line_index < total_lines) { // Check if this line index is valid
               for (int j = 0; j < line_lengths[line_index] && j < VGA_COLS; ++j) {
                    put_char_at(input_lines[line_index][j], j, screen_row);
               }
          }
     }

     // Update cursor position
     int cursor_screen_row = input_start_row + (current_line - first_visible_line);
     terminal_set_cursor_pos(input_cursor, cursor_screen_row);
 }

 /**
  * update_desired_column - Updates desired_column to current input_cursor.
  */
 static void update_desired_column(void) {
     desired_column = input_cursor;
 }

 /**
  * erase_character - Erases the character at the current cursor position or merges lines.
  */
 static void erase_character(void) {
     if (current_line < 0 || current_line >= total_lines) return; // Safety check

     if (input_cursor > 0) { // Erase within the line
         memmove(&input_lines[current_line][input_cursor - 1],
                 &input_lines[current_line][input_cursor],
                 line_lengths[current_line] - input_cursor + 1); // +1 for null terminator
         line_lengths[current_line]--;
         input_cursor--;
     } else if (current_line > 0) { // Merge with previous line
         int prev_len = line_lengths[current_line - 1];
         int current_len = line_lengths[current_line];
         if (prev_len + current_len < MAX_INPUT_LENGTH) { // Check if merge fits
             // Append current line to previous line
             memcpy(&input_lines[current_line - 1][prev_len],
                    input_lines[current_line],
                    current_len + 1); // +1 for null terminator
             line_lengths[current_line - 1] += current_len;

             // Shift subsequent lines up
             for (int l = current_line; l < total_lines - 1; l++) {
                 memcpy(input_lines[l], input_lines[l + 1], MAX_INPUT_LENGTH);
                 line_lengths[l] = line_lengths[l + 1];
             }
             total_lines--;
             current_line--;
             input_cursor = prev_len; // Set cursor to merge point
         }
     }
 }

 /**
  * insert_character - Inserts a character at the current cursor position.
  */
 static void insert_character(char c) {
    if (current_line < 0 || current_line >= total_lines) return; // Safety check

     if (line_lengths[current_line] < MAX_INPUT_LENGTH - 1) {
         // Make space for the new character
         memmove(&input_lines[current_line][input_cursor + 1],
                 &input_lines[current_line][input_cursor],
                 line_lengths[current_line] - input_cursor + 1); // +1 for null terminator
         // Insert the character
         input_lines[current_line][input_cursor] = c;
         line_lengths[current_line]++;
         input_cursor++;
     }
 }

 /**
  * terminal_handle_key_event - Processes key events for multi-line interactive editing.
  */
 void terminal_handle_key_event(const KeyEvent event) {
     if (event.action != KEY_PRESS && event.action != KEY_REPEAT) // Handle repeats too
         return;

     // Boundary checks
     if (current_line < 0) current_line = 0;
     if (current_line >= total_lines) current_line = total_lines - 1;
     if (input_cursor < 0) input_cursor = 0;
     if (input_cursor > line_lengths[current_line]) input_cursor = line_lengths[current_line];

     int code = (int)event.code;
     switch (code) {
         case KEY_LEFT:
             if (input_cursor > 0) {
                 input_cursor--;
             } else if (current_line > 0) { // Wrap to previous line end
                 current_line--;
                 input_cursor = line_lengths[current_line];
             }
             update_desired_column();
             break;
         case KEY_RIGHT:
             if (input_cursor < line_lengths[current_line]) {
                 input_cursor++;
             } else if (current_line < total_lines - 1) { // Wrap to next line beginning
                 current_line++;
                 input_cursor = 0;
             }
             update_desired_column();
             break;
         case KEY_HOME:
             input_cursor = 0;
             update_desired_column();
             break;
         case KEY_END:
             input_cursor = line_lengths[current_line];
             update_desired_column();
             break;
         case KEY_UP:
             if (current_line > 0) {
                 current_line--;
                 // Try to maintain column, clamp if needed
                 input_cursor = (desired_column > line_lengths[current_line]) ? line_lengths[current_line] : desired_column;
             }
             break;
         case KEY_DOWN:
             if (current_line < total_lines - 1) {
                 current_line++;
                 // Try to maintain column, clamp if needed
                 input_cursor = (desired_column > line_lengths[current_line]) ? line_lengths[current_line] : desired_column;
             }
             break;
         case '\b':  // Backspace
             erase_character();
             update_desired_column();
             break;
         case KEY_DELETE: // Forward delete
             if (input_cursor < line_lengths[current_line]) {
                 memmove(&input_lines[current_line][input_cursor],
                         &input_lines[current_line][input_cursor + 1],
                         line_lengths[current_line] - input_cursor); // No +1 needed here
                 line_lengths[current_line]--;
             } else if (current_line < total_lines - 1) { // Merge with next line if at end
                 int current_len = line_lengths[current_line];
                 int next_len = line_lengths[current_line + 1];
                 if (current_len + next_len < MAX_INPUT_LENGTH) {
                     memcpy(&input_lines[current_line][current_len],
                            input_lines[current_line + 1],
                            next_len + 1); // Copy null terminator
                     line_lengths[current_line] += next_len;
                     // Shift subsequent lines up
                     for (int l = current_line + 1; l < total_lines - 1; l++) {
                         memcpy(input_lines[l], input_lines[l + 1], MAX_INPUT_LENGTH);
                         line_lengths[l] = line_lengths[l + 1];
                     }
                     total_lines--;
                 }
             }
             // Desired column doesn't change on forward delete
             break;
         case (int)'\n': { // Enter: split the current line
             if (total_lines < MAX_INPUT_LINES) {
                 // Move text after cursor to new line
                 int remain = line_lengths[current_line] - input_cursor;
                 // Shift existing lines down first
                 for (int l = total_lines; l > current_line + 1; --l) {
                      memcpy(input_lines[l], input_lines[l - 1], MAX_INPUT_LENGTH);
                      line_lengths[l] = line_lengths[l - 1];
                 }
                 // Copy the remaining part to the new line
                 memcpy(input_lines[current_line + 1],
                        &input_lines[current_line][input_cursor],
                        remain + 1); // +1 for null terminator
                 line_lengths[current_line + 1] = remain;
                 // Truncate the current line
                 input_lines[current_line][input_cursor] = '\0';
                 line_lengths[current_line] = input_cursor;

                 current_line++; // Move to the new line
                 total_lines++;
                 input_cursor = 0; // Cursor at beginning of new line
                 update_desired_column();
             }
             break;
         }
         case (int)'\t': { // Tab: expand to spaces
             int next_tab_stop = ((input_cursor / TAB_WIDTH) + 1) * TAB_WIDTH;
             int spaces = next_tab_stop - input_cursor;
             for (int i = 0; i < spaces; i++) {
                 insert_character(' '); // Will stop if line gets full
             }
             update_desired_column();
             break;
         }
         default: // Printable character
             // Use ASCII range directly for check
             if (code >= 0x20 && code <= 0x7E) { // Standard printable ASCII
                 char ch = apply_modifiers_extended((char)code, event.modifiers);
                 insert_character(ch);
                 update_desired_column();
             }
             break;
     }
     redraw_input(); // Redraw after every key press/repeat
 }

 /**
  * terminal_start_input - Begins an interactive multi-line input session.
  */
 void terminal_start_input(const char* prompt) {
     current_line = 0;
     total_lines = 1;
     input_cursor = 0;
     input_start_row = cursor_y;  // Input starts below current kernel output
     desired_column = 0;
     // Ensure safe access to arrays
     if (MAX_INPUT_LINES > 0) {
          line_lengths[0] = 0;
          input_lines[0][0] = '\0';
     }
     // Clear remaining lines in buffer just in case
     for(int i = 1; i < MAX_INPUT_LINES; ++i) {
          line_lengths[i] = 0;
          input_lines[i][0] = '\0';
     }

     if (prompt) {
         terminal_write(prompt); // This writes to VGA & Serial
         // We need to know where the cursor ended up AFTER the prompt
         terminal_get_cursor_pos(&cursor_x, &cursor_y);
         input_start_row = cursor_y; // Update start row after prompt
         input_cursor = cursor_x;    // Start input cursor where prompt ended
         desired_column = input_cursor;
         // Note: This simple prompt handling assumes prompt fits on one line
         // and doesn't handle wrapping.
     }
     redraw_input(); // Draw initial state
 }

 /**
  * terminal_get_input - Returns the current interactive input concatenated.
  */
 const char* terminal_get_input(void) {
     static char combined[MAX_INPUT_LINES * MAX_INPUT_LENGTH]; // Static buffer
     combined[0] = '\0';
     size_t current_pos = 0;
     size_t max_size = sizeof(combined);

     for (int i = 0; i < total_lines && current_pos < max_size - 1; i++) {
         size_t line_len = line_lengths[i];
         // Check if line content fits
         if (current_pos + line_len >= max_size - 1) {
              line_len = max_size - 1 - current_pos; // Truncate
         }
         memcpy(&combined[current_pos], input_lines[i], line_len);
         current_pos += line_len;

         // Add newline if not the last line and space allows
         if (i < total_lines - 1 && current_pos < max_size - 1) {
             combined[current_pos++] = '\n';
         }
     }
     combined[current_pos] = '\0'; // Ensure null termination
     return combined;
 }

 /**
  * terminal_complete_input - Completes the interactive input session.
  */
 void terminal_complete_input(void) {
     // Calculate final cursor position based on drawn input
     int visible_lines = (total_lines > (VGA_ROWS - input_start_row)) ? (VGA_ROWS - input_start_row) : total_lines;
     if (visible_lines < 0) visible_lines = 0;
     cursor_y = input_start_row + visible_lines; // Position below the input area
     cursor_x = 0;

     // Scroll if needed
      if (cursor_y >= VGA_ROWS) {
           scroll_one_line();
           cursor_y = VGA_ROWS - 1;
      }

     // Ensure a newline is printed to both VGA and Serial after input
     terminal_write_char('\n');
     update_hardware_cursor(); // Update hardware cursor to the new position
 }

 /**
  * terminal_putchar - Alias for terminal_write_char (non-interactive output).
  */
 void terminal_putchar(char c) {
     terminal_write_char(c);
 }