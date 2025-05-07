/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for UiAOS
 * @version 6.0 - Added command to switch keyboard to Scan Code Set 1 during init.
 */

//============================================================================
// Includes
//============================================================================
#include "keyboard.h"
#include "keyboard_hw.h"   // <<< Include the consolidated hardware definitions
#include "types.h"
#include "idt.h"
#include <isr_frame.h>
#include "terminal.h"      // Needed for the callback registration/use
#include "port_io.h"
#include "pit.h"           // For get_pit_ticks() in event timestamping
#include "string.h"
#include "serial.h"        // For serial logging
#include "spinlock.h"
#include "assert.h"
#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stddef.h> // For NULL definition

//============================================================================
// Definitions and Constants
//============================================================================
#define KB_BUFFER_SIZE 256
#define KBC_WAIT_TIMEOUT 300000 // Timeout for KBC waits

//============================================================================
// Module Static Data
//============================================================================
static struct {
    bool        key_states[KEY_COUNT];  // Tracks if a key is currently pressed
    uint8_t     modifiers;              // Current modifier state (Shift, Ctrl, Alt, etc.)
    KeyEvent    buffer[KB_BUFFER_SIZE]; // Circular buffer for key events
    uint8_t     buf_head;               // Head index for the buffer
    uint8_t     buf_tail;               // Tail index for the buffer
    spinlock_t  buffer_lock;            // Lock protecting the buffer & related state
    uint16_t    current_keymap[128];    // Current keymap array (scancode -> KeyCode/ASCII)
    bool        extended_code_active;   // Flag for handling 0xE0 scancode prefix
    void        (*event_callback)(KeyEvent); // Registered callback function for key events
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
char apply_modifiers_extended(char c, uint8_t modifiers); // Needs to be declared if used internally, or defined before use
extern void terminal_handle_key_event(const KeyEvent event); // Callback function implemented in terminal.c

//============================================================================
// Keymap Data (Typically loaded from keymap.c or default here)
//============================================================================
extern const uint16_t keymap_us_qwerty[128]; // Use extern if defined elsewhere
#define DEFAULT_KEYMAP_US keymap_us_qwerty // Use the external definition

//============================================================================
// KBC Helper Functions (Implementations use defines from keyboard_hw.h)
//============================================================================

/**
 * @brief Introduces a short busy-wait delay.
 * @note Calibrate loop count based on target machine speed if needed.
 */
static void very_short_delay(void) {
    // Loop count might need adjustment depending on CPU speed and optimization level
    for (volatile int i = 0; i < 50000; ++i) {
        asm volatile("pause"); // Hint to CPU this is a spin-wait loop
    }
}

/**
 * @brief Waits until the KBC Input Buffer (IBF) is empty (ready to accept data/cmd).
 * @note Includes a timeout mechanism and serial logging on timeout.
 */
static inline void kbc_wait_for_send_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    // Wait while bit 1 (IBF) of the status register is set
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IBF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WaitSend TIMEOUT] Status: 0x"); serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}

/**
 * @brief Waits until the KBC Output Buffer (OBF) is full (data available to read).
 * @note Includes a timeout mechanism and serial logging on timeout.
 */
static inline void kbc_wait_for_recv_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    // Wait while bit 0 (OBF) of the status register is clear
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WaitRecv TIMEOUT] Status: 0x"); serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}

/**
 * @brief Reads a byte from the KBC Data Port (0x60) after waiting for OBF.
 * @return The byte read from the data port.
 */
static uint8_t kbc_read_data(void) {
    kbc_wait_for_recv_ready(); // Wait for data to be available
    return inb(KBC_DATA_PORT); // Read from port 0x60
}

/**
 * @brief Writes a byte to the KBC Data Port (0x60) after waiting for IBF clear.
 * @param data The byte to write (e.g., command/data to keyboard device).
 */
static void kbc_send_data_port(uint8_t data) {
    kbc_wait_for_send_ready(); // Wait for KBC to be ready
    outb(KBC_DATA_PORT, data);  // Write to port 0x60
}

/**
 * @brief Writes a byte to the KBC Command Port (0x64) after waiting for IBF clear.
 * @param cmd The command byte to send to the KBC controller.
 */
