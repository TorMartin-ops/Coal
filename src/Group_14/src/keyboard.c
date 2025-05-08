/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for UiAOS
 * @version 6.3 - Robust Initialization with ACK Flush & Config Verification
 *
 * Changelog:
 * - v6.3: Removed Set Scan Code command (0xF0 0x01) to rely on KBC translation.
 * Added explicit read from 0x60 after 0xF4 ACK.
 * Removed polling for Status INH bit after 0xAE, relying on Config Byte verification instead.
 * Enhanced logging and comments for clarity.
 * - v6.2: Fixed interpretation of status bit 4 vs config bit 4.
 * - v6.1: Added KB Reset + Reordered Self-Test.
 */

//============================================================================
// Includes
//============================================================================
#include "keyboard.h"
#include "keyboard_hw.h"   // Hardware definitions
#include "types.h"
#include "idt.h"
#include <isr_frame.h>
#include "terminal.h"      // Terminal callback registration
#include "port_io.h"
#include "pit.h"           // get_pit_ticks()
#include "string.h"        // memcpy, memset
#include "serial.h"        // serial_*, essential for debugging init
#include "spinlock.h"
#include "assert.h"
#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stddef.h>  // NULL

//============================================================================
// Definitions and Constants
//============================================================================
#define KB_BUFFER_SIZE 256
#define KBC_WAIT_TIMEOUT 300000 // Timeout loops for KBC waits
#define KBC_MAX_FLUSH 100       // Max bytes to read when flushing OBF

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
static void kbc_flush_output_buffer(const char* context); // Added flush helper
static void very_short_delay(void);
char apply_modifiers_extended(char c, uint8_t modifiers);
extern void terminal_handle_key_event(const KeyEvent event);

//============================================================================
// Keymap Data
//============================================================================
extern const uint16_t keymap_us_qwerty[128];
#define DEFAULT_KEYMAP_US keymap_us_qwerty

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

/**
 * @brief Flushes the KBC Output Buffer (Port 0x60) by reading until empty or limit.
 * @param context Debugging context string.
 */
