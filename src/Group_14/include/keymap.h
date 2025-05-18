#ifndef KEYMAP_H
#define KEYMAP_H

#ifdef __cplusplus
extern "C" {
#endif

// *** THIS IS THE CRUCIAL FIX for "stdint.h: No such file or directory" ***
// It MUST point to your project's stdint.h because of -nostdinc compiler flag
#include <libc/stdint.h> // For uint16_t

typedef enum {
    KEYMAP_US_QWERTY = 0,
    KEYMAP_UK_QWERTY,
    KEYMAP_DVORAK,
    KEYMAP_COLEMAK,
    KEYMAP_NORWEGIAN
} KeymapLayout;

void keymap_load(KeymapLayout layout);

extern const uint16_t keymap_us_qwerty[128];
extern const uint16_t keymap_uk_qwerty[128];
extern const uint16_t keymap_dvorak[128];
extern const uint16_t keymap_colemak[128];
extern const uint16_t keymap_norwegian[128];

#ifdef __cplusplus
}
#endif

#endif // KEYMAP_H