/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for UiAOS
 * @version 4.8 (Fix build errors for KEY_F11/F12/ENTER)
 *
 * @details Implements a driver for a standard PS/2 keyboard using Scan Code Set 1.
 * Handles reading scancodes via IRQ 1, processing extended codes, tracking
 * modifier states (Shift, Ctrl, Alt, Caps Lock), buffering key events,
 * translating scancodes to KeyCodes via a keymap, and providing an interface
 * for polling events or registering a callback. Includes initialization logic
 * that now explicitly enables the keyboard controller.
 */

//============================================================================
// Includes
//============================================================================

#include "types.h"          // Core type definitions
#include "keyboard.h"       // Public header for this driver (MUST define KEY_*, MOD_*, KeyCode, KeyEvent)
#include "idt.h"            // For interrupt handler registration (register_int_handler)
#include <isr_frame.h>      // Definition of isr_frame_t
#include "terminal.h"       // For default callback output (terminal_*)
#include "port_io.h"        // For inb/outb functions
#include "pit.h"            // For get_pit_ticks() timestamping
#include "string.h"         // For memset and memcpy
#include "serial.h"         // For serial port debugging output
#include "spinlock.h"       // For spinlocks and interrupt control functions
#include "assert.h"         // KERNEL_ASSERT

//============================================================================
// Definitions and Constants
//============================================================================

/* I/O port definitions for the 8042 PS/2 Controller */
#define KBC_DATA_PORT    0x60  // Keyboard Controller Data Port (Read/Write)
#define KBC_STATUS_PORT  0x64  // Keyboard Controller Status Register (Read)
#define KBC_CMD_PORT     0x64  // Keyboard Controller Command Register (Write)

/* KBC Status Register Bits */
#define KBC_SR_OUT_BUF   0x01  // Bit 0: Output buffer full (data available to read from 0x60)
#define KBC_SR_IN_BUF    0x02  // Bit 1: Input buffer full (controller busy, don't write to 0x60/0x64)

/* Keyboard Commands (sent to Data Port 0x60) */
#define KBC_CMD_SET_LEDS    0xED // Set keyboard LEDs (Caps, Num, Scroll Lock)
#define KBC_CMD_ENABLE_SCAN 0xF4 // Enable keyboard scanning (start sending codes)
#define KBC_CMD_SET_TYPEMATIC 0xF3 // Set typematic rate/delay command

/* Keyboard Responses (received from Data Port 0x60) */
#define KBC_RESP_ACK        0xFA // Command acknowledged (ACK)
#define KBC_RESP_RESEND     0xFE // Command error, request resend

/* Scancode Prefixes */
#define SCANCODE_EXTENDED_PREFIX 0xE0
#define SCANCODE_PAUSE_PREFIX   0xE1

/* Buffer size for circular event buffer */
#define KB_BUFFER_SIZE 256

/* Timeout loops for KBC interaction */
#define KBC_WAIT_TIMEOUT 100000 // Simple loop count for timeout

//============================================================================
// Module Static Data
//============================================================================

/** @brief Internal structure holding the keyboard driver's state. */
static struct {
    bool        key_states[KEY_COUNT];      // Tracks key presses. KEY_COUNT from keyboard.h
    uint8_t     modifiers;                  // Active modifiers. MOD_* from keyboard.h
    KeyEvent    buffer[KB_BUFFER_SIZE];     // Event buffer. KeyEvent from keyboard.h
    uint8_t     buf_head;                   // Write index
    uint8_t     buf_tail;                   // Read index
    spinlock_t  buffer_lock;                // Lock for polling buffer access
    uint16_t    current_keymap[128];        // Active keymap. KeyCode from keyboard.h
    bool        extended_code_active;       // E0 prefix flag
    void        (*event_callback)(KeyEvent); // Callback function pointer
} keyboard;

//============================================================================
// Forward Declarations
//============================================================================

static void keyboard_irq1_handler(isr_frame_t *frame);
static void default_event_callback(KeyEvent event);
static inline void kbc_wait_for_send_ready(void);
static inline void kbc_wait_for_recv_ready(void);
static uint8_t kbc_read_data(void);
static void kbc_send_data(uint8_t data);
static bool kbc_expect_ack(const char* command_name);
char apply_modifiers_extended(char c, uint8_t modifiers);
// Ensure terminal_backspace is declared, likely in terminal.h
extern void terminal_backspace(void);

