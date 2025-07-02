/**
 * @file keyboard.c
 * @brief PS/2 Keyboard Driver for CoalOS
 * @version 6.5.3 - Corrected build errors: KB_RESP_RESEND and kbc_expect_ack return path.
 */

//============================================================================
// Includes
//============================================================================
#include <kernel/drivers/input/keyboard.h>
#include <kernel/drivers/input/keyboard_hw.h> // Should define KB_RESP_ACK, etc.
#include <kernel/core/types.h>
#include <kernel/cpu/idt.h>          // For PIC_EOI, outb, PIC1_COMMAND, PIC2_COMMAND constants
#include <kernel/lib/port_io.h>      // For outb, inb
#include <kernel/cpu/isr_frame.h>
#include <kernel/drivers/display/terminal.h>
#include <kernel/drivers/timer/pit.h>          // For get_pit_ticks()
#include <kernel/lib/string.h>
#include <kernel/drivers/display/serial.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/assert.h>
#include <libc/stdbool.h>
#include <libc/stdint.h>
#include <libc/stddef.h>
#include <kernel/drivers/input/keymap.h>

//============================================================================
// Definitions and Constants
//============================================================================
#define KB_BUFFER_SIZE 256
#define KBC_WAIT_TIMEOUT 300000 
#define KBC_MAX_FLUSH 100       

#define DEFAULT_KEYMAP_US keymap_us_qwerty 

// Define KB_RESP_RESEND if not present in keyboard_hw.h
// Ideally, this should be in keyboard_hw.h
#ifndef KB_RESP_RESEND
#define KB_RESP_RESEND 0xFE
#endif

volatile uint32_t g_keyboard_irq_fire_count = 0;

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
static inline void pic_send_eoi(uint8_t irq);

//============================================================================
// PIC EOI Helper
//============================================================================
static inline void pic_send_eoi(uint8_t irq) {
    if (irq >= 8 && irq <= 15) { 
        outb(PIC2_COMMAND, PIC_EOI); 
    }
    outb(PIC1_COMMAND, PIC_EOI); 
}

//============================================================================
// KBC Helper Functions
//============================================================================
static void very_short_delay(void) {
    for (volatile int i = 0; i < 15000; ++i) { asm volatile("pause"); }
}
static inline void kbc_wait_for_send_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IBF) && timeout-- > 0) { io_wait(); }
    if (timeout <= 0) { /*serial_write("[KB WaitSend TIMEOUT]\n");*/ }
}
static inline void kbc_wait_for_recv_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF) && timeout-- > 0) { io_wait(); }
    if (timeout <= 0) { /*serial_write("[KB WaitRecv TIMEOUT]\n");*/ }
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
    (void)context; int flush_count = 0; uint8_t status_val;
    while (((status_val = inb(KBC_STATUS_PORT)) & KBC_SR_OBF) && flush_count < KBC_MAX_FLUSH) {
        (void)inb(KBC_DATA_PORT); flush_count++; io_wait();
    }
}

// Corrected kbc_expect_ack
static bool kbc_expect_ack(const char* command_name) {
    uint8_t resp = kbc_read_data();
    if (resp == KB_RESP_ACK) {
        return true;
    } else if (resp == KB_RESP_RESEND) { // Check for RESEND
        terminal_printf("[KB Debug INFO] Keyboard requested RESEND for %s (got 0xFE).\n", command_name);
        return false; // Indicate resend was requested, caller might retry or handle
    } else {
        terminal_printf("[KB Debug WARNING] Unexpected 0x%02x for %s (expected ACK 0xFA or RESEND 0xFE).\n", resp, command_name);
        return false; // Generic failure
    }
    // Fallthrough return false if neither ACK nor RESEND, though the else handles it.
    // Adding explicit return here for clarity or if the above conditions are changed.
    // return false; // This line is now effectively unreachable due to the else.
}

