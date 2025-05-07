/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for UiAOS
 * @version 5.3 (Refined KBC Initialization Sequence)
 *
 * @details Includes a refined, more comprehensive initialization sequence
 * to attempt to clear the KBC INH (inhibit) state.
 */

//============================================================================
// Includes
//============================================================================
#include "keyboard.h"
#include "types.h"
#include "idt.h"
#include <isr_frame.h>
#include "terminal.h"
#include "port_io.h"
#include "pit.h"
#include "string.h"
#include "serial.h"
#include "spinlock.h"
#include "assert.h"
#include <libc/stdbool.h>
#include <libc/stdint.h>

//============================================================================
// Definitions and Constants
//============================================================================
#define KBC_DATA_PORT    0x60
#define KBC_STATUS_PORT  0x64
#define KBC_CMD_PORT     0x64

#define KBC_SR_OBF       0x01 // Output Buffer Full
#define KBC_SR_IBF       0x02 // Input Buffer Full
#define KBC_SR_SYS_FLAG  0x04 // System Flag
#define KBC_SR_A2        0x08 // Command/Data
#define KBC_SR_INH       0x10 // Inhibit Switch / Keyboard Interface Disabled

#define KBC_CMD_READ_CONFIG         0x20 // Read KBC Configuration Byte
#define KBC_CMD_WRITE_CONFIG        0x60 // Write KBC Configuration Byte
#define KBC_CMD_SELF_TEST           0xAA // KBC Self-Test (Controller)
#define KBC_CMD_KB_INTERFACE_TEST   0xAB // Keyboard Interface Test
#define KBC_CMD_DISABLE_KB_IFACE    0xAD // Disable Keyboard Interface (KBC command)
#define KBC_CMD_ENABLE_KB_IFACE     0xAE // Enable Keyboard Interface (KBC command)

#define KBC_CFG_INT_KB              0x01 // Bit 0: Keyboard Interrupt Enable (IRQ1)
#define KBC_CFG_DISABLE_KB          0x10 // Bit 4: Keyboard Interface Disable (0=Enabled, 1=Disabled)
#define KBC_CFG_TRANSLATION         0x40 // Bit 6: Translation Enable (Set 1 -> Set 2)

#define KB_CMD_SET_LEDS             0xED // To Keyboard Device
#define KB_CMD_ENABLE_SCAN          0xF4 // To Keyboard Device
#define KB_CMD_DISABLE_SCAN         0xF5 // To Keyboard Device
#define KB_CMD_SET_TYPEMATIC        0xF3 // To Keyboard Device
#define KB_CMD_RESET                0xFF // To Keyboard Device

#define KB_RESP_ACK                 0xFA // From Keyboard Device or KBC
#define KB_RESP_RESEND              0xFE // From Keyboard Device or KBC
#define KB_RESP_SELF_TEST_PASS      0xAA // From Keyboard Device after 0xFF reset
#define KBC_RESP_SELF_TEST_PASS     0x55 // From KBC after 0xAA self-test

#define SCANCODE_EXTENDED_PREFIX    0xE0
#define SCANCODE_PAUSE_PREFIX       0xE1

#define KB_BUFFER_SIZE 256
#define KBC_WAIT_TIMEOUT 200000 // Slightly increased timeout

//============================================================================
// Module Static Data
//============================================================================
static struct {
    bool        key_states[KEY_COUNT];
    uint8_t     modifiers;
    KeyEvent    buffer[KB_BUFFER_SIZE];
    uint8_t     buf_head;
    uint8_t     buf_tail;
    spinlock_t  buffer_lock;
    uint16_t    current_keymap[128];
    bool        extended_code_active;
    void        (*event_callback)(KeyEvent);
} keyboard_state;

//============================================================================
// Forward Declarations
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame);
static inline void kbc_wait_for_send_ready(void);
static inline void kbc_wait_for_recv_ready(void);
static uint8_t kbc_read_data(void);
static void kbc_send_data_port(uint8_t data);
static void kbc_send_command_port(uint8_t cmd);
static bool kbc_expect_ack(const char* command_name);
static void very_short_delay(void);
char apply_modifiers_extended(char c, uint8_t modifiers);
extern void terminal_handle_key_event(const KeyEvent event);
extern void terminal_backspace(void);

