/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for UiAOS
 * @version 6.4.5 - Corrected serial/terminal print functions and BAT constant usage.
 */

//============================================================================
// Includes
//============================================================================
#include "keyboard.h"
#include "keyboard_hw.h" 
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
#include <libc/stddef.h>
#include "keymap.h"   

//============================================================================
// Definitions and Constants
//============================================================================
#define KB_BUFFER_SIZE 256
#define KBC_WAIT_TIMEOUT 300000
#define KBC_MAX_FLUSH 100

#define DEFAULT_KEYMAP_US keymap_us_qwerty

//============================================================================
// Module Static Data
//============================================================================
static struct {
    bool      key_states[KEY_COUNT];
    uint8_t   modifiers;
    KeyEvent  buffer[KB_BUFFER_SIZE];
    uint8_t   buf_head;
    uint8_t   buf_tail;
    spinlock_t buffer_lock;
    uint16_t  current_keymap[128];
    bool      extended_code_active;
    void      (*event_callback)(KeyEvent);
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
static void kbc_flush_output_buffer(const char* context);
static void very_short_delay(void);
extern void terminal_handle_key_event(KeyEvent event);

//============================================================================
// KBC Helper Functions
//============================================================================
static void very_short_delay(void) {
    for (volatile int i = 0; i < 50000; ++i) { asm volatile("pause"); }
}

static inline void kbc_wait_for_send_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IBF) && timeout-- > 0) { asm volatile("pause"); }
    if (timeout <= 0) { serial_write("[KB WaitSend TIMEOUT]\n"); }
}

static inline void kbc_wait_for_recv_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF) && timeout-- > 0) { asm volatile("pause"); }
    if (timeout <= 0) { serial_write("[KB WaitRecv TIMEOUT]\n"); }
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

static void kbc_flush_output_buffer(const char* context) {
    int flush_count = 0;
    uint8_t status_val;
    // terminal_printf("[KB Flush ENTRY: %s]\n", context); 
    while (((status_val = inb(KBC_STATUS_PORT)) & KBC_SR_OBF) && flush_count < KBC_MAX_FLUSH) {
        uint8_t discard = inb(KBC_DATA_PORT);
        (void)discard; 
        // terminal_printf("  [KB Flush Loop: %s] Discarded 0x%02x (Status was 0x%02x)\n", context, discard, status_val);
        flush_count++;
        very_short_delay();
    }
    // if (flush_count > 0) { 
    //    terminal_printf("  [KB Flush EXIT: %s] Flush loop finished. Final Status: 0x%02x\n", context, inb(KBC_STATUS_PORT));
    // }
}

static bool kbc_expect_ack(const char* command_name) {
    uint8_t resp = kbc_read_data();
    if (resp == KB_RESP_ACK) { 
        terminal_printf("[KB Debug] ACK (0xFA) for %s.\n", command_name);
        return true;
    } else {
        terminal_printf("[KB Debug WARNING] Unexpected 0x%02x for %s (expected ACK 0xFA).\n", resp, command_name);
        return false;
    }
}