//============================================================================
// Keymap Data (Scan Code Set 1 - US Default with FIXES for build)
//============================================================================

// Using US QWERTY as the default internal map. Load others via keymap_load()
// *** FIXED: Replaced undeclared KEY_F11, KEY_F12 with KEY_UNKNOWN ***
static const uint16_t DEFAULT_KEYMAP_US[128] = {
    [0x00] = KEY_UNKNOWN,
    [0x01] = KEY_ESC,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6',  [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',
    [0x0C] = '-',  [0x0D] = '=',  [0x0E] = KEY_BACKSPACE, /* Backspace */ [0x0F] = KEY_TAB, /* Tab */
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',
    [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',
    [0x1A] = '[',  [0x1B] = ']',  [0x1C] = '\n', /* Enter */ [0x1D] = KEY_CTRL, /* Left Ctrl */
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',
    [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',  [0x26] = 'l',  [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',  [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x36] = KEY_RIGHT_SHIFT,[0x37] = KEY_UNKNOWN, /* Keypad * */
    [0x38] = KEY_ALT, /* Left Alt */ [0x39] = ' ', /* Space bar */
    [0x3A] = KEY_CAPS, /* Caps Lock */
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10, /* F1-F10 */
    [0x45] = KEY_NUM,  /* Assume Num Lock maps to KEY_NUM */
    [0x46] = KEY_SCROLL, /* Assume Scroll Lock maps to KEY_SCROLL */
    [0x47] = KEY_HOME,    /* Often Keypad 7 or Home */
    [0x48] = KEY_UP,      /* Often Keypad 8 or Up Arrow */
    [0x49] = KEY_PAGE_UP, /* Often Keypad 9 or PgUp */
    [0x4A] = KEY_UNKNOWN, /* Keypad - */
    [0x4B] = KEY_LEFT,    /* Often Keypad 4 or Left Arrow */
    [0x4C] = KEY_UNKNOWN, /* Often Keypad 5 */
    [0x4D] = KEY_RIGHT,   /* Often Keypad 6 or Right Arrow */
    [0x4E] = KEY_UNKNOWN, /* Keypad + */
    [0x4F] = KEY_END,     /* Often Keypad 1 or End */
    [0x50] = KEY_DOWN,    /* Often Keypad 2 or Down Arrow */
    [0x51] = KEY_PAGE_DOWN, /* Often Keypad 3 or PgDn */
    [0x52] = KEY_INSERT,  /* Often Keypad 0 or Insert */
    [0x53] = KEY_DELETE,  /* Often Keypad . or Delete */
    [0x54] = KEY_UNKNOWN, /* Alt+SysRq */
    [0x57] = KEY_UNKNOWN, /* FIXED: Was KEY_F11 */
    [0x58] = KEY_UNKNOWN, /* FIXED: Was KEY_F12 */
    [0x59 ... 0x7F] = KEY_UNKNOWN
};

//============================================================================
// KBC Helper Functions (Implementations - unchanged)
//============================================================================
static inline void kbc_wait_for_send_ready(void) { /* ... as before ... */
    int timeout = KBC_WAIT_TIMEOUT;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IN_BUF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WARNING] Timeout waiting for KBC send ready.\n");
    }
}
static inline void kbc_wait_for_recv_ready(void) { /* ... as before ... */
    int timeout = KBC_WAIT_TIMEOUT;
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OUT_BUF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WARNING] Timeout waiting for KBC recv ready.\n");
    }
}
static uint8_t kbc_read_data(void) { /* ... as before ... */
    kbc_wait_for_recv_ready();
    return inb(KBC_DATA_PORT);
}
static void kbc_send_data(uint8_t data) { /* ... as before ... */
    kbc_wait_for_send_ready();
    outb(KBC_DATA_PORT, data);
}
static bool kbc_expect_ack(const char* command_name) { /* ... as before, uses serial_write/print_hex ... */
    uint8_t ack = kbc_read_data();
    if (ack == KBC_RESP_ACK) {
        serial_write("[KB Init] Received ACK (0xFA) for ");
        serial_write(command_name);
        serial_write(".\n");
        return true;
    } else if (ack == KBC_RESP_RESEND) {
        serial_write("[KB Init WARNING] Received Resend (0xFE) for ");
        serial_write(command_name);
        serial_write(".\n");
    } else {
        serial_write("[KB Init WARNING] Received unexpected byte 0x");
        serial_print_hex(ack);
        serial_write(" waiting for ACK for ");
        serial_write(command_name);
        serial_write(".\n");
    }
    return false;
}


