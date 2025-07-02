#include <kernel/drivers/input/keymap.h>
#include <kernel/drivers/input/keyboard.h> // For KEY_... definitions and keyboard_set_keymap
#include <kernel/lib/string.h>   // For memcpy (though keyboard_set_keymap is in keyboard.c)
#include <libc/stdint.h>   // For uint16_t

// Norwegian layout
const uint16_t keymap_norwegian[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '+', [0x0D] = '\\', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = 0xE5, /* å */ [0x1B] = KEY_UNKNOWN, /* ¨ */
    [0x1C] = KEY_ENTER, [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = 0xF8, /* ø */
    [0x28] = 0xE6, /* æ */ [0x29] = KEY_TILDE, /* § or ` */ [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '\'', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c',
    [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '-', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, /* * on numpad or E0 printscreen */ [0x38] = KEY_ALT, [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,  [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,  [0x40] = KEY_F6,  [0x41] = KEY_F7,  [0x42] = KEY_F8,
    [0x43] = KEY_F9,  [0x44] = KEY_F10,
    [0x57] = KEY_UNKNOWN, /* F11 */ [0x58] = KEY_UNKNOWN, /* F12 */
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME,      [0x48] = KEY_UP,        [0x49] = KEY_PAGE_UP,   [0x4A] = '-', /* Numpad - */
    [0x4B] = KEY_LEFT,      [0x4C] = KEY_UNKNOWN,   [0x4D] = KEY_RIGHT,     [0x4E] = '+', /* Numpad + */
    [0x4F] = KEY_END,       [0x50] = KEY_DOWN,      [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,    [0x53] = KEY_DELETE,    /* Numpad . */
    [0x54 ... 0x56] = KEY_UNKNOWN, /* Includes 0x56 which is sometimes < > | on NO layouts with AltGr */
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

// US QWERTY layout
const uint16_t keymap_us_qwerty[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ', [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x57] = KEY_UNKNOWN, /* F11 */ [0x58] = KEY_UNKNOWN, /* F12 */
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, /* Numpad 5 */ [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x56] = KEY_UNKNOWN,
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

// UK QWERTY layout
const uint16_t keymap_uk_qwerty[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2', /* Shift: " */
    [0x04] = '3', /* Shift: £ */ [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', /* Shift: @ */ [0x29] = '`', /* Shift: ¬ */
    [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '#', /* Shift: ~ */
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ', [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x57] = KEY_UNKNOWN, /* F11 */ [0x58] = KEY_UNKNOWN, /* F12 */
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x56] = '\\', /* Shift: | */ // Additional key on UK layouts near left shift
    [0x54 ... 0x55] = KEY_UNKNOWN,
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

// Dvorak layout
const uint16_t keymap_dvorak[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '[', [0x0D] = ']', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = '\'', [0x11] = ',', [0x12] = '.', [0x13] = 'p',
    [0x14] = 'y', [0x15] = 'f', [0x16] = 'g', [0x17] = 'c',
    [0x18] = 'r', [0x19] = 'l', [0x1A] = '/', [0x1B] = '=',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 'o',
    [0x20] = 'e', [0x21] = 'u', [0x22] = 'i', [0x23] = 'd',
    [0x24] = 'h', [0x25] = 't', [0x26] = 'n', [0x27] = 's',
    [0x28] = '-', [0x29] = '`', [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = ';', [0x2D] = 'q', [0x2E] = 'j', [0x2F] = 'k',
    [0x30] = 'x', [0x31] = 'b', [0x32] = 'm', [0x33] = 'w',
    [0x34] = 'v', [0x35] = 'z', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ', [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x57] = KEY_UNKNOWN, /* F11 */ [0x58] = KEY_UNKNOWN, /* F12 */
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x56] = KEY_UNKNOWN,
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

// Colemak layout
const uint16_t keymap_colemak[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'f', [0x13] = 'p',
    [0x14] = 'g', [0x15] = 'j', [0x16] = 'l', [0x17] = 'u',
    [0x18] = 'y', [0x19] = ';', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 'r',
    [0x20] = 's', [0x21] = 't', [0x22] = 'd', [0x23] = 'h',
    [0x24] = 'n', [0x25] = 'e', [0x26] = 'i', [0x27] = 'o',
    [0x28] = '\'', [0x29] = '`', [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'k', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ', [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x57] = KEY_UNKNOWN, /* F11 */ [0x58] = KEY_UNKNOWN, /* F12 */
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x56] = KEY_UNKNOWN,
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

void keymap_load(KeymapLayout layout) {
    const uint16_t* selected_keymap = NULL;
    switch (layout) {
        case KEYMAP_US_QWERTY:  selected_keymap = keymap_us_qwerty; break;
        case KEYMAP_UK_QWERTY:  selected_keymap = keymap_uk_qwerty; break;
        case KEYMAP_DVORAK:     selected_keymap = keymap_dvorak;    break;
        case KEYMAP_COLEMAK:    selected_keymap = keymap_colemak;   break;
        case KEYMAP_NORWEGIAN:  selected_keymap = keymap_norwegian; break;
        default:                selected_keymap = keymap_us_qwerty; break;
    }
    if (selected_keymap) {
        keyboard_set_keymap(selected_keymap);
    }
}