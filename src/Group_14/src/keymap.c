#include "keymap.h"
#include "keyboard.h"
#include "string.h" // For memcpy hvis du skulle bruke det, men ikke nå

/* * Norwegian layout mapping for PS/2 Set 1 scancodes.
 * Multi-character constants are replaced with their actual character or a KEY_UNKNOWN.
 * For special Norwegian characters (Æ, Ø, Å), their ISO-8859-1 (Latin-1) values are used
 * if the terminal is expected to render them directly. Otherwise, use KEY_UNKNOWN or custom codes.
 */
const uint16_t keymap_norwegian[128] = {
    // Rad 0
    [0x00] = KEY_UNKNOWN,         // Null
    [0x01] = KEY_ESC,             // Escape
    [0x02] = '1',                 // 1
    [0x03] = '2',                 // 2
    [0x04] = '3',                 // 3
    [0x05] = '4',                 // 4
    [0x06] = '5',                 // 5
    [0x07] = '6',                 // 6
    [0x08] = '7',                 // 7
    [0x09] = '8',                 // 8
    [0x0A] = '9',                 // 9
    [0x0B] = '0',                 // 0
    [0x0C] = '+',                 // + (og ? med Shift)
    [0x0D] = '\\',                // \ (og | med Shift, ´ med AltGr) - ofte likhetstegn på US, men \ på NO
    [0x0E] = '\b',                // Backspace
    [0x0F] = '\t',                // Tab
    // Rad 1 (QWERTY-rad)
    [0x10] = 'q',                 // Q
    [0x11] = 'w',                 // W
    [0x12] = 'e',                 // E
    [0x13] = 'r',                 // R
    [0x14] = 't',                 // T
    [0x15] = 'y',                 // Y
    [0x16] = 'u',                 // U
    [0x17] = 'i',                 // I
    [0x18] = 'o',                 // O
    [0x19] = 'p',                 // P
    [0x1A] = 0xE5,                // å (ISO-8859-1 for liten å)
    [0x1B] = KEY_UNKNOWN,         // ¨ (død-tast, ^ med shift) - vanskelig å representere som én KeyCode
    [0x1C] = '\n',                // Enter
    // Rad 2 (ASDFGH-rad)
    [0x1D] = KEY_CTRL,            // Left Ctrl
    [0x1E] = 'a',                 // A
    [0x1F] = 's',                 // S
    [0x20] = 'd',                 // D
    [0x21] = 'f',                 // F
    [0x22] = 'g',                 // G
    [0x23] = 'h',                 // H
    [0x24] = 'j',                 // J
    [0x25] = 'k',                 // K
    [0x26] = 'l',                 // L
    [0x27] = 0xF8,                // ø (ISO-8859-1 for liten ø)
    [0x28] = 0xE6,                // æ (ISO-8859-1 for liten æ)
    [0x29] = KEY_TILDE,           // § (og ½ med Shift) - ofte `~ på US
    [0x2A] = KEY_LEFT_SHIFT,      // Left Shift
    // Rad 3 (ZXCVBN-rad)
    [0x2B] = '\'',                // ' (og * med Shift) - ofte \ på US, men ' på NO
    [0x2C] = 'z',                 // Z
    [0x2D] = 'x',                 // X
    [0x2E] = 'c',                 // C
    [0x2F] = 'v',                 // V
    [0x30] = 'b',                 // B
    [0x31] = 'n',                 // N
    [0x32] = 'm',                 // M
    [0x33] = ',',                 // , (og ; med Shift)
    [0x34] = '.',                 // . (og : med Shift)
    [0x35] = '-',                 // - (og _ med Shift)
    [0x36] = KEY_RIGHT_SHIFT,     // Right Shift
    // Andre
    [0x37] = KEY_UNKNOWN,         // Keypad * (eller Print Screen hvis E0 prefix)
    [0x38] = KEY_ALT,             // Left Alt
    [0x39] = ' ',                 // Space
    [0x3A] = KEY_CAPS,            // Caps Lock
    // Funksjonstaster
    [0x3B] = KEY_F1,  [0x3C] = KEY_F2,  [0x3D] = KEY_F3,  [0x3E] = KEY_F4,
    [0x3F] = KEY_F5,  [0x40] = KEY_F6,  [0x41] = KEY_F7,  [0x42] = KEY_F8,
    [0x43] = KEY_F9,  [0x44] = KEY_F10, // F11 (0x57), F12 (0x58) er ikke her
    // Kontrolltaster
    [0x45] = KEY_NUM,             // Num Lock
    [0x46] = KEY_SCROLL,          // Scroll Lock
    // Numpad (antar NumLock er på for tall)
    [0x47] = KEY_HOME,            // Numpad 7 / Home
    [0x48] = KEY_UP,              // Numpad 8 / Up Arrow
    [0x49] = KEY_PAGE_UP,         // Numpad 9 / Page Up
    [0x4A] = '-',                 // Numpad -
    [0x4B] = KEY_LEFT,            // Numpad 4 / Left Arrow
    [0x4C] = KEY_UNKNOWN,         // Numpad 5 (representerer ofte '5' eller ingenting spesielt som KeyCode)
    [0x4D] = KEY_RIGHT,           // Numpad 6 / Right Arrow
    [0x4E] = '+',                 // Numpad +
    [0x4F] = KEY_END,             // Numpad 1 / End
    [0x50] = KEY_DOWN,            // Numpad 2 / Down Arrow
    [0x51] = KEY_PAGE_DOWN,       // Numpad 3 / Page Down
    [0x52] = KEY_INSERT,          // Numpad 0 / Insert
    [0x53] = KEY_DELETE,          // Numpad . / Delete
    // Resterende til 0x7F er ukjent eller E0-prefixed
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* * US QWERTY layout mapping for PS/2 Set 1 scancodes.
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
    [0x0C] = '-',
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
    [0x29] = '`', // Grave accent / Tilde
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
    [0x37] = KEY_UNKNOWN, // Keypad * or PrintScreen (often E0 prefixed)
    [0x38] = KEY_ALT,   // Left Alt
    [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM,    // Num Lock
    [0x46] = KEY_SCROLL, // Scroll Lock
    // Numpad keys (US assumes NumLock ON for numbers, OFF for control)
    [0x47] = KEY_HOME,       // Numpad 7 / Home
    [0x48] = KEY_UP,         // Numpad 8 / Up Arrow
    [0x49] = KEY_PAGE_UP,    // Numpad 9 / Page Up
    [0x4A] = '-',            // Numpad -
    [0x4B] = KEY_LEFT,       // Numpad 4 / Left Arrow
    [0x4C] = KEY_UNKNOWN,    // Numpad 5 (no special KeyCode, usually '5')
    [0x4D] = KEY_RIGHT,      // Numpad 6 / Right Arrow
    [0x4E] = '+',            // Numpad +
    [0x4F] = KEY_END,        // Numpad 1 / End
    [0x50] = KEY_DOWN,       // Numpad 2 / Down Arrow
    [0x51] = KEY_PAGE_DOWN,  // Numpad 3 / Page Down
    [0x52] = KEY_INSERT,     // Numpad 0 / Insert
    [0x53] = KEY_DELETE,     // Numpad . / Delete
    // F11 (0x57), F12 (0x58) - Add if needed
    [0x57] = KEY_UNKNOWN, // F11
    [0x58] = KEY_UNKNOWN, // F12
    // Unassigned scancodes map to KEY_UNKNOWN
    [0x54 ... 0x56] = KEY_UNKNOWN,
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

/* * UK QWERTY layout mapping.
 */
const uint16_t keymap_uk_qwerty[128] = {
    // Ligner veldig på US, med noen viktige unntak
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', // Grave, ofte £ med Shift på UK
    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = '#', // UK '#' er her (US er '\') - Shift gir '~'
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    // Scancode 0x56 for UK er ofte \ og |
    [0x56] = '\\',
    [0x54 ... 0x55] = KEY_UNKNOWN,
    [0x57 ... 0x7F] = KEY_UNKNOWN
};

/* * Dvorak layout mapping.
 */
const uint16_t keymap_dvorak[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '[', [0x0D] = ']', // Var - og =
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = '\'', [0x11] = ',', [0x12] = '.', [0x13] = 'p',
    [0x14] = 'y', [0x15] = 'f', [0x16] = 'g', [0x17] = 'c',
    [0x18] = 'r', [0x19] = 'l', [0x1A] = '/', [0x1B] = '=', // Var [ og ]
    [0x1C] = '\n', [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 'o',
    [0x20] = 'e', [0x21] = 'u', [0x22] = 'i', [0x23] = 'd',
    [0x24] = 'h', [0x25] = 't', [0x26] = 'n', [0x27] = 's',
    [0x28] = '-', [0x29] = '`', // Var ; og '
    [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = ';', [0x2D] = 'q', [0x2E] = 'j', [0x2F] = 'k', // Var z, x, c, v
    [0x30] = 'x', [0x31] = 'b', [0x32] = 'm', [0x33] = 'w', // Var b, n, m, ,
    [0x34] = 'v', [0x35] = 'z', // Var . og /
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ',
    [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/* * Colemak layout mapping.
 */
const uint16_t keymap_colemak[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2',
    [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'f', [0x13] = 'p', // E->F, R->P
    [0x14] = 'g', [0x15] = 'j', [0x16] = 'l', [0x17] = 'u', // T->G, Y->J, U->L, I->U
    [0x18] = 'y', [0x19] = ';', // O->Y, P->;
    [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 'r', // S->R
    [0x20] = 's', [0x21] = 't', [0x22] = 'd', // D->S, F->T, G->D
    [0x23] = 'h', [0x24] = 'n', [0x25] = 'e', // J->N, K->E
    [0x26] = 'i', [0x27] = 'o', // L->I, ; -> O
    [0x28] = '\'', [0x29] = '`',
    [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'k', [0x32] = 'm', // N->K
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, [0x38] = KEY_ALT, [0x39] = ' ',
    [0x3A] = KEY_CAPS, // CapsLock er ofte Backspace i Colemak, men KEY_CAPS beholdes her
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP, [0x4A] = '-',
    [0x4B] = KEY_LEFT, [0x4C] = KEY_UNKNOWN, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x54 ... 0x7F] = KEY_UNKNOWN
};

/**
 * keymap_load - Loads the specified keyboard layout.
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
            // Fallback to a default layout if an unknown layout is specified
            keyboard_set_keymap(keymap_us_qwerty); // Default to US QWERTY
            break;
    }
}