//============================================================================
// Interrupt Handler and Callback (unchanged from previous fix attempt)
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) { /* ... as before ... */
    (void)frame;

    serial_write("[KB] IRQ1 Handler Enter\n");
    uint8_t status = inb(KBC_STATUS_PORT);
    serial_write("[KB] Status before read: 0x");
    serial_print_hex(status);
    serial_write("\n");

    if (!(status & KBC_SR_OUT_BUF)) {
        serial_write("[KB WARNING] IRQ1 fired but KBC output buffer not full!\n");
    }

    uint8_t scancode = inb(KBC_DATA_PORT);
    serial_write("[KB] Scancode: 0x");
    serial_print_hex(scancode);
    serial_write("\n");

    bool is_break_code;

    if (scancode == SCANCODE_PAUSE_PREFIX) {
        serial_write("[KB] Pause/Break prefix (0xE1) - Ignoring.\n");
        return;
    }

    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        keyboard.extended_code_active = true;
        serial_write("[KB] Extended code prefix (0xE0)\n");
        return;
    }

    is_break_code = (scancode & 0x80) != 0;
    uint8_t base_scancode = scancode & 0x7F;
    KeyCode kc = KEY_UNKNOWN;

    if (keyboard.extended_code_active) {
        serial_write("[KB] Processing Extended Code: 0x");
        serial_print_hex(base_scancode);
        serial_write("\n");
        switch (base_scancode) {
            case 0x1D: kc = KEY_CTRL;       break;
            case 0x38: kc = KEY_ALT;        break;
            case 0x48: kc = KEY_UP;         break;
            case 0x50: kc = KEY_DOWN;       break;
            case 0x4B: kc = KEY_LEFT;       break;
            case 0x4D: kc = KEY_RIGHT;      break;
            case 0x47: kc = KEY_HOME;       break;
            case 0x4F: kc = KEY_END;        break;
            case 0x49: kc = KEY_PAGE_UP;    break;
            case 0x51: kc = KEY_PAGE_DOWN;  break;
            case 0x52: kc = KEY_INSERT;     break;
            case 0x53: kc = KEY_DELETE;     break;
            case 0x1C: kc = '\n';           break;
            case 0x35: kc = '/';            break;
            default:
                kc = KEY_UNKNOWN;
                serial_write("[KB] E0 prefix unhandled for base scancode 0x");
                serial_print_hex(base_scancode);
                serial_write("\n");
                break;
        }
        keyboard.extended_code_active = false;
    } else {
        kc = keyboard.current_keymap[base_scancode];
    }

    if (kc == KEY_UNKNOWN) {
        serial_write("[KB] Unknown/unmapped scancode: Base=0x");
        serial_print_hex(base_scancode);
        serial_write(" (Raw=0x");
        serial_print_hex(scancode);
        serial_write(")\n");
        keyboard.extended_code_active = false;
        return;
    }

    if (kc < KEY_COUNT) {
        keyboard.key_states[kc] = !is_break_code;
    }

    switch (kc) {
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT:
            if (!is_break_code) keyboard.modifiers |= MOD_SHIFT;
            else keyboard.modifiers &= ~MOD_SHIFT;
            break;
        case KEY_CTRL:
            if (!is_break_code) keyboard.modifiers |= MOD_CTRL;
            else keyboard.modifiers &= ~MOD_CTRL;
            break;
        case KEY_ALT:
            if (!is_break_code) keyboard.modifiers |= MOD_ALT;
            else keyboard.modifiers &= ~MOD_ALT;
            break;
        case KEY_CAPS:
            if (!is_break_code) { keyboard.modifiers ^= MOD_CAPS; }
            break;
        case KEY_NUM:
           #ifdef MOD_NUM_LOCK
           if (!is_break_code) { keyboard.modifiers ^= MOD_NUM_LOCK; }
           #endif
           break;
        case KEY_SCROLL:
           #ifdef MOD_SCROLL_LOCK
           if (!is_break_code) { keyboard.modifiers ^= MOD_SCROLL_LOCK; }
           #endif
           break;
        default:
            break;
    }

    KeyEvent event = {
        .code       = kc,
        .action     = is_break_code ? KEY_RELEASE : KEY_PRESS,
        .modifiers  = keyboard.modifiers,
        .timestamp  = get_pit_ticks()
    };

    uint8_t next_head = (keyboard.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard.buf_tail) {
        serial_write("[KB WARNING] Buffer overflow! Discarding oldest event.\n");
        keyboard.buf_tail = (keyboard.buf_tail + 1) % KB_BUFFER_SIZE;
    }
    keyboard.buffer[keyboard.buf_head] = event;
    keyboard.buf_head = next_head;

    if (keyboard.event_callback) {
        serial_write("[KB] Calling Callback\n");
        keyboard.event_callback(event);
    } else {
        serial_write("[KB WARNING] No keyboard callback registered!\n");
    }
    serial_write("[KB] IRQ1 Handler Exit\n");
}

