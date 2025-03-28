/**
 * keyboard_map.c
 * Basic Set-1 scancode-to-ASCII conversion.
 */

 #include "keyboard_map.h"

 // Basic Set-1 scancode map for keys 0..0x7F.
 // Release events (scancode >= 0x80) are ignored.
 static char _scancode_to_char[128] = {
     /* 0x00 */ 0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
     /* 0x10 */ 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,   'a','s',
     /* 0x20 */ 'd','f','g','h','j','k','l',';','\'','`',   0, '\\','z','x','c','v',
     /* 0x30 */ 'b','n','m',',','.','/',   0,  '*', 0,  ' ',
     // 0x3A..0x7F are omitted or set to 0.
 };
 
 /**
  * scancode_to_ascii
  * Converts a scancode to its corresponding ASCII character.
  */
 char scancode_to_ascii(uint8_t scancode) {
     if (scancode > 127)
         return 0;
     return _scancode_to_char[scancode];
 }
 