/**
 * terminal.c
 * VGA text-mode driver for a 32-bit x86 OS.
 */

 #include "terminal.h"
 #include <libc/stdint.h>
 #include <libc/stddef.h>
 #include "port_io.h"  // for outb, inb
 
 // VGA constants
 #define VGA_ADDRESS 0xB8000
 #define VGA_COLS    80
 #define VGA_ROWS    25
 
 static uint8_t terminal_color = 0x07; // Light gray on black
 static uint16_t* vga_buffer = (uint16_t*)VGA_ADDRESS;
 static int cursor_x = 0;
 static int cursor_y = 0;
 
 static void update_hardware_cursor(void) {
     uint16_t pos = (cursor_y * VGA_COLS) + cursor_x;
     outb(0x3D4, 14);                // High byte
     outb(0x3D5, (pos >> 8) & 0xFF);
     outb(0x3D4, 15);                // Low byte
     outb(0x3D5, pos & 0xFF);
 }
 
 static void enable_hardware_cursor(void) {
     // Set cursor shape (block cursor: scanlines 14-15)
     outb(0x3D4, 0x0A);
     outb(0x3D5, (inb(0x3D5) & 0xC0) | 14);
     outb(0x3D4, 0x0B);
     outb(0x3D5, (inb(0x3D5) & 0xE0) | 15);
 }
 
 static void put_char_at(char c, int x, int y) {
     int index = y * VGA_COLS + x;
     vga_buffer[index] = (uint16_t)(c | (terminal_color << 8));
 }
 
 void terminal_clear(void) {
     for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
         vga_buffer[i] = (uint16_t)(' ' | (terminal_color << 8));
     }
     cursor_x = 0;
     cursor_y = 0;
     update_hardware_cursor();
 }
 
 void terminal_clear_row(int row) {
     if (row < 0 || row >= VGA_ROWS)
         return;
     int start = row * VGA_COLS;
     for (int col = 0; col < VGA_COLS; col++) {
         vga_buffer[start + col] = (uint16_t)(' ' | (terminal_color << 8));
     }
 }
 
 static void scroll_one_line(void) {
     for (int row = 0; row < VGA_ROWS - 1; row++) {
         for (int col = 0; col < VGA_COLS; col++) {
             vga_buffer[row * VGA_COLS + col] =
                 vga_buffer[(row + 1) * VGA_COLS + col];
         }
     }
     terminal_clear_row(VGA_ROWS - 1);
 }
 
 void terminal_init(void) {
     terminal_color = 0x07; // default color
     terminal_clear();
     enable_hardware_cursor();
     update_hardware_cursor();
 }
 
 void terminal_set_color(uint8_t color) {
     terminal_color = color;
 }
 
 void terminal_set_foreground(uint8_t fg) {
     terminal_color = (terminal_color & 0xF0) | (fg & 0x0F);
 }
 
 void terminal_set_background(uint8_t bg) {
     terminal_color = ((bg & 0x0F) << 4) | (terminal_color & 0x0F);
 }
 
 void terminal_get_cursor_pos(int* x, int* y) {
     if (x) *x = cursor_x;
     if (y) *y = cursor_y;
 }
 
 void terminal_set_cursor_pos(int x, int y) {
     if (x < 0) x = 0;
     if (y < 0) y = 0;
     if (x >= VGA_COLS) x = VGA_COLS - 1;
     if (y >= VGA_ROWS) y = VGA_ROWS - 1;
     cursor_x = x;
     cursor_y = y;
     update_hardware_cursor();
 }
 
 void terminal_write_char(char c) {
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
             put_char_at(c, cursor_x, cursor_y);
             cursor_x++;
             if (cursor_x >= VGA_COLS) {
                 cursor_x = 0;
                 cursor_y++;
             }
             break;
     }
     if (cursor_y >= VGA_ROWS) {
         scroll_one_line();
         cursor_y = VGA_ROWS - 1;
     }
     update_hardware_cursor();
 }
 
 void terminal_write(const char* str) {
     for (int i = 0; str[i] != '\0'; i++) {
         terminal_write_char(str[i]);
     }
 }
 