//============================================================================
// Interrupt Handler
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) {
    // *** Use serial_putchar as suggested by compiler, ensure it's declared (e.g., in serial.h) ***
    serial_putchar('K'); serial_putchar('B'); serial_putchar('_');
    serial_putchar('I'); serial_putchar('R'); serial_putchar('Q');
    serial_putchar('1'); serial_putchar('!'); serial_putchar('\n');

    (void)frame;

    uint8_t status_before_read = inb(KBC_STATUS_PORT);
    if (!(status_before_read & KBC_SR_OBF)) {
        terminal_printf("[KB IRQ1] OBF not set on entry! Status: 0x%02x\n", status_before_read);
        return;
    }

    uint8_t scancode = inb(KBC_DATA_PORT);
    terminal_printf("[KB IRQ] Scancode=0x%02x\n", scancode);

    bool is_break_code;
    KeyCode kc = KEY_UNKNOWN;

    if (scancode == SCANCODE_PAUSE_PREFIX) {
        keyboard_state.extended_code_active = false; 
        terminal_printf("[KB IRQ] Pause Prefix (0xE1) detected.\n");
        return;
    }
    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        keyboard_state.extended_code_active = true;
        return;
    }

    is_break_code = (scancode & 0x80) != 0;
    uint8_t base_scancode = scancode & 0x7F;

    if (keyboard_state.extended_code_active) {
        switch (base_scancode) {
            case 0x1D: kc = KEY_CTRL; break;
            case 0x38: kc = KEY_ALT; break; 
            case 0x48: kc = KEY_UP; break;
            case 0x50: kc = KEY_DOWN; break;
            case 0x4B: kc = KEY_LEFT; break;
            case 0x4D: kc = KEY_RIGHT; break;
            case 0x47: kc = KEY_HOME; break;
            case 0x4F: kc = KEY_END; break;
            case 0x49: kc = KEY_PAGE_UP; break;
            case 0x51: kc = KEY_PAGE_DOWN; break;
            case 0x52: kc = KEY_INSERT; break;
            case 0x53: kc = KEY_DELETE; break;
            case 0x1C: kc = KEY_ENTER; break; 
            case 0x35: kc = '/'; break;      
            default:   kc = KEY_UNKNOWN;
                       terminal_printf("[KB IRQ] Unhandled E0 scancode: 0x%02x\n", base_scancode);
                       break;
        }
        keyboard_state.extended_code_active = false;
    } else {
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }

    if (kc == KEY_UNKNOWN && base_scancode != 0) {
         terminal_printf("[KB IRQ] KEY_UNKNOWN for base_scancode: 0x%02x (Ext: %d)\n", base_scancode, keyboard_state.extended_code_active);
        return;
    }
     if (kc == 0 && base_scancode != 0 ) { 
        terminal_printf("[KB IRQ] Mapped to 0 for base_scancode: 0x%02x (Ext: %d)\n", base_scancode, keyboard_state.extended_code_active);
        return;
    }

    if (kc >= KEY_UNKNOWN && kc < KEY_COUNT) {
        keyboard_state.key_states[kc] = !is_break_code;
    }

    switch (kc) {
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_SHIFT) : (keyboard_state.modifiers | MOD_SHIFT);
            break;
        case KEY_CTRL:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_CTRL) : (keyboard_state.modifiers | MOD_CTRL);
            break;
        case KEY_ALT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_ALT) : (keyboard_state.modifiers | MOD_ALT);
            break;
        case KEY_CAPS:   if (!is_break_code) keyboard_state.modifiers ^= MOD_CAPS; break;
        case KEY_NUM:    if (!is_break_code) keyboard_state.modifiers ^= MOD_NUM; break;
        case KEY_SCROLL: if (!is_break_code) keyboard_state.modifiers ^= MOD_SCROLL; break;
        default: break;
    }

    KeyEvent event = {
        .code = kc,
        .action = is_break_code ? KEY_RELEASE : KEY_PRESS,
        .modifiers = keyboard_state.modifiers,
        .timestamp = get_pit_ticks()
    };

    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) {
        terminal_printf("[KB WARNING] Keyboard event buffer overflow! Oldest event dropped.\n");
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
    }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);

    if (keyboard_state.event_callback) {
        keyboard_state.event_callback(event);
    }
}

//============================================================================
// KBC Configuration Re-check
//============================================================================
void keyboard_recheck_kbc_config(void) {
    uint8_t target_kbc_config = (KBC_CFG_INT_KB | KBC_CFG_TRANSLATION | KBC_CFG_DISABLE_MOUSE) & ~(KBC_CFG_INT_MOUSE | KBC_CFG_DISABLE_KB);
    uint8_t current_config;

    terminal_printf("  [Re-Check] Reading KBC Config Byte (0x20): ");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    current_config = kbc_read_data();
    terminal_printf("0x%02x\n", current_config);

    if (current_config != target_kbc_config) {
        terminal_printf("  [Re-Check] Current KBC Config 0x%02x is NOT desired 0x%02x. Forcing...\n", current_config, target_kbc_config);
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG);
        kbc_send_data_port(target_kbc_config);
        very_short_delay();

        kbc_send_command_port(KBC_CMD_READ_CONFIG);
        current_config = kbc_read_data();
        terminal_printf("  [Re-Check] KBC Config after write attempt: 0x%02x\n", current_config);
        if (current_config != target_kbc_config) {
            terminal_printf("  [KERNEL WARNING] KBC Config re-check FAILED to set 0x%02x! Now is 0x%02x\n", target_kbc_config, current_config);
        } else {
            terminal_printf("  [Re-Check] KBC Config successfully set to 0x%02x.\n", target_kbc_config);
        }
    } else {
        terminal_printf("  [Re-Check] KBC Config Byte 0x%02x is already correct (0x%02x).\n", current_config, target_kbc_config);
    }
    terminal_printf("  [Re-Check] KBC Status register *after* config check: 0x%02x\n", inb(KBC_STATUS_PORT));
}

