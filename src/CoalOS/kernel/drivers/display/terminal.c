/**
 * @file terminal.c
 * @brief VGA text-mode driver for a 32-bit x86 OS.
 * @version 5.8 
 * @author Tor Martin Kohle
 */

 #include <kernel/drivers/display/terminal.h>
 #include <kernel/drivers/display/vga_hardware.h>
 #include <kernel/drivers/display/vga_cursor.h>
 #include <kernel/drivers/display/terminal_buffer.h>
 #include <kernel/lib/port_io.h>
 #include <kernel/drivers/input/keyboard.h>       // For KeyEvent, KeyCode, and apply_modifiers_extended declaration
 #include <kernel/core/types.h>          // For pid_t, ssize_t, etc.
 #include <kernel/sync/spinlock.h>
 #include <kernel/drivers/display/serial.h>         // For serial_write, serial_print_hex, serial_putchar
 #include <kernel/lib/assert.h>
 #include <kernel/process/scheduler.h>      // For get_current_task, schedule, tcb_t, TASK_BLOCKED, TASK_READY, scheduler_unblock_task
 #include <kernel/fs/vfs/fs_errno.h>       // For error codes like -EINTR if interrupting sleep
 
 #include <libc/stdarg.h>
 #include <libc/stdbool.h>
 #include <libc/stddef.h>
 #include <libc/stdint.h>
 #include <kernel/lib/string.h>         // Kernel's string functions
 
 /* ------------------------------------------------------------------------- */
 /* Utility Macros                                                            */
 /* ------------------------------------------------------------------------- */
 #ifndef MIN
 #define MIN(a, b) (((a) < (b)) ? (a) : (b))
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
 // VGA_ADDRESS, VGA_COLS, VGA_ROWS kommer fra terminal.h
 #define VGA_CMD_PORT        0x3D4
 #define VGA_DATA_PORT       0x3D5
 #define VGA_REG_CURSOR_HI   0x0E
 #define VGA_REG_CURSOR_LO   0x0F
 #define VGA_REG_CURSOR_START 0x0A
 #define VGA_REG_CURSOR_END   0x0B
 #define CURSOR_SCANLINE_START 14 // Typiske verdier for en blokkmarkør
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
 static volatile uint16_t * const vga_buffer = (volatile uint16_t *)VGA_ADDRESS; 
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
 
 // --- Static Helper Function: itoa_simple --- (UNUSED)