/** @brief Default callback function for key events. */
static void default_event_callback(KeyEvent event) {
    serial_write("[KB Callback] Enter (Code: ");
    serial_print_hex((uint32_t)event.code);
    serial_write(", Action: ");
    serial_print_hex((uint32_t)event.action);
    serial_write(", Mods: ");
    serial_print_hex((uint32_t)event.modifiers);
    serial_write(")\n");

    if (event.action == KEY_PRESS) {
        char raw_char = 0;
        if (event.code > 0 && event.code < 128) {
             raw_char = (char)event.code;
        }

        if (raw_char >= ' ' && raw_char <= '~') {
            char adjusted_char = apply_modifiers_extended(raw_char, event.modifiers);
            terminal_write_char(adjusted_char);
        } else {
            // *** FIXED: Use char literals matching keymap, NOT KeyCode constants ***
            switch (event.code) { // Switch on KeyCode
                case '\n': // Handle newline character directly (mapped from 0x1C)
                    terminal_write_char('\n');
                    break;
                case KEY_BACKSPACE: // Use KeyCode if defined
                    terminal_backspace();
                    break;
                case KEY_TAB: // Use KeyCode if defined
                    terminal_write_char('\t');
                    break;
                // Handle other specific KeyCodes
                case KEY_UP:    serial_write("[CB:UP]"); break;
                case KEY_DOWN:  serial_write("[CB:DOWN]"); break;
                case KEY_LEFT:  serial_write("[CB:LEFT]"); break;
                case KEY_RIGHT: serial_write("[CB:RIGHT]"); break;
                default:
                    serial_write("[KB Callback] Ignoring non-printable/unhandled KeyCode: ");
                    serial_print_hex((uint32_t)event.code);
                    serial_write("\n");
                    break;
            }
        }
    }
    serial_write("[KB Callback] Exit\n");
}


//============================================================================
// Public API Functions (Initialize spinlock, use correct interrupt control)
//============================================================================