static void kbc_send_command_port(uint8_t cmd) {
    kbc_wait_for_send_ready(); // Wait for KBC to be ready
    outb(KBC_CMD_PORT, cmd);   // Write to port 0x64
}

/**
 * @brief Reads a byte from the KBC data port, expecting an ACK (0xFA).
 * @param command_name Name of the command sent (for logging).
 * @return true if ACK received, false otherwise (logs warning/error).
 */
static bool kbc_expect_ack(const char* command_name) {
    uint8_t resp = kbc_read_data(); // Includes wait
    if (resp == KB_RESP_ACK) {
        serial_write("[KB Debug] ACK (0xFA) for "); serial_write(command_name); serial_write(".\n");
        return true;
    } else if (resp == KB_RESP_RESEND) {
        serial_write("[KB Debug WARNING] RESEND (0xFE) for "); serial_write(command_name); serial_write(".\n");
        // Optionally add retry logic here
    } else {
        serial_write("[KB Debug WARNING] Unexpected 0x"); serial_print_hex(resp);
        serial_write(" for "); serial_write(command_name); serial_write(" (expected ACK 0xFA).\n");
    }
    return false;
}


//============================================================================
// Interrupt Handler
//============================================================================
/**
 * @brief IRQ1 handler for the PS/2 Keyboard.
 * Reads scancode, processes prefixes (0xE0, 0xE1), determines key press/release,
 * maps scancode to KeyCode, updates modifier state, buffers the event,
 * and calls the registered callback.
 * @param frame Pointer to the interrupt stack frame (unused).
 */
static void keyboard_irq1_handler(isr_frame_t *frame) {
    (void)frame; // Mark unused parameter
    uint8_t status_before_read = inb(KBC_STATUS_PORT);

    // Check if Output Buffer Full bit is set in status
    if (!(status_before_read & KBC_SR_OBF)) {
        // serial_write("[KB IRQ] Spurious IRQ1? OBF not set.\n"); // Optional log
        return; // Nothing to read
    }

    // Read scancode from data port
    uint8_t scancode = inb(KBC_DATA_PORT);
    serial_write("[KB IRQ] Scancode=0x"); serial_print_hex(scancode); serial_write(" "); // Debug log

    // --- Add debug print for raw scancode (as suggested in verification step) ---
    // serial_print_hex(scancode); serial_write(" ");
    // --- End debug print ---

    bool is_break_code; // Is it a key release event?

    // Handle Pause/Break key sequence (prefix 0xE1) - complex, often ignored or partially handled
    if (scancode == SCANCODE_PAUSE_PREFIX) {
        // serial_write("[KB IRQ] Pause Prefix (0xE1) detected - Ignoring sequence.\n");
        // TODO: Implement state machine for full Pause/Break if needed
        keyboard_state.extended_code_active = false; // Reset extended state
        return;
    }

    // Handle Extended Key sequence (prefix 0xE0)
    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        // serial_write("[KB IRQ] Extended Prefix (0xE0) detected.\n");
        keyboard_state.extended_code_active = true;
        return; // Wait for the next scancode which identifies the actual key
    }

    // Determine if it's a break code (key release) - highest bit (0x80) is set
    is_break_code = (scancode & 0x80) != 0;
    uint8_t base_scancode = scancode & 0x7F; // Mask off the break code bit to get base scancode

    KeyCode kc = KEY_UNKNOWN; // Initialize KeyCode

    // Process based on whether an extended prefix (0xE0) was just received
    if (keyboard_state.extended_code_active) {
        // Handle known extended keys
        switch (base_scancode) {
            // Map common extended keys (add more as needed based on keyboard_hw.h/keymap.h)
            case 0x1D: kc = KEY_CTRL; break;   // Right Ctrl (often shares code with left)
            case 0x38: kc = KEY_ALT; break;    // Right Alt (AltGr) (often shares code with left)
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
            case 0x1C: kc = '\n'; break;      // Keypad Enter
            case 0x35: kc = '/'; break;       // Keypad Divide (as example, map depends on keymap)
            // Handle other E0 keys like Windows keys if desired (map to KEY_SUPER or KEY_UNKNOWN)
            default:   kc = KEY_UNKNOWN; break;
        }
        keyboard_state.extended_code_active = false; // Reset extended state after processing
    } else {
        // Handle normal keys using the current keymap
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }

    // If KeyCode is unknown (not mapped), ignore the scancode
    if (kc == KEY_UNKNOWN) {
         serial_write("(KC_UNKNOWN)\n"); // Log unknown key codes
         return;
    }
    serial_write("\n"); // Finish the debug log line if key was known

    // Update general key state array (if KeyCode is within bounds)
    if (kc < KEY_COUNT) {
        keyboard_state.key_states[kc] = !is_break_code; // true for press, false for release
    }

    // Update modifier states based on the KeyCode
    switch (kc) {
        case KEY_LEFT_SHIFT: // Fallthrough
        case KEY_RIGHT_SHIFT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_SHIFT) : (keyboard_state.modifiers | MOD_SHIFT);
            break;
        case KEY_CTRL: // Covers both Left and Right Ctrl if mapped to the same KeyCode
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_CTRL) : (keyboard_state.modifiers | MOD_CTRL);
            break;
        case KEY_ALT: // Covers both Left and Right Alt (AltGr) if mapped to the same KeyCode
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_ALT) : (keyboard_state.modifiers | MOD_ALT);
            break;
        case KEY_CAPS:
            if (!is_break_code) keyboard_state.modifiers ^= MOD_CAPS; // Toggle Caps Lock on press only
            break;
        case KEY_NUM:
            if (!is_break_code) keyboard_state.modifiers ^= MOD_NUM; // Toggle Num Lock on press only
            break;
        case KEY_SCROLL:
            if (!is_break_code) keyboard_state.modifiers ^= MOD_SCROLL; // Toggle Scroll Lock on press only
            break;
        default:
            // Not a modifier key
            break;
    }

    // Create the event structure
    KeyEvent event = {
        .code = kc,
        .action = is_break_code ? KEY_RELEASE : KEY_PRESS,
        .modifiers = keyboard_state.modifiers,
        .timestamp = get_pit_ticks() // Get timestamp from PIT (or other time source)
    };

    // --- Add event to the circular buffer ---
    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) {
        // Buffer full: Overwrite oldest event (simplest strategy)
         serial_write("[KB WARNING] Keyboard buffer overflow! Discarding oldest event.\n");
         keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
    }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);

    // --- Call the registered callback function (if any) ---
    if (keyboard_state.event_callback) {
        keyboard_state.event_callback(event); // Pass the event details
    }
}

