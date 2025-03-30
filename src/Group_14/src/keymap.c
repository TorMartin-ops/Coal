#include "keymap.h"
#include "keyboard.h"
#include "string.h"

/* 
 * Norwegian layout mapping for PS/2 Set 1 scancodes.
 * Note: Multi-character constants have been replaced with proper single-byte characters
 * where possible. For non-ASCII characters, you may consider using Unicode code points 
 * if your terminal supports them.
 */
const uint16_t keymap_norwegian[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '+',      // Key after 0 is often '+' on Norwegian keyboards.
    [0x0D] = '=',      // Using '=' here instead of a multi-character constant.
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = 0xC5,     // Å (Latin Capital Letter A with ring above) - using its ISO 8859-1 code 0xC5.
    [0x1B] = KEY_UNKNOWN,
    [0x1C] = '\n',
    [0x1D] = KEY_CTRL,
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = 0xF8,     // ø (Latin Small Letter O with stroke) - ISO code 0xF8.
    [0x28] = 0xE6,     // æ (Latin Small Letter AE) - ISO code 0xE6.
    [0x29] = KEY_UNKNOWN,
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '<',      // Often the key to left shift is '<' on Norwegian keyboards.
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '-',      // Remapped to '-' (instead of '/' in US layout)
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN,  // Typically keypad '*'
    [0x38] = KEY_ALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1,
    [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,
    [0x40] = KEY_F6,
    [0x41] = KEY_F7,
    [0x42] = KEY_F8,
    [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x45] = KEY_NUM,
    [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4A] = '-',      // Keypad '-'
    [0x4B] = KEY_LEFT,
    [0x4C] = KEY_UNKNOWN,  // Keypad '5'
    [0x4D] = KEY_RIGHT,
    [0x4E] = '+',      // Keypad '+'
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* 
 * US QWERTY layout mapping for PS/2 Set 1 scancodes.
 */
const uint16_t keymap_us_qwerty[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',          // Minus
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',
    [0x1D] = KEY_CTRL,
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN,    // Typically keypad '*'
    [0x38] = KEY_ALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1,
    [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,
    [0x40] = KEY_F6,
    [0x41] = KEY_F7,
    [0x42] = KEY_F8,
    [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x45] = KEY_NUM,
    [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4A] = '-',          // Keypad '-'
    [0x4B] = KEY_LEFT,
    [0x4C] = KEY_UNKNOWN,
    [0x4D] = KEY_RIGHT,
    [0x4E] = '+',          // Keypad '+'
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* 
 * UK QWERTY layout mapping.
 */
const uint16_t keymap_uk_qwerty[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',          // May be different
    [0x0D] = KEY_UNKNOWN,  // Often '£'
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',
    [0x1D] = KEY_CTRL,
    [0x1E] = 'a',
    [0x1F] = 's',
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '#',          // UK mapping for '#'
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN,
    [0x38] = KEY_ALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1,
    [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,
    [0x40] = KEY_F6,
    [0x41] = KEY_F7,
    [0x42] = KEY_F8,
    [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x45] = KEY_NUM,
    [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4A] = '-',          // Keypad '-'
    [0x4B] = KEY_LEFT,
    [0x4C] = KEY_UNKNOWN,
    [0x4D] = KEY_RIGHT,
    [0x4E] = '+',          // Keypad '+'
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* 
 * Dvorak layout mapping.
 */
const uint16_t keymap_dvorak[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = '\'',
    [0x11] = ',',
    [0x12] = '.',
    [0x13] = 'p',
    [0x14] = 'y',
    [0x15] = 'f',
    [0x16] = 'g',
    [0x17] = 'c',
    [0x18] = 'r',
    [0x19] = 'l',
    [0x1A] = '/',
    [0x1B] = '=',
    [0x1C] = '\n',
    [0x1D] = KEY_CTRL,
    [0x1E] = 'a',
    [0x1F] = 'o',
    [0x20] = 'e',
    [0x21] = 'u',
    [0x22] = 'i',
    [0x23] = 'd',
    [0x24] = 'h',
    [0x25] = 't',
    [0x26] = 'n',
    [0x27] = 's',
    [0x28] = '-',
    [0x29] = '`',
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\\',
    [0x2C] = ';',
    [0x2D] = 'q',
    [0x2E] = 'j',
    [0x2F] = 'k',
    [0x30] = 'x',
    [0x31] = 'b',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN,
    [0x38] = KEY_ALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1,
    [0x3C] = KEY_F2,
    [0x3D] = KEY_F3,
    [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,
    [0x40] = KEY_F6,
    [0x41] = KEY_F7,
    [0x42] = KEY_F8,
    [0x43] = KEY_F9,
    [0x44] = KEY_F10,
    [0x45] = KEY_NUM,
    [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4A] = '-', 
    [0x4B] = KEY_LEFT,
    [0x4C] = KEY_UNKNOWN,
    [0x4D] = KEY_RIGHT,
    [0x4E] = '+',
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* 
 * Colemak layout mapping.
 * Only select keys are rearranged relative to US QWERTY.
 */
const uint16_t keymap_colemak[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',
    [0x0F] = '\t',
    [0x10] = 'q',       // Q remains.
    [0x11] = 'w',       // W remains.
    [0x12] = 'f',       // E remaps to f.
    [0x13] = 'p',       // R remaps to p.
    [0x14] = 'g',       // T remaps to g.
    [0x15] = 'j',       // Y remaps to j.
    [0x16] = 'l',       // U remaps to l.
    [0x17] = 'u',       // I remaps to u.
    [0x18] = 'y',       // O remaps to y.
    [0x19] = ';',       // P remaps to ;
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',
    [0x1D] = KEY_CTRL,
    [0x1E] = 'a',       // A remains.
    [0x1F] = 'r',       // S remaps to r.
    [0x20] = 's',       // D remaps to s.
    [0x21] = 't',       // F remaps to t.
    [0x22] = 'd',       // G remaps to d.
    [0x23] = 'h',       // H remains.
    [0x24] = 'n',       // J remaps to n.
    [0x25] = 'e',       // K remaps to e.
    [0x26] = 'i',       // L remaps to i.
    [0x27] = 'o',       // ; remaps to o.
    [0x28] = '\'',
    [0x29] = '`',
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\\',
    [0x2C] = 'z',       // Z remains.
    [0x2D] = 'x',       // X remains.
    [0x2E] = 'c',       // C remains.
    [0x2F] = 'v',       // V remains.
    [0x30] = 'b',       // B remains.
    [0x31] = 'k',       // N remaps to k.
    [0x32] = 'm',       // M remains.
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN,
    [0x38] = KEY_ALT,
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B ... 0x7F] = KEY_UNKNOWN
};

/**
 * keymap_load - Loads the specified keyboard layout.
 *
 * This function selects one of the predefined keymaps and passes it to the keyboard driver.
 *
 * @param layout The keyboard layout to load.
 */
void keymap_load(KeymapLayout layout) {
    switch (layout) {
        case KEYMAP_US_QWERTY:
            keyboard_set_keymap(keymap_us_qwerty);
            break;
        case KEYMAP_UK_QWERTY:
            keyboard_set_keymap(keymap_uk_qwerty);
            break;
        case KEYMAP_DVORAK:
            keyboard_set_keymap(keymap_dvorak);
            break;
        case KEYMAP_COLEMAK:
            keyboard_set_keymap(keymap_colemak);
            break;
        case KEYMAP_NORWEGIAN:
            keyboard_set_keymap(keymap_norwegian);
            break;
        default:
            keyboard_set_keymap(keymap_us_qwerty);
            break;
    }
}