void keyboard_init(void) {
    serial_write("[KB Init] Initializing keyboard driver...\n");
    memset(&keyboard, 0, sizeof(keyboard));
    spinlock_init(&keyboard.buffer_lock);

    memcpy(keyboard.current_keymap, DEFAULT_KEYMAP_US, sizeof(DEFAULT_KEYMAP_US));
    serial_write("[KB Init] Default US keymap loaded.\n");

    // 1. Attempt to Enable Keyboard Scanning (this sends 0xF4 to data port 0x60)
    serial_write("[KB Init] Attempting to enable keyboard scanning (0xF4 to data port 0x60)...\n");
    kbc_send_data(KBC_CMD_ENABLE_SCAN);
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
        serial_write("[KB Init WARNING] Did not receive expected ACK for Enable Scan command.\n");
        // Don't necessarily give up here, KBC state might be odd.
    } else {
        serial_write("[KB Init] Keyboard scanning enabled successfully (or was already enabled).\n");
    }
    very_short_delay(); // Give KBC time

    // 2. Check and Modify KBC Configuration Byte to ensure keyboard is enabled
    serial_write("[KB Init] Reading KBC Configuration Byte (Cmd 0x20 to 0x64, Read from 0x60)...\n");
    kbc_send_command(KBC_CMD_READ_CONFIG); // Send command to read config byte
    uint8_t kbc_config = kbc_read_data();   // Read config byte from data port
    serial_write("[KB Init] KBC Config Byte Read: 0x");
    serial_print_hex(kbc_config);
    serial_write("\n");

    uint8_t new_kbc_config = kbc_config;
    bool config_changed = false;

    // Bit 0: Keyboard Interrupt (IRQ1) - should be enabled
    if (!(new_kbc_config & KBC_CFG_INT_KB)) {
        serial_write("[KB Init] KBC Config: Keyboard IRQ1 was disabled. Enabling.\n");
        new_kbc_config |= KBC_CFG_INT_KB;
        config_changed = true;
    }

    // Bit 4: Keyboard Interface Disable (0 = enabled, 1 = disabled)
    // This bit often corresponds to the INH (Inhibit) status bit.
    if (new_kbc_config & KBC_CFG_DISABLE_KB) {
        serial_write("[KB Init WARNING] KBC Config: Keyboard Interface was DISABLED (INH). Enabling.\n");
        new_kbc_config &= ~KBC_CFG_DISABLE_KB; // Clear bit to enable
        config_changed = true;
    }

    // Bit 6: Translation - Should usually be enabled (1) for PC/AT Set 1 scan codes.
    // If it's 0, scancodes might be XT codes.
    if (!(new_kbc_config & KBC_CFG_TRANSLATION)) {
        serial_write("[KB Init] KBC Config: Scancode translation was OFF. Enabling (Set 1).\n");
        new_kbc_config |= KBC_CFG_TRANSLATION;
        config_changed = true;
    }

    if (config_changed) {
        serial_write("[KB Init] Writing modified KBC Configuration Byte 0x");
        serial_print_hex(new_kbc_config);
        serial_write(" (Cmd 0x60 to 0x64, Data to 0x60)...\n");
        kbc_send_command(KBC_CMD_WRITE_CONFIG); // Command to write config byte
        kbc_send_data(new_kbc_config);          // Send the new config byte to data port
        very_short_delay(); // Give KBC time to process the new config

        // Optionally, re-read to verify (some KBCs might not allow immediate re-read or might reset)
        kbc_send_command(KBC_CMD_READ_CONFIG);
        uint8_t kbc_config_after_write = kbc_read_data();
        serial_write("[KB Init] KBC Config Byte after write attempt: 0x");
        serial_print_hex(kbc_config_after_write);
        serial_write("\n");
        if (kbc_config_after_write != new_kbc_config) {
            serial_write("[KB Init WARNING] KBC Config Byte did not verify after write!\n");
        }
    } else {
        serial_write("[KB Init] KBC Configuration Byte seems okay (IRQ1 enabled, KB Interface enabled, Translation on).\n");
    }

    // Final status check
    uint8_t final_status_check = inb(KBC_STATUS_PORT);
    serial_write("[KB Init] Final KBC Status before IRQ registration: 0x");
    serial_print_hex(final_status_check);
    serial_write("\n");
    if (final_status_check & 0x10) { // Check INH bit (bit 4)
         serial_write("[KB Init ERROR] Keyboard Inhibit (INH) bit is STILL SET after configuration attempts!\n");
    }


    // 3. Register IRQ Handler
    register_int_handler(33, keyboard_irq1_handler, NULL); // IRQ 1 is vector 33
    serial_write("[KB Init] IRQ1 handler registered (Vector 33).\n");

    // 4. Register Default Callback
    keyboard_register_callback(default_event_callback);
    serial_write("[KB Init] Default event callback registered.\n");

    terminal_write("[Keyboard] Initialized.\n"); // Log to main terminal
}

bool keyboard_poll_event(KeyEvent* event) {
    KERNEL_ASSERT(event != NULL, "NULL event pointer passed to keyboard_poll_event");

    // *** FIXED: Use spinlock functions for interrupt control ***
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard.buffer_lock);

    if (keyboard.buf_head == keyboard.buf_tail) {
        spinlock_release_irqrestore(&keyboard.buffer_lock, irq_flags);
        return false; // Buffer empty
    }

    *event = keyboard.buffer[keyboard.buf_tail];
    keyboard.buf_tail = (keyboard.buf_tail + 1) % KB_BUFFER_SIZE;

    spinlock_release_irqrestore(&keyboard.buffer_lock, irq_flags);

    return true;
}