//============================================================================
// Keymap Data (US Default)
//============================================================================
static const uint16_t DEFAULT_KEYMAP_US[128] = {
    [0x00] = KEY_UNKNOWN, [0x01] = KEY_ESC, [0x02] = '1', [0x03] = '2', [0x04] = '3',
    [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9',
    [0x0B] = '0', [0x0C] = '-', [0x0D] = '=', [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n', [0x1D] = KEY_CTRL, [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd',
    [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = ';', [0x28] = '\'',[0x29] = '`', [0x2A] = KEY_LEFT_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b', [0x31] = 'n',
    [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x36] = KEY_RIGHT_SHIFT,
    [0x37] = KEY_UNKNOWN, /* Keypad * */ [0x38] = KEY_ALT, [0x39] = ' ', [0x3A] = KEY_CAPS,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4, [0x3F] = KEY_F5,
    [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8, [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM, [0x46] = KEY_SCROLL, [0x47] = KEY_HOME, [0x48] = KEY_UP,
    [0x49] = KEY_PAGE_UP, [0x4A] = KEY_UNKNOWN, /* Keypad - */ [0x4B] = KEY_LEFT,
    [0x4C] = KEY_UNKNOWN, /* Keypad 5 */ [0x4D] = KEY_RIGHT, [0x4E] = KEY_UNKNOWN, /* Keypad + */
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN, [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE, [0x54] = KEY_UNKNOWN, [0x57] = KEY_UNKNOWN, /* F11 */
    [0x58] = KEY_UNKNOWN, /* F12 */ [0x59 ... 0x7F] = KEY_UNKNOWN
};

//============================================================================
// KBC Helper Functions
//============================================================================
static void very_short_delay(void) {
    for (volatile int i = 0; i < 30000; ++i) { // Slightly longer delay
        asm volatile("pause");
    }
}

static inline void kbc_wait_for_send_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IBF) && timeout-- > 0) { asm volatile("pause"); }
    if (timeout <= 0) {
        serial_write("[KB WARNING] Timeout: KBC input buffer not clear. Status: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}

static inline void kbc_wait_for_recv_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF) && timeout-- > 0) { asm volatile("pause"); }
    if (timeout <= 0) {
        serial_write("[KB WARNING] Timeout: KBC output buffer not full. Status: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}

static uint8_t kbc_read_data(void) {
    kbc_wait_for_recv_ready();
    return inb(KBC_DATA_PORT);
}

static void kbc_send_data_port(uint8_t data) {
    kbc_wait_for_send_ready();
    outb(KBC_DATA_PORT, data);
}

static void kbc_send_command_port(uint8_t cmd) {
    kbc_wait_for_send_ready();
    outb(KBC_CMD_PORT, cmd);
}

static bool kbc_expect_ack(const char* command_name) {
    kbc_wait_for_recv_ready(); // Ensure data is available before reading
    uint8_t resp = inb(KBC_DATA_PORT);
    if (resp == KB_RESP_ACK) {
        serial_write("[KB Init] ACK (0xFA) for "); serial_write(command_name); serial_write(".\n");
        return true;
    } else if (resp == KB_RESP_RESEND) {
        serial_write("[KB Init WARNING] RESEND (0xFE) for "); serial_write(command_name); serial_write(".\n");
    } else {
        serial_write("[KB Init WARNING] Unexpected 0x"); serial_print_hex(resp);
        serial_write(" for "); serial_write(command_name); serial_write(" (expected ACK 0xFA).\n");
    }
    return false;
}

//============================================================================
// Interrupt Handler
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) {
    (void)frame;
    uint8_t status_before_read = inb(KBC_STATUS_PORT);
    if (!(status_before_read & KBC_SR_OBF)) { return; }
    uint8_t scancode = inb(KBC_DATA_PORT);
    bool is_break_code;
    if (scancode == SCANCODE_PAUSE_PREFIX) { keyboard_state.extended_code_active = false; return; }
    if (scancode == SCANCODE_EXTENDED_PREFIX) { keyboard_state.extended_code_active = true; return; }
    is_break_code = (scancode & 0x80) != 0;
    uint8_t base_scancode = scancode & 0x7F;
    KeyCode kc = KEY_UNKNOWN;
    if (keyboard_state.extended_code_active) {
        switch (base_scancode) {
            case 0x1D: kc = KEY_CTRL; break; case 0x38: kc = KEY_ALT; break;
            case 0x48: kc = KEY_UP; break; case 0x50: kc = KEY_DOWN; break;
            case 0x4B: kc = KEY_LEFT; break; case 0x4D: kc = KEY_RIGHT; break;
            case 0x47: kc = KEY_HOME; break; case 0x4F: kc = KEY_END; break;
            case 0x49: kc = KEY_PAGE_UP; break; case 0x51: kc = KEY_PAGE_DOWN; break;
            case 0x52: kc = KEY_INSERT; break; case 0x53: kc = KEY_DELETE; break;
            case 0x1C: kc = '\n'; break; case 0x35: kc = '/'; break;
            default: kc = KEY_UNKNOWN; break;
        }
        keyboard_state.extended_code_active = false;
    } else {
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }
    if (kc == KEY_UNKNOWN) { return; }
    if (kc < KEY_COUNT) keyboard_state.key_states[kc] = !is_break_code;
    switch (kc) {
        case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_SHIFT) : (keyboard_state.modifiers | MOD_SHIFT); break;
        case KEY_CTRL:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_CTRL) : (keyboard_state.modifiers | MOD_CTRL); break;
        case KEY_ALT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_ALT) : (keyboard_state.modifiers | MOD_ALT); break;
        case KEY_CAPS: if (!is_break_code) keyboard_state.modifiers ^= MOD_CAPS; break;
        case KEY_NUM: if (!is_break_code) keyboard_state.modifiers ^= MOD_NUM; break;
        case KEY_SCROLL: if (!is_break_code) keyboard_state.modifiers ^= MOD_SCROLL; break;
        default: break;
    }
    KeyEvent event = {kc, is_break_code ? KEY_RELEASE : KEY_PRESS, keyboard_state.modifiers, get_pit_ticks()};
    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) { keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE; }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);
    if (keyboard_state.event_callback) keyboard_state.event_callback(event);
}

