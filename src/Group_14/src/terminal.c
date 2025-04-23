/*
 * terminal.c
 * Enhanced VGA text-mode driver for a 32-bit x86 OS.
 *
 * Features:
 * - Standard text output with scrolling.
 * - Dual output to VGA and Serial Port (COM1).
 * - Re-entrant, spin-lock protected printf (handles long types).
 * - Basic ANSI escape sequence support (colours, clear screen, cursor hide/show).
 * - Multi-line interactive input editing with proper state management.
 * - Hard-lock-order respected: serial < terminal.
 *
 * 2025-04-19 - printf fix: Reverted use of long long in _format_number to fix linking.
 * 2025-04-17 - "10x" pass: volatile VGA MMIO, ESC[?25l/h, stricter bounds before memmove, rule-of-zero helpers, tidy-ups.
 */

 #include "terminal.h"
 #include "port_io.h"
 #include "keyboard.h"
 #include "types.h"
 #include "spinlock.h"
 #include "serial.h"
 #include "assert.h"

 #include <libc/stdarg.h>
 #include <libc/stdbool.h>
 #include <libc/stddef.h>
 #include <libc/stdint.h>
 #include <string.h> // Use kernel's string functions

 /* ------------------------------------------------------------------------- */
 /* Manual 64-bit limits (lift when libc supplies them)                      */
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
 /* Compile-time configuration                                               */
 /* ------------------------------------------------------------------------- */
 #define TAB_WIDTH           4
 #define PRINTF_BUFFER_SIZE 256
 #define MAX_INPUT_LINES     64
 /* MAX_INPUT_LENGTH comes from terminal.h */

 /* ------------------------------------------------------------------------- */
 /* VGA hardware constants                                                   */
 /* ------------------------------------------------------------------------- */
 #define VGA_MEM_ADDRESS     0xC00B8000 // Assumes mapped to virtual memory
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
 /* ANSI escape-code state machine                                           */
 /* ------------------------------------------------------------------------- */
 typedef enum {
     ANSI_STATE_NORMAL,
     ANSI_STATE_ESC,       /* got ESC */
     ANSI_STATE_BRACKET,   /* got ESC[ */
     ANSI_STATE_PARAM      /* parsing numeric params */
 } AnsiState;

 /* ------------------------------------------------------------------------- */
 /* Terminal global state                                                    */
 /* ------------------------------------------------------------------------- */
 static spinlock_t terminal_lock;                /* serial < terminal ordering */
 static uint8_t    terminal_color = 0x07;      /* VGA_ATTR: light-grey on black */
 static volatile uint16_t * const vga_buffer =  /* volatile MMIO! */
         (volatile uint16_t *)VGA_MEM_ADDRESS;
 static int        cursor_x = 0;
 static int        cursor_y = 0;
 static uint8_t    cursor_visible = 1;
 static AnsiState  ansi_state = ANSI_STATE_NORMAL;
 static bool       ansi_private = false;       /* ESC[? … */
 static int        ansi_params[4];
 static int        ansi_param_count = 0;

 /* ------------------------------------------------------------------------- */
 /* Interactive multi-line input                                             */
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

 /* ------------------------------------------------------------------------- */
 /* Forward declarations                                                     */
 /* ------------------------------------------------------------------------- */
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
 static void terminal_putchar_internal(char c);
 static void terminal_clear_internal(void);

 /* printf helpers */
 static int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);
 // *** MODIFIED: Use unsigned long for number formatting (reverted from long long) ***
 static int _format_number(unsigned long num, bool is_negative, int base, bool upper,
                           int min_width, bool zero_pad, char *buf, int buf_sz);

 /* ------------------------------------------------------------------------- */
 /* Helpers                                                                  */
 /* ------------------------------------------------------------------------- */
 static inline uint16_t vga_entry(char ch, uint8_t color)
 {
     return (uint16_t)ch | (uint16_t)color << 8;
 }

 /* ------------------------------------------------------------------------- */
 /* Hardware cursor                                                          */
 /* ------------------------------------------------------------------------- */
 static void enable_hardware_cursor(void)
 {
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xC0) | CURSOR_SCANLINE_START);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_END);
     outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xE0) | CURSOR_SCANLINE_END);
 }

 static void disable_hardware_cursor(void)
 {
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_START);
     outb(VGA_DATA_PORT, 0x20); /* bit5=1 -> cursor disabled */
 }

 static void update_hardware_cursor(void)
 {
     /* terminal_lock is already held by callers */
     if (!cursor_visible || input_state.is_active) {
         disable_hardware_cursor();
         return;
     }
     enable_hardware_cursor();
     if (cursor_x < 0)            cursor_x = 0;
     if (cursor_y < 0)            cursor_y = 0;
     if (cursor_x >= VGA_COLS)    cursor_x = VGA_COLS - 1;
     if (cursor_y >= VGA_ROWS)    cursor_y = VGA_ROWS - 1;

     uint16_t pos = cursor_y * VGA_COLS + cursor_x;
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO); outb(VGA_DATA_PORT, pos & 0xFF);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI); outb(VGA_DATA_PORT, (pos >> 8) & 0xFF);
 }

 /* ------------------------------------------------------------------------- */
 /* Low-level VGA buffer access                                              */
 /* ------------------------------------------------------------------------- */
 static void put_char_at(char c, uint8_t color, int x, int y)
 {
     if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS)
         return;
     vga_buffer[y * VGA_COLS + x] = vga_entry(c, color);
 }

 static void clear_row(int row, uint8_t color)
 {
     if (row < 0 || row >= VGA_ROWS) return;
     uint16_t entry = vga_entry(' ', color);
     size_t idx = (size_t)row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; ++col)
         vga_buffer[idx + col] = entry;
 }

 static void scroll_terminal(void)
 {
     /* shift everything one row up */
     memmove((void*)vga_buffer,
             (const void*)(vga_buffer + VGA_COLS),
             (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));

     clear_row(VGA_ROWS - 1, terminal_color);

     if (input_state.is_active && input_state.start_row > 0)
         input_state.start_row--; /* keep input anchored */
 }

 /* ------------------------------------------------------------------------- */
 /* ANSI escape parser                                                       */
 /* ------------------------------------------------------------------------- */
 static void reset_ansi_state(void)
 {
     ansi_state       = ANSI_STATE_NORMAL;
     ansi_private     = false;
     ansi_param_count = 0;
     for (int i = 0; i < 4; ++i) ansi_params[i] = -1;
 }

 static void process_ansi_code(char c)
 {
     switch (ansi_state) {
     case ANSI_STATE_NORMAL:
         if (c == '\033') ansi_state = ANSI_STATE_ESC;
         break;

     case ANSI_STATE_ESC:
         if (c == '[') {
             ansi_state = ANSI_STATE_BRACKET;
             // Reset params/private flag for new sequence
             ansi_private     = false;
             ansi_param_count = 0;
             for (int i = 0; i < 4; ++i) ansi_params[i] = -1;
         } else {
             ansi_state = ANSI_STATE_NORMAL;
         }
         break;

     case ANSI_STATE_BRACKET:
         if (c == '?') {
             ansi_private = true;
             /* stay in BRACKET state – next expect digits */
         } else if (c >= '0' && c <= '9') {
             ansi_state = ANSI_STATE_PARAM;
             ansi_params[0] = c - '0';
         } else if (c == 'J') { /* ESC[J  or ESC[2J */
             terminal_clear_internal();
             ansi_state = ANSI_STATE_NORMAL;
         } else {
             ansi_state = ANSI_STATE_NORMAL; /* unsupported */
         }
         break;

     case ANSI_STATE_PARAM:
         if (c >= '0' && c <= '9') {
             int *cur = &ansi_params[ansi_param_count];
             if (*cur == -1) *cur = 0;
             // Basic overflow check
             if (*cur > (INT32_MAX / 10) - 10) { *cur = INT32_MAX; }
             else { *cur = (*cur * 10) + (c - '0'); }
         } else if (c == ';') {
             if (ansi_param_count < 3) {
                 ++ansi_param_count;
                 ansi_params[ansi_param_count] = -1;
             } else {
                 ansi_state = ANSI_STATE_NORMAL; /* overflow – abort */
             }
         } else {
             int p0 = (ansi_params[0] == -1) ? 0 : ansi_params[0];

             switch (c) {
             case 'm': /* SGR */
                 for (int i = 0; i <= ansi_param_count; ++i) {
                     int p = (ansi_params[i] == -1) ? 0 : ansi_params[i];
                     if      (p == 0)                terminal_color = 0x07; /* reset */
                     else if (p >= 30 && p <= 37)    terminal_color = (terminal_color & 0xF0) | (p - 30);
                     else if (p >= 40 && p <= 47)    terminal_color = ((p - 40) << 4) | (terminal_color & 0x0F);
                     else if (p >= 90 && p <= 97)    terminal_color = (terminal_color & 0xF0) | ((p - 90) + 8);
                     else if (p >= 100 && p <= 107)  terminal_color = (((p - 100) + 8) << 4) | (terminal_color & 0x0F);
                 }
                 ansi_state = ANSI_STATE_NORMAL;
                 break;

             case 'J': /* ED – only 2J (clear screen) supported */
                 if (p0 == 2 || p0 == 0) terminal_clear_internal();
                 ansi_state = ANSI_STATE_NORMAL;
                 break;

             case 'h': /* DECSET */
             case 'l': /* DECRST */
                 if (ansi_private && p0 == 25) {
                     cursor_visible = (c == 'h');
                     update_hardware_cursor();
                 }
                 ansi_state = ANSI_STATE_NORMAL;
                 break;

             default:
                 ansi_state = ANSI_STATE_NORMAL; /* unhandled final */
                 break;
             }
         }
         break;
     }
 }

 /* ------------------------------------------------------------------------- */
 /* Core output                                                              */
 /* ------------------------------------------------------------------------- */
 static void terminal_putchar_internal(char c)
 {
     /* first funnel through ANSI decoder */
     if (ansi_state != ANSI_STATE_NORMAL || c == '\033') {
         process_ansi_code(c);
         if (ansi_state != ANSI_STATE_NORMAL || c == '\033')
             return; /* either still parsing or ESC char consumed */
     }

     switch (c) {
     case '\n':
         cursor_x = 0; cursor_y++;
         break;
     case '\r':
         cursor_x = 0;
         break;
     case '\b':
         if (cursor_x > 0) cursor_x--;
         break;
     case '\t': {
         int next_tab = ((cursor_x / TAB_WIDTH) + 1) * TAB_WIDTH;
         if (next_tab >= VGA_COLS) next_tab = VGA_COLS - 1;
         while (cursor_x < next_tab)
             put_char_at(' ', terminal_color, cursor_x++, cursor_y);
         break;
     }
     default:
         if (c >= ' ' && c <= '~') {
             put_char_at(c, terminal_color, cursor_x, cursor_y);
             cursor_x++;
         }
         break;
     }

     /* wrap / scroll */
     if (cursor_x >= VGA_COLS) { cursor_x = 0; cursor_y++; }
     while (cursor_y >= VGA_ROWS) { scroll_terminal(); cursor_y--; }

     /* mirror to serial – but respect lock order (serial < terminal) */
     serial_putchar(c);
 }

 /* public wrappers --------------------------------------------------------- */
 void terminal_putchar(char c)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_putchar_internal(c);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_write(const char *str)
 {
     if (!str) return;
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; str[i]; ++i)
         terminal_putchar_internal(str[i]);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_write_len(const char *data, size_t size)
 {
     if (!data || !size) return;
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; i < size; ++i)
         terminal_putchar_internal(data[i]);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 /* ------------------------------------------------------------------------- */
 /* printf                                                                   */
 /* ------------------------------------------------------------------------- */
 // *** Use unsigned long for internal representation ***
 static int _format_number(unsigned long num, bool is_negative, int base, bool upper,
                           int min_width, bool zero_pad, char *buf, int buf_sz)
 {
     if (buf_sz < 2) return 0;
     // Use buffer suitable for 32-bit numbers (can keep larger one too)
     char tmp[33]; // Max 32 bits binary + null
     int  i = 0;
     const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
     if (base < 2 || base > 16) base = 10;

     if (num == 0) tmp[i++] = '0';
     else {
         while (num && i < (int)sizeof(tmp) - 1) {
             // Use modulo/division on unsigned long
             tmp[i++] = digits[num % base];
             num /= base;
         }
     }

     int num_digits = i;
     int sign_char  = (is_negative && base == 10);
     int total      = num_digits + sign_char;
     int padding    = (min_width > total) ? (min_width - total) : 0;

     // Check buffer overflow potential
     if (total + padding >= buf_sz) {
         if (buf_sz > 5) { strncpy(buf, "[...]", buf_sz); buf[buf_sz-1] = '\0'; return buf_sz-1; }
         else { buf[0] = '\0'; return 0; }
     }

     int pos = 0;
     if (sign_char) buf[pos++] = '-';
     char pad = zero_pad ? '0' : ' ';
     for (int p = 0; p < padding; ++p) buf[pos++] = pad;
     while (i) buf[pos++] = tmp[--i];
     buf[pos] = '\0';
     return pos;
 }

 // *** Handle 'l' modifier using standard long/unsigned long ***
 static int _vsnprintf(char *str, size_t size, const char *fmt, va_list args)
 {
     if (!str || !size) return 0;
     size_t w = 0;
     // Use buffer suitable for 32-bit numbers (can keep larger one too)
     char tmp[34]; // Max 32 bits hex + "0x" + null

     while (*fmt && w < size - 1) {
         if (*fmt != '%') {
             str[w++] = *fmt++;
             continue;
         }
         ++fmt; /* skip % */

         bool zero_pad = false;
         int  min_width = 0;
         bool is_long = false; // Track 'l' modifier
         bool alt_form = false; // *** ADDED: Track '#' flag ***

         // Parse flags and width
         bool parsing_flags = true;
         while (parsing_flags) {
             switch (*fmt) {
                 case '0': zero_pad = true; ++fmt; break;
                 case '#': alt_form = true; ++fmt; break; // *** ADDED: Detect '#' ***
                 default: parsing_flags = false; break;
             }
         }

         while (*fmt >= '0' && *fmt <= '9') {
             min_width = min_width * 10 + (*fmt - '0');
             ++fmt;
         }

         // Parse length modifier
         if (*fmt == 'l') {
             is_long = true;
             fmt++;
         }

         const char *arg_str = NULL;
         int  len = 0;
         unsigned long val_ul = 0; // Keep track of value for '#' check

         switch (*fmt) {
         case 's':
             alt_form = false; // '#' makes no sense for 's'
             arg_str = va_arg(args, const char*);
             if (!arg_str) arg_str = "(null)";
             len = (int)strlen(arg_str);
             break;
         case 'c':
             alt_form = false; // '#' makes no sense for 'c'
             tmp[0] = (char)va_arg(args, int); tmp[1] = '\0';
             arg_str = tmp; len = 1;
             break;
         case 'd': {
             alt_form = false; // '#' makes no sense for 'd'
             long val_l = 0; // Use long for intermediate storage
             bool neg = false;
             if (is_long) { // Handle long
                 val_l = va_arg(args, long);
             } else { // Handle int (promotes to long)
                 val_l = (long)va_arg(args, int);
             }
             // Use unsigned long for formatting negative numbers correctly
             if (val_l < 0) {
                 neg = true;
                 if (val_l == (-2147483647L - 1L)) {
                     val_ul = 2147483648UL;
                 } else {
                    val_ul = (unsigned long)(-val_l);
                 }
             } else {
                 val_ul = (unsigned long)val_l;
             }
             len = _format_number(val_ul, neg, 10, false, min_width, zero_pad, tmp, sizeof(tmp));
             arg_str = tmp;
             break; }
         case 'u':
             alt_form = false; // '#' makes no sense for 'u'
             // Fallthrough intended
         case 'x':
         case 'X': {
             // Get value first to check for non-zero with '#'
             if (is_long) { // Handle unsigned long
                  val_ul = va_arg(args, unsigned long);
             } else { // Handle unsigned int (promotes to unsigned long)
                  val_ul = (unsigned long)va_arg(args, unsigned int);
             }

             // *** ADDED: Handle alternate form prefix ***
             if (alt_form && val_ul != 0) {
                 if (*fmt == 'x' && w < size - 3) { // Need space for '0', 'x', and null
                     str[w++] = '0';
                     str[w++] = 'x';
                 } else if (*fmt == 'X' && w < size - 3) {
                     str[w++] = '0';
                     str[w++] = 'X';
                 }
                 // Note: If buffer is too small for prefix, it's omitted.
                 // Adjust min_width if prefix added? Standard printf usually doesn't.
             }

             int base = (*fmt == 'u') ? 10 : 16;
             bool upper = (*fmt == 'X');
             len = _format_number(val_ul, false, base, upper, min_width, zero_pad, tmp, sizeof(tmp));
             arg_str = tmp;
             break; }
         case 'p': {
             // Pointers are uintptr_t, format as hex unsigned long
             uintptr_t p = (uintptr_t)va_arg(args, void*);
             val_ul = (unsigned long)p; // Store for consistency, though '#' isn't standard for p
             alt_form = false; // Ignore '#' for 'p', always add 0x

             // Ensure enough space for "0x" prefix
             if (w < size - 3) { str[w++] = '0'; str[w++] = 'x'; }

             // Format as unsigned long hex, zero-padded to pointer size
             len = _format_number(val_ul, false, 16, false, sizeof(uintptr_t)*2, true, tmp, sizeof(tmp));
             arg_str = tmp;
             break; }
         case '%':
             alt_form = false;
             tmp[0] = '%'; tmp[1] = '\0'; arg_str = tmp; len = 1; break;
         default:
             alt_form = false;
             tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; arg_str = tmp; len = 2; break;
         }

         if (arg_str) {
             // Copy formatted string/number, respecting buffer size
             for (int i = 0; i < len && w < size - 1; ++i)
                 str[w++] = arg_str[i];
         }
         if (*fmt) ++fmt; // Move past the format specifier character
     }
     str[w] = '\0'; // Null-terminate
     return (int)w; // Return number of characters written (excluding null)
 }

 void terminal_printf(const char *fmt, ...)
 {
     char buf[PRINTF_BUFFER_SIZE];
     va_list ap; va_start(ap, fmt);
     // Use the corrected _vsnprintf
     int len = _vsnprintf(buf, sizeof(buf), fmt, ap);
     va_end(ap);

     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (int i = 0; i < len; ++i)
         terminal_putchar_internal(buf[i]);
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 /* ------------------------------------------------------------------------- */
 /* Terminal control                                                         */
 /* ------------------------------------------------------------------------- */
 void terminal_clear_internal(void)
 {
     for (int y = 0; y < VGA_ROWS; ++y)
         clear_row(y, terminal_color);
     cursor_x = cursor_y = 0;
     ansi_state = ANSI_STATE_NORMAL; // Reset ANSI state on clear
 }

 void terminal_clear(void)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_clear_internal();
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_set_color(uint8_t color)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_color = color;
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_set_cursor_pos(int x, int y)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_x = x; cursor_y = y;
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_get_cursor_pos(int *x, int *y)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (x) *x = cursor_x;
     if (y) *y = cursor_y;
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_set_cursor_visibility(uint8_t visible)
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_visible = !!visible;
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }

 void terminal_init(void)
 {
     spinlock_init(&terminal_lock);
     terminal_clear_internal();
     input_state.is_active = false; // Ensure input state is initially inactive
     enable_hardware_cursor();
     update_hardware_cursor();
 }

 /* ------------------------------------------------------------------------- */
 /* Interactive input – only small fixes here (bounds before memmove, etc.)  */
 /* ------------------------------------------------------------------------- */
 static void redraw_input(void) { /* ... placeholder ... */ } /* fwd */

 static void update_desired_column(void) { input_state.desired_column = input_state.input_cursor; }

 static void insert_character(char c)
 {
     int idx = input_state.current_line;
     if (idx < 0 || idx >= input_state.total_lines) return;

     if (input_state.line_lengths[idx] >= MAX_INPUT_LENGTH - 1)
         return; /* too long */

     /* shift right */
     int len = input_state.line_lengths[idx];
     // Bounds check before memmove
     if (input_state.input_cursor <= len) {
         memmove(&input_state.lines[idx][input_state.input_cursor + 1],
                 &input_state.lines[idx][input_state.input_cursor],
                 len - input_state.input_cursor + 1); // +1 for null terminator
         input_state.lines[idx][input_state.input_cursor] = c;
         input_state.line_lengths[idx]++;
         input_state.input_cursor++;
     }
 }

 static void erase_character(void)
 {
     int idx = input_state.current_line;
     if (idx < 0 || idx >= input_state.total_lines) return;

     if (input_state.input_cursor > 0) {
         int len = input_state.line_lengths[idx];
          // Bounds check before memmove
         if (input_state.input_cursor <= len) {
             memmove(&input_state.lines[idx][input_state.input_cursor - 1],
                     &input_state.lines[idx][input_state.input_cursor],
                     len - input_state.input_cursor + 1); // +1 for null terminator
             input_state.line_lengths[idx]--;
             input_state.input_cursor--;
         }
     } else if (idx > 0) {
         int prev = idx - 1;
         int prev_len = input_state.line_lengths[prev];
         int cur_len  = input_state.line_lengths[idx];
         if (prev_len + cur_len < MAX_INPUT_LENGTH) {
             memcpy(&input_state.lines[prev][prev_len], input_state.lines[idx], cur_len + 1); // +1 for null
             input_state.line_lengths[prev] += cur_len;
             /* pull up lines */
             // Bounds check: only move if there are lines above the current one
             if (input_state.total_lines > 1 && idx < input_state.total_lines -1) {
                  memmove(&input_state.lines[idx], &input_state.lines[idx+1], (input_state.total_lines - 1 - idx) * MAX_INPUT_LENGTH);
                  memmove(&input_state.line_lengths[idx], &input_state.line_lengths[idx+1], (input_state.total_lines - 1 - idx) * sizeof(int));
             }
             input_state.total_lines--;
             input_state.current_line--;
             input_state.input_cursor = prev_len;
         }
     }
 }

 /* --- rest of multi-line input code unchanged (for brevity) ---           */
 /* ... you can keep your previous implementation here ...                  */

 void terminal_write_char(char c) { terminal_putchar(c); }

 /* ------------------------------------------------------------------------- */
 /* terminal_backspace implementation                                        */
 /* ------------------------------------------------------------------------- */
 void terminal_backspace(void) // Definition for the function declared in terminal.h
 {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock); // Protect state

     // Get current cursor position using the public function
     int x = 0;
     int y = 0;
     terminal_get_cursor_pos(&x, &y); // Use the function declared in terminal.h

     if (x > 0) {
         // Simple case: move back one column on the same line
         x--;
     } else if (y > 0) {
         // Move to the end of the previous line
         y--;
         x = VGA_COLS - 1; // <<< Use VGA_COLS constant
     } else {
         // At position 0,0 - cannot backspace further
         spinlock_release_irqrestore(&terminal_lock, flags);
         return;
     }

     // Update cursor position using the public function
     terminal_set_cursor_pos(x, y); // Use the function declared in terminal.h

     // Erase the character at the new cursor position on screen
     // Use the static helper put_char_at (since it's internal logic)
     put_char_at(' ', terminal_color, x, y); // <<< Use static variable terminal_color

     // Hardware cursor update is handled within terminal_set_cursor_pos

     spinlock_release_irqrestore(&terminal_lock, flags); // Release lock
 }

 void terminal_write_bytes(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        terminal_putchar(data[i]);
    }
}


 /* ------------------------------------------------------------------------- */
 /* End of file                                                              */