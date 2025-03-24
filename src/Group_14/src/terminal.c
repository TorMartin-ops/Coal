/**
 * terminal.c
 *
 * A “world-class” VGA text-mode driver for a 32-bit x86 OS:
 *  - Maintains (cursor_x, cursor_y)
 *  - Supports backspace, newline, carriage return, tab
 *  - Scrolls if output passes the bottom row
 *  - Allows dynamic color changes (foreground & background)
 *  - Hardware cursor automatically updated (via port_io.h)
 *  - Additional helper functions (clear row, set cursor, etc.)
 */

 #include "terminal.h"
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include "port_io.h"  // for outb, if you haven't inlined them
 
 // VGA text mode constants
 #define VGA_ADDRESS        0xB8000
 #define VGA_COLS           80
 #define VGA_ROWS           25
 
 // A typical default color: light gray on black => 0x07
 static uint8_t terminal_color = 0x07;
 static uint16_t* vga_buffer = (uint16_t*)VGA_ADDRESS;
 
 // Cursor position
 static int cursor_x = 0;
 static int cursor_y = 0;
 
 /**
  * update_hardware_cursor
  *
  * Tells the VGA which cell the blinking text cursor is in,
  * using ports 0x3D4 and 0x3D5. 
  */
 static void update_hardware_cursor(void)
 {
     uint16_t pos = (cursor_y * VGA_COLS) + cursor_x;
 
     // High byte of cursor index
     outb(0x3D4, 14);
     outb(0x3D5, (pos >> 8) & 0xFF);
 
     // Low byte
     outb(0x3D4, 15);
     outb(0x3D5, pos & 0xFF);
 }
 
 /**
  * enable_hardware_cursor
  *
  * Optionally sets the cursor scanline start/end for shape, 
  * then ensures the cursor is visible.
  */
 static void enable_hardware_cursor(void)
 {
     // Cursor Start Register (0x0A), Cursor End Register (0x0B)
     // Example: make a block cursor from scanline 14-15:
     outb(0x3D4, 0x0A);
     outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);  // top scanline
     outb(0x3D4, 0x0B);
     outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);  // bottom scanline
 }
 
 /**
  * put_char_at
  *
  * Writes character 'c' at (x, y) with the current 'terminal_color'.
  */
 static void put_char_at(char c, int x, int y)
 {
     int index = y * VGA_COLS + x;
     vga_buffer[index] = (uint16_t)(c | (terminal_color << 8));
 }
 
 /**
  * terminal_clear
  *
  * Clears the entire 80x25 screen to spaces using the current color,
  * and resets cursor to (0,0).
  */
 void terminal_clear(void)
 {
     for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
         vga_buffer[i] = (uint16_t)(' ' | (terminal_color << 8));
     }
     cursor_x = 0;
     cursor_y = 0;
     update_hardware_cursor();
 }
 
 /**
  * terminal_clear_row
  *
  * Clears a single row 'row' with spaces in the current color.
  * If 'row' is out of bounds, do nothing.
  */
 void terminal_clear_row(int row)
 {
     if (row < 0 || row >= VGA_ROWS) return;
     int start = row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; col++) {
         vga_buffer[start + col] = (uint16_t)(' ' | (terminal_color << 8));
     }
 }
 
 /**
  * scroll_one_line
  *
  * Scrolls the screen up by one line, 
  * then clears the last row.
  */
 static void scroll_one_line(void)
 {
     // Move rows 1..24 => 0..23
     for (int row = 0; row < VGA_ROWS - 1; row++) {
         for (int col = 0; col < VGA_COLS; col++) {
             vga_buffer[row * VGA_COLS + col] =
                 vga_buffer[(row + 1) * VGA_COLS + col];
         }
     }
     // Clear the final row
     terminal_clear_row(VGA_ROWS - 1);
 }
 
 /**
  * terminal_init
  *
  * Sets up the terminal with default color, clears screen,
  * resets cursor, and enables hardware cursor.
  */
 void terminal_init(void)
 {
     terminal_color = 0x07; // default: light gray on black
     terminal_clear();
 
     // Enable & position hardware cursor
     enable_hardware_cursor();
     update_hardware_cursor();
 }
 
 /**
  * terminal_set_color
  *
  * Changes the text color for subsequent prints. 
  * Existing text won't change color, only new output.
  */
 void terminal_set_color(uint8_t color)
 {
     terminal_color = color;
 }
 
 /**
  * terminal_set_foreground
  *
  * Sets the low nibble of 'terminal_color' to 'fg'.
  * Leaves the high nibble (background) unchanged.
  */
 void terminal_set_foreground(uint8_t fg)
 {
     uint8_t bg = (terminal_color & 0xF0);
     terminal_color = bg | (fg & 0x0F);
 }
 
 /**
  * terminal_set_background
  *
  * Sets the high nibble of 'terminal_color' to 'bg'.
  * Leaves the low nibble (foreground) unchanged.
  */
 void terminal_set_background(uint8_t bg)
 {
     uint8_t fg = (terminal_color & 0x0F);
     terminal_color = ((bg & 0x0F) << 4) | fg;
 }
 
 /**
  * terminal_get_cursor_pos
  *
  * Returns the current (cursor_x, cursor_y).
  */
 void terminal_get_cursor_pos(int* x, int* y)
 {
     if (x) *x = cursor_x;
     if (y) *y = cursor_y;
 }
 
 /**
  * terminal_set_cursor_pos
  *
  * Moves the cursor to (x, y), clamping if necessary,
  * and updates the hardware cursor.
  */
 void terminal_set_cursor_pos(int x, int y)
 {
     if (x < 0) x = 0;
     if (y < 0) y = 0;
     if (x >= VGA_COLS) x = VGA_COLS - 1;
     if (y >= VGA_ROWS) y = VGA_ROWS - 1;
 
     cursor_x = x;
     cursor_y = y;
     update_hardware_cursor();
 }
 
 /**
  * terminal_write_char
  *
  * Writes a single character to the screen, interpreting:
  *   '\n' => newline (cursor_x=0, cursor_y++)
  *   '\r' => carriage return (cursor_x=0)
  *   '\b' => backspace (erase last char if possible)
  *   '\t' => tab (move 4 columns)
  */
 void terminal_write_char(char c)
 {
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
                 put_char_at(' ', cursor_x, cursor_y);
             } else if (cursor_y > 0) {
                 cursor_y--;
                 cursor_x = VGA_COLS - 1;
                 put_char_at(' ', cursor_x, cursor_y);
             }
             break;
         case '\t':
             cursor_x += 4;
             if (cursor_x >= VGA_COLS) {
                 cursor_x = 0;
                 cursor_y++;
             }
             break;
         default:
             // Normal char
             put_char_at(c, cursor_x, cursor_y);
             cursor_x++;
             // Wrap line if needed
             if (cursor_x >= VGA_COLS) {
                 cursor_x = 0;
                 cursor_y++;
             }
             break;
     }
 
     // If we scrolled past bottom, scroll
     if (cursor_y >= VGA_ROWS) {
         scroll_one_line();
         cursor_y = VGA_ROWS - 1;
     }
     update_hardware_cursor();
 }
 
 /**
  * terminal_write
  *
  * Writes a null-terminated string. Each char goes through terminal_write_char.
  */
 void terminal_write(const char* str)
 {
     for (int i = 0; str[i] != '\0'; i++) {
         terminal_write_char(str[i]);
     }
 }
 