//============================================================================
// Public API Functions
//============================================================================

void keyboard_init(void) {
    serial_write("[KB Init] Initializing keyboard driver (v5.3)...\n");
    memset(&keyboard_state, 0, sizeof(keyboard_state));
    spinlock_init(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(DEFAULT_KEYMAP_US));
    serial_write("[KB Init] Default US keymap loaded.\n");

    // Step 0: Clear any stale KBC output
    serial_write("[KB Init] Clearing stale KBC OBF (if any)...\n");
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        uint8_t stale_data = inb(KBC_DATA_PORT);
        serial_write("[KB Init] Cleared stale KBC data: 0x"); serial_print_hex(stale_data); serial_write("\n");
    }

    // Step 1: KBC Self-Test
    serial_write("[KB Init] KBC Self-Test (0xAA to CMD 0x64)...\n");
    kbc_send_command_port(KBC_CMD_SELF_TEST);
    uint8_t test_result = kbc_read_data();
    if (test_result == KBC_RESP_SELF_TEST_PASS) serial_write("[KB Init] KBC Self-Test PASSED (0x55).\n");
    else { serial_write("[KB Init WARNING] KBC Self-Test FAILED/unexpected: 0x"); serial_print_hex(test_result); serial_write("\n"); }
    very_short_delay();

    // Step 2: Disable Keyboard and Mouse Interfaces (to ensure a known state)
    serial_write("[KB Init] Sending Disable Keyboard Interface (0xAD to CMD 0x64)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE); // Disable Keyboard
    very_short_delay();
    // serial_write("[KB Init] Sending Disable Mouse Interface (0xA7 to CMD 0x64)...\n");
    // kbc_send_command_port(0xA7); // Disable Mouse (if present and interfering)
    // very_short_delay();
    // uint8_t status_after_disable = inb(KBC_STATUS_PORT);
    // serial_write("[KB Init] Status after interface disables: 0x"); serial_print_hex(status_after_disable); serial_write("\n");

    // Step 3: Flush KBC Output Buffer Again
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        uint8_t post_disable_data = inb(KBC_DATA_PORT);
        serial_write("[KB Init] Cleared KBC data after disable cmds: 0x"); serial_print_hex(post_disable_data); serial_write("\n");
    }

    // Step 4: Read KBC Configuration Byte
    serial_write("[KB Init] Reading KBC Config Byte (0x20 to CMD 0x64)...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t kbc_config = kbc_read_data();
    serial_write("[KB Init] KBC Config Byte Read: 0x"); serial_print_hex(kbc_config); serial_write("\n");

    // Step 5: Modify and Write KBC Configuration Byte
    uint8_t new_kbc_config = kbc_config;
    bool config_changed = false;
    // Bit 0: Keyboard Interrupt Enable (IRQ1) -> SET to 1
    if (!(new_kbc_config & KBC_CFG_INT_KB)) { new_kbc_config |= KBC_CFG_INT_KB; config_changed = true; serial_write("  Config: Enabling KB IRQ1.\n");}
    // Bit 1: Mouse Interrupt Enable (IRQ12) -> CLEAR to 0 (disable mouse if not used)
    // if (new_kbc_config & 0x02) { new_kbc_config &= ~0x02; config_changed = true; serial_write("  Config: Disabling Mouse IRQ12.\n");}
    // Bit 4: Keyboard Interface Disable -> CLEAR to 0 (enable interface)
    if (new_kbc_config & KBC_CFG_DISABLE_KB) { new_kbc_config &= ~KBC_CFG_DISABLE_KB; config_changed = true; serial_write("  Config: Enabling KB Interface (clearing bit 4).\n");}
    // Bit 6: Translation -> SET to 1
    if (!(new_kbc_config & KBC_CFG_TRANSLATION)) { new_kbc_config |= KBC_CFG_TRANSLATION; config_changed = true; serial_write("  Config: Enabling Translation.\n");}

    if (config_changed) {
        serial_write("[KB Init] Writing modified KBC Config Byte 0x"); serial_print_hex(new_kbc_config); serial_write(" (0x60 to CMD 0x64, data to 0x60)...\n");
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG);
        kbc_send_data_port(new_kbc_config);
        very_short_delay();
    } else {
        serial_write("[KB Init] KBC Configuration Byte already optimal.\n");
    }
    uint8_t status_after_cfg = inb(KBC_STATUS_PORT);
    serial_write("[KB Init] Status after KBC config write: 0x"); serial_print_hex(status_after_cfg);
    if(status_after_cfg & KBC_SR_INH) serial_write(" (INH still SET!)\n"); else serial_write(" (INH clear)\n");


    // Step 6: Explicitly Enable Keyboard Interface (Command 0xAE) - Try again after config
    serial_write("[KB Init] Re-sending Enable Keyboard Interface (0xAE to CMD 0x64)...\n");
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE);
    very_short_delay();
    uint8_t status_after_ae_retry = inb(KBC_STATUS_PORT);
    serial_write("[KB Init] Status after 0xAE retry: 0x"); serial_print_hex(status_after_ae_retry);
    if (status_after_ae_retry & KBC_SR_INH) serial_write(" (INH still SET!)\n"); else serial_write(" (INH CLEARED!)\n");

    // Step 7: Reset Keyboard Device
    serial_write("[KB Init] Sending Reset Keyboard Device (0xFF to Data 0x60)...\n");
    kbc_send_data_port(KB_CMD_RESET);
    if (kbc_expect_ack("Keyboard Reset (0xFF)")) {
        serial_write("[KB Init] Keyboard ACKed reset. Waiting for BAT (0xAA)...\n");
        uint8_t bat_result = kbc_read_data();
        if (bat_result == KB_RESP_SELF_TEST_PASS) serial_write("[KB Init] Keyboard Self-Test (BAT) PASSED (0xAA).\n");
        else { serial_write("[KB Init WARNING] Keyboard BAT FAILED/unexpected: 0x"); serial_print_hex(bat_result); serial_write("\n"); }
    } else {
        serial_write("[KB Init WARNING] Keyboard did not ACK reset command.\n");
    }
    very_short_delay();

    // Step 8: Enable Keyboard Scanning
    serial_write("[KB Init] Sending Enable Scanning (0xF4 to Data 0x60)...\n");
    kbc_send_data_port(KB_CMD_ENABLE_SCAN);
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
        serial_write("[KB Init WARNING] No ACK for Enable Scan command.\n");
    }
    very_short_delay();

    // Step 9: Final Status Check
    uint8_t final_status = inb(KBC_STATUS_PORT);
    serial_write("[KB Init] Final KBC Status: 0x"); serial_print_hex(final_status);
    if (final_status & KBC_SR_INH) serial_write(" (INH IS SET! - Keyboard likely won't work)\n");
    else serial_write(" (INH is clear - Good!)\n");
    if (final_status & KBC_SR_OBF) {
        uint8_t lingering_data = inb(KBC_DATA_PORT);
        serial_write("[KB Init WARNING] Final KBC OBF is SET. Lingering data: 0x"); serial_print_hex(lingering_data); serial_write("\n");
    }

    // Step 10: Register IRQ Handler & Callback
    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL);
    serial_write("[KB Init] IRQ1 handler registered (Vector 33).\n");
    keyboard_register_callback(terminal_handle_key_event);
    serial_write("[KB Init] Registered 'terminal_handle_key_event' as callback.\n");

    terminal_write("[Keyboard] Initialized.\n");
}