//============================================================================
// Initialization
//============================================================================
void keyboard_init(void) {
    terminal_printf("[KB Init v%s] Initializing keyboard driver...\n", "6.4.5");
    memset(&keyboard_state, 0, sizeof(keyboard_state));
    spinlock_init(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(keyboard_state.current_keymap));
    terminal_write("  [KB Init] Default US keymap loaded.\n");

    terminal_write("  [KB Init] Flushing KBC Output Buffer (pre-init)...\n");
    kbc_flush_output_buffer("Pre-Init");

    terminal_write("  [KB Init] Sending 0xAD (Disable KB Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xAD");

    terminal_write("  [KB Init] Sending 0xA7 (Disable Mouse Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_MOUSE_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xA7");

    terminal_write("  [KB Init] Reading KBC Config Byte (0x20)...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t current_config_init = kbc_read_data();
    terminal_printf("  [KB Init] Read Config = 0x%02x\n", current_config_init);

    uint8_t desired_kbc_config = (KBC_CFG_INT_KB | KBC_CFG_TRANSLATION | KBC_CFG_DISABLE_MOUSE) & ~(KBC_CFG_INT_MOUSE | KBC_CFG_DISABLE_KB);
    terminal_printf("  [KB Init] Desired KBC Config: 0x%02x\n", desired_kbc_config);

    if (current_config_init != desired_kbc_config) {
        terminal_printf("  [KB Init] Writing KBC Config Byte 0x%02x (Command 0x60)...\n", desired_kbc_config);
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG);
        kbc_send_data_port(desired_kbc_config);
        very_short_delay();
        kbc_flush_output_buffer("After KBC 0x60 write");
    } else {
        terminal_printf("  [KB Init] KBC Config Byte 0x%02x is already desired.\n", current_config_init);
    }

    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t readback_config = kbc_read_data();
    terminal_printf("  [KB Init] Config Byte Readback after attempt = 0x%02x\n", readback_config);
    if (readback_config != desired_kbc_config) {
        terminal_printf("  [KB ERROR] Failed to set KBC Config Byte! Expected 0x%02x, got 0x%02x\n", desired_kbc_config, readback_config);
    }

    terminal_write("  [KB Init] Performing KBC Self-Test (0xAA)...\n");
    kbc_send_command_port(KBC_CMD_SELF_TEST);
    uint8_t test_result = kbc_read_data();
    terminal_printf("  [KB Init] KBC Test Result = 0x%02x", test_result);
    if (test_result != KBC_RESP_SELF_TEST_PASS) { // This is 0x55 for KBC controller
        terminal_write(" (FAIL!)\n");
        KERNEL_PANIC_HALT("KBC Self-Test Failed!");
    } else {
        terminal_write(" (PASS)\n");
    }
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xAA");

    terminal_write("  [KB Init] Sending 0xAE (Enable KB Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xAE");

    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t config_after_enable = kbc_read_data();
    terminal_printf("  [KB Init] Config Byte after 0xAE = 0x%02x\n", config_after_enable);
    if (config_after_enable & KBC_CFG_DISABLE_KB) {
         KERNEL_PANIC_HALT("KBC Config still shows KB Clock Disabled after 0xAE!");
    } else {
         terminal_write("  [KB Init] Confirmed Config Byte OK after 0xAE (KB Clock Enabled).\n");
    }

    terminal_write("  [KB Init] Sending 0xFF (Reset) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_RESET);
    if (!kbc_expect_ack("KB Device Reset (0xFF)")) {
        terminal_write("  [KB Init] Warning: No ACK for KB Device Reset command.\n");
    } else {
        terminal_write("  [KB Init] Waiting for KB BAT result after Reset...\n");
        uint8_t bat_result = kbc_read_data();
        terminal_printf("  [KB Init] Keyboard BAT Result = 0x%02x", bat_result);
        // *** CORRECTED CONSTANT: Use KB_RESP_SELF_TEST_PASS. ***
        // Ensure KB_RESP_SELF_TEST_PASS is defined as 0xAA in keyboard_hw.h for the keyboard device BAT.
        if (bat_result != KB_RESP_SELF_TEST_PASS) { 
             terminal_write(" (FAIL/WARN)\n");
        } else {
             terminal_write(" (PASS)\n");
        }
    }
    very_short_delay();
    kbc_flush_output_buffer("After KB Device Reset");

    terminal_write("  [KB Init] Sending 0xF4 (Enable Scanning) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_ENABLE_SCAN);
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
         terminal_write("  [KB Init] Warning: Failed to get ACK for Enable Scanning!\n");
    } else {
         terminal_write("  [KB Init] Scanning Enabled on device. ACK received.\n");
    }
    very_short_delay();
    kbc_flush_output_buffer("After KB 0xF4 sequence");

    terminal_write("  [KB Init] Final KBC Config Check...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t final_config_check = kbc_read_data();
    terminal_printf("  [KB Init] Final Config Readback = 0x%02x\n", final_config_check);
    if (final_config_check != desired_kbc_config) {
         terminal_printf("  [KB WARNING] Final KBC Config (0x%02x) differs from desired (0x%02x)!\n", final_config_check, desired_kbc_config);
    }
    if (final_config_check & KBC_CFG_DISABLE_KB) {
        KERNEL_PANIC_HALT("Keyboard Init Failed: Interface Clock Disabled in final config check!");
    }
    if (!(final_config_check & KBC_CFG_INT_KB)) {
         terminal_write("  [KB Init WARNING] Keyboard Interrupt Disabled in final config check!\n");
    }

    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL);
    terminal_write("  [KB Init] IRQ1 handler registered.\n");
    keyboard_register_callback(terminal_handle_key_event);
    terminal_write("  [KB Init] Registered terminal handler as callback.\n");

    terminal_printf("[Keyboard] Initialized (v%s).\n", "6.4.5");
}

//============================================================================
// Public API Functions
//============================================================================
bool keyboard_poll_event(KeyEvent* event) {
    KERNEL_ASSERT(event != NULL, "NULL event pointer to keyboard_poll_event");
    bool event_found = false;
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    if (keyboard_state.buf_head != keyboard_state.buf_tail) {
        *event = keyboard_state.buffer[keyboard_state.buf_tail];
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
        event_found = true;
    }
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    return event_found;
}

bool keyboard_is_key_down(KeyCode key) {
    if (key >= KEY_UNKNOWN && key < KEY_COUNT) {
        return keyboard_state.key_states[key];
    }
    return false;
}

uint8_t keyboard_get_modifiers(void) {
    return keyboard_state.modifiers;
}

void keyboard_set_leds(bool scroll, bool num, bool caps) {
    uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
    kbc_send_data_port(KB_CMD_SET_LEDS);
    if (kbc_expect_ack("Set LEDs (0xED)")) {
        kbc_send_data_port(led_state);
        // ACK for data byte is often not sent.
        // kbc_expect_ack("Set LEDs Data Byte"); 
    }
}

void keyboard_set_keymap(const uint16_t* keymap) {
    KERNEL_ASSERT(keymap != NULL, "NULL keymap passed to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, keymap, sizeof(keyboard_state.current_keymap));
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    terminal_write("[KB] Keymap updated.\n");
}

void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
    delay &= 0x03;
    speed &= 0x1F;
    kbc_send_data_port(KB_CMD_SET_TYPEMATIC);
    if (kbc_expect_ack("Set Typematic (0xF3)")) {
        kbc_send_data_port((delay << 5) | speed);
        // ACK for data byte is often not sent.
        // kbc_expect_ack("Set Typematic Data Byte");
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

    if (c >= 'a' && c <= 'z') {
        return (shift ^ caps) ? (char)(c - 'a' + 'A') : c;
    }
    if (c >= 'A' && c <= 'Z') {
        return (shift ^ caps) ? (char)(c - 'A' + 'a') : c;
    }

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
// END OF FILE