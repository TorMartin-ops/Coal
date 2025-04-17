/**
 * terminal.c
 * Enhanced VGA text-mode driver for a 32-bit x86 OS.
 *
 * Features:
 * - Standard text output with scrolling.
 * - Dual output to VGA and Serial Port (COM1).
 * - Robust internal printf implementation with 64-bit support (%lld, %llu, %llx)
 * and basic width/padding.
 * - Basic ANSI escape sequence support (colors, clear screen).
 * - Multi-line interactive input editing with improved state management.
 * - Spinlock protection for concurrent access safety.
 *
 * Author: Enhanced by Gemini (based on Group_14 code)
 * Date: 2025-04-17 // Updated 2025-04-17 for fixes
 */

 #include "terminal.h"      // Includes MAX_INPUT_LENGTH, uses updated terminal_get_input signature
 #include "port_io.h"       // For outb, inb
 #include "keyboard.h"      // For KeyEvent, KeyCode, apply_modifiers_extended
 #include "types.h"
 #include "spinlock.h"      // For spinlock_t, lock/unlock functions
 #include "serial.h"        // For serial_putchar
 #include "assert.h"        // For KERNEL_ASSERT
 
 #include <libc/stdarg.h>   // For va_list
 #include <libc/stdbool.h>  // For bool type
 #include <libc/stddef.h>   // For size_t, NULL
 #include <libc/stdint.h>   // For uint64_t, int64_t, etc.
 #include <libc/limits.h>   // <--- Added for LLONG_MIN/MAX
 #include <string.h>        // For memset, memcpy, memmove, strlen
 
 /* --- Configuration --- */
 #define TAB_WIDTH         4      // Number of spaces per tab
 #define PRINTF_BUFFER_SIZE 256    // Max buffer size for printf output
 #define MAX_INPUT_LINES   64     // Max lines for interactive input
 // MAX_INPUT_LENGTH is now defined in terminal.h
 
 /* --- VGA Hardware Constants --- */
 #define VGA_MEM_ADDRESS   0xC00B8000 // Virtual address of VGA buffer (Mapped in Paging)
 #define VGA_COLS          80
 #define VGA_ROWS          25
 #define VGA_CMD_PORT      0x3D4
 #define VGA_DATA_PORT     0x3D5
 #define VGA_REG_CURSOR_HI 0x0E
 #define VGA_REG_CURSOR_LO 0x0F
 #define VGA_REG_CURSOR_START 0x0A
 #define VGA_REG_CURSOR_END   0x0B
 #define CURSOR_SCANLINE_START 14   // Makes a block cursor
 #define CURSOR_SCANLINE_END   15
 
 /* --- ANSI Escape Code States --- */
 typedef enum {
     ANSI_STATE_NORMAL,
     ANSI_STATE_ESC,        // Received ESC '\033'
     ANSI_STATE_BRACKET,    // Received '['
     ANSI_STATE_PARAM       // Parsing parameters
 } AnsiState;
 
 /* --- Terminal Global State --- */
 DEFINE_SPINLOCK(terminal_lock);        // <--- Corrected spinlock definition
 static uint8_t   terminal_color = 0x07; // Current color (Light gray on black)
 static uint16_t* vga_buffer = (uint16_t*)VGA_MEM_ADDRESS;
 static int       cursor_x = 0;
 static int       cursor_y = 0;
 static uint8_t   cursor_visible = 1;   // 1: visible; 0: hidden.
 static AnsiState ansi_state = ANSI_STATE_NORMAL;
 static int       ansi_params[4];       // Buffer for ANSI parameters
 static int       ansi_param_count = 0;
 
 /* --- Multi-Line Interactive Input State --- */
 typedef struct {
     char    lines[MAX_INPUT_LINES][MAX_INPUT_LENGTH];
     int     line_lengths[MAX_INPUT_LINES];
     int     current_line;      // Index of line being edited
     int     total_lines;       // Total number of lines used
     int     input_cursor;      // Horizontal cursor position within current line
     int     start_row;         // Screen row where input editing begins
     int     desired_column;    // Target column for vertical movement
     bool    is_active;         // Is interactive input mode active?
 } terminal_input_state_t;
 
 static terminal_input_state_t input_state;
 
 /* --- Forward Declarations --- */
 static void update_hardware_cursor(void);
 static void enable_hardware_cursor(void);
 static void disable_hardware_cursor(void);
 static void put_char_at(char c, uint8_t color, int x, int y);
 static void clear_row(int row, uint8_t color);
 static void scroll_terminal(void);
 static void redraw_input(void);
 static void update_desired_column(void);
 static void erase_character(void);
 static void insert_character(char c);
 static void process_ansi_code(char c);
 static void terminal_putchar_internal(char c); // For use when lock is held
 static void terminal_clear_internal(void);   // <--- Added forward declaration
 
 // vsnprintf and helpers
 static int _vsnprintf(char *str, size_t size, const char *format, va_list args);
 static void _reverse(char* str, int len);
 static int _format_number(uint64_t num, bool is_negative, int base, bool uppercase,
                           int min_width, bool zero_pad, char* buf, int buf_size);
 
 /* --- Helper Functions --- */
 
 static inline uint16_t vga_entry(char uc, uint8_t color) {
     return (uint16_t) uc | (uint16_t) color << 8;
 }
 
 // Note: Inline lock helpers removed as flags need to be managed per-call site
 
 /* --- Hardware Cursor Management --- */
 
 static void enable_hardware_cursor(void) {
     // Assumes lock is held by caller OR called during init
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xC0) | CURSOR_SCANLINE_START);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_END);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xE0) | CURSOR_SCANLINE_END);
 }
 
 static void disable_hardware_cursor(void) {
     // Assumes lock is held by caller OR called during init
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, 0x20); // Set start scanline > end scanline to hide
 }
 
 static void update_hardware_cursor(void) {
     // Assumes lock is held by caller
     if (!cursor_visible || input_state.is_active) { // Hide HW cursor during interactive input
         disable_hardware_cursor();
         return;
     }
 
     enable_hardware_cursor(); // Ensure cursor shape is set
 
     // Clamp coordinates
     if (cursor_x < 0) cursor_x = 0;
     if (cursor_y < 0) cursor_y = 0;
     if (cursor_x >= VGA_COLS) cursor_x = VGA_COLS - 1;
     if (cursor_y >= VGA_ROWS) cursor_y = VGA_ROWS - 1;
 
     uint16_t pos = cursor_y * VGA_COLS + cursor_x;
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO);
     outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI);
     outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
 }
 
 /* --- Low-Level VGA Buffer Access --- */
 
 static void put_char_at(char c, uint8_t color, int x, int y) {
     // Assumes lock is held by caller
     if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) return;
     const size_t index = y * VGA_COLS + x;
     vga_buffer[index] = vga_entry(c, color);
 }
 
 static void clear_row(int row, uint8_t color) {
     // Assumes lock is held by caller
     if (row < 0 || row >= VGA_ROWS) return;
     uint16_t entry = vga_entry(' ', color);
     size_t start_index = row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; col++) {
         vga_buffer[start_index + col] = entry;
     }
 }
 
 static void scroll_terminal(void) {
     // Assumes lock is held by caller
     // Move lines up
     memmove(vga_buffer, vga_buffer + VGA_COLS, (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
     // Clear the last line
     clear_row(VGA_ROWS - 1, terminal_color);
 
     // Adjust interactive input start row if it was scrolled off
     if (input_state.is_active && input_state.start_row > 0) {
         input_state.start_row--;
     }
 }
 
 /* --- ANSI Escape Code Handling --- */
 
 static void process_ansi_code(char c) {
     // Assumes lock is held by caller
     switch (ansi_state) {
         case ANSI_STATE_NORMAL:
             if (c == '\033') {
                 ansi_state = ANSI_STATE_ESC;
             } // Else: character handled normally by caller
             break;
 
         case ANSI_STATE_ESC:
             if (c == '[') {
                 ansi_state = ANSI_STATE_BRACKET;
                 ansi_param_count = 0;
                 // Initialize params for safety, -1 indicates not set
                 for(int i=0; i<4; ++i) ansi_params[i] = -1;
             } else {
                 ansi_state = ANSI_STATE_NORMAL; // Invalid sequence
             }
             break;
 
         case ANSI_STATE_BRACKET:
              if (c >= '0' && c <= '9') {
                  ansi_state = ANSI_STATE_PARAM;
                  // Ensure first param is initialized from -1 to 0 before adding digit
                  if(ansi_params[ansi_param_count] == -1) ansi_params[ansi_param_count] = 0;
                  ansi_params[ansi_param_count] = ansi_params[ansi_param_count] * 10 + (c - '0');
              } else if (c == ';') {
                   // Separator needs a parameter before it
                   ansi_state = ANSI_STATE_NORMAL; // Invalid sequence
              } else {
                 // Handle commands without explicit numeric params (param[0] will be -1)
                 int p1 = ansi_params[0]; // Will be -1 if no digits seen
                  if (c == 'J') { // Erase display
                      if (p1 == 2 || p1 == -1) { // ED 2 or ED (default) clear entire screen
                          terminal_clear_internal(); // Call internal clear
                      } // Other ED codes ignored for now
                  }
                  // Add other commands here (e.g., cursor movement H, f)
                 ansi_state = ANSI_STATE_NORMAL;
             }
             break;
 
         case ANSI_STATE_PARAM:
             if (c >= '0' && c <= '9') {
                 if (ansi_params[ansi_param_count] == -1) ansi_params[ansi_param_count] = 0;
                 ansi_params[ansi_param_count] = ansi_params[ansi_param_count] * 10 + (c - '0');
             } else if (c == ';') {
                 if (ansi_param_count < 3) { // Allow up to 4 parameters
                     ansi_param_count++;
                     ansi_params[ansi_param_count] = -1; // Initialize next param
                 } else {
                     ansi_state = ANSI_STATE_NORMAL; // Too many parameters
                 }
             } else if (c == 'm') { // SGR - Select Graphic Rendition (Colors)
                 // Apply params iteratively
                 for (int i = 0; i <= ansi_param_count; ++i) {
                      int p = (ansi_params[i] == -1) ? 0 : ansi_params[i]; // Default to 0 if omitted
                      if (p == 0) { // Reset
                          terminal_color = 0x07; // Default light grey on black
                      } else if (p >= 30 && p <= 37) { // Foreground color
                          terminal_color = (terminal_color & 0xF0) | (p - 30);
                      } else if (p >= 40 && p <= 47) { // Background color
                          terminal_color = ((p - 40) << 4) | (terminal_color & 0x0F);
                      } else if (p >= 90 && p <= 97) { // Bright foreground
                          terminal_color = (terminal_color & 0xF0) | ((p - 90) + 8);
                      } else if (p >= 100 && p <= 107) { // Bright background
                          terminal_color = (((p - 100) + 8) << 4) | (terminal_color & 0x0F);
                      } // Add other SGR codes (bold, underline etc.) if needed
                 }
                 ansi_state = ANSI_STATE_NORMAL;
             } else if (c == 'J') { // Erase display (handles cases with params)
                   int p1 = (ansi_params[0] == -1) ? 0 : ansi_params[0];
                   if (p1 == 2) { // ED 2: Clear entire screen
                        terminal_clear_internal();
                   }
                   // Other ED codes ignored
                   ansi_state = ANSI_STATE_NORMAL;
             }
              // Add other commands here (e.g., cursor movement H, f)
             else {
                 ansi_state = ANSI_STATE_NORMAL; // Unknown command
             }
             break;
     }
 }
 
 /* --- Core Output Functions --- */
 
 // Internal write function - handles actual char output, scrolling, newline, etc.
 // Assumes lock is HELD.
 static void terminal_putchar_internal(char c) {
     // Process ANSI state machine first
     if (ansi_state != ANSI_STATE_NORMAL || c == '\033') {
         process_ansi_code(c);
         // If the character was consumed by ANSI processing, return
         if (ansi_state != ANSI_STATE_NORMAL) return;
         // If it was the start of a sequence (\033) but invalid,
         // ansi_state is reset to NORMAL. Standard terminals usually ignore lone ESC.
         if (c == '\033') return; // Don't print lone ESC
     }
 
     // Handle standard control characters or printables
     switch (c) {
         case '\n':
             cursor_x = 0;
             cursor_y++;
             break;
         case '\r':
             cursor_x = 0;
             break;
         case '\b': // Backspace
              if (cursor_x > 0) {
                   cursor_x--;
                   // Optional: put_char_at(' ', terminal_color, cursor_x, cursor_y);
              } // Else: Do nothing if at start of line/screen
              break;
         case '\t': {
             int next_tab_stop = ((cursor_x / TAB_WIDTH) + 1) * TAB_WIDTH;
             // Clamp tab stop to screen width
             if (next_tab_stop >= VGA_COLS) next_tab_stop = VGA_COLS;
             while (cursor_x < next_tab_stop && cursor_x < VGA_COLS) { // Ensure we don't exceed bounds
                 put_char_at(' ', terminal_color, cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
         }
         default:
             // Print printable characters
             if (c >= ' ' && c <= '~') {
                 put_char_at(c, terminal_color, cursor_x, cursor_y);
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
     while (cursor_y >= VGA_ROWS) { // Use while in case of multiple scrolls needed
         scroll_terminal();
         cursor_y--;
     }
 
     // Serial output is done *after* processing ANSI for VGA state changes
     serial_putchar(c);
 
     // Hardware cursor update is done by the calling context (e.g., terminal_write)
 }
 
 /**
  * @brief Writes a single character to the terminal (VGA and Serial).
  * Handles newline, backspace, tab, scrolling, and basic ANSI codes.
  * This function is thread-safe.
  * @param c Character to write.
  */
 void terminal_putchar(char c) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock); // <-- Acquire lock and save flags
     terminal_putchar_internal(c);
     update_hardware_cursor(); // Update HW cursor position after processing char
     spinlock_release_irqrestore(&terminal_lock, flags); // <-- Release lock restoring flags
 }
 
 /**
  * @brief Writes a null-terminated string to the terminal (VGA and Serial).
  * This function is thread-safe.
  * @param str The string to write.
  */
 void terminal_write(const char* str) {
     if (!str) return;
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; str[i] != '\0'; i++) {
         terminal_putchar_internal(str[i]);
     }
     update_hardware_cursor(); // Update cursor once after string
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Writes raw data of a specific length to the terminal.
  * Useful for writing data that might contain null bytes.
  * This function is thread-safe.
  * @param data Pointer to the data.
  * @param size Number of bytes to write.
  */
 void terminal_write_len(const char* data, size_t size) {
     if (!data || size == 0) return;
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; i < size; i++) {
         terminal_putchar_internal(data[i]);
     }
     update_hardware_cursor(); // Update cursor once after write
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 
 /* --- Formatted Printing (`printf`) --- */
 
 // Helper to reverse a string
 static void _reverse(char* str, int len) {
     int i = 0, j = len - 1;
     while (i < j) {
         char temp = str[i];
         str[i] = str[j];
         str[j] = temp;
         i++; j--;
     }
 }
 
 // Core number formatting function
 // Returns number of chars written to buf (excluding null)
 static int _format_number(uint64_t num, bool is_negative, int base, bool uppercase,
                           int min_width, bool zero_pad, char* buf, int buf_size)
 {
     if (buf_size < 2) return 0; // Need space for digit + null
 
     char temp_digits[65]; // Temp buffer for digits in reverse
     int i = 0;
     const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
 
     if (base < 2 || base > 16) base = 10;
 
     // Handle zero case separately
     if (num == 0) {
         temp_digits[i++] = '0';
     } else {
         uint64_t current_num = num;
         while (current_num != 0 && i < (int)sizeof(temp_digits) -1) {
             temp_digits[i++] = digits[current_num % base];
             current_num /= base;
         }
          // Check for overflow during conversion (unlikely with 65 chars)
          if (current_num != 0) {
              strncpy(buf, "[NUM_OOM]", buf_size); return strlen("[NUM_OOM]");
          }
     }
     int num_digits = i;
 
     // Calculate padding needed
     int sign_char = (is_negative && base == 10) ? 1 : 0;
     int total_chars_needed = num_digits + sign_char;
     int padding = (min_width > total_chars_needed) ? (min_width - total_chars_needed) : 0;
 
     // Check if everything fits in the output buffer
     if (total_chars_needed + padding >= buf_size) {
         strncpy(buf, "[FMT_OOM]", buf_size); return strlen("[FMT_OOM]");
     }
 
     int current_pos = 0;
     // Add sign
     if (sign_char) {
         buf[current_pos++] = '-';
     }
     // Add padding
     char pad_char = zero_pad ? '0' : ' ';
     for (int p = 0; p < padding; p++) {
         buf[current_pos++] = pad_char;
     }
     // Add digits (reversed from temp_digits)
     for (int d = num_digits - 1; d >= 0; d--) {
         buf[current_pos++] = temp_digits[d];
     }
 
     buf[current_pos] = '\0';
     return current_pos;
 }
 
 
 // Internal vsnprintf implementation
 static int _vsnprintf(char *str, size_t size, const char *format, va_list args) {
     if (!str || size == 0) return 0;
 
     size_t written = 0;
     char temp_buf[66]; // Buffer for number conversions + NUL
 
     while (*format && written < size - 1) {
         if (*format != '%') {
             str[written++] = *format++;
             continue;
         }
 
         format++; // Skip '%'
 
         bool is_long_long = false;
         bool zero_pad = false;
         int min_width = 0;
 
         if (*format == '0') {
             zero_pad = true;
             format++;
         }
         while (*format >= '0' && *format <= '9') {
             min_width = min_width * 10 + (*format - '0');
             format++;
         }
         if (format[0] == 'l' && format[1] == 'l') {
             is_long_long = true;
             format += 2;
         }
 
         int num_len = 0;
         const char* str_arg = NULL;
 
         switch (*format) {
             case 's': {
                 str_arg = va_arg(args, const char *);
                 if (!str_arg) str_arg = "(null)";
                 // Apply width minimally (copy up to width or buffer allows)
                 num_len = 0;
                 while(str_arg[num_len] != '\0') num_len++;
                 if(min_width > 0 && num_len < min_width) {
                     int pad = min_width - num_len;
                     for(int k=0; k < pad && written < size -1; ++k) str[written++]=' ';
                 }
                  // Copy string content after padding
                  for(int k=0; k < num_len && written < size -1; ++k) str[written++]=str_arg[k];
                  num_len = 0; // Prevent double copy below
                  str_arg = NULL;
                  break; // Skip common copy logic
             }
             case 'c': {
                 temp_buf[0] = (char)va_arg(args, int);
                 temp_buf[1] = '\0';
                 str_arg = temp_buf;
                 num_len = 1;
                 // Apply width padding if needed
                  if(min_width > 1) {
                      int pad = min_width - 1;
                      for(int k=0; k < pad && written < size -1; ++k) str[written++]=' ';
                  }
                 break;
             }
             case 'd': {
                 int base = 10;
                 bool is_negative = false;
                 uint64_t val_u;
                 if (is_long_long) {
                     long long val_s = va_arg(args, long long);
                     if (val_s < 0) {
                          is_negative = true;
                          // Use condition carefully to avoid overflow on LLONG_MIN
                          val_u = (val_s == LLONG_MIN) ? ((uint64_t)LLONG_MAX + 1) : (uint64_t)(-val_s);
                     } else {
                          val_u = (uint64_t)val_s;
                     }
                 } else { // Default int
                     int val_s = va_arg(args, int);
                     if (val_s < 0) {
                          is_negative = true;
                          val_u = (val_s == (-2147483647 - 1)) ? 2147483648U : (uint32_t)(-val_s);
                     } else {
                          val_u = (uint32_t)val_s;
                     }
                 }
                 num_len = _format_number(val_u, is_negative, base, false, min_width, zero_pad, temp_buf, sizeof(temp_buf));
                 str_arg = temp_buf;
                 break;
             }
             case 'u':
             case 'x':
             case 'X': {
                 int base = (*format == 'u') ? 10 : 16;
                 bool uppercase = (*format == 'X');
                 uint64_t val_u;
                 if (is_long_long) {
                     val_u = va_arg(args, unsigned long long);
                 } else { // Default unsigned int
                     val_u = va_arg(args, unsigned int);
                 }
                 num_len = _format_number(val_u, false, base, uppercase, min_width, zero_pad, temp_buf, sizeof(temp_buf));
                 str_arg = temp_buf;
                 break;
             }
              case 'p': { // Pointer (%p)
                  void* p_arg = va_arg(args, void*);
                  uintptr_t ptr_val = (uintptr_t)p_arg;
                  if (written < size - 3) { str[written++] = '0'; str[written++] = 'x'; }
                  num_len = _format_number(ptr_val, false, 16, false, sizeof(uintptr_t)*2, true, temp_buf, sizeof(temp_buf)); // Pad hex to pointer size
                  str_arg = temp_buf;
                  break;
              }
             case '%': {
                 temp_buf[0] = '%'; temp_buf[1] = '\0';
                 str_arg = temp_buf; num_len = 1;
                 break;
             }
             default:
                 temp_buf[0] = '%';
                 int mod_len = 0;
                 if (is_long_long) { temp_buf[1] = 'l'; temp_buf[2] = 'l'; mod_len = 2; }
                 temp_buf[1+mod_len] = *format;
                 temp_buf[2+mod_len] = '\0';
                 str_arg = temp_buf; num_len = strlen(str_arg);
                 break;
         }
 
         // Write the formatted string/number (if not handled specially like %s/%c)
         if (str_arg) {
             for (int k = 0; k < num_len && written < size - 1; k++) {
                 str[written++] = str_arg[k];
             }
         }
 
         if (*format) format++; // Move past the processed format specifier
 
     } // End while loop
 
     str[written] = '\0'; // Null-terminate
     return written;
 }
 
 /**
  * @brief Formatted printing to the terminal (thread-safe).
  * Supports standard formats including %lld, %llu, %llx, %p, %c, %s, %d, %u, %x.
  * Supports basic width/padding like %08x.
  * @param format The format string.
  * @param ... Variable arguments.
  */
 void terminal_printf(const char* format, ...) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock); // Lock before formatting
 
     va_list args;
     va_start(args, format);
     char buf[PRINTF_BUFFER_SIZE];
 
     int len = _vsnprintf(buf, sizeof(buf), format, args);
     va_end(args);
 
     // Write the formatted buffer (lock still held)
     for (int i = 0; i < len; i++) {
         terminal_putchar_internal(buf[i]);
     }
     update_hardware_cursor(); // Update cursor once after printf
 
     spinlock_release_irqrestore(&terminal_lock, flags); // Release lock
 }
 
 
 /* --- Terminal Control Functions --- */
 
 /**
  * @brief Initializes the terminal, clears the screen, sets up cursor.
  */
 void terminal_init(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_clear_internal(); // Clear screen (internal, lock held)
     input_state.is_active = false; // Ensure input mode is off initially
     // Enable HW cursor shape here, visibility managed by update_hardware_cursor
     enable_hardware_cursor();
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 // Internal clear function, assumes lock is held
 void terminal_clear_internal(void) {
     for (int y = 0; y < VGA_ROWS; y++) {
         clear_row(y, terminal_color);
     }
     cursor_x = 0;
     cursor_y = 0;
     input_state.is_active = false; // Clearing stops input mode
     ansi_state = ANSI_STATE_NORMAL; // Reset ANSI state
     update_hardware_cursor();
 }
 
 /**
  * @brief Clears the entire terminal screen and resets cursor. Thread-safe.
  */
 void terminal_clear(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_clear_internal();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Sets the VGA text color attribute.
  * @param color VGA color byte (Background << 4 | Foreground).
  */
 void terminal_set_color(uint8_t color) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_color = color;
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Moves the hardware cursor to the specified position. Thread-safe.
  * @param x Column (0-based).
  * @param y Row (0-based).
  */
 void terminal_set_cursor_pos(int x, int y) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_x = x;
     cursor_y = y;
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Gets the current cursor position. Thread-safe.
  * @param x Pointer to store column.
  * @param y Pointer to store row.
  */
 void terminal_get_cursor_pos(int* x, int* y) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (x) *x = cursor_x;
     if (y) *y = cursor_y;
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Sets the visibility of the hardware cursor. Thread-safe.
  * @param visible 0 to hide, non-zero to show.
  */
 void terminal_set_cursor_visibility(uint8_t visible) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_visible = (visible != 0);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 
 /* --- Interactive Multi-Line Input Functions --- */
 
 // Helper to redraw the interactive input area
 static void redraw_input(void) {
     // Assumes lock is held
     KERNEL_ASSERT(input_state.is_active, "redraw_input called when not active");
 
     if (input_state.start_row >= VGA_ROWS) input_state.start_row = VGA_ROWS - 1;
     if (input_state.start_row < 0) input_state.start_row = 0;
 
     int available_rows = VGA_ROWS - input_state.start_row;
     if (available_rows <= 0) return;
 
     int first_visible_line = 0;
     if (input_state.total_lines > available_rows) {
         if (input_state.current_line >= input_state.start_row + available_rows) {
              first_visible_line = input_state.current_line - available_rows + 1;
         } else if (input_state.current_line < input_state.start_row) {
              // This case might need more thought if start_row can change dynamically easily
              first_visible_line = input_state.current_line;
         }
         // Simplified: Assume start_row is fixed during one input session for now
         // Calculate based on keeping current_line visible
          if (input_state.current_line >= available_rows) {
              first_visible_line = input_state.current_line - available_rows + 1;
          }
 
     }
 
     uint8_t input_color = 0x0F; // White on black for input area
     for (int i = 0; i < available_rows; ++i) {
         int line_index = first_visible_line + i;
         int screen_row = input_state.start_row + i;
         clear_row(screen_row, input_color);
 
         if (line_index < input_state.total_lines) {
             int len = input_state.line_lengths[line_index];
             for (int j = 0; j < len && j < VGA_COLS; ++j) {
                 put_char_at(input_state.lines[line_index][j], input_color, j, screen_row);
             }
             if (line_index == input_state.current_line) {
                  if (input_state.input_cursor < VGA_COLS) {
                       size_t cursor_vga_idx = screen_row * VGA_COLS + input_state.input_cursor;
                       char existing_char = (char)(vga_buffer[cursor_vga_idx] & 0xFF);
                       if (existing_char == 0) existing_char = ' '; // Use space if cell was empty
                       uint8_t cursor_color = 0x70; // Black on Light Grey
                       put_char_at(existing_char, cursor_color, input_state.input_cursor, screen_row);
                  }
             }
         }
     }
     // Hardware cursor is kept disabled during interactive input
 }
 
 
 static void update_desired_column(void) {
     input_state.desired_column = input_state.input_cursor;
 }
 
 // Erases character before cursor, or merges lines if at start of line
 static void erase_character(void) {
     // Assumes lock is held
     KERNEL_ASSERT(input_state.is_active, "erase_character called when not active");
     int line_idx = input_state.current_line;
     KERNEL_ASSERT(line_idx >= 0 && line_idx < input_state.total_lines, "Invalid current_line");
 
     if (input_state.input_cursor > 0) { // Erase within the line
         memmove(&input_state.lines[line_idx][input_state.input_cursor - 1],
                 &input_state.lines[line_idx][input_state.input_cursor],
                 input_state.line_lengths[line_idx] - input_state.input_cursor + 1); // +1 for null
         input_state.line_lengths[line_idx]--;
         input_state.input_cursor--;
     } else if (line_idx > 0) { // Merge with previous line
         int prev_line_idx = line_idx - 1;
         int prev_len = input_state.line_lengths[prev_line_idx];
         int current_len = input_state.line_lengths[line_idx];
 
         if (prev_len + current_len < MAX_INPUT_LENGTH) { // Check if merge fits
             memcpy(&input_state.lines[prev_line_idx][prev_len],
                    input_state.lines[line_idx], current_len + 1);
             input_state.line_lengths[prev_line_idx] += current_len;
 
             for (int l = line_idx; l < input_state.total_lines - 1; l++) {
                 memcpy(input_state.lines[l], input_state.lines[l + 1], MAX_INPUT_LENGTH);
                 input_state.line_lengths[l] = input_state.line_lengths[l + 1];
             }
             input_state.lines[input_state.total_lines - 1][0] = '\0';
             input_state.line_lengths[input_state.total_lines - 1] = 0;
 
             input_state.total_lines--;
             input_state.current_line--;
             input_state.input_cursor = prev_len;
         } // Else: Optionally Beep
     }
 }
 
 // Inserts character at cursor position
 static void insert_character(char c) {
     // Assumes lock is held
     KERNEL_ASSERT(input_state.is_active, "insert_character called when not active");
     int line_idx = input_state.current_line;
     KERNEL_ASSERT(line_idx >= 0 && line_idx < input_state.total_lines, "Invalid current_line");
 
     if (input_state.line_lengths[line_idx] < MAX_INPUT_LENGTH - 1) {
         memmove(&input_state.lines[line_idx][input_state.input_cursor + 1],
                 &input_state.lines[line_idx][input_state.input_cursor],
                 input_state.line_lengths[line_idx] - input_state.input_cursor + 1);
         input_state.lines[line_idx][input_state.input_cursor] = c;
         input_state.line_lengths[line_idx]++;
         input_state.input_cursor++;
     } // Else: Optionally Beep
 }
 
 
 /**
  * @brief Processes a key event for interactive input editing.
  * @param event The keyboard event.
  */
 void terminal_handle_key_event(const KeyEvent event) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock); // Lock the terminal state
 
     if (!input_state.is_active) {
         spinlock_release_irqrestore(&terminal_lock, flags);
         return; // Ignore if not in interactive mode
     }
 
     if (event.action != KEY_PRESS && event.action != KEY_REPEAT) {
         spinlock_release_irqrestore(&terminal_lock, flags);
         return; // Only handle press/repeat
     }
 
     // --- Sanity check state ---
     if (input_state.current_line < 0) input_state.current_line = 0;
     if (input_state.current_line >= input_state.total_lines) input_state.current_line = input_state.total_lines - 1;
     if (input_state.input_cursor < 0) input_state.input_cursor = 0;
     if (input_state.input_cursor > input_state.line_lengths[input_state.current_line]) {
          input_state.input_cursor = input_state.line_lengths[input_state.current_line];
     }
     // --- End Sanity check ---
 
     int code = (int)event.code;
     bool redraw_needed = true;
 
     switch (code) {
         case KEY_LEFT:
             if (input_state.input_cursor > 0) input_state.input_cursor--;
             else if (input_state.current_line > 0) {
                 input_state.current_line--;
                 input_state.input_cursor = input_state.line_lengths[input_state.current_line];
             }
             update_desired_column();
             break;
         case KEY_RIGHT:
             if (input_state.input_cursor < input_state.line_lengths[input_state.current_line]) input_state.input_cursor++;
             else if (input_state.current_line < input_state.total_lines - 1) {
                 input_state.current_line++;
                 input_state.input_cursor = 0;
             }
             update_desired_column();
             break;
         case KEY_UP:
             if (input_state.current_line > 0) {
                 input_state.current_line--;
                 input_state.input_cursor = input_state.desired_column;
                 if (input_state.input_cursor > input_state.line_lengths[input_state.current_line])
                     input_state.input_cursor = input_state.line_lengths[input_state.current_line];
             }
             break;
         case KEY_DOWN:
             if (input_state.current_line < input_state.total_lines - 1) {
                 input_state.current_line++;
                 input_state.input_cursor = input_state.desired_column;
                 if (input_state.input_cursor > input_state.line_lengths[input_state.current_line])
                     input_state.input_cursor = input_state.line_lengths[input_state.current_line];
             }
             break;
         case KEY_HOME:
             input_state.input_cursor = 0; update_desired_column(); break;
         case KEY_END:
             input_state.input_cursor = input_state.line_lengths[input_state.current_line]; update_desired_column(); break;
         case '\b':
             erase_character(); update_desired_column(); break;
         case KEY_DELETE:
             {
                  int line_idx = input_state.current_line;
                  if (input_state.input_cursor < input_state.line_lengths[line_idx]) {
                       memmove(&input_state.lines[line_idx][input_state.input_cursor],
                               &input_state.lines[line_idx][input_state.input_cursor + 1],
                               input_state.line_lengths[line_idx] - input_state.input_cursor);
                       input_state.line_lengths[line_idx]--;
                  } else if (line_idx < input_state.total_lines - 1) {
                       int next_line_idx = line_idx + 1;
                       int current_len = input_state.line_lengths[line_idx];
                       int next_len = input_state.line_lengths[next_line_idx];
                       if (current_len + next_len < MAX_INPUT_LENGTH) {
                             memcpy(&input_state.lines[line_idx][current_len],
                                    input_state.lines[next_line_idx], next_len + 1);
                             input_state.line_lengths[line_idx] += next_len;
                             for (int l = next_line_idx; l < input_state.total_lines - 1; l++) {
                                 memcpy(input_state.lines[l], input_state.lines[l + 1], MAX_INPUT_LENGTH);
                                 input_state.line_lengths[l] = input_state.line_lengths[l + 1];
                             }
                             input_state.lines[input_state.total_lines - 1][0] = '\0';
                             input_state.line_lengths[input_state.total_lines - 1] = 0;
                             input_state.total_lines--;
                       } // Else: Optionally beep
                  }
             }
             break;
         case '\n':
              if (input_state.total_lines < MAX_INPUT_LINES) {
                   int line_idx = input_state.current_line;
                   int next_line_idx = line_idx + 1;
                   int remain = input_state.line_lengths[line_idx] - input_state.input_cursor;
 
                   for (int l = input_state.total_lines; l > next_line_idx; --l) {
                        memcpy(input_state.lines[l], input_state.lines[l - 1], MAX_INPUT_LENGTH);
                        input_state.line_lengths[l] = input_state.line_lengths[l - 1];
                   }
                   memcpy(input_state.lines[next_line_idx],
                          &input_state.lines[line_idx][input_state.input_cursor], remain + 1);
                   input_state.line_lengths[next_line_idx] = remain;
 
                   input_state.lines[line_idx][input_state.input_cursor] = '\0';
                   input_state.line_lengths[line_idx] = input_state.input_cursor;
 
                   input_state.current_line++;
                   input_state.total_lines++;
                   input_state.input_cursor = 0;
                   update_desired_column();
              } // Else: Optionally beep
              break;
         case '\t': {
             int next_tab_stop = ((input_state.input_cursor / TAB_WIDTH) + 1) * TAB_WIDTH;
             int spaces_to_insert = next_tab_stop - input_state.input_cursor;
             for (int i = 0; i < spaces_to_insert; i++) insert_character(' ');
             update_desired_column();
             break;
         }
         default: // Printable character
             if (code >= 0x20 && code <= 0x7E) {
                 char ch = apply_modifiers_extended((char)code, event.modifiers);
                 if (ch != 0) {
                      insert_character(ch); update_desired_column();
                 } else redraw_needed = false;
             } else redraw_needed = false;
             break;
     }
 
     if (redraw_needed) {
         redraw_input();
     }
 
     spinlock_release_irqrestore(&terminal_lock, flags); // Release lock
 }
 
 
 /**
  * @brief Starts an interactive multi-line input session. Thread-safe.
  * @param prompt Optional prompt string to display.
  */
 void terminal_start_input(const char* prompt) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
 
     memset(&input_state, 0, sizeof(input_state));
     input_state.total_lines = 1;
     input_state.current_line = 0;
     input_state.input_cursor = 0;
     input_state.desired_column = 0;
     input_state.is_active = true;
     input_state.start_row = cursor_y; // Start input where cursor currently is
 
     if (prompt) {
         input_state.is_active = false; // Disable input temporarily for prompt
         for (size_t i = 0; prompt[i] != '\0'; i++) terminal_putchar_internal(prompt[i]);
         input_state.is_active = true;  // Re-enable input
         input_state.start_row = cursor_y; // Update start row after prompt
         input_state.input_cursor = cursor_x; // Update input cursor
         input_state.desired_column = cursor_x;
     }
 
     redraw_input(); // Draw the initial input area (lock still held)
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /**
  * @brief Concatenates all input lines into a provided buffer.
  * Lines are separated by newline characters. Thread-safe.
  * @param buffer Destination buffer.
  * @param size Size of the destination buffer.
  * @return Number of bytes written to the buffer (excluding null terminator),
  * or -1 if buffer is too small (output might be truncated).
  */
 int terminal_get_input(char* buffer, size_t size) {
     if (!buffer || size == 0) return -1;
 
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (!input_state.is_active) {
          spinlock_release_irqrestore(&terminal_lock, flags);
          buffer[0] = '\0';
          return 0; // Return empty string if not active
     }
 
     size_t current_pos = 0;
     bool truncated = false;
 
     for (int i = 0; i < input_state.total_lines; i++) {
         size_t line_len = input_state.line_lengths[i];
         bool is_last_line = (i == input_state.total_lines - 1);
         size_t space_needed_for_line = line_len;
         size_t space_needed_for_newline = is_last_line ? 0 : 1;
 
         // Check if line fits
         if (current_pos + space_needed_for_line >= size) {
              line_len = (size > current_pos + 1) ? (size - current_pos - 1) : 0; // Leave space for NUL
              truncated = true;
         }
         // Copy truncated or full line
         if (line_len > 0) {
              memcpy(&buffer[current_pos], input_state.lines[i], line_len);
              current_pos += line_len;
         }
         // Check if newline fits (only if line wasn't truncated and it's not the last line)
         if (!truncated && !is_last_line) {
              if (current_pos + space_needed_for_newline < size) {
                   buffer[current_pos++] = '\n';
              } else {
                   truncated = true; // No space for newline, mark truncated
              }
         }
         if (truncated) break; // Stop if buffer full
     }
 
     buffer[current_pos] = '\0'; // Ensure null termination
     spinlock_release_irqrestore(&terminal_lock, flags);
 
     return truncated ? -1 : (int)current_pos;
 }
 
 /**
  * @brief Completes the interactive input session, moving cursor below input.
  * Thread-safe.
  */
 void terminal_complete_input(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
 
     if (!input_state.is_active) {
          spinlock_release_irqrestore(&terminal_lock, flags);
          return;
     }
 
     input_state.is_active = false; // Turn off input mode
 
     // Calculate where cursor should go based on number of lines *drawn*
     int available_rows = VGA_ROWS - input_state.start_row;
     if (available_rows <= 0) available_rows = 1;
     int visible_lines = (input_state.total_lines > available_rows) ? available_rows : input_state.total_lines;
     cursor_y = input_state.start_row + visible_lines;
     cursor_x = 0;
 
     // Ensure cursor is on screen, scroll if necessary
     while (cursor_y >= VGA_ROWS) {
         scroll_terminal();
         cursor_y--;
          // Need to potentially adjust start_row if we scrolled past it during completion?
          // This gets complex, simpler to just place cursor at bottom after scroll.
         // Let's force cursor to last line after scroll
         cursor_y = VGA_ROWS - 1;
     }
 
 
     // Ensure a newline is printed *after* input area (moves cursor down if possible)
     // We only call the internal function if we are sure the cursor isn't already
     // at the very bottom-left after the calculations above AND a scroll happened.
     // This avoids an unnecessary scroll just for the final newline.
      terminal_putchar_internal('\n'); // Print final newline
 
     update_hardware_cursor(); // Show hardware cursor again at final position
     spinlock_release_irqrestore(&terminal_lock, flags);
 }