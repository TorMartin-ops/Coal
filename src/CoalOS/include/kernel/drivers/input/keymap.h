/**
 * @file keymap.h
 * @brief Keyboard layouts: scancode set 1 -> Latin-1 characters.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include <kernel/core/types.h>

typedef enum {
    KEYMAP_US_QWERTY = 0,
    KEYMAP_NORWEGIAN,
} KeymapLayout;

/** Selects the active layout. Safe to call at any time. */
void keymap_load(KeymapLayout layout);

/**
 * Translates a set-1 make code (0x01..0x58, without the 0x80 break bit) to a
 * Latin-1 character, honouring Shift, AltGr and Caps Lock from `mods`.
 * Returns 0 for keys that produce no character.
 */
uint8_t keymap_translate(uint8_t scancode, uint8_t mods);

#endif /* KEYMAP_H */
