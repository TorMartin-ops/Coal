/**
 * keyboard_map.h
 *
 * Provides a function to convert PS/2 “Set 1” scancodes into ASCII 
 * without handling Shift, Caps Lock, or other modifiers. 
 *
 * For advanced usage, you can add SHIFT, Caps Lock, or other logic 
 * by expanding the lookup table or storing state about key presses.
 */

 #ifndef KEYBOARD_MAP_H
 #define KEYBOARD_MAP_H
 
 #include <libc/stdint.h>
 
 /**
  * scancode_to_ascii
  *
  * Converts a single “Set 1” scancode (0..127) into a corresponding ASCII char. 
  * If the scancode is out of range or corresponds to a special/release code, returns 0.
  *
  * @param scancode  The PS/2 Set 1 scancode (usually < 128 on press).
  * @return          The corresponding ASCII character, or 0 if none.
  */
 char scancode_to_ascii(uint8_t scancode);
 
 #endif // KEYBOARD_MAP_H
 