#if 0
 static int itoa_simple(size_t val, char *buf, int base) {
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
 
     do {
         if (p - buf >= 11) break; 
         *p++ = (ud % base) + '0';
         digits++;
     } while (ud /= base);
 
     *p = '\0'; 
 
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
#endif // 0 - End of unused itoa_simple
 
 /* ------------------------------------------------------------------------- */
 /* Forward declarations for other static functions                           */
 /* ------------------------------------------------------------------------- */
 static void update_hardware_cursor(void);
 static void enable_hardware_cursor(void);
 static void disable_hardware_cursor(void);
 static void put_char_at(char c, uint8_t color, int x, int y);
 static void clear_row(int row, uint8_t color);
 static void scroll_terminal(void);
 static void __attribute__((unused)) redraw_input(void); 
 static void __attribute__((unused)) update_desired_column(void); 
 static void __attribute__((unused)) insert_character(char c);
 static void __attribute__((unused)) erase_character(void);
 static void process_ansi_code(char c);
 static void terminal_putchar_internal(char c); 
 static void terminal_clear_internal(void); 
 
 int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);
 // *** MODIFIED _format_number signature ***
 static int _format_number(unsigned long num, bool is_negative, int base, bool upper, // Changed from unsigned long long
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
     int local_cursor_x = cursor_x;
     int local_cursor_y = cursor_y;
 
     if (local_cursor_x < 0)            { local_cursor_x = 0; }
     if (local_cursor_y < 0)            { local_cursor_y = 0; }
     if (local_cursor_x >= VGA_COLS)    { local_cursor_x = VGA_COLS - 1; }
     if (local_cursor_y >= VGA_ROWS)    { local_cursor_y = VGA_ROWS - 1; }
     
     uint16_t pos = (uint16_t)(local_cursor_y * VGA_COLS + local_cursor_x);
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_LO); outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));
     outb(VGA_CMD_PORT, VGA_REG_CURSOR_HI); outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
 
     if (cursor_visible && !input_state.is_active) { 
         enable_hardware_cursor();
     } else {
         disable_hardware_cursor();
     }
 }
 
 /* ------------------------------------------------------------------------- */
 /* Low-level VGA buffer access                                               */
 /* ------------------------------------------------------------------------- */
 static void put_char_at(char c, uint8_t color, int x, int y) {
     if (x < 0 || x >= VGA_COLS || y < 0 || y >= VGA_ROWS) {
         return;
     }
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
     if (cursor_y > 0) cursor_y--; 
     
     if (input_state.is_active && input_state.start_row > 0) {
         input_state.start_row--;
     }
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
     case ANSI_STATE_NORMAL:
         if (c == '\033') { 
             ansi_state = ANSI_STATE_ESC;
         }
         break;
 
     case ANSI_STATE_ESC:
         if (c == '[') { 
             ansi_state = ANSI_STATE_BRACKET;
             ansi_private = false;
             ansi_param_count = 0;
             for (int i = 0; i < 4; ++i) { ansi_params[i] = -1; }
         } else { 
             reset_ansi_state();
         }
         break;
 
     case ANSI_STATE_BRACKET:
         if (c == '?') {
             ansi_private = true;
         } else if (c >= '0' && c <= '9') {
             ansi_state = ANSI_STATE_PARAM;
             ansi_params[ansi_param_count] = c - '0'; 
         } else if (c == 'J') { 
             terminal_clear_internal(); 
             reset_ansi_state();
         } else if (c == 'm') { 
             terminal_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
             reset_ansi_state();
         }
         else { 
             reset_ansi_state();
         }
         break;
 
     case ANSI_STATE_PARAM:
         if (c >= '0' && c <= '9') {
             int *cur = &ansi_params[ansi_param_count];
             if (*cur == -1) { *cur = 0; } 
             if (*cur > (INT32_MAX / 10) - (c - '0')) { *cur = INT32_MAX; } 
             else { *cur = (*cur * 10) + (c - '0'); }
         } else if (c == ';') {
             if (ansi_param_count < 3) { 
                 ansi_param_count++;
                 ansi_params[ansi_param_count] = -1; 
             } else { 
                 reset_ansi_state();
             }
         } else { 
             for(int i=0; i <= ansi_param_count; ++i) {
                 if(ansi_params[i] == -1) ansi_params[i] = 0; // Default til 0 for uspesifiserte params
             }
 
             int p0 = ansi_params[0];
             
             switch (c) {
                 case 'm': 
                     for (int i = 0; i <= ansi_param_count; ++i) {
                         int p = ansi_params[i];
                         if (p == 0) { terminal_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK); } 
                         else if (p >= 30 && p <= 37) { terminal_color = (terminal_color & 0xF0) | (p - 30); } 
                         else if (p >= 40 && p <= 47) { terminal_color = ((p - 40) << 4) | (terminal_color & 0x0F); } 
                         else if (p >= 90 && p <= 97) { terminal_color = (terminal_color & 0xF0) | ((p - 90) + 8); } 
                         else if (p >= 100 && p <= 107){ terminal_color = (((p - 100) + 8) << 4) | (terminal_color & 0x0F); } 
                     }
                     break;
                 case 'J': 
                     if (p0 == 2 || p0 == 0) { terminal_clear_internal(); } 
                     break;
                 case 'H': // Cursor Position
                 case 'f': // Også Cursor Position
                    {
                        int row = (ansi_params[0] > 0) ? ansi_params[0] - 1 : 0; // 1-basert til 0-basert
                        int col = (ansi_params[1] > 0) ? ansi_params[1] - 1 : 0;
                        if (row >= VGA_ROWS) row = VGA_ROWS - 1;
                        if (col >= VGA_COLS) col = VGA_COLS - 1;
                        cursor_x = col;
                        cursor_y = row;
                    }
                    break;
                 case 'h': 
                 case 'l': 
                     if (ansi_private && p0 == 25) { 
                         cursor_visible = (c == 'h');
                     }
                     break;
             }
             reset_ansi_state(); 
         }
         break;
     }
 }
 
 /* ------------------------------------------------------------------------- */
 /* Core output - intern, antar lås holdes                                     */
 /* ------------------------------------------------------------------------- */
 static void terminal_putchar_internal(char c) {
     if (ansi_state != ANSI_STATE_NORMAL || c == '\033') {
         process_ansi_code(c);
         if (ansi_state != ANSI_STATE_NORMAL || c == '\033') {
             return;
         }
     }
 
     switch (c) {
         case '\n':
             cursor_x = 0;
             cursor_y++;
             break;
         case '\r':
             cursor_x = 0;
             break;
         case '\b': 
             if (cursor_x > 0) {
                 cursor_x--;
             } else if (cursor_y > 0) { 
                 cursor_y--;
                 cursor_x = VGA_COLS - 1;
             }
             break;
         case '\t': {
             int next_tab_stop = (cursor_x / TAB_WIDTH + 1) * TAB_WIDTH;
             int spaces_to_add = MIN(next_tab_stop, VGA_COLS) - cursor_x;
             for (int i = 0; i < spaces_to_add; ++i) {
                 if (cursor_x < VGA_COLS) { 
                     put_char_at(' ', terminal_color, cursor_x, cursor_y);
                     cursor_x++;
                 } else { break; } 
             }
             break;
         }
         default: 
             if (c >= ' ' && c <= '~') { 
                 put_char_at(c, terminal_color, cursor_x, cursor_y);
                 cursor_x++;
             }
             break;
     }
 
     if (cursor_x >= VGA_COLS) {
         cursor_x = 0;
         cursor_y++;
     }
     while (cursor_y >= VGA_ROWS) { 
         scroll_terminal();
     }
     serial_putchar(c);
 }
 
 /* ------------------------------------------------------------------------- */
 /* Public Output Wrappers (tar lås)                                          */
 /* ------------------------------------------------------------------------- */
 
 void terminal_putchar(char c) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_putchar_internal(c);
     update_hardware_cursor(); 
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 void terminal_write(const char *str) {
     if (!str) { return; }
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; str[i]; ++i) {
         terminal_putchar_internal(str[i]);
     }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 void terminal_write_len(const char *data, size_t size) {
     if (!data || !size) { return; }
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     for (size_t i = 0; i < size; ++i) {
         terminal_putchar_internal(data[i]);
     }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* printf implementasjon                                                     */
 /* ------------------------------------------------------------------------- */
 
 // *** MODIFIED _format_number to use unsigned long (32-bit on i386) ***
 static int _format_number(unsigned long val_ul, bool is_negative, int base, bool upper,
                           int min_width, bool zero_pad, char *buf, int buf_sz) {
     if (buf_sz < 2) { return 0; } 
     char tmp_buf[33]; // Max for 32-bit binary + null
     int i = 0;
     const char *digits_map = upper ? "0123456789ABCDEF" : "0123456789abcdef";
 
     if (base < 2 || base > 16) base = 10; 
 
     if (val_ul == 0) {
         tmp_buf[i++] = '0';
     } else {
         while (val_ul > 0 && i < (int)sizeof(tmp_buf) -1) {
             tmp_buf[i++] = digits_map[val_ul % base]; // 32-bit % int -> no __umoddi3
             val_ul /= base;                      // 32-bit / int -> no __udivdi3
         }
     }
     int num_digits = i;
     int sign_len = (is_negative && base == 10) ? 1 : 0;
     int total_content_len = num_digits + sign_len;
     int padding_len = (min_width > total_content_len) ? (min_width - total_content_len) : 0;
 
     if (total_content_len + padding_len >= buf_sz) { 
         if (buf_sz > 5) { strncpy(buf, "[OVF]", (size_t)buf_sz); buf[buf_sz-1] = '\0'; return buf_sz-1; }
         else { buf[0] = '\0'; return 0; }
     }
 
     int current_pos = 0;
     char pad_char = ' ';
     if (zero_pad && !is_negative) { 
         pad_char = '0';
     }
      if (zero_pad && is_negative && base == 10) { 
         buf[current_pos++] = '-';
         sign_len = 0; 
     }
 
     for (int k = 0; k < padding_len; ++k) buf[current_pos++] = pad_char;
     if (sign_len) buf[current_pos++] = '-';
     while (i > 0) buf[current_pos++] = tmp_buf[--i];
 
     buf[current_pos] = '\0';
     return current_pos;
 }
 
 // *** MODIFIED _vsnprintf to handle unsigned long and avoid long long where possible ***
 int _vsnprintf(char *str, size_t size, const char *fmt, va_list args) {
     if (!str || !size) { if (size == 1 && str) str[0] = '\0'; return 0; }
 
     size_t written_chars = 0;
     char temp_num_buf[34]; 
 
     while (*fmt && written_chars < size - 1) {
         if (*fmt != '%') {
             str[written_chars++] = *fmt++;
             continue;
         }
         fmt++; 
 
         bool zero_pad = false;
         int min_width = 0;
         bool is_long = false;
         // bool is_long_long = false; // Avoid if possible
         bool alt_form = false; 
 
         if (*fmt == '0') { zero_pad = true; fmt++; }
         if (*fmt == '#') { alt_form = true; fmt++; }
 
         while (*fmt >= '0' && *fmt <= '9') {
             min_width = min_width * 10 + (*fmt - '0');
             fmt++;
         }
 
         if (*fmt == 'l') {
             is_long = true; fmt++;
             if (*fmt == 'l') { 
                 // is_long_long = true; // Mark it, but try to avoid 64-bit math
                 serial_write("Warning: %ll specifier used, may truncate or cause issues without libgcc 64bit support.\n");
                 fmt++; 
             }
         }
 
         const char *s_val;
         int len_s;
         unsigned long ul_val; // Use unsigned long for most numeric conversions
         long l_val;
         bool neg;
 
         switch (*fmt) {
             case 's':
                 s_val = va_arg(args, const char *);
                 if (!s_val) s_val = "(null)";
                 len_s = strlen(s_val);
                 // Padding for string (left-pad with spaces)
                 if(min_width > len_s) {
                     for(int k=0; k < (min_width - len_s) && written_chars < size -1; ++k) str[written_chars++] = ' ';
                 }
                 for (int k=0; k<len_s && written_chars < size -1; ++k) str[written_chars++] = s_val[k];
                 break;
             case 'c': str[written_chars++] = (char)va_arg(args, int); break;
             case 'd': case 'i':
                 // Assume long long is not commonly needed for kernel messages, try to use long
                 if (is_long) { l_val = va_arg(args, long); } // Handles %ld
                 else { l_val = va_arg(args, int); }        // Handles %d
                 neg = l_val < 0;
                 // Be careful with MIN_INT
                 ul_val = neg ? ((l_val == (-2147483647L - 1L)) ? 2147483648UL : (unsigned long)-l_val) : (unsigned long)l_val;
                 len_s = _format_number(ul_val, neg, 10, false, min_width, zero_pad, temp_num_buf, sizeof(temp_num_buf));
                 for (int k=0; k<len_s && written_chars < size -1; ++k) str[written_chars++] = temp_num_buf[k];
                 break;
             case 'u': case 'x': case 'X': case 'o':
                 // Assume long long is not commonly needed
                 if (is_long) { ul_val = va_arg(args, unsigned long); } // Handles %lu, %lx etc.
                 else { ul_val = va_arg(args, unsigned int); }         // Handles %u, %x etc.
                 
                 int base = (*fmt == 'u') ? 10 : ((*fmt == 'o') ? 8 : 16);
                 bool upper = (*fmt == 'X');
                 if (alt_form && ul_val != 0 && written_chars < size - 1) {
                     if (base == 16 && written_chars < size -2) { str[written_chars++] = '0'; str[written_chars++] = upper ? 'X' : 'x';}
                     else if (base == 8 && written_chars < size -1) { str[written_chars++] = '0';}
                 }
                 len_s = _format_number(ul_val, false, base, upper, min_width, zero_pad, temp_num_buf, sizeof(temp_num_buf));
                 for (int k=0; k<len_s && written_chars < size -1; ++k) str[written_chars++] = temp_num_buf[k];
                 break;
             case 'p': // Pointer
                 ul_val = (unsigned long)(uintptr_t)va_arg(args, void*); // Pointers are 32-bit
                 if (written_chars < size -1) str[written_chars++] = '0';
                 if (written_chars < size -1) str[written_chars++] = 'x';
                 int ptr_w = sizeof(void*) * 2; 
                 // if (min_width < ptr_w) min_width = ptr_w; // Ensure full pointer width
                 len_s = _format_number(ul_val, false, 16, false, ptr_w, true, temp_num_buf, sizeof(temp_num_buf)); // Pointers zero-padded
                 for (int k=0; k<len_s && written_chars < size -1; ++k) str[written_chars++] = temp_num_buf[k];
                 break;
             case '%': str[written_chars++] = '%'; break;
             default: if(written_chars < size -1) str[written_chars++] = '%'; if(*fmt && written_chars < size -1) str[written_chars++] = *fmt; break;
         }
         if(*fmt) fmt++; 
     }
     str[written_chars] = '\0';
     return (int)written_chars;
 }
 
 void terminal_printf(const char *fmt, ...) {
     char buf[PRINTF_BUFFER_SIZE];
     va_list args;
     va_start(args, fmt);
     int len = _vsnprintf(buf, sizeof(buf), fmt, args);
     va_end(args);
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&terminal_lock);
     for (int i = 0; i < len; ++i) {
         terminal_putchar_internal(buf[i]);
     }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, irq_flags);
 }

 void terminal_vprintf(const char *fmt, va_list args) {
     char buf[PRINTF_BUFFER_SIZE];
     int len = _vsnprintf(buf, sizeof(buf), fmt, args);
 
     uintptr_t irq_flags = spinlock_acquire_irqsave(&terminal_lock);
     for (int i = 0; i < len; ++i) {
         terminal_putchar_internal(buf[i]);
     }
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, irq_flags);
 }
 
 /* ------------------------------------------------------------------------- */
 /* Terminal control                                                          */
 /* ------------------------------------------------------------------------- */
 static void terminal_clear_internal(void) { 
     for (int y = 0; y < VGA_ROWS; ++y) {
         clear_row(y, terminal_color);
     }
     cursor_x = 0;
     cursor_y = 0;
     reset_ansi_state();
 }
 
 void terminal_clear(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_clear_internal();
     update_hardware_cursor();
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 void terminal_set_color(uint8_t color) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     terminal_color = color;
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 void terminal_set_cursor_pos(int x, int y) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     cursor_x = x;
     cursor_y = y;
     if (cursor_x < 0) cursor_x = 0; else if (cursor_x >= VGA_COLS) cursor_x = VGA_COLS -1;
     if (cursor_y < 0) cursor_y = 0; else if (cursor_y >= VGA_ROWS) cursor_y = VGA_ROWS -1;
     update_hardware_cursor();
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
     cursor_visible = !!visible; 
     update_hardware_cursor();
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
     update_hardware_cursor();   
     serial_write("[Terminal] Initialized (VGA + Serial + Single-line input buffer)\n");
 }
 
 /* ------------------------------------------------------------------------- */
 /* Interactive Input - Keyboard Event Handler                                */
 /* ------------------------------------------------------------------------- */
 void terminal_handle_key_event(const KeyEvent event) {
    if (event.action != KEY_PRESS) { // Only process key presses for line input
        return;
    }

    // This outer lock protects s_line_buffer, s_line_buffer_len, 
    // s_line_ready_for_read, and s_waiting_task.
    uintptr_t line_buf_irq_flags = spinlock_acquire_irqsave(&s_line_buffer_lock);

    char char_to_add = 0;
    bool is_newline = false;

    // Determine the character to process, converting KeyCode to char if applicable
    // and handling special keys like Enter and Backspace.
    if (event.code == KEY_ENTER || event.code == '\n') { // Check for KEY_ENTER or if keymap produces '\n'
        char_to_add = '\n';
        is_newline = true;
    } else if (event.code == KEY_BACKSPACE) {
        char_to_add = '\b';
    } else if (event.code > 0 && event.code < 0x80) { // Printable ASCII range from keymap
        char_to_add = apply_modifiers_extended((char)event.code, event.modifiers);
        // Check if the modified character itself is a newline (e.g., Ctrl+J if mapped)
        if (char_to_add == '\n') {
            is_newline = true;
        }
    }
    // Note: apply_modifiers_extended should handle shift, caps etc. for printable characters.
    // If KEY_ENTER or KEY_BACKSPACE are > 0x80, this logic is fine.
    // If they are mapped to '\n' and '\b' directly by the keymap, the first condition handles it.

    // If a character was processed (printable, newline, or backspace)
    if (char_to_add != 0) {
        
        // Serial logging for debug (uncomment and adapt as needed)
        /*
        serial_write("[Terminal KeyEvt] Char='");
        if (is_newline) { serial_write("\\n"); }
        else if (char_to_add == '\b') { serial_write("\\b"); }
        else if (char_to_add >= ' ' && char_to_add <= '~') { serial_putchar(char_to_add); }
        else { serial_print_hex((uint8_t)char_to_add); }
        serial_write("', Mod=0x"); serial_print_hex(event.modifiers); serial_write("\n");
        */

        if (is_newline) {
            s_line_buffer[s_line_buffer_len] = '\0'; // Null-terminate the completed line
            s_line_ready_for_read = true;            // Signal that the line is ready
            
            // serial_write("[Terminal] Newline. Buffer: '"); serial_write(s_line_buffer);
            // serial_write("', Len: "); 
            // char len_str[12]; itoa_simple(s_line_buffer_len, len_str, 10); serial_write(len_str); 
            // serial_write("\n");

            tcb_t* task_to_unblock = (tcb_t*)s_waiting_task; // Copy before potentially clearing
            
            if (task_to_unblock) {
                s_waiting_task = NULL; // Clear s_waiting_task *while still holding the lock*
                                       // to prevent races if the unblocked task re-enters read quickly.
            }
            
            // Release the line buffer lock BEFORE calling the scheduler or terminal output functions
            spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
            
            if (task_to_unblock) {
                // serial_write("[Terminal] Unblocking waiting task PID: ");
                // if (task_to_unblock->process) serial_print_hex(task_to_unblock->process->pid); else serial_write("UNKNOWN");
                // serial_write("\n");
                scheduler_unblock_task(task_to_unblock); // This might yield/schedule
            } else {
                // serial_write("[Terminal] Line ready, but no task was waiting.\n");
            }

            // Echo newline to terminal (needs terminal_lock, acquired separately)
            uintptr_t term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
            terminal_putchar_internal('\n'); 
            update_hardware_cursor();
            spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);

            return; // Exit after handling newline

        } else if (char_to_add == '\b') { // Backspace
            if (s_line_buffer_len > 0) {
                s_line_buffer_len--;
                s_line_buffer[s_line_buffer_len] = '\0'; // Null-terminate the shortened string
                
                // Release line buffer lock BEFORE terminal output
                spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);

                // Echo backspace, space, backspace to terminal
                uintptr_t term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
                terminal_putchar_internal('\b'); 
                terminal_putchar_internal(' ');  
                terminal_putchar_internal('\b'); 
                update_hardware_cursor();
                spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);
                
                // serial_write("[Terminal] Backspace. Buffer len: "); 
                // char len_str[12]; itoa_simple(s_line_buffer_len, len_str, 10); serial_write(len_str);
                // serial_write("\n");
                return; // Exit after handling backspace
            }
            // If buffer is empty, nothing to backspace. Just release lock.
            spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
            return;

        } else { // Regular character (printable)
            if (s_line_buffer_len < MAX_INPUT_LENGTH - 1) {
                s_line_buffer[s_line_buffer_len++] = char_to_add;
                // Optionally keep s_line_buffer null-terminated during input for easier debugging,
                // but terminal_read_line_blocking is responsible for final null termination
                // before copying to user.
                // s_line_buffer[s_line_buffer_len] = '\0'; 
                
                // Release line buffer lock BEFORE terminal output
                spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
                
                // Echo character to terminal output
                uintptr_t term_out_irq_flags = spinlock_acquire_irqsave(&terminal_lock);
                terminal_putchar_internal(char_to_add); 
                update_hardware_cursor(); 
                spinlock_release_irqrestore(&terminal_lock, term_out_irq_flags);
                
                return; // Exit after handling character
            } else {
                // serial_write("[Terminal WARNING] Single-line input buffer full. Char discarded.\n");
                // Buffer full, just release lock
                spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
                return;
            }
        }
    } else {
        // char_to_add was 0 (e.g., a modifier key press like Shift alone, or unhandled special key)
        // No action needed for the line buffer.
        spinlock_release_irqrestore(&s_line_buffer_lock, line_buf_irq_flags);
    }
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
             schedule(); 
             serial_write("[Terminal] terminal_read_line_blocking: Woke up from schedule(). Re-checking line.\n");
         }
     }
     return -EIO; 
 }
 
 /* ------------------------------------------------------------------------- */
 /* Multi-line input editor (Stubs - ikke fullt implementert)                 */
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
     memset(&input_state, 0, sizeof(input_state)); 
     input_state.is_active = true; 
     input_state.start_row = cursor_y;
     input_state.desired_column = cursor_x; 
     update_hardware_cursor(); 
     spinlock_release_irqrestore(&terminal_lock, flags);
 }
 
 int terminal_get_input(char* buffer, size_t size) {
     if (!input_state.is_active || !buffer || size == 0) { return -1; }
     return -1; 
 }
 
 void terminal_complete_input(void) {
     uintptr_t flags = spinlock_acquire_irqsave(&terminal_lock);
     if (!input_state.is_active) { spinlock_release_irqrestore(&terminal_lock, flags); return; }
     input_state.is_active = false;
     update_hardware_cursor(); 
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