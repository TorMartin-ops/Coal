#ifndef KEYBOARD_H
#define KEYBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stddef.h>

typedef enum {
    KEY_UNKNOWN = 0x80, 
    KEY_ESC,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_TILDE,      
    KEY_BACKSPACE,  
    KEY_TAB,        
    KEY_ENTER,      
    KEY_CTRL,       
    KEY_LEFT_SHIFT,
    KEY_RIGHT_SHIFT,
    KEY_ALT,        
    KEY_CAPS,
    KEY_NUM,
    KEY_SCROLL,
    KEY_HOME, KEY_UP, KEY_PAGE_UP, KEY_LEFT,
    KEY_RIGHT, KEY_END, KEY_DOWN, KEY_PAGE_DOWN,
    KEY_INSERT, KEY_DELETE,
    KEY_COUNT      
} KeyCode;

typedef enum {
    MOD_NONE   = 0,
    MOD_SHIFT  = 1 << 0,
    MOD_CTRL   = 1 << 1,
    MOD_ALT    = 1 << 2, 
    MOD_CAPS   = 1 << 3,
    MOD_NUM    = 1 << 4,
    MOD_SCROLL = 1 << 5,
    MOD_ALT_GR = 1 << 6, 
    MOD_SUPER  = 1 << 7  
} KeyModifier;

typedef enum {
    KEY_PRESS,     
    KEY_RELEASE,   
    KEY_REPEAT     
} KeyAction;

typedef struct {
    KeyCode     code;     
    KeyAction   action;   
    uint8_t     modifiers; 
    uint32_t    timestamp; 
} KeyEvent;

void      keyboard_init(void);
bool      keyboard_poll_event(KeyEvent *ev);
bool      keyboard_is_key_down(KeyCode key);
uint8_t   keyboard_get_modifiers(void);
void      keyboard_set_leds(bool scroll, bool num, bool caps);
void      keyboard_set_keymap(const uint16_t *km);
void      keyboard_set_repeat_rate(uint8_t delay, uint8_t speed);
void      keyboard_register_callback(void (*cb)(KeyEvent));
char      apply_modifiers_extended(char c, uint8_t mods);
void      keyboard_recheck_kbc_config(void);

extern volatile uint32_t g_keyboard_irq_fire_count;
#ifdef __cplusplus
}
#endif

#endif // KEYBOARD_H