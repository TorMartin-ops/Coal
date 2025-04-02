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
 
 /* VGA text mode constants */
 #define VGA_ADDRESS 0xB8000
 #define VGA_COLS    80
 #define VGA_ROWS    25
 
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
 static int input_start_row = 35;
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
 
 /* Internal function that writes a character without affecting interactive input */
 static void terminal_write_char_internal(char c);
 
 /*
  * Public function to output a character.
  * This symbol is used by the keyboard driver (default callback) so it must be defined.
  */
 void terminal_write_char(char c);
 
 /* --- Minimal vsnprintf Implementation --- */
 static int mini_vsnprintf(char *str, size_t size, const char *format, va_list args) {
     size_t i = 0, j = 0;
     char temp[32];
     while (format[i] && j < size - 1) {
         if (format[i] != '%') {
             str[j++] = format[i++];
         } else {
             i++;
             if (format[i] == 's') {
                 char *s = va_arg(args, char*);
                 while (*s && j < size - 1) {
                     str[j++] = *s++;
                 }
                 i++;
             } else if (format[i] == 'd') {
                 int d = va_arg(args, int);
                 int k = 0;
                 if (d < 0) {
                     str[j++] = '-';
                     d = -d;
                 }
                 if (d == 0) {
                     temp[k++] = '0';
                 } else {
                     while (d) {
                         temp[k++] = '0' + (d % 10);
                         d /= 10;
                     }
                 }
                 while (k-- > 0 && j < size - 1) {
                     str[j++] = temp[k];
                 }
                 i++;
             } else if (format[i] == 'x') {
                 unsigned int x = va_arg(args, unsigned int);
                 int k = 0;
                 if (x == 0) {
                     temp[k++] = '0';
                 } else {
                     while (x) {
                         int rem = x % 16;
                         temp[k++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
                         x /= 16;
                     }
                 }
                 while (k-- > 0 && j < size - 1) {
                     str[j++] = temp[k];
                 }
                 i++;
             } else if (format[i] == '%') {
                 str[j++] = '%';
                 i++;
             } else {
                 str[j++] = '%';
             }
         }
     }
     str[j] = '\0';
     return j;
 }
 
 static int vsnprintf(char *str, size_t size, const char *format, va_list args) {
     return mini_vsnprintf(str, size, format, args);
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
     outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
     outb(0x3D4, 0x0B);
     outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
 }
 
 /**
  * put_char_at - Writes a character at a specific (x, y) location.
  */
 static void put_char_at(char c, int x, int y) {
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
     // Set the input area to start below the printed kernel text.
     input_start_row = cursor_y;
     desired_column = 0;
     line_lengths[0] = 0;
     input_lines[0][0] = '\0';
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
     memmove(vga_buffer, vga_buffer + VGA_COLS, (VGA_ROWS - 1) * VGA_COLS * 2);
     terminal_clear_row(VGA_ROWS - 1);
 }
 
 /**
  * update_hardware_cursor - Updates the hardware cursor to (cursor_x, cursor_y).
  */
 static void update_hardware_cursor(void) {
     if (!cursor_visible) {
         outb(0x3D4, 0x0A);
         outb(0x3D5, 0x20);
         return;
     }
     uint16_t pos = cursor_y * VGA_COLS + cursor_x;
     outb(0x3D4, 0x0F);
     outb(0x3D5, (uint8_t)(pos & 0xFF));
     outb(0x3D4, 0x0E);
     outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
 }
 
 /**
  * terminal_set_cursor_pos - Moves the cursor to (x, y).
  */
 void terminal_set_cursor_pos(int x, int y) {
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
     cursor_visible = visible;
     update_hardware_cursor();
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
  */
 static void terminal_write_char_internal(char c) {
     switch (c) {
         case '\n':
             cursor_x = 0;
             cursor_y++;
             break;
         case '\r':
             cursor_x = 0;
             break;
         case '\b':
         case 0x7F:
             /* Backspace is handled in interactive mode; ignore here */
             break;
         case '\t': {
             int next_tab_stop = ((cursor_x / TAB_WIDTH) + 1) * TAB_WIDTH;
             int spaces = next_tab_stop - cursor_x;
             for (int i = 0; i < spaces; i++) {
                 put_char_at(' ', cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
         }
         default:
             if (c >= ' ' && c <= '~') {
                 put_char_at(c, cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
     }
     if (cursor_x >= VGA_COLS) {
         cursor_x = 0;
         cursor_y++;
     }
     if (cursor_y >= VGA_ROWS) {
         scroll_one_line();
         cursor_y = VGA_ROWS - 1;
     }
     update_hardware_cursor();
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
     for (size_t i = 0; str[i] != '\0'; i++) {
         terminal_write_char(str[i]);
     }
 }
 
 /**
  * terminal_printf - Formatted printing.
  */
 void terminal_printf(const char* format, ...) {
     va_list args;
     va_start(args, format);
     char buf[128];
     vsnprintf(buf, sizeof(buf), format, args);
     terminal_write(buf);
     va_end(args);
 }
 
 /**
  * terminal_print_key_event - Prints a formatted key event (for debugging).
  */
 void terminal_print_key_event(const void* event) {
     const KeyEvent* ke = event;
     char hex[9];
     hex[8] = '\0';
     
     terminal_write("KeyEvent: code = 0x");
     uint32_t key = ke->code;
     for (int i = 0; i < 8; i++) {
         uint8_t nibble = (key >> ((7 - i) * 4)) & 0xF;
         hex[i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
     }
     terminal_write(hex);
     terminal_write(", action = ");
     if (ke->action == KEY_PRESS)
         terminal_write("PRESS");
     else if (ke->action == KEY_RELEASE)
         terminal_write("RELEASE");
     else if (ke->action == KEY_REPEAT)
         terminal_write("REPEAT");
     else
         terminal_write("UNKNOWN");
     terminal_write(", modifiers = 0x");
     uint32_t mods = ke->modifiers;
     for (int i = 0; i < 8; i++) {
         uint8_t nibble = (mods >> ((7 - i) * 4)) & 0xF;
         hex[i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
     }
     terminal_write(hex);
     terminal_write("\n");
 }
 
 /* --- Interactive Multi-Line Input Functions --- */
 
 /**
  * redraw_input - Redraws the visible portion of the interactive input.
  *
  * Only the bottom portion is shown if the input exceeds available rows.
  * The hardware cursor is set at (input_cursor, input_start_row + visible_line),
  * where visible_line is the current line relative to the visible input window.
  */
 static void redraw_input(void) {
     int available = VGA_ROWS - input_start_row; // available rows for input
     int first_line = 0;
     if (total_lines > available) {
         first_line = total_lines - available;
     }
     for (int line = first_line; line < total_lines; line++) {
         int row = input_start_row + (line - first_line);
         terminal_clear_row(row);
         for (int i = 0; i < line_lengths[line] && i < VGA_COLS; i++) {
             put_char_at(input_lines[line][i], i, row);
         }
     }
     int visible_line = current_line - first_line;
     if (visible_line < 0)
         visible_line = 0;
     terminal_set_cursor_pos(input_cursor, input_start_row + visible_line);
 }
 
 /**
  * update_desired_column - Updates desired_column to current input_cursor.
  */
 static void update_desired_column(void) {
     desired_column = input_cursor;
 }
 
 /**
  * erase_character - Erases the character at the current cursor position in the current line.
  * If at the beginning of a line (and not the first), merges with the previous line.
  */
 static void erase_character(void) {
     if (input_cursor > 0) {
         memmove(&input_lines[current_line][input_cursor - 1],
                 &input_lines[current_line][input_cursor],
                 line_lengths[current_line] - input_cursor + 1);
         line_lengths[current_line]--;
         input_cursor--;
     } else if (current_line > 0) {
         int prev_len = line_lengths[current_line - 1];
         if (prev_len + line_lengths[current_line] < MAX_INPUT_LENGTH) {
             memcpy(&input_lines[current_line - 1][prev_len],
                    input_lines[current_line],
                    line_lengths[current_line] + 1);
             line_lengths[current_line - 1] += line_lengths[current_line];
             for (int l = current_line; l < total_lines - 1; l++) {
                 memcpy(input_lines[l], input_lines[l + 1], MAX_INPUT_LENGTH);
                 line_lengths[l] = line_lengths[l + 1];
             }
             total_lines--;
             current_line--;
             input_cursor = line_lengths[current_line];
         }
     }
 }
 
 /**
  * insert_character - Inserts a character at the current cursor position in the current line.
  */
 static void insert_character(char c) {
     if (line_lengths[current_line] < MAX_INPUT_LENGTH - 1) {
         memmove(&input_lines[current_line][input_cursor + 1],
                 &input_lines[current_line][input_cursor],
                 line_lengths[current_line] - input_cursor + 1);
         input_lines[current_line][input_cursor] = c;
         line_lengths[current_line]++;
         input_cursor++;
     }
 }
 
 /**
  * terminal_handle_key_event - Processes key events for multi-line interactive editing.
  *
  * Supports:
  *   - LEFT/RIGHT: move within current line; if at beginning or end, wrap to previous/next line.
  *   - HOME/END: jump to beginning/end of current line.
  *   - UP/DOWN: move vertically. UP is blocked if already on the first input line
  *              (thus, user cannot move into kernel output area).
  *   - Backspace/Delete: delete characters (merging lines as necessary).
  *   - Enter: splits the current line.
  *   - Tab: expands to spaces.
  *   - Printable characters: inserted after modifier adjustment.
  */
 void terminal_handle_key_event(const KeyEvent event) {
     if (event.action != KEY_PRESS)
         return;
     
     int code = (int)event.code;
     switch (code) {
         case KEY_LEFT:
             if (input_cursor > 0) {
                 input_cursor--;
             } else if (current_line > 0) {
                 current_line--;
                 input_cursor = line_lengths[current_line];
             }
             update_desired_column();
             break;
         case KEY_RIGHT:
             if (input_cursor < line_lengths[current_line]) {
                 input_cursor++;
             } else if (current_line < total_lines - 1) {
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
             // Prevent moving upward beyond the input area.
             if (current_line > 0) {
                 current_line--;
                 if (desired_column > line_lengths[current_line])
                     input_cursor = line_lengths[current_line];
                 else
                     input_cursor = desired_column;
             }
             break;
         case KEY_DOWN:
             if (current_line < total_lines - 1) {
                 current_line++;
                 if (desired_column > line_lengths[current_line])
                     input_cursor = line_lengths[current_line];
                 else
                     input_cursor = desired_column;
             }
             break;
         case '\b':  // Backspace
             erase_character();
             update_desired_column();
             break;
         case KEY_DELETE:
             if (input_cursor < line_lengths[current_line]) {
                 memmove(&input_lines[current_line][input_cursor],
                         &input_lines[current_line][input_cursor + 1],
                         line_lengths[current_line] - input_cursor);
                 line_lengths[current_line]--;
             } else if (current_line < total_lines - 1) {
                 if (line_lengths[current_line] + line_lengths[current_line + 1] < MAX_INPUT_LENGTH) {
                     memcpy(&input_lines[current_line][line_lengths[current_line]],
                            input_lines[current_line + 1],
                            line_lengths[current_line + 1] + 1);
                     line_lengths[current_line] += line_lengths[current_line + 1];
                     for (int l = current_line + 1; l < total_lines - 1; l++) {
                         memcpy(input_lines[l], input_lines[l + 1], MAX_INPUT_LENGTH);
                         line_lengths[l] = line_lengths[l + 1];
                     }
                     total_lines--;
                 }
             }
             break;
         case (int)'\n': { // Enter: split the current line.
             if (total_lines < MAX_INPUT_LINES) {
                 int remain = line_lengths[current_line] - input_cursor;
                 memcpy(input_lines[current_line + 1],
                        &input_lines[current_line][input_cursor],
                        remain + 1);
                 line_lengths[current_line + 1] = remain;
                 input_lines[current_line][input_cursor] = '\0';
                 line_lengths[current_line] = input_cursor;
                 current_line++;
                 total_lines++;
                 input_cursor = 0;
                 update_desired_column();
             }
             break;
         }
         case (int)'\t': { // Tab: expand to spaces.
             int next_tab_stop = ((input_cursor / TAB_WIDTH) + 1) * TAB_WIDTH;
             int spaces = next_tab_stop - input_cursor;
             for (int i = 0; i < spaces; i++) {
                 insert_character(' ');
             }
             update_desired_column();
             break;
         }
         default:
             if (code >= ' ' && code <= '~') {
                 char ch = apply_modifiers_extended((char)code, keyboard_get_modifiers());
                 insert_character(ch);
                 update_desired_column();
             }
             break;
     }
     redraw_input();
 }
 
 /**
  * terminal_start_input - Begins an interactive multi-line input session with an optional prompt.
  *
  * The interactive input area is set to start at the current terminal cursor row.
  * Once started, the user cannot move the input cursor above this row.
  */
 void terminal_start_input(const char* prompt) {
     current_line = 0;
     total_lines = 1;
     input_cursor = 0;
     input_start_row = cursor_y;  // Input starts below kernel output.
     desired_column = 0;
     line_lengths[0] = 0;
     input_lines[0][0] = '\0';
     
     if (prompt) {
         terminal_write(prompt);
         int plen = strlen(prompt);
         terminal_set_cursor_pos(plen, cursor_y);
         input_cursor = 0;
         desired_column = 0;
     }
     redraw_input();
 }
 
 /**
  * terminal_get_input - Returns the current interactive input as a single concatenated string.
  *
  * The lines are joined with newline characters. The returned string is stored in a static buffer.
  */
 const char* terminal_get_input(void) {
     static char combined[MAX_INPUT_LINES * MAX_INPUT_LENGTH];
     combined[0] = '\0';
     for (int i = 0; i < total_lines; i++) {
         strcat(combined, input_lines[i]);
         if (i < total_lines - 1)
             strcat(combined, "\n");
     }
     return combined;
 }
 
 /**
  * terminal_complete_input - Completes the interactive input session.
  *
  * Finalizes the input by outputting a newline and moving the terminal cursor to just below the input area.
  */
 void terminal_complete_input(void) {
     terminal_write_char_internal('\n');
     cursor_y = input_start_row + total_lines;
     update_hardware_cursor();
 }
 
 /**
  * terminal_putchar - Alias for terminal_write_char (non-interactive output).
  */
 void terminal_putchar(char c) {
     terminal_write_char(c);
 }
 