// Other public functions (keyboard_is_key_down, etc.) remain the same as the previous version

bool keyboard_is_key_down(KeyCode key) { /* ... as before ... */
    if (key >= KEY_COUNT) { return false; }
    return keyboard.key_states[key];
}
uint8_t keyboard_get_modifiers(void) { /* ... as before ... */
    return keyboard.modifiers;
}
void keyboard_set_leds(bool scroll, bool num, bool caps) { /* ... as before ... */
    serial_write("[KB] Setting LEDs: Scroll=");
    serial_write(scroll ? "1" : "0");
    serial_write(", Num=");
    serial_write(num ? "1" : "0");
    serial_write(", Caps=");
    serial_write(caps ? "1" : "0");
    serial_write("\n");

    uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);

    kbc_send_data(KBC_CMD_SET_LEDS);
    if (kbc_expect_ack("Set LEDs (0xED)")) {
        kbc_send_data(led_state);
        if (!kbc_expect_ack("Set LEDs Data")) {
             serial_write("[KB WARNING] Did not receive ACK after sending LED state byte.\n");
        }
    } else {
         serial_write("[KB WARNING] Did not receive ACK for Set LEDs command.\n");
    }
}
void keyboard_set_keymap(const uint16_t* keymap) { /* ... as before ... */
    KERNEL_ASSERT(keymap != NULL, "NULL keymap passed to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard.buffer_lock);
    memcpy(keyboard.current_keymap, keymap, sizeof(keyboard.current_keymap));
    spinlock_release_irqrestore(&keyboard.buffer_lock, irq_flags);
    serial_write("[KB] Keymap updated.\n");
}
void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) { /* ... as before ... */
    delay &= 0x03;
    speed &= 0x1F;
    serial_write("[KB] Setting Typematic Rate: Delay=");
    serial_print_hex(delay);
    serial_write(", Speed=");
    serial_print_hex(speed);
    serial_write("\n");

    kbc_send_data(KBC_CMD_SET_TYPEMATIC);
    if (kbc_expect_ack("Set Typematic (0xF3)")) {
        uint8_t typematic_byte = (delay << 5) | speed;
        kbc_send_data(typematic_byte);
         if (!kbc_expect_ack("Set Typematic Data")) {
             serial_write("[KB WARNING] Did not receive ACK after sending typematic byte.\n");
        }
    } else {
         serial_write("[KB WARNING] Did not receive ACK for Set Typematic command.\n");
    }
}
void keyboard_register_callback(void (*callback)(KeyEvent)) { /* ... as before ... */
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard.buffer_lock);
    keyboard.event_callback = callback;
    spinlock_release_irqrestore(&keyboard.buffer_lock, irq_flags);
    serial_write("[KB] Event callback ");
    serial_write(callback ? "registered" : "cleared");
    serial_write(".\n");
}

// Helper function (unchanged)
char apply_modifiers_extended(char c, uint8_t modifiers) { /* ... as before ... */
     bool shift = (modifiers & MOD_SHIFT) != 0;
     bool caps  = (modifiers & MOD_CAPS) != 0;
     // bool altgr = (modifiers & MOD_ALT_GR) != 0; // Assuming MOD_ALT_GR is defined

     /* Alphabetic */
     if (c >= 'a' && c <= 'z') { return (shift ^ caps) ? (c - 'a' + 'A') : c; }
     if (c >= 'A' && c <= 'Z') { return (shift ^ caps) ? (c - 'A' + 'a') : c; }

     /* Digits/Symbols (Shift) */
     if (shift) {
         switch (c) {
             case '1': return '!'; case '2': return '@'; case '3': return '#';
             case '4': return '$'; case '5': return '%'; case '6': return '^';
             case '7': return '&'; case '8': return '*'; case '9': return '(';
             case '0': return ')'; case '-': return '_'; case '=': return '+';
             case '[': return '{'; case ']': return '}'; case '\\': return '|';
             case ';': return ':'; case '\'': return '"'; case ',': return '<';
             case '.': return '>'; case '/': return '?'; case '`': return '~';
             default: break;
         }
     }
     /* AltGr if needed */
     // if (altgr) { ... }
     return c;
}