/**
 * keyboard.c
 *
 * A robust PS/2 keyboard driver for x86 (32-bit).
 *  - Binds to IRQ1 (vector 33) after PIC remap
 *  - Reads scancodes from port 0x60
 *  - Converts scancode => ASCII using keyboard_map.h
 *  - Prints characters via terminal_write_char
 * 
 * If you wish to handle special keys (Shift, Caps Lock, etc.),
 * expand keyboard_map or add logic in keyboard_irq_handler.
 */

 #include <stddef.h>         // for NULL
 #include <libc/stdint.h>    // for uint8_t
 #include "keyboard.h"
 #include "idt.h"            // register_int_handler
 #include "terminal.h"       // terminal_write_char (for printing)
 #include "port_io.h"        // inb for reading port 0x60
 #include "keyboard_map.h"   // scancode_to_ascii
 
 /**
  * keyboard_irq_handler
  *
  * The callback for IRQ1 (vector 33). 
  * Reads a scancode from 0x60, converts it to ASCII, and prints if valid.
  */
 static void keyboard_irq_handler(void* data)
 {
     (void)data;  // unused in this simple example
 
     // 1) Read the scancode from port 0x60
     uint8_t scancode = inb(0x60);
 
     // 2) Convert scancode to ASCII
     char c = scancode_to_ascii(scancode);
 
     // 3) If valid, print the character
     if (c != 0) {
         terminal_write_char(c);
     }
 
     // If you want to handle SHIFT, CAPS, arrow keys, etc.,
     // expand logic here or in keyboard_map.
 }
 
 /**
  * init_keyboard
  *
  * Installs the keyboard_irq_handler on IRQ1, which is vector 33
  * after the PIC is remapped (IRQ0 => 32, IRQ1 => 33, etc.).
  */
 void init_keyboard(void)
 {
     register_int_handler(33, keyboard_irq_handler, NULL);
 }
 