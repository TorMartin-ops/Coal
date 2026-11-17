/**
 * @file keyboard.c
 * @brief PS/2 keyboard driver. The i8042 is left in translation mode, so the
 *        IRQ handler always sees scancode set 1.
 */
#include <kernel/drivers/input/keyboard.h>
#include <kernel/drivers/input/keyboard_hw.h>
#include <kernel/drivers/input/keymap.h>
#include <kernel/drivers/pic.h>
#include <kernel/cpu/idt.h>
#include <kernel/lib/port_io.h>

/* Bits of g_held: modifier keys currently down. */
#define HELD_LSHIFT 0x01
#define HELD_RSHIFT 0x02
#define HELD_LCTRL  0x04
#define HELD_RCTRL  0x08
#define HELD_ALT    0x10
#define HELD_ALTGR  0x20

static keyboard_handler_t g_handler;
static uint8_t g_held;
static uint8_t g_locks;      /* MOD_CAPS | MOD_NUM | MOD_SCROLL */
static uint8_t g_acks;       /* ACK bytes still to come for an LED update */
static bool    g_extended;   /* previous byte was 0xE0 */
static uint8_t g_skip;       /* bytes of an 0xE1 sequence (Pause) still to ignore */

/* ---- Controller access ----------------------------------------------- */

static void kbc_wait_write(void) {
    for (int i = 0; i < KBC_WAIT_TIMEOUT_CYCLES && (inb(KBC_STATUS_PORT) & KBC_SR_IBF); i++) io_wait();
}

static bool kbc_read(uint8_t *out) {
    for (int i = 0; i < KBC_WAIT_TIMEOUT_CYCLES; i++) {
        if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
            *out = inb(KBC_DATA_PORT);
            return true;
        }
        io_wait();
    }
    return false;
}

static void kbc_command(uint8_t cmd) {
    kbc_wait_write();
    outb(KBC_CMD_PORT, cmd);
}

static void kbc_write_data(uint8_t data) {
    kbc_wait_write();
    outb(KBC_DATA_PORT, data);
}

static void kbc_flush(void) {
    for (int i = 0; i < KBC_FLUSH_MAX_ATTEMPTS && (inb(KBC_STATUS_PORT) & KBC_SR_OBF); i++) {
        (void)inb(KBC_DATA_PORT);
        io_wait();
    }
}

/* Mirrors the lock keys on the keyboard LEDs. The keyboard answers each of the two bytes with an ACK, which decode() skips. */
static void update_leds(void) {
    uint8_t leds = (uint8_t)(((g_locks & MOD_SCROLL) ? KB_LED_SCROLL : 0) | ((g_locks & MOD_NUM) ? KB_LED_NUM : 0) |
                             ((g_locks & MOD_CAPS) ? KB_LED_CAPS : 0));

    g_acks = 2;
    kbc_write_data(KB_CMD_SET_LEDS);
    kbc_write_data(leds);
}

/* ---- Scancode decoding ----------------------------------------------- */

static uint8_t current_modifiers(void) {
    uint8_t m = g_locks;
    if (g_held & (HELD_LSHIFT | HELD_RSHIFT)) m |= MOD_SHIFT;
    if (g_held & (HELD_LCTRL | HELD_RCTRL))   m |= MOD_CTRL;
    if (g_held & HELD_ALT)                    m |= MOD_ALT;
    if (g_held & HELD_ALTGR)                  m |= MOD_ALTGR;
    return m;
}

static void set_held(uint8_t bit, bool down) {
    g_held = down ? (g_held | bit) : (g_held & (uint8_t)~bit);
}

/* Keypad keys 0x47..0x53: navigation when NumLock is off, digits when on. */
static const KeyCode kp_nav[13] = {
    KEY_HOME, KEY_UP, KEY_PAGE_UP, KEY_NONE, KEY_LEFT, KEY_NONE, KEY_RIGHT,
    KEY_NONE, KEY_END, KEY_DOWN, KEY_PAGE_DOWN, KEY_INSERT, KEY_DELETE,
};
static const char kp_digits[] = "789-456+1230.";

static KeyCode extended_key(uint8_t sc) {
    switch (sc) {
    case 0x48: return KEY_UP;     case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;   case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;   case 0x4F: return KEY_END;
    case 0x49: return KEY_PAGE_UP; case 0x51: return KEY_PAGE_DOWN;
    case 0x52: return KEY_INSERT; case 0x53: return KEY_DELETE;
    default:   return KEY_NONE;
    }
}