// Add this helper function within src/keyboard.c (or ensure it's declared if placed elsewhere)
/**
 * @brief Polls the KBC status register until specific bits are CLEAR.
 * @param mask The bitmask of flags to wait for (wait until (status & mask) == 0).
 * @param timeout Maximum number of loops to wait.
 * @param context Debugging string for timeout messages.
 * @return 0 on success (bits cleared), -1 on timeout.
 */
 static int kbc_poll_status_clear(uint8_t mask, uint32_t timeout, const char* context) {
    while (timeout--) {
        uint8_t status = inb(KBC_STATUS_PORT);
        if (!(status & mask)) { // Wait until the specific mask bits are CLEAR
            return 0; // Success
        }
        asm volatile("pause"); // Hint CPU we are spinning
    }
    // Use %lu for uint32_t based on previous compiler warnings
    terminal_printf("[KB Poll Clear TIMEOUT] Waiting for mask 0x%x clear (%s). Last Status=0x%x\n",
                     mask, context ? context : "N/A", inb(KBC_STATUS_PORT));
    return -1; // Timeout
}


//============================================================================
// Initialization (v6.0 - Added Scan Code Set 1 command + Polling Waits + INH Poll)
//============================================================================
void keyboard_init(void) {
    serial_write("[KB Init] Initializing keyboard driver (v6.0 - Set Scancode Set 1)...\n");
    memset(&keyboard_state, 0, sizeof(keyboard_state));
    spinlock_init(&keyboard_state.buffer_lock);
    // Load default keymap
    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(DEFAULT_KEYMAP_US));
    serial_write("[KB Init] Default US keymap loaded.\n");

    uint8_t status;
    // uint8_t dummy_read; // Unused variable removed

    // --- 1. Flush Output Buffer ---
    serial_write("  Flushing KBC Output Buffer...\n");
    int flush_count = 0;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_OBF) && flush_count < 100) {
        (void)inb(KBC_DATA_PORT);
        very_short_delay();
        flush_count++;
    }
    status = inb(KBC_STATUS_PORT);
    serial_write("  Status after OBF flush: 0x"); serial_print_hex(status); serial_write("\n");

    // --- 2. Disable Keyboard Interface ---
    serial_write("  Sending 0xAD (Disable KB Interface)...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE);
    very_short_delay();
    // Optional: Flush OBF after command if needed
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) { (void)inb(KBC_DATA_PORT); }

    // --- 3. Disable Mouse Interface ---
    serial_write("  Sending 0xA7 (Disable Mouse Interface)...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_command_port(KBC_CMD_DISABLE_MOUSE_IFACE);
    very_short_delay();
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) { (void)inb(KBC_DATA_PORT); } // Flush potential response

    // --- 4. Perform KBC Self-Test ---
    serial_write("  Sending 0xAA (Self-Test)...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_command_port(KBC_CMD_SELF_TEST);
    uint8_t test_result = kbc_read_data(); // Includes wait for OBF
    serial_write("  KBC Test Result: 0x"); serial_print_hex(test_result); serial_write(test_result == KBC_RESP_SELF_TEST_PASS ? " (PASS)\n" : " (FAIL/WARN)\n");
    very_short_delay();

    // --- 5. Set KBC Configuration Byte ---
    serial_write("  Reading KBC Config (0x20)...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t config = kbc_read_data(); // Includes wait for OBF
    serial_write("  Read Config: 0x"); serial_print_hex(config); serial_write("\n");

    // Ensure Keyboard Interface Enabled (Bit 4=0), Keyboard Interrupt Enabled (Bit 0=1)
    uint8_t new_config = (config | KBC_CFG_INT_KB) & ~KBC_CFG_DISABLE_KB;
    // Optionally ensure mouse interrupt is disabled (Bit 1=0)
    new_config &= ~KBC_CFG_INT_MOUSE;

    if (config != new_config) {
        serial_write("  Writing KBC Config 0x"); serial_print_hex(new_config); serial_write(" (0x60 cmd, data)...\n");
        kbc_wait_for_send_ready(); // <<< WAIT ADDED
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG); // Send 0x60
        kbc_wait_for_send_ready(); // <<< WAIT ADDED
        kbc_send_data_port(new_config); // Send the config byte
        very_short_delay();
    } else {
        serial_write("  KBC Config byte 0x"); serial_print_hex(config); serial_write(" already has desired settings (KB Int Enabled, KB Iface Enabled).\n");
    }
    status = inb(KBC_STATUS_PORT); serial_write("  Status after Config Write/Check: 0x"); serial_print_hex(status); serial_write("\n");

    // --- 6. Explicitly Enable Keyboard Interface (Command 0xAE) ---
    serial_write("  Sending Explicit 0xAE (Enable KB Interface)...\n");
    kbc_wait_for_send_ready(); // Wait IBF=0 BEFORE sending command
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE); // Sends 0xAE to port 0x64
    kbc_wait_for_send_ready(); // Wait IBF=0 AFTER command sent (controller accepted)

    // <<< FIX: Add poll to wait for INH bit (0x10) to CLEAR in status >>>
    terminal_write("    Polling Status Port 0x64 until INH bit (0x10) is clear...\n");
    if (kbc_poll_status_clear(KBC_SR_INH, KBC_WAIT_TIMEOUT * 2, "EnableKB_INH_Clear") != 0) {
        terminal_printf("    [WARN] Timeout waiting for KBC Status INH bit to clear after 0xAE command! Status: 0x%x\n", inb(KBC_STATUS_PORT));
        // Log warning but proceed - maybe keyboard will work anyway or another command clears it.
    } else {
        terminal_printf("    KBC Status INH bit cleared successfully after 0xAE. Status: 0x%x\n", inb(KBC_STATUS_PORT));
    }
    very_short_delay(); // Keep existing delay

    // --- 7. Enable Scanning (Keyboard Device Command 0xF4) ---
    serial_write("  Sending 0xF4 (Enable Scanning) to Keyboard Device...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_data_port(KB_CMD_ENABLE_SCAN); // Send 0xF4 to port 0x60
    (void)kbc_expect_ack("Enable Scanning (0xF4)"); // Read ACK (includes wait for OBF)
    very_short_delay();
    // Optional: Flush OBF again after ACK
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) { (void)inb(KBC_DATA_PORT); }

    // --- 8. Set Scan Code Set to 1 ---
    serial_write("  Sending 0xF0 0x01 (Set Scancode Set 1)...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_data_port(KB_CMD_SET_SCANCODE_SET); // Send 0xF0 command
    if (kbc_expect_ack("Select Set (0xF0)")) {
        kbc_wait_for_send_ready(); // <<< WAIT ADDED
        kbc_send_data_port(0x01); // Send data byte: 0x01 for Set 1
        if (!kbc_expect_ack("Set 1 data (0x01)")) {
            serial_write("   WARNING: No ACK for Set 1 data byte! Switch may have failed.\n");
        } else {
            serial_write("   Keyboard ACKed Set 1 command sequence.\n");
        }
    } else {
         serial_write("   WARNING: No ACK for Select Set command! Keyboard might not support switching.\n");
    }
    very_short_delay();
    // Flush OBF after Set 1 sequence
    flush_count = 0;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_OBF) && flush_count < 10) {
         (void)inb(KBC_DATA_PORT);
         flush_count++;
    }
    if(flush_count > 0) { serial_write("   Flushed stray bytes after Set 1 command.\n"); }
    serial_write("  Keyboard hopefully switched to Scan Code Set 1.\n");

    // --- 9. FINAL CHECK ---
    serial_write("  Reading KBC Config (0x20) for final verification...\n");
    kbc_wait_for_send_ready(); // <<< WAIT ADDED
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t final_config = kbc_read_data(); // Includes wait for OBF
    serial_write("  Final KBC Config Read: 0x"); serial_print_hex(final_config); serial_write("\n");

    // Check critical config bits again
    if (final_config & KBC_CFG_DISABLE_KB) {
        serial_write(" (**FATAL: KBC Config Byte *still* shows Keyboard Interface DISABLED!**)\n");
        KERNEL_PANIC_HALT("Keyboard interface config bit (KBC_CFG_DISABLE_KB) could not be cleared/kept clear!");
    } else {
        serial_write("  (Config Byte shows Keyboard Interface ENABLED - OK)\n");
    }
    if (!(final_config & KBC_CFG_INT_KB)) {
         serial_write(" (**WARN: KBC Config Byte shows Keyboard Interrupt DISABLED! Check config logic.**)\n");
    }

    // Final flush of OBF
    if (inb(KBC_STATUS_PORT) & KBC_SR_OBF) {
        serial_write("  OBF set after final checks, flushing...\n");
        (void)inb(KBC_DATA_PORT);
    }

    // --- Register IRQ Handler & Callback ---
    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL);
    serial_write("[KB Init] IRQ1 handler registered.\n");
    keyboard_register_callback(terminal_handle_key_event);
    serial_write("[KB Init] Registered terminal handler as callback.\n");

    terminal_write("[Keyboard] Initialized (Attempted Set Scancode 1).\n"); // Updated final message
}


