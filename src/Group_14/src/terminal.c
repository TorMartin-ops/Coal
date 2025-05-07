/**
 * @file terminal.c
 * @brief VGA text-mode driver for a 32-bit x86 OS.
 * @version 5.6 (Corrected itoa_simple placement, unused function attributes)
 * @author Tor Martin Kohle
 *
 * Features:
 * - Standard text output with scrolling.
 * - Dual output to VGA and Serial Port (COM1).
 * - Re-entrant, spin-lock protected printf.
 * - Basic ANSI escape sequence support.
 * - Simple blocking single-line input for syscalls.
 * - Separate multi-line interactive input editing state.
 */

 #include "terminal.h"
 #include "port_io.h"
 #include "keyboard.h"       // For KeyEvent, KeyCode, and apply_modifiers_extended declaration
 #include "types.h"          // For pid_t, ssize_t, etc.
 #include "spinlock.h"
 #include "serial.h"         // For serial_write, serial_print_hex, serial_putchar
 #include "assert.h"
 #include "scheduler.h"      // For get_current_task, schedule, tcb_t, TASK_BLOCKED, TASK_READY, scheduler_unblock_task
 #include "fs_errno.h"       // For error codes like -EINTR if interrupting sleep
 
 #include <libc/stdarg.h>
 #include <libc/stdbool.h>
 #include <libc/stddef.h>
 #include <libc/stdint.h>
 #include <string.h>         // Kernel's string functions
 
 /* ------------------------------------------------------------------------- */
 /* Utility Macros                                                            */
 /* ------------------------------------------------------------------------- */
 #ifndef MIN
 #define MIN(a, b) (((a) < (b)) ? (a) : (b))
 #endif
 
 /* ------------------------------------------------------------------------- */
 /* Manual 64-bit limits (lift when libc supplies them)                       */
 /* ------------------------------------------------------------------------- */
 #ifndef LLONG_MAX
 #   define LLONG_MAX  9223372036854775807LL
 #endif
 #ifndef LLONG_MIN
 #   define LLONG_MIN (-LLONG_MAX - 1LL)
 #endif
 #ifndef ULLONG_MAX
 #   define ULLONG_MAX 18446744073709551615ULL
 #endif
 
 /* ------------------------------------------------------------------------- */
 /* Compile-time configuration                                                */
 /* ------------------------------------------------------------------------- */
 #define TAB_WIDTH           4
 #define PRINTF_BUFFER_SIZE 256
 #define MAX_INPUT_LINES     64
 /* MAX_INPUT_LENGTH comes from terminal.h */
 
 /* ------------------------------------------------------------------------- */
 /* VGA hardware constants                                                    */
 /* ------------------------------------------------------------------------- */
 #define VGA_MEM_ADDRESS     0xC00B8000
 #define VGA_COLS            80
 #define VGA_ROWS            25
 #define VGA_CMD_PORT        0x3D4
 #define VGA_DATA_PORT       0x3D5
 #define VGA_REG_CURSOR_HI   0x0E
 #define VGA_REG_CURSOR_LO   0x0F
 #define VGA_REG_CURSOR_START 0x0A
 #define VGA_REG_CURSOR_END   0x0B
 #define CURSOR_SCANLINE_START 14
 #define CURSOR_SCANLINE_END   15
 
 /* Simple colour helpers */
 #define VGA_RGB(fg,bg)   (uint8_t)(((bg)&0x0F)<<4 | ((fg)&0x0F))
 #define VGA_FG(attr)     ((attr)&0x0F)
 #define VGA_BG(attr)     (((attr)>>4)&0x0F)
 
 /* ------------------------------------------------------------------------- */
 /* ANSI escape-code state machine                                            */
 /* ------------------------------------------------------------------------- */
 typedef enum {
     ANSI_STATE_NORMAL,
     ANSI_STATE_ESC,
     ANSI_STATE_BRACKET,
     ANSI_STATE_PARAM
 } AnsiState;
 
 /* ------------------------------------------------------------------------- */
 /* Terminal global state                                                     */
 /* ------------------------------------------------------------------------- */
 static spinlock_t terminal_lock;
 static uint8_t    terminal_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
 static volatile uint16_t * const vga_buffer = (volatile uint16_t *)VGA_MEM_ADDRESS;
 static int        cursor_x = 0;
 static int        cursor_y = 0;
 static uint8_t    cursor_visible = 1;
 static AnsiState  ansi_state = ANSI_STATE_NORMAL;
 static bool       ansi_private = false;
 static int        ansi_params[4];
 static int        ansi_param_count = 0;
 
 /* ------------------------------------------------------------------------- */
 /* Single-Line Input Buffer for SYS_READ_TERMINAL                            */
 /* ------------------------------------------------------------------------- */
 static char       s_line_buffer[MAX_INPUT_LENGTH];
 static volatile size_t s_line_buffer_len = 0;
 static volatile bool s_line_ready_for_read = false;
 static volatile tcb_t *s_waiting_task = NULL;
 static spinlock_t s_line_buffer_lock;
 
 /* ------------------------------------------------------------------------- */
 /* Interactive multi-line input (Separate from single-line syscall input)    */
 /* ------------------------------------------------------------------------- */
 typedef struct {
     char  lines[MAX_INPUT_LINES][MAX_INPUT_LENGTH];
     int   line_lengths[MAX_INPUT_LINES];
     int   current_line;
     int   total_lines;
     int   input_cursor;
     int   start_row;
     int   desired_column;
     bool  is_active;
 } terminal_input_state_t;
 
 static terminal_input_state_t input_state;
 
 // --- Static Helper Function: itoa_simple ---
 // Moved to the top of the file, before any function that calls it.
 static int itoa_simple(size_t val, char *buf, int base) { // Changed val to size_t
     char *p = buf;
     char *p1, *p2;
     uint32_t ud = val; 
     int digits = 0;
 
     if (base != 10) {
         if (buf) {
             buf[0] = '?'; 
             buf[1] = '\0';
         }
         return 1;
     }
 
     if (val == 0) {
         if (buf) {
             buf[0] = '0';
             buf[1] = '\0';
         }
         return 1;
     }
 
     // Calculate characters (for positive numbers, base 10)
     do {
         if (p - buf >= 11) break; 
         *p++ = (ud % base) + '0';
         digits++;
     } while (ud /= base);
 
     *p = '\0'; // Null-terminate
 
     // Reverse the string
     p1 = buf;
     p2 = p - 1;
     while (p1 < p2) {
         char tmp = *p1;
         *p1 = *p2;
         *p2 = tmp;
         p1++;
         p2--;
     }
     return digits;
 }
 
 
 /* ------------------------------------------------------------------------- */
 /* Forward declarations for other static functions                           */
 /* ------------------------------------------------------------------------- */
 static void update_hardware_cursor(void);
 static void enable_hardware_cursor(void);
 static void disable_hardware_cursor(void);
 static void put_char_at(char c, uint8_t color, int x, int y);
 static void clear_row(int row, uint8_t color);
 static void scroll_terminal(void);
 static void __attribute__((unused)) redraw_input(void); // Marked unused
 static void __attribute__((unused)) update_desired_column(void); // Marked unused
 static void __attribute__((unused)) insert_character(char c); // Marked unused
 static void __attribute__((unused)) erase_character(void); // Marked unused
 static void process_ansi_code(char c);
 static void terminal_putchar_internal(char c);
 static void terminal_clear_internal(void);
 
 static int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);
 static int _format_number(unsigned long num, bool is_negative, int base, bool upper,
                           int min_width, bool zero_pad, char *buf, int buf_sz);
 
 
 /* ------------------------------------------------------------------------- */
 /* Helpers                                                                   */
 /* ------------------------------------------------------------------------- */
 static inline uint16_t vga_entry(char ch, uint8_t color) {
     return (uint16_t)ch | (uint16_t)color << 8;
 }
 
 /* ------------------------------------------------------------------------- */
 /* Hardware cursor                                                           */
 /* ------------------------------------------------------------------------- */
 static void enable_hardware_cursor(void) {
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xC0) | CURSOR_SCANLINE_START);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_END);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xE0) | CURSOR_SCANLINE_END);
 }
 static void disable_hardware_cursor(void) {
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, 0x20);
 }
 static void update_hardware_cursor(void) {
     if (!cursor_visible || input_state.is_active || s_waiting_task != NULL) {
         disable_hardware_cursor();
         return;
     }
     enable_hardware_cursor();
     if (cursor_x < 0)            { cursor_x = 0; }
     if (cursor_y < 0)            { cursor_y = 0; }
     if (cursor_x >= VGA_COLS)    { cursor_x = VGA_COLS - 1; }
     if (cursor_y >= VGA_ROWS)    { cursor_y = VGA_ROWS - 1; }
     uint16_t pos = (uint16_t)(cursor_y * VGA_COLS + cursor_x);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO); outb(VGA_DATA_PORT, pos & 0xFF);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI); outb(VGA_DATA_PORT, (pos >> 8) & 0xFF);
 }
 
 /* ------------------------------------------------------------------------- */
 /* Low-level VGA buffer access                                               */
 /* ------------------------------------------------------------------------- */
 static void put_char_at(char c, uint8_t color, int x, int y) {
     if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) { return; }
     vga_buffer[y * VGA_COLS + x] = vga_entry(c, color);
 }
 static void clear_row(int row, uint8_t color) {
     if (row < 0 || row >= VGA_ROWS) { return; }
     uint16_t entry = vga_entry(' ', color);
     size_t idx = (size_t)row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; ++col) { vga_buffer[idx + col] = entry; }
 }
 static void scroll_terminal(void) {
     memmove((void*)vga_buffer, (const void*)(vga_buffer + VGA_COLS), (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
     clear_row(VGA_ROWS - 1, terminal_color);
     if (input_state.is_active && input_state.start_row > 0) { input_state.start_row--; }
 }
 
 /* ------------------------------------------------------------------------- */
 /* ANSI escape parser                                                        */
 /* ------------------------------------------------------------------------- */
 static void reset_ansi_state(void) {
     ansi_state       = ANSI_STATE_NORMAL;
     ansi_private     = false;
     ansi_param_count = 0;
     for (int i = 0; i < 4; ++i) { ansi_params[i] = -1; }
 }
 static void process_ansi_code(char c) {
     switch (ansi_state) {
     case ANSI_STATE_NORMAL: if (c == '\033') { ansi_state = ANSI_STATE_ESC; } break;
     case ANSI_STATE_ESC:
         if (c == '[') {
             ansi_state = ANSI_STATE_BRACKET; ansi_private = false; ansi_param_count = 0;
             for (int i = 0; i < 4; ++i) { ansi_params[i] = -1; }
         } else { ansi_state = ANSI_STATE_NORMAL; }
         break;
     case ANSI_STATE_BRACKET:
         if (c == '?') { ansi_private = true; }
         else if (c >= '0' && c <= '9') { ansi_state = ANSI_STATE_PARAM; ansi_params[0] = c - '0';}
         else if (c == 'J') { terminal_clear_internal(); ansi_state = ANSI_STATE_NORMAL; }
         else { ansi_state = ANSI_STATE_NORMAL; }
         break;
     case ANSI_STATE_PARAM:
         if (c >= '0' && c <= '9') {
             int *cur = &ansi_params[ansi_param_count];
             if (*cur == -1) { *cur = 0; }
             if (*cur > (INT32_MAX / 10) - 10) { *cur = INT32_MAX; } else { *cur = (*cur * 10) + (c - '0'); }
         } else if (c == ';') {
             if (ansi_param_count < 3) { ++ansi_param_count; ansi_params[ansi_param_count] = -1; }
             else { ansi_state = ANSI_STATE_NORMAL; }
         } else {
             int p0 = (ansi_params[0] == -1) ? 0 : ansi_params[0];
             switch (c) {
             case 'm':
                 for (int i = 0; i <= ansi_param_count; ++i) {
                     int p = (ansi_params[i] == -1) ? 0 : ansi_params[i];
                     if (p == 0) { terminal_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK); }
                     else if (p >= 30 && p <= 37) { terminal_color = (terminal_color & 0xF0) | (p - 30); }
                     else if (p >= 40 && p <= 47) { terminal_color = ((p - 40) << 4) | (terminal_color & 0x0F); }
                     else if (p >= 90 && p <= 97) { terminal_color = (terminal_color & 0xF0) | ((p - 90) + 8); }
                     else if (p >= 100 && p <= 107) { terminal_color = (((p - 100) + 8) << 4) | (terminal_color & 0x0F); }
                 }
                 ansi_state = ANSI_STATE_NORMAL; break;
             case 'J': if (p0 == 2 || p0 == 0) { terminal_clear_internal(); } ansi_state = ANSI_STATE_NORMAL; break;
             case 'h': case 'l':
                 if (ansi_private && p0 == 25) { cursor_visible = (c == 'h'); update_hardware_cursor(); }
                 ansi_state = ANSI_STATE_NORMAL; break;
             default: ansi_state = ANSI_STATE_NORMAL; break;
             }
         }
         break;
     }
 }
 
 /* ------------------------------------------------------------------------- */
 /* Core output                                                               */
 /* ------------------------------------------------------------------------- */
 static void terminal_putchar_internal(char c) {
     if (ansi_state != ANSI_STATE_NORMAL || c == '\033') {
         process_ansi_code(c);
         if (ansi_state != ANSI_STATE_NORMAL || c == '\033') { return; }
     }
     switch (c) {
     case '\n': cursor_x = 0; cursor_y++; break;
     case '\r': cursor_x = 0; break;
     case '\b': if (cursor_x > 0) { cursor_x--; } break;
     case '\t': {
         int next_tab = ((cursor_x / TAB_WIDTH) + 1) * TAB_WIDTH;
         if (next_tab >= VGA_COLS) { next_tab = VGA_COLS - 1; }
         while (cursor_x < next_tab) { put_char_at(' ', terminal_color, cursor_x++, cursor_y); }
         break;
     }
     default:
         if (c >= ' ' && c <= '~') { put_char_at(c, terminal_color, cursor_x, cursor_y); cursor_x++; }
         break;
     }
     if (cursor_x >= VGA_COLS) { cursor_x = 0; cursor_y++; }
     while (cursor_y >= VGA_ROWS) { scroll_terminal(); cursor_y--; }
     serial_putchar(c);
 }
 
 /* Public output wrappers */
 void terminal_putchar(char c) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_putchar_internal(c);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_write(const char *str) {
     if (!str) { return; }
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; str[i]; ++i) { terminal_putchar_internal(str[i]); }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_write_len(const char *data, size_t size) {
     if (!data || !size) { return; }
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; i < size; ++i) { terminal_putchar_internal(data[i]); }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* printf                                                                    */
 /* ------------------------------------------------------------------------- */
 static int _format_number(unsigned long num, bool is_negative, int base, bool upper,
                           int min_width, bool zero_pad, char *buf, int buf_sz) {
     if (buf_sz < 2) { return 0; }
     char tmp[33]; int i = 0;
     const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
     if (base < 2 || base > 16) { base = 10; }
     if (num == 0) { tmp[i++] = '0'; }
     else { while (num && i < (int)sizeof(tmp) - 1) { tmp[i++] = digits[num % base]; num /= base; } }
     int num_digits = i; int sign_char = (is_negative && base == 10);
     int total = num_digits + sign_char; int padding = (min_width > total) ? (min_width - total) : 0;
     if (total + padding >= buf_sz) { if (buf_sz > 5) { strncpy(buf, "[...]", (size_t)buf_sz); buf[buf_sz-1] = '\0'; return buf_sz-1; } else { buf[0] = '\0'; return 0; } }
     int pos = 0; if (sign_char) { buf[pos++] = '-'; }
     char pad_char = zero_pad ? '0' : ' '; for (int p = 0; p < padding; ++p) { buf[pos++] = pad_char; }
     while (i > 0) { buf[pos++] = tmp[--i]; }
     buf[pos] = '\0'; return pos;
 }
 
 static int _vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
     if (!str || !size) { return 0; }
     size_t w = 0; char tmp[34];
     char* current_str_pos = str;
 
     while (*fmt && w < size - 1) {
         if (*fmt != '%') {
             *current_str_pos++ = *fmt++;
             w++;
             continue;
         }
         fmt++;
 
         bool zero_pad = false;
         int min_width = 0;
         bool is_long = false;
         bool alt_form = false;
         bool parsing_flags_width = true;
 
         while(parsing_flags_width) {
             switch(*fmt) {
                 case '0':
                     if (min_width == 0) {
                         zero_pad = true;
                     } else {
                         min_width = min_width * 10 + (*fmt - '0');
                     }
                     fmt++;
                     break;
                 case '#': alt_form = true; fmt++; break;
                 case '1': case '2': case '3': case '4': case '5':
                 case '6': case '7': case '8': case '9':
                     min_width = min_width * 10 + (*fmt - '0');
                     fmt++;
                     break;
                 default: parsing_flags_width = false; break;
             }
         }
 
         if (*fmt == 'l') {
             is_long = true;
             fmt++;
             if (*fmt == 'l') { // for %ll
                 // is_long_long = true; // If you need to differentiate for 64-bit
                 fmt++;
             }
         }
 
         const char *arg_s = NULL;
         int len_num_str = 0;
         unsigned long val_ul = 0;
 
         switch (*fmt) {
             case 's':
                 alt_form = false;
                 arg_s = va_arg(args, const char *);
                 if (!arg_s) { arg_s = "(null)"; }
                 len_num_str = (int)strlen(arg_s);
                 break;
             case 'c':
                 alt_form = false;
                 tmp[0] = (char)va_arg(args, int);
                 tmp[1] = '\0';
                 arg_s = tmp;
                 len_num_str = 1;
                 break;
             case 'd':
                 {
                     alt_form = false;
                     long val_l = 0;
                     bool is_negative = false;
                     if (is_long) { val_l = va_arg(args, long); }
                     else { val_l = (long)va_arg(args, int); }
 
                     if (val_l < 0) {
                         is_negative = true;
                         if (val_l == (-2147483647L - 1L)) { // INT32_MIN
                              val_ul = 2147483648UL;
                         } else {
                             val_ul = (unsigned long)(-val_l);
                         }
                     } else {
                         val_ul = (unsigned long)val_l;
                     }
                     len_num_str = _format_number(val_ul, is_negative, 10, false, min_width, zero_pad, tmp, sizeof(tmp));
                     arg_s = tmp;
                 }
                 break;
             case 'u':
                 alt_form = false;
                 // fallthrough // Explicitly note the fallthrough for -Wimplicit-fallthrough
             case 'x':
             case 'X':
                 {
                     if (is_long) { val_ul = va_arg(args, unsigned long); }
                     else { val_ul = (unsigned long)va_arg(args, unsigned int); }
 
                     if (alt_form && val_ul != 0 && (*fmt == 'x' || *fmt == 'X')) {
                         if (w < size - 3) { // Check space for "0x" or "0X"
                             *current_str_pos++ = '0'; w++;
                             *current_str_pos++ = (*fmt == 'X' ? 'X' : 'x'); w++;
                         }
                     }
                     int base = (*fmt == 'u') ? 10 : 16;
                     bool uppercase_hex = (*fmt == 'X');
                     len_num_str = _format_number(val_ul, false, base, uppercase_hex, min_width, zero_pad, tmp, sizeof(tmp));
                     arg_s = tmp;
                 }
                 break;
             case 'p':
                 {
                     uintptr_t ptr_val = (uintptr_t)va_arg(args, void*);
                     val_ul = (unsigned long)ptr_val;
                     alt_form = false; // '#' for pointers is typically handled by '0x' prefix
 
                     if (w < size - 3) { // Check space for "0x"
                         *current_str_pos++ = '0'; w++;
                         *current_str_pos++ = 'x'; w++;
                     }
                     // Ensure enough width for a 32-bit pointer (8 hex digits)
                     int ptr_min_width = sizeof(uintptr_t) * 2;
                     if (min_width < ptr_min_width) min_width = ptr_min_width;
                     zero_pad = true; // Pointers are usually zero-padded
 
                     len_num_str = _format_number(val_ul, false, 16, false, min_width, zero_pad, tmp, sizeof(tmp));
                     arg_s = tmp;
                 }
                 break;
             case '%':
                 alt_form = false;
                 tmp[0] = '%'; tmp[1] = '\0';
                 arg_s = tmp; len_num_str = 1;
                 break;
             default:
                 alt_form = false;
                 if (w < size - 2) {
                     *current_str_pos++ = '%'; w++;
                     if(*fmt) { *current_str_pos++ = *fmt; w++; }
                 } else if (w < size -1) {
                     *current_str_pos++ = '%'; w++;
                 }
                 break;
         }
 
         if (arg_s) {
             for (int i = 0; i < len_num_str && w < size - 1; ++i) {
                 *current_str_pos++ = arg_s[i];
                 w++;
             }
         }
         if (*fmt) { fmt++; }
     }
     *current_str_pos = '\0';
     return (int)w;
 }
 
 void terminal_printf(const char *fmt, ...) {
     char buf[PRINTF_BUFFER_SIZE]; va_list ap; va_start(ap, fmt);
     int len = _vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (int i = 0; i < len; ++i) { terminal_putchar_internal(buf[i]); }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* Terminal control                                                          */
 /* ------------------------------------------------------------------------- */
 void terminal_clear_internal(void) {
     for (int y = 0; y < VGA_ROWS; ++y) { clear_row(y, terminal_color); }
     cursor_x = cursor_y = 0; reset_ansi_state();
 }
 void terminal_clear(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_clear_internal(); update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_set_color(uint8_t color) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_color = color; spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_set_cursor_pos(int x, int y) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_x = x; cursor_y = y; update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_get_cursor_pos(int *x, int *y) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (x) { *x = cursor_x; }
     if (y) { *y = cursor_y; }
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 void terminal_set_cursor_visibility(uint8_t visible) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_visible = !!visible; update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* Initialization                                                            */
 /* ------------------------------------------------------------------------- */
 void terminal_init(void) {
     spinlock_init(&terminal_lock);
     spinlock_init(&s_line_buffer_lock);
     s_line_buffer_len = 0;
     s_line_ready_for_read = false;
     s_waiting_task = NULL;
     memset(s_line_buffer, 0, MAX_INPUT_LENGTH);
 
     terminal_clear_internal();
     input_state.is_active = false;
     enable_hardware_cursor();
     update_hardware_cursor();
     serial_write("[Terminal] Initialized (VGA + Serial + Single-line input buffer)\n");
 }
 
 /* ------------------------------------------------------------------------- */
 /* Interactive Input - Keyboard Event Handler                                */
 /* ------------------------------------------------------------------------- */
 void terminal_handle_key_event(const KeyEvent event) {
    if (event.action != KEY_PRESS) {
        return;
    }

    uintptr_t line_buf_irq_flags = spinlock_acquire_irqsave(&s_line_buffer_lock);
    uintptr_t term_out_irq_flags; // To be used when terminal_lock is acquired

    char char_to_add = 0;

    // Determine character based on KeyCode
    if (event.code > 0 && event.code < 0x80) { // Printable ASCII or control char like \n, \b
        // Use apply_modifiers_extended which should handle special keys if they are mapped to ASCII in keymap
        char_to_add = apply_modifiers_extended((char)event.code, event.modifiers);
    } else if (event.code == KEY_BACKSPACE) { // Explicitly handle KeyCodes for special keys
        char_to_add = '\b'; // Map KEY_BACKSPACE to ASCII backspace
    }
    // Note: Newline ('\n') should directly come as event.code if mapped in keymap_*.c files from scancode 0x1C.
    // If KEY_ENTER is used, it should be mapped to '\n' there or handled here.
    // Assuming event.code will be '\n' for Enter key presses based on current keymaps.

    if (char_to_add != 0) {
        if (char_to_add == '\n') {
            s_line_buffer[s_line_buffer_len] = '\0'; // Null-terminate the collected line
            s_line_ready_for_read = true;            // Mark line as ready

            // --- Enhanced Serial Logging ---
            serial_write("[Terminal] Newline processed. Line ready. Buffer: '");
            serial_write(s_line_buffer); // Log the content of the buffer
            serial_write("', Length: ");
            char len_str[12]; // Temp buffer for itoa_simple
            itoa_simple(s_line_buffer_len, len_str, 10); // Convert length to string
            serial_write(len_str);
            serial_write("\n");
            // --- End Enhanced Logging ---

            if (s_waiting_task) {
                // --- Enhanced Serial Logging ---
                serial_write("[Terminal] Found waiting task. PID: ");
                if (s_waiting_task->process) { // Check if process pointer is valid
                     serial_print_hex(s_waiting_task->process->pid); // Log PID
                } else {
                     serial_write("UNKNOWN_PID (process ptr null)");
                }
                serial_write(". Attempting to unblock.\n");
                // --- End Enhanced Logging ---

                tcb_t* task_to_unblock = (tcb_t*)s_waiting_task;
                s_waiting_task = NULL; // Clear before unblocking

                scheduler_unblock_task(task_to_unblock); // Unblock the task
                serial_write("[Terminal] scheduler_unblock_task called for the waiting task.\n");
            } else {
                serial_write("[Terminal] Line ready, but no task was waiting (s_waiting_task is NULL).\n");
            }
            
            // Echo newline to terminal display
            term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
            terminal_putchar_internal('\n'); 
            update_hardware_cursor();
            spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);

        } else if (char_to_add == '\b') { // Handle Backspace
            if (s_line_buffer_len > 0) {
                s_line_buffer_len--;
                s_line_buffer[s_line_buffer_len] = '\0'; // Keep it null-terminated

                // Echo backspace to terminal display
                term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
                terminal_putchar_internal('\b'); // Move cursor back
                terminal_putchar_internal(' ');  // Erase char
                terminal_putchar_internal('\b'); // Move cursor back again
                update_hardware_cursor();
                spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);
                serial_write("[Terminal] Backspace processed. Buffer len: ");
                char len_str[12]; itoa_simple(s_line_buffer_len, len_str, 10); serial_write(len_str); serial_write("\n");
            }
        } else { // Printable character
            if (s_line_buffer_len < MAX_INPUT_LENGTH - 1) {
                s_line_buffer[s_line_buffer_len++] = char_to_add;
                s_line_buffer[s_line_buffer_len] = '\0'; // Keep it null-terminated

                // Echo character to terminal display
                term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
                terminal_putchar_internal(char_to_add);
                update_hardware_cursor();
                spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);
            } else {
                 serial_write("[Terminal WARNING] Single-line input buffer full. Character discarded.\n");
            }
        }
    } else {
        // Non-printable, non-special key (e.g., F-keys, arrow keys if not mapped to chars)
        // serial_write("[Terminal] Non-addable key event received. Code: 0x");
        // serial_print_hex(event.code);
        // serial_write("\n");
    }
    spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
}
 
 
 /* ------------------------------------------------------------------------- */
 /* Blocking Terminal Read for Syscall                                        */
 /* ------------------------------------------------------------------------- */
 ssize_t terminal_read_line_blocking(char *kbuf, size_t len) {
     if (!kbuf || len == 0) {
         return -EINVAL;
     }
 
     serial_write("[Terminal] terminal_read_line_blocking: Enter\n");
     ssize_t bytes_copied = 0;
 
     while (true) {
         uintptr_t line_buf_irq_flags = spinlock_acquire_irqsave(&s_line_buffer_lock);
 
         if (s_line_ready_for_read) {
             serial_write("[Terminal] terminal_read_line_blocking: Line is ready.\n");
             size_t copy_len = MIN(s_line_buffer_len, len - 1); 
             memcpy(kbuf, s_line_buffer, copy_len);
             kbuf[copy_len] = '\0';
             bytes_copied = (ssize_t)copy_len;
 
             s_line_ready_for_read = false;
             s_line_buffer_len = 0;
             memset(s_line_buffer, 0, MAX_INPUT_LENGTH);
 
             spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
             
             serial_write("[Terminal] terminal_read_line_blocking: Copied bytes: '");
             serial_write(kbuf); 
             serial_write("'\n");
             return bytes_copied;
         } else {
             serial_write("[Terminal] terminal_read_line_blocking: Line not ready, blocking task.\n");
             tcb_t *current_task_non_volatile = (tcb_t*)get_current_task();
             if (!current_task_non_volatile) {
                 spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
                 serial_write("[Terminal] terminal_read_line_blocking: ERROR - No current task to block!\n");
                 return -EFAULT;
             }
 
             if (s_waiting_task != NULL && s_waiting_task != current_task_non_volatile) {
                 spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
                 serial_write("[Terminal] terminal_read_line_blocking: ERROR - Another task is already waiting!\n");
                 return -EBUSY;
             }
 
             s_waiting_task = current_task_non_volatile;
             current_task_non_volatile->state = TASK_BLOCKED;
 
             spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
 
             serial_write("[Terminal] terminal_read_line_blocking: Calling schedule().\n");
             schedule(); // Yield CPU
             serial_write("[Terminal] terminal_read_line_blocking: Woke up from schedule(). Re-checking line.\n");
         }
     }
     // Should not be reached
     return -EIO;
 }
 
 
 /* ------------------------------------------------------------------------- */
 /* Multi-line input editor (Stubs)                                           */
 /* ------------------------------------------------------------------------- */
 static void __attribute__((unused)) redraw_input(void) { /* Placeholder */ }
 static void __attribute__((unused)) update_desired_column(void) { /* Placeholder */ }
 static void __attribute__((unused)) insert_character(char c) { (void)c; /* Placeholder */ }
 static void __attribute__((unused)) erase_character(void) { /* Placeholder */ }
 
 void terminal_start_input(const char* prompt) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (input_state.is_active) { terminal_complete_input(); }
     if (prompt) { terminal_write(prompt); }
     input_state.is_active = true;
     // ... (rest of your multi-line init logic) ...
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 int terminal_get_input(char* buffer, size_t size) {
     if (!input_state.is_active || !buffer || size == 0) { return -1; }
     // ... (rest of your multi-line get logic) ...
     return -1; // Placeholder
 }
 
 void terminal_complete_input(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (!input_state.is_active) { spinlock_release_irqrestore(&terminal_lock, flags); return; }
     input_state.is_active = false;
     // ... (rest of your multi-line completion logic) ...
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 
 /* ------------------------------------------------------------------------- */
 /* Legacy/Misc                                                               */
 /* ------------------------------------------------------------------------- */
 void terminal_write_char(char c) { terminal_putchar(c); }
 
 void terminal_backspace(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (cursor_x > 0) {
         cursor_x--;
     } else if (cursor_y > 0) {
         cursor_y--;
         cursor_x = VGA_COLS - 1;
     } else {
         spinlock_release_irqrestore(&terminal_lock, flags);
         return;
     }
     put_char_at(' ', terminal_color, cursor_x, cursor_y);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 void terminal_write_bytes(const char* data, size_t size) {
     if (!data || size == 0) { return; }
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; i < size; ++i) {
         terminal_putchar_internal(data[i]);
     }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* End of file                                                               */
 /* ------------------------------------------------------------------------- */