static void deliver(uint8_t sc, bool ext, uint8_t mods) {
    KeyEvent ev = { .key = KEY_NONE, .ch = 0, .modifiers = mods };

    if (ext) {
        ev.key = extended_key(sc);
        if (sc == 0x1C) ev.ch = '\r';  /* keypad Enter */
        if (sc == 0x35) ev.ch = '/';   /* keypad slash */
    } else if (sc >= 0x3B && sc <= 0x44) {
        ev.key = (KeyCode)(KEY_F1 + (sc - 0x3B));
    } else if (sc == 0x57 || sc == 0x58) {
        ev.key = (KeyCode)(KEY_F11 + (sc - 0x57));
    } else if (sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4E) {
        if (mods & MOD_NUM) ev.ch = (uint8_t)kp_digits[sc - 0x47];
        else                ev.key = kp_nav[sc - 0x47];
    } else {
        ev.ch = keymap_translate(sc, mods);
    }

    if ((ev.key != KEY_NONE || ev.ch) && g_handler) {
        g_handler(&ev);
    }
}

static void decode(uint8_t byte) {
    if (g_acks && byte == KB_RESP_ACK) { g_acks--; return; }
    if (g_skip) { g_skip--; return; }
    if (byte == SCANCODE_PAUSE_PREFIX)    { g_skip = 5; return; }  /* Pause: E1 1D 45 E1 9D C5 */
    if (byte == SCANCODE_EXTENDED_PREFIX) { g_extended = true; return; }

    bool ext = g_extended;
    bool down = !(byte & 0x80);
    uint8_t sc = byte & 0x7F;
    g_extended = false;

    if (ext && (sc == 0x2A || sc == 0x36)) return; /* fake shifts around Print Screen etc. */

    switch (sc) {
    case 0x2A: if (!ext) { set_held(HELD_LSHIFT, down); return; } break;
    case 0x36: if (!ext) { set_held(HELD_RSHIFT, down); return; } break;
    case 0x1D: set_held(ext ? HELD_RCTRL : HELD_LCTRL, down); return;
    case 0x38: set_held(ext ? HELD_ALTGR : HELD_ALT, down); return;
    case 0x3A: case 0x45: case 0x46:                    /* Caps, Num and Scroll Lock toggle on press */
        if (ext) break;
        if (down) {
            g_locks ^= sc == 0x3A ? MOD_CAPS : sc == 0x45 ? MOD_NUM : MOD_SCROLL;
            update_leds();
        }
        return;
    }
    if (down) deliver(sc, ext, current_modifiers());
}

static void keyboard_irq(isr_frame_t *frame) {
    (void)frame;
    pic_send_eoi(1);
    while (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        decode(inb(KBC_DATA_PORT));
    }
}

/* ---- Public API ------------------------------------------------------ */

void keyboard_set_handler(keyboard_handler_t handler) {
    g_handler = handler;
}

void keyboard_init(void) {
    keymap_load(KEYMAP_US_QWERTY);

    kbc_command(KBC_CMD_DISABLE_KB_IFACE);
    kbc_command(KBC_CMD_DISABLE_MOUSE_IFACE);
    kbc_flush();

    /* IRQ1 on, mouse IRQ off, keyboard clock enabled, translation to set 1 on. */
    uint8_t cfg = 0;
    kbc_command(KBC_CMD_READ_CONFIG);
    kbc_read(&cfg);
    cfg = (uint8_t)((cfg | KBC_CFG_INT_KB | KBC_CFG_TRANSLATION) &
                    ~(KBC_CFG_INT_MOUSE | KBC_CFG_DISABLE_KB));
    kbc_command(KBC_CMD_WRITE_CONFIG);
    kbc_write_data(cfg);

    kbc_command(KBC_CMD_ENABLE_KB_IFACE);

    /* Reset the keyboard (ACK, then self-test result) and start scanning. */
    uint8_t resp;
    kbc_write_data(KB_CMD_RESET);
    if (kbc_read(&resp) && resp == KB_RESP_ACK) {
        kbc_read(&resp);
    }
    kbc_flush();
    kbc_write_data(KB_CMD_ENABLE_SCAN);
    kbc_read(&resp);
    kbc_flush();
    update_leds();                                      /* all off until a lock key is pressed */

    register_int_handler(IRQ1_VECTOR, keyboard_irq, NULL);
}