// Other public functions (keyboard_poll_event, etc.)
bool keyboard_poll_event(KeyEvent* event) {
    KERNEL_ASSERT(event != NULL, "NULL event pointer to keyboard_poll_event");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    if (keyboard_state.buf_head == keyboard_state.buf_tail) {
        spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
        return false;
    }
    *event = keyboard_state.buffer[keyboard_state.buf_tail];
    keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    return true;
}

bool keyboard_is_key_down(KeyCode key) {
    if (key >= KEY_COUNT) return false;
    return keyboard_state.key_states[key];
}

uint8_t keyboard_get_modifiers(void) {
    return keyboard_state.modifiers;
}

void keyboard_set_leds(bool scroll, bool num, bool caps) {
    uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
    kbc_send_data_port(KB_CMD_SET_LEDS);
    if (kbc_expect_ack("Set LEDs (0xED)")) {
        kbc_send_data_port(led_state);
        kbc_expect_ack("Set LEDs Data Byte");
    }
}

void keyboard_set_keymap(const uint16_t* keymap) {
    KERNEL_ASSERT(keymap != NULL, "NULL keymap to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, keymap, sizeof(keyboard_state.current_keymap));
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    serial_write("[KB] Keymap updated.\n");
}

void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
    delay &= 0x03; speed &= 0x1F;
    kbc_send_data_port(KB_CMD_SET_TYPEMATIC);
    if (kbc_expect_ack("Set Typematic (0xF3)")) {
        kbc_send_data_port((delay << 5) | speed);
        kbc_expect_ack("Set Typematic Data Byte");
    }
}

void keyboard_register_callback(void (*callback)(KeyEvent)) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    keyboard_state.event_callback = callback;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
}

char apply_modifiers_extended(char c, uint8_t modifiers) {
     bool shift = (modifiers & MOD_SHIFT) != 0;
     bool caps  = (modifiers & MOD_CAPS) != 0;
     if (c >= 'a' && c <= 'z') return (shift ^ caps) ? (c - 'a' + 'A') : c;
     if (c >= 'A' && c <= 'Z') return (shift ^ caps) ? (c - 'A' + 'a') : c;
     if (shift) {
         switch (c) {
             case '1': return '!'; case '2': return '@'; case '3': return '#';
             case '4': return '$'; case '5': return '%'; case '6': return '^';
             case '7': return '&'; case '8': return '*'; case '9': return '(';
             case '0': return ')'; case '-': return '_'; case '=': return '+';
             case '[': return '{'; case ']': return '}'; case '\\': return '|';
             case ';': return ':'; case '\'': return '"'; case ',': return '<';
             case '.': return '>'; case '/': return '?'; case '`': return '~';
         }
     }
     return c;
}