//============================================================================
// Interrupt Handler
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) {
    g_keyboard_irq_fire_count++;
    (void)frame;
    
    // Debug logging removed - keyboard interrupts verified working
    
    uint8_t status_from_kbc = inb(KBC_STATUS_PORT);

    if (!(status_from_kbc & KBC_SR_OBF)) {
        // No data in buffer - send EOI and return
        pic_send_eoi(1); 
        return; 
    }
    uint8_t scancode = inb(KBC_DATA_PORT);
    
    // Process scancode

    bool is_break_code;
    KeyCode kc = KEY_UNKNOWN;

    if (scancode == SCANCODE_PAUSE_PREFIX) {
        keyboard_state.extended_code_active = false; 
        pic_send_eoi(1); 
        return; 
    }
    if (scancode == SCANCODE_EXTENDED_PREFIX) { 
        keyboard_state.extended_code_active = true;
        pic_send_eoi(1); 
        return; 
    }

    is_break_code = (scancode & 0x80) != 0; 
    uint8_t base_scancode = scancode & 0x7F;   

    if (keyboard_state.extended_code_active) {
        switch (base_scancode) {
            case 0x1D: kc = KEY_CTRL; break; 
            case 0x38: kc = KEY_ALT;  break; 
            case 0x48: kc = KEY_UP;    break;
            case 0x50: kc = KEY_DOWN;  break;
            case 0x4B: kc = KEY_LEFT;  break;
            case 0x4D: kc = KEY_RIGHT; break;
            case 0x47: kc = KEY_HOME;   break;
            case 0x4F: kc = KEY_END;    break;
            case 0x49: kc = KEY_PAGE_UP; break;
            case 0x51: kc = KEY_PAGE_DOWN; break;
            case 0x52: kc = KEY_INSERT; break;
            case 0x53: kc = KEY_DELETE; break;
            case 0x1C: kc = KEY_ENTER;  break; 
            case 0x35: kc = '/';        break; 
            default:   kc = KEY_UNKNOWN; break;
        }
        keyboard_state.extended_code_active = false; 
    } else {
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }

    if ((kc == KEY_UNKNOWN || kc == 0) && base_scancode != 0) {
        pic_send_eoi(1); 
        return;
    }
    
    if ((int)kc >= 0 && kc < KEY_COUNT) {
        keyboard_state.key_states[(int)kc] = !is_break_code;
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

    KeyEvent event = {
        .code = kc,
        .action = is_break_code ? KEY_RELEASE : KEY_PRESS,
        .modifiers = keyboard_state.modifiers,
        .timestamp = get_pit_ticks() 
    };

    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) {
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE;
    }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);

    // Send EOI before calling callback to ensure interrupts continue even if callback blocks
    pic_send_eoi(1);
    
    if (keyboard_state.event_callback) {
        keyboard_state.event_callback(event);
    } 
}

