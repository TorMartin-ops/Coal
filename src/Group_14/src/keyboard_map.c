/**
 * keyboard_map.c
 *
 * Implements a basic “Set 1” scancode-to-ASCII conversion 
 * ignoring Shift, Caps Lock, and other modifiers.
 * 
 * Press events for letters and numbers are mapped to ASCII. 
 * Release events (scancode >= 0x80) return 0 (ignored).
 */

 #include "keyboard_map.h"

 // Basic Set-1 scancode map for keys 0..0x7F
 // This array does not handle SHIFT, CAPS, or extended codes (e.g. 0xE0).
 static char _scancode_to_char[128] = {
   /* 0x00 */ 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
   /* 0x10 */ 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,   'a','s',
   /* 0x20 */ 'd','f','g','h','j','k','l',';','\'','`',   0, '\\','z','x','c','v',
   /* 0x30 */ 'b','n','m',',','.','/',   0,  '*', 0,  ' ', /* fill with 0 if unknown */
   /* 0x3A..0x7F omitted or set to 0 for now */
 };
 
 /**
  * scancode_to_ascii
  *
  * Returns the ASCII character for the given scancode, 
  * or 0 if the scancode is out of range or currently unhandled.
  */
 char scancode_to_ascii(uint8_t scancode) {
     // If scancode >= 0x80, it’s usually a key release in Set 1 => ignore
     if (scancode > 127) {
         return 0;
     }
 
     return _scancode_to_char[scancode];
 }
 