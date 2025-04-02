#pragma once
#ifndef KEYMAP_H
#define KEYMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

/**
 * @brief Enumeration of supported keyboard layouts.
 */
typedef enum {
    KEYMAP_US_QWERTY = 0,
    KEYMAP_UK_QWERTY,
    KEYMAP_DVORAK,
    KEYMAP_COLEMAK,
    KEYMAP_NORWEGIAN  // Added Norwegian keyboard layout.
} KeymapLayout;

/**
 * @brief Loads the specified keyboard layout into the keyboard driver.
 *
 * @param layout The keyboard layout to load.
 */
void keymap_load(KeymapLayout layout);

#ifdef __cplusplus
}
#endif

#endif // KEYMAP_H