//============================================================================
// KBC Configuration Re-check
//============================================================================
void keyboard_recheck_kbc_config(void) { 
    uint8_t target_kbc_config = 0x41; 
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
        kbc_flush_output_buffer("Post-Recheck-Write-Config");

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
    terminal_printf("[KB Init v%s] Initializing keyboard driver...\n", "6.5.2"); 
    memset(&keyboard_state, 0, sizeof(keyboard_state));
    spinlock_init(&keyboard_state.buffer_lock);

    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(keyboard_state.current_keymap));
    serial_write("  [KB Init] Default US keymap (Set 1 style) loaded. Device will use Set 2. KBC Translation will be ON (config 0x41).\n");

    serial_write("  [KB Init] Flushing KBC Output Buffer (pre-init)...\n");
    kbc_flush_output_buffer("Pre-Init Flush");

    serial_write("  [KB Init] Sending 0xAD (Disable KB Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE);
    very_short_delay(); 
    kbc_flush_output_buffer("After KBC 0xAD (Disable KB)");

    serial_write("  [KB Init] Sending 0xA7 (Disable Mouse Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_MOUSE_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xA7 (Disable Mouse)");

    serial_write("  [KB Init] Reading KBC Config Byte (0x20) before modification...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t current_config_init = kbc_read_data();
    serial_printf("  [KB Init] Read Config = 0x%02x\n", current_config_init);

    // CRITICAL FIX: Use 0x41, NOT 0x61!
    // Bit 4 (0x10) disables keyboard when set, so we must keep it clear!
    uint8_t desired_kbc_config = 0x41; 
    serial_printf("  [KB Init] Desired KBC Config: 0x%02x\n", desired_kbc_config);

    serial_printf("  [KB Init] Writing KBC Config Byte 0x%02x (Command 0x60)...\n", desired_kbc_config);
    kbc_send_command_port(KBC_CMD_WRITE_CONFIG);
    kbc_send_data_port(desired_kbc_config);
    very_short_delay();
    kbc_flush_output_buffer("Post-Config Write");

    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t readback_config = kbc_read_data();
    serial_printf("  [KB Init] Config Byte Readback after write = 0x%02x\n", readback_config);
    if (readback_config != desired_kbc_config) {
        terminal_printf("  [KB ERROR] Failed to set KBC Config Byte correctly! Expected 0x%02x, got 0x%02x.\n", desired_kbc_config, readback_config);
    }

    serial_write("  [KB Init] Performing KBC Self-Test (0xAA)...\n");
    kbc_send_command_port(KBC_CMD_SELF_TEST);
    uint8_t test_result = kbc_read_data();
    serial_printf("  [KB Init] KBC Test Result = 0x%02x", test_result);
    if (test_result != KBC_RESP_SELF_TEST_PASS) { 
        terminal_write(" (KBC SELF-TEST FAIL!)\n");
    } else {
        terminal_write(" (PASS)\n");
    }
    kbc_flush_output_buffer("After KBC 0xAA (Self-Test)");

    serial_write("  [KB Init] Sending 0xAE (Enable KB Interface - KBC cmd)...\n");
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE);
    very_short_delay();
    kbc_flush_output_buffer("After KBC 0xAE (Enable KB)");

    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t config_after_kb_enable = kbc_read_data();
    serial_printf("  [KB Init] Config Byte after KB Enable cmd (0xAE) = 0x%02x\n", config_after_kb_enable);
    if (config_after_kb_enable & KBC_CFG_DISABLE_KB) { 
         terminal_printf("  [KB ERROR] KBC Config shows KB Clock still Disabled after 0xAE command!\n");
    } else {
         serial_write("  [KB Init] Confirmed KB Clock Enabled in Config Byte.\n");
    }

    serial_write("  [KB Init] Sending 0xFF (Reset) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_RESET); 
    if (!kbc_expect_ack("KB Device Reset (0xFF)")) {
        serial_write("  [KB Init] Warning: No ACK for KB Device Reset command. Keyboard may not respond.\n");
    } else {
        serial_write("  [KB Init] Waiting for KB BAT result after Reset...\n");
        uint8_t bat_result = kbc_read_data();
        serial_printf("  [KB Init] Keyboard BAT Result = 0x%02x", bat_result);
        if (bat_result != KB_RESP_SELF_TEST_PASS) { 
             terminal_write(" (KEYBOARD SELF-TEST FAIL/WARN)\n");
        } else {
             terminal_write(" (PASS)\n");
        }
    }
    kbc_flush_output_buffer("After KB Device Reset Sequence");

    // --- Modified Set Scancode Set Logic ---
    serial_write("  [KB Init] Setting Scancode Set 2 (0xF0, 0x02)...\n");
    kbc_send_data_port(KB_CMD_SET_SCANCODE_SET); 
    if (kbc_expect_ack("Set Scancode-Set CMD (0xF0)")) {
        kbc_send_data_port(0x02); 
        // Now explicitly wait for ACK for the data byte 0x02
        if (!kbc_expect_ack("Set Scancode-Set DATA (0x02)")) {
            serial_write("  [KB Init] Warning: Keyboard did not ACK scancode-set data byte (0x02) or requested RESEND.\n");
            // You could add retry logic here if resp was KB_RESP_RESEND
        }
        serial_write("  [KB Init] Scancode-Set #2 selected (commands sent).\n");
    } else {
        serial_write("  [KB Init] WARNING: Keyboard did not ACK scancode-set command (0xF0), leaving default.\n");
    }
    kbc_flush_output_buffer("After Set Scancode Set Sequence"); 

    serial_write("  [KB Init] Sending 0xF4 (Enable Scanning) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_ENABLE_SCAN);
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
         serial_write("  [KB Init] Warning: Failed to get ACK for Enable Scanning! Keyboard might not send keys.\n");
    } else {
         serial_write("  [KB Init] Scanning Enabled on device. ACK received.\n");
    }
    kbc_flush_output_buffer("After KB 0xF4 (Enable Scan)");

    serial_write("  [KB Init] Final KBC Config Check...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t final_config_check = kbc_read_data();
    serial_printf("  [KB Init] Final Config Readback = 0x%02x\n", final_config_check);
    if (final_config_check != desired_kbc_config) { 
         terminal_printf("  [KB WARNING] Final KBC Config (0x%02x) differs from desired (0x%02x)!\n", final_config_check, desired_kbc_config);
    }
    if (final_config_check & KBC_CFG_DISABLE_KB) { 
        terminal_printf("  [KB ERROR] FINAL CHECK: Keyboard Interface Clock is DISABLED (Config Bit 4 is SET)!\n");
    }
    if (!(final_config_check & KBC_CFG_INT_KB)) { 
         terminal_write("  [KB WARNING] FINAL CHECK: Keyboard Interrupt is DISABLED (Config Bit 0 is CLEAR)!\n");
    }
    if (!(final_config_check & KBC_CFG_TRANSLATION)) { 
        terminal_write("  [KB WARNING] FINAL CHECK: Scancode Translation is DISABLED (Config Bit 6 is CLEAR)! Expected ON for 0x41.\n");
    }

    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL);
    serial_write("  [KB Init] IRQ1 handler registered.\n");

    keyboard_register_callback(terminal_handle_key_event);
    serial_write("  [KB Init] Registered terminal handler as callback.\n");

    terminal_printf("[Keyboard] Initialized (v%s).\n", "6.5.2");
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
    if ((int)key >= 0 && key < KEY_COUNT) { 
        return keyboard_state.key_states[(int)key];
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
        return (shift ^ caps) ? c : (char)(c - 'A' + 'a');
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