//============================================================================
// Public API Functions (No changes needed in these)
//============================================================================

/**
 * @brief Polls for a keyboard event from the internal buffer.
 * @param event Pointer to a KeyEvent struct to be filled if an event is available.
 * @return true if an event was retrieved, false if the buffer is empty.
 */
bool keyboard_poll_event(KeyEvent* event) {
    KERNEL_ASSERT(event != NULL, "NULL event pointer to keyboard_poll_event");
    bool event_found = false;
    // Protect buffer access with lock, save/restore IRQ state
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);

    if (keyboard_state.buf_head != keyboard_state.buf_tail) { // Check if buffer is not empty
        *event = keyboard_state.buffer[keyboard_state.buf_tail]; // Get event from tail
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE; // Advance tail
        event_found = true;
    }

    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    return event_found;
}

/**
 * @brief Checks if a specific key is currently held down.
 * @param key The KeyCode to check.
 * @return true if the key is down, false otherwise.
 */
bool keyboard_is_key_down(KeyCode key) {
    if (key >= KEY_COUNT) return false; // Bounds check
    // Reading a boolean array element is usually atomic enough, no lock needed here
    // unless updates could race in a very specific way (unlikely for simple state bool).
    return keyboard_state.key_states[key];
}

/**
 * @brief Gets the current state of modifier keys.
 * @return A bitmask (KeyModifier enum) representing active modifiers.
 */