static void kbc_flush_output_buffer(const char* context) {
    int flush_count = 0;
    uint8_t status;
    // Check status *before* potential read
    while (((status = inb(KBC_STATUS_PORT)) & KBC_SR_OBF) && flush_count < KBC_MAX_FLUSH) {
        uint8_t discard = inb(KBC_DATA_PORT);
        serial_write("[KB Flush: "); serial_write(context);
        serial_write("] Discarded 0x"); serial_print_hex(discard);
        serial_write(" (Status was 0x"); serial_print_hex(status); serial_write(")\n");
        flush_count++;
        very_short_delay(); // Small delay between reads
    }
    // Final status log after flush attempt
    if (flush_count > 0) {
        serial_write("[KB Flush: "); serial_write(context);
        serial_write("] Flush loop finished. Final Status: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}


static bool kbc_expect_ack(const char* command_name) {
    uint8_t resp = kbc_read_data(); // Includes wait
    if (resp == KB_RESP_ACK) {
        serial_write("[KB Debug] ACK (0xFA) for "); serial_write(command_name); serial_write(".\n");
        return true;
    } else {
        serial_write("[KB Debug WARNING] Unexpected 0x"); serial_print_hex(resp);
        serial_write(" for "); serial_write(command_name); serial_write(" (expected ACK 0xFA).\n");
        return false; // Return false even on RESEND for simplicity here
    }
}

//============================================================================
// Interrupt Handler (Implementation unchanged from previous correct version)
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) {
    (void)frame;
    uint8_t status_before_read = inb(KBC_STATUS_PORT);
    if (!(status_before_read & KBC_SR_OBF)) return;

    uint8_t scancode = inb(KBC_DATA_PORT);
    // serial_write("[KB IRQ] Scancode=0x"); serial_print_hex(scancode); serial_write("\n"); // Verbose

    bool is_break_code;
    if (scancode == SCANCODE_PAUSE_PREFIX) { keyboard_state.extended_code_active = false; return; }
    if (scancode == SCANCODE_EXTENDED_PREFIX) { keyboard_state.extended_code_active = true; return; }

    is_break_code = (scancode & 0x80) != 0;
    uint8_t base_scancode = scancode & 0x7F;
    KeyCode kc = KEY_UNKNOWN;

    if (keyboard_state.extended_code_active) {
        // Map E0-prefixed codes
        switch (base_scancode) {
            case 0x1D: kc = KEY_CTRL; break;   // RCtrl
            case 0x38: kc = KEY_ALT; break;    // RAlt (AltGr)
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
            case 0x1C: kc = '\n'; break;       // Keypad Enter
            case 0x35: kc = '/'; break;        // Keypad Divide
            default:   kc = KEY_UNKNOWN; break;
        }
        keyboard_state.extended_code_active = false;
    } else {
        // Map normal codes
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }

    if (kc == KEY_UNKNOWN) return;

    // Update key state and modifiers
    if (kc < KEY_COUNT) {
        keyboard_state.key_states[kc] = !is_break_code;
    }
    switch (kc) {
        case KEY_LEFT_SHIFT: case KEY_RIGHT_SHIFT:
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

    // Buffer event
    KeyEvent event = { kc, is_break_code ? KEY_RELEASE : KEY_PRESS, keyboard_state.modifiers, get_pit_ticks() };
    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) { /* Buffer full, drop oldest */
        serial_write("[KB WARNING] Buffer overflow\n");
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
    }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);

    // Call callback
    if (keyboard_state.event_callback) {
        keyboard_state.event_callback(event);
    }
}

//============================================================================
// Initialization (v6.3 - Refined sequence)
//============================================================================
void keyboard_init(void) {
    serial_write("[KB Init v6.3] Initializing keyboard driver...\n");
    memset(&keyboard_state, 0, sizeof(keyboard_state));
    spinlock_init(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(DEFAULT_KEYMAP_US));
    serial_write("  [KB Init] Default US keymap loaded.\n");

    // --- Step 1: Flush KBC Output Buffer ---
    serial_write("  [KB Init] Flushing KBC Output Buffer (pre-init)...\n");
    kbc_flush_output_buffer("Pre-Init");

    // --- Step 2: Disable Keyboard and Mouse Interfaces ---
    serial_write("  [KB Init] Sending 0xAD (Disable KB Interface)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE); // 0xAD
    very_short_delay();
    kbc_flush_output_buffer("After 0xAD");

    serial_write("  [KB Init] Sending 0xA7 (Disable Mouse Interface)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_MOUSE_IFACE); // 0xA7
    very_short_delay();
    kbc_flush_output_buffer("After 0xA7");

    // --- Step 3: Set KBC Configuration Byte ---
    // We want: KB Int Enabled (IRQ1), Mouse Int Disabled, KB Interface Enabled (Clock Active), Translation Enabled (usually safest default)
    serial_write("  [KB Init] Reading KBC Config Byte (0x20)...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t current_config = kbc_read_data();
    serial_write("  [KB Init] Read Config = 0x"); serial_print_hex(current_config); serial_write("\n");

    // Bits to SET: Bit 0 (KB INT Enable), Bit 6 (Translation Enable)
    // Bits to CLEAR: Bit 1 (Mouse INT Enable), Bit 4 (KB Disable), Bit 5 (Mouse Disable)
    uint8_t desired_config = (current_config | KBC_CFG_INT_KB | KBC_CFG_TRANSLATION)
                             & ~(KBC_CFG_INT_MOUSE | KBC_CFG_DISABLE_KB | KBC_CFG_DISABLE_MOUSE);

    if (current_config != desired_config) {
        serial_write("  [KB Init] Writing KBC Config Byte 0x"); serial_print_hex(desired_config); serial_write(" (Command 0x60)...\n");
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG);
        kbc_send_data_port(desired_config); // Send the actual config byte
        very_short_delay();
        kbc_flush_output_buffer("After 0x60");
    } else {
        serial_write("  [KB Init] KBC Config Byte 0x"); serial_print_hex(current_config); serial_write(" is already correct.\n");
    }
    // Verify config write took effect
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t readback_config = kbc_read_data();
    serial_write("  [KB Init] Config Byte Readback = 0x"); serial_print_hex(readback_config); serial_write("\n");
    if (readback_config != desired_config) {
        KERNEL_PANIC_HALT("Failed to set KBC Configuration Byte!");
    }

    // --- Step 4: KBC Self-Test ---
    serial_write("  [KB Init] Performing KBC Self-Test (0xAA)...\n");
    kbc_send_command_port(KBC_CMD_SELF_TEST);
    uint8_t test_result = kbc_read_data();
    serial_write("  [KB Init] KBC Test Result = 0x"); serial_print_hex(test_result);
    if (test_result != KBC_RESP_SELF_TEST_PASS) {
        serial_write(" (FAIL!)\n");
        KERNEL_PANIC_HALT("KBC Self-Test Failed!");
    } else {
        serial_write(" (PASS)\n");
    }
    very_short_delay();
    kbc_flush_output_buffer("After 0xAA");

    // --- Step 5: Enable Keyboard Interface (again, explicitly) ---
    // Although the config byte has bit 4 clear, sending 0xAE ensures the interface is active.
    serial_write("  [KB Init] Sending 0xAE (Enable KB Interface)...\n");
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After 0xAE");

    // Verify Config Byte again after explicit enable
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t config_after_enable = kbc_read_data();
    serial_write("  [KB Init] Config Byte after 0xAE = 0x"); serial_print_hex(config_after_enable); serial_write("\n");
    if (config_after_enable & KBC_CFG_DISABLE_KB) {
         KERNEL_PANIC_HALT("KBC Config still shows KB Disabled after 0xAE!");
    } else {
         serial_write("  [KB Init] Confirmed Config Byte OK after 0xAE.\n");
    }

    // --- Step 6: Reset Keyboard Device ---
    serial_write("  [KB Init] Sending 0xFF (Reset) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_RESET); // Send 0xFF to Data Port
    if (!kbc_expect_ack("KB Reset (0xFF)")) {
        serial_write("  [KB Init] Warning: No ACK for KB Reset command.\n");
        // Continue, but keyboard might be in an unknown state
    } else {
        // Wait for BAT result after ACK
        serial_write("  [KB Init] Waiting for KB BAT result after Reset...\n");
        uint8_t bat_result = kbc_read_data();
        serial_write("  [KB Init] Keyboard BAT Result = 0x"); serial_print_hex(bat_result);
        if (bat_result != KB_RESP_SELF_TEST_PASS) {
             serial_write(" (FAIL/WARN)\n");
        } else {
             serial_write(" (PASS)\n");
        }
    }
    very_short_delay();
    kbc_flush_output_buffer("After KB Reset");


    // --- Step 7: Enable Scanning ---
    serial_write("  [KB Init] Sending 0xF4 (Enable Scanning) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_ENABLE_SCAN);
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
         // Handle error? For now, just log and continue.
         serial_write("  [KB Init] Warning: Failed to get ACK for Enable Scanning!\n");
    } else {
         // *** IMPORTANT: Flush the ACK from the buffer ***
         very_short_delay();
         kbc_flush_output_buffer("After 0xF4 ACK");
         serial_write("  [KB Init] Scanning Enabled. ACK flushed.\n");
    }

    // --- Step 8: REMOVED Set Scan Code Set 1 ---
    // serial_write("  [KB Init] SKIPPING Set Scan Code Set 1 (0xF0 0x01).\n");

    // --- Step 9: Final Verification & Handler Registration ---
    serial_write("  [KB Init] Final KBC Config Check...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t final_config_check = kbc_read_data();
    serial_write("  [KB Init] Final Config Readback = 0x"); serial_print_hex(final_config_check); serial_write("\n");
    if (final_config_check & KBC_CFG_DISABLE_KB) {
        KERNEL_PANIC_HALT("Keyboard Init Failed: Interface Disabled in final config check!");
    }
    if (!(final_config_check & KBC_CFG_INT_KB)) {
         serial_write("  [KB Init WARNING] Keyboard Interrupt Disabled in final config check!\n");
    }

    // Register IRQ Handler and Callback
    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL);
    serial_write("  [KB Init] IRQ1 handler registered.\n");
    keyboard_register_callback(terminal_handle_key_event);
    serial_write("  [KB Init] Registered terminal handler as callback.\n");

    terminal_write("[Keyboard] Initialized (v6.3 - ACK Flush, No SetScanCode).\n");
}

//============================================================================
// Public API Functions (Unchanged from v6.2)
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
    KERNEL_ASSERT(keymap != NULL, "NULL keymap passed to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, keymap, sizeof(keyboard_state.current_keymap));
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    serial_write("[KB] Keymap updated.\n");
}

void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
    delay &= 0x03;
    speed &= 0x1F;
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

// --- Modifier Application (Unchanged) ---
char apply_modifiers_extended(char c, uint8_t modifiers) {
    bool shift = (modifiers & MOD_SHIFT) != 0;
    bool caps  = (modifiers & MOD_CAPS) != 0;

    if (c >= 'a' && c <= 'z') { return (shift ^ caps) ? (c - 'a' + 'A') : c; }
    if (c >= 'A' && c <= 'Z') { return (shift ^ caps) ? (c - 'A' + 'a') : c; }
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