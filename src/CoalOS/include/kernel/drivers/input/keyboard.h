/**
 * @file keyboard.h
 * @brief PS/2 keyboard driver. Decodes scancodes in the IRQ handler and hands
 *        press events to a single registered handler.
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <kernel/core/types.h>

/** Keys that have no character of their own (characters arrive in KeyEvent.ch). */
typedef enum {
    KEY_NONE = 0,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT,
    KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_INSERT, KEY_DELETE,
} KeyCode;

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_ALTGR 0x08
#define MOD_CAPS  0x10
#define MOD_NUM   0x20
#define MOD_SCROLL 0x40

typedef struct {
    KeyCode key;       /* non-character key, or KEY_NONE */
    uint8_t ch;        /* Latin-1 character with modifiers applied (Ctrl gives control codes), or 0 */
    uint8_t modifiers; /* MOD_* state at the time of the press */
} KeyEvent;

typedef void (*keyboard_handler_t)(const KeyEvent *ev);

/** Initialises the controller and keyboard and installs the IRQ1 handler. */
void keyboard_init(void);

/** Registers the receiver of key presses; called from interrupt context. */
void keyboard_set_handler(keyboard_handler_t handler);

#endif /* KEYBOARD_H */
