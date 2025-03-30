#ifndef KEYMAP_H
#define KEYMAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "keyboard.h"

/* External keymap arrays */
extern const uint16_t keymap_us_qwerty[128];
extern const uint16_t keymap_uk_qwerty[128];
extern const uint16_t keymap_dvorak[128];
extern const uint16_t keymap_colemak[128];

/**
 * @brief Enumeration of supported keyboard layout types.
 *
 * This enumeration defines the available keyboard layouts that can be loaded
 * by the keymap module.
 */
typedef enum {
    KEYMAP_US_QWERTY, /**< Standard US QWERTY layout */
    KEYMAP_UK_QWERTY, /**< UK QWERTY layout */
    KEYMAP_DVORAK,    /**< Dvorak layout */
    KEYMAP_COLEMAK    /**< Colemak layout */
} KeymapLayout;

/**
 * @brief Loads the specified keyboard layout.
 *
 * This function selects one of the predefined keymaps and updates the active
 * keymap in the keyboard driver accordingly.
 *
 * @param layout The keyboard layout to load.
 */
void keymap_load(KeymapLayout layout);

#ifdef __cplusplus
}
#endif

#endif // KEYMAP_H
