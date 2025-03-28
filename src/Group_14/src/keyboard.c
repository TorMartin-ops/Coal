/**
 * keyboard.c
 * PS/2 keyboard driver for x86 (32-bit).
 */

 #include <stddef.h>
 #include <libc/stdint.h>
 #include "keyboard.h"
 #include "idt.h"
 #include "terminal.h"
 #include "port_io.h"
 #include "keyboard_map.h"
 
 // IRQ1 handler: reads a scancode, converts it to ASCII, and prints it.
 static void keyboard_irq_handler(void* data) {
     (void)data;  // Unused
     uint8_t scancode = inb(0x60);
     char c = scancode_to_ascii(scancode);
     if (c != 0) {
         terminal_write_char(c);
     }
 }
 
 // Installs the IRQ1 handler on vector 33.
 void init_keyboard(void) {
     register_int_handler(33, keyboard_irq_handler, NULL);
 }
 