uint8_t keyboard_get_modifiers(void) {
    // Reading uint8_t is atomic, no lock needed.
    return keyboard_state.modifiers;
}

/**
 * @brief Sets the keyboard LEDs (Scroll Lock, Num Lock, Caps Lock).
 * @param scroll Scroll Lock state (true=on).
 * @param num Num Lock state (true=on).
 * @param caps Caps Lock state (true=on).
 */
void keyboard_set_leds(bool scroll, bool num, bool caps) {
    uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
    kbc_send_data_port(KB_CMD_SET_LEDS); // Send 0xED command
    if (kbc_expect_ack("Set LEDs (0xED)")) {
        kbc_send_data_port(led_state); // Send LED state byte
        kbc_expect_ack("Set LEDs Data Byte"); // Expect ACK for data byte
    }
}

/**
 * @brief Sets the keyboard layout map to be used for scancode translation.
 * @param keymap Pointer to a 128-entry array mapping scancodes to KeyCodes.
 */
void keyboard_set_keymap(const uint16_t* keymap) {
    KERNEL_ASSERT(keymap != NULL, "NULL keymap passed to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    memcpy(keyboard_state.current_keymap, keymap, sizeof(keyboard_state.current_keymap));
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    serial_write("[KB] Keymap updated.\n");
}

/**
 * @brief Sets the keyboard typematic rate and delay.
 * @param delay Delay code (0=250ms, 1=500ms, 2=750ms, 3=1000ms).
 * @param speed Speed code (0=30.0cps .. 31=2.0cps).
 */
void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
    delay &= 0x03; // Ensure delay is within range 0-3
    speed &= 0x1F; // Ensure speed is within range 0-31
    kbc_send_data_port(KB_CMD_SET_TYPEMATIC); // Send 0xF3 command
    if (kbc_expect_ack("Set Typematic (0xF3)")) {
        kbc_send_data_port((delay << 5) | speed); // Send combined rate/delay byte
        kbc_expect_ack("Set Typematic Data Byte"); // Expect ACK for data byte
    }
}

/**
 * @brief Registers a callback function to be invoked when a key event occurs.
 * @param callback Pointer to the function (void (*func)(KeyEvent)). Set to NULL to disable.
 */
void keyboard_register_callback(void (*callback)(KeyEvent)) {
    // Protect access to the callback pointer
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    keyboard_state.event_callback = callback;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
}

/**
 * @brief Applies shift and caps lock modifiers to a character.
 * (Provided as a utility, potentially useful in callbacks or terminal)
 * @param c The base character (e.g., 'a').
 * @param modifiers The current modifier bitmask.
 * @return The modified character (e.g., 'A' if Shift/Caps applies).
 * @note This is a simplified US-layout oriented version.
 */
char apply_modifiers_extended(char c, uint8_t modifiers) {
     bool shift = (modifiers & MOD_SHIFT) != 0;
     bool caps  = (modifiers & MOD_CAPS) != 0;

     // Apply Shift/Caps Lock to letters
     if (c >= 'a' && c <= 'z') {
         return (shift ^ caps) ? (c - 'a' + 'A') : c; // XOR handles Caps Lock interaction
     }
     if (c >= 'A' && c <= 'Z') {
         // If Caps Lock is on, Shift reverses it for letters
         return (shift ^ caps) ? (c - 'A' + 'a') : c;
     }

     // Apply Shift to numbers and symbols (US QWERTY assumed)
     if (shift) {
         switch (c) {
             case '1': return '!'; case '2': return '@'; case '3': return '#';
             case '4': return '$'; case '5': return '%'; case '6': return '^';
             case '7': return '&'; case '8': return '*'; case '9': return '(';
             case '0': return ')'; case '-': return '_'; case '=': return '+';
             case '[': return '{'; case ']': return '}'; case '\\': return '|';
             case ';': return ':'; case '\'': return '"'; case ',': return '<';
             case '.': return '>'; case '/': return '?'; case '`': return '~';
             // Add other shifted symbols based on the loaded keymap if necessary
         }
     }

     // Could add Ctrl handling here if needed (e.g., map Ctrl+C to ETX)

     return c; // Return original character if no applicable modifier
}