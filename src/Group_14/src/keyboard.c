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
 * - v6.1:
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
    uint16_t    current_keymap[128]; // Holder det aktive keymap
    bool        extended_code_active; // For E0 scancodes
    void        (*event_callback)(KeyEvent); // Funksjonspeker for callback
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
char apply_modifiers_extended(char c, uint8_t modifiers); // Erklært her
// Callback funksjonen (terminal_handle_key_event) er ekstern
extern void terminal_handle_key_event(const KeyEvent event);

//============================================================================
// Keymap Data
//============================================================================
// Anta at keymap_us_qwerty er definert i keymap.c
extern const uint16_t keymap_us_qwerty[128];
#define DEFAULT_KEYMAP_US keymap_us_qwerty

//============================================================================
// KBC Helper Functions
//============================================================================

static void very_short_delay(void) {
    // Enkel busy-wait for korte forsinkelser, f.eks. etter KBC-kommandoer
    for (volatile int i = 0; i < 50000; ++i) { // Juster antall iterasjoner etter behov
        asm volatile("pause"); // Hint til CPU-en
    }
}

static inline void kbc_wait_for_send_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    // Vent til Input Buffer Full (IBF) bit (bit 1) i statusregisteret er 0
    while ((inb(KBC_STATUS_PORT) & KBC_SR_IBF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WaitSend TIMEOUT]\n");
    }
}

static inline void kbc_wait_for_recv_ready(void) {
    int timeout = KBC_WAIT_TIMEOUT;
    // Vent til Output Buffer Full (OBF) bit (bit 0) i statusregisteret er 1
    while (!(inb(KBC_STATUS_PORT) & KBC_SR_OBF) && timeout-- > 0) {
        asm volatile("pause");
    }
    if (timeout <= 0) {
        serial_write("[KB WaitRecv TIMEOUT]\n");
    }
}

static uint8_t kbc_read_data(void) {
    kbc_wait_for_recv_ready(); // Vent til data er klare
    return inb(KBC_DATA_PORT);   // Les fra dataport
}

static void kbc_send_data_port(uint8_t data) {
    kbc_wait_for_send_ready(); // Vent til KBC er klar til å motta
    outb(KBC_DATA_PORT, data);   // Skriv til dataport
}

static void kbc_send_command_port(uint8_t cmd) {
    kbc_wait_for_send_ready(); // Vent til KBC er klar til å motta kommando
    outb(KBC_CMD_PORT, cmd);     // Skriv til kommandoport
}

static void kbc_flush_output_buffer(const char* context) {
    int flush_count = 0;
    uint8_t status;
    while (((status = inb(KBC_STATUS_PORT)) & KBC_SR_OBF) && flush_count < KBC_MAX_FLUSH) {
        uint8_t discard = inb(KBC_DATA_PORT);
        serial_write("[KB Flush: "); serial_write(context);
        serial_write("] Discarded 0x"); serial_print_hex(discard);
        serial_write(" (Status was 0x"); serial_print_hex(status); serial_write(")\n");
        flush_count++;
        very_short_delay();
    }
    if (flush_count > 0) {
        serial_write("[KB Flush: "); serial_write(context);
        serial_write("] Flush loop finished. Final Status: 0x");
        serial_print_hex(inb(KBC_STATUS_PORT)); serial_write("\n");
    }
}

static bool kbc_expect_ack(const char* command_name) {
    uint8_t resp = kbc_read_data(); // Inkluderer venting
    if (resp == KB_RESP_ACK) { // 0xFA
        serial_write("[KB Debug] ACK (0xFA) for "); serial_write(command_name); serial_write(".\n");
        return true;
    } else {
        serial_write("[KB Debug WARNING] Unexpected 0x"); serial_print_hex(resp);
        serial_write(" for "); serial_write(command_name); serial_write(" (expected ACK 0xFA).\n");
        // Håndter KB_RESP_RESEND (0xFE) her hvis du vil prøve på nytt
        return false;
    }
}

//============================================================================
// Interrupt Handler
//============================================================================
static void keyboard_irq1_handler(isr_frame_t *frame) {
    (void)frame; // Unngå "unused parameter" advarsel hvis frame ikke brukes direkte

    // Les status FØR datalesing for å sjekke om OBF er satt
    uint8_t status_before_read = inb(KBC_STATUS_PORT);
    if (!(status_before_read & KBC_SR_OBF)) {
        // Hvis OBF ikke er satt, var avbruddet kanskje ikke fra keyboardet,
        // eller data ble allerede lest (f.eks. spuriøst avbrudd, eller race condition).
        // serial_write("[KB IRQ] OBF not set on entry, ignoring. Status: 0x"); serial_print_hex(status_before_read); serial_write("\n");
        return;
    }

    uint8_t scancode = inb(KBC_DATA_PORT); // Les skannekoden
    // *** AVKOMMENTER DENNE FOR Å SE HVER SKANNEKODE I SERIELL LOGG ***
    serial_write("[KB IRQ] Scancode=0x"); serial_print_hex(scancode); serial_write("\n");

    bool is_break_code; // Er det en "key release" kode?

    // Håndter spesielle prefix-koder
    if (scancode == SCANCODE_PAUSE_PREFIX) { // 0xE1, for Pause/Break (kompleks sekvens)
        keyboard_state.extended_code_active = false; // Reset E0-state
        // Pause/Break krever mer logikk for å håndtere hele sekvensen, ignoreres for nå
        return;
    }
    if (scancode == SCANCODE_EXTENDED_PREFIX) { // 0xE0
        keyboard_state.extended_code_active = true;
        return; // Vent på neste byte
    }

    is_break_code = (scancode & 0x80) != 0;    // Bit 7 satt indikerer break code
    uint8_t base_scancode = scancode & 0x7F;  // Masker vekk break-bit for oppslag

    KeyCode kc = KEY_UNKNOWN;

    if (keyboard_state.extended_code_active) {
        // Map E0-prefixed scancodes (piltaster, RCtrl, RAlt, etc.)
        switch (base_scancode) {
            // Eksempler (juster basert på keyboard_hw.h og KeyCode enum)
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
            // Legg til flere E0-koder etter behov
            default:   kc = KEY_UNKNOWN; break;
        }
        keyboard_state.extended_code_active = false; // Reset E0-state
    } else {
        // Map vanlige (ikke-E0) scancodes
        kc = (base_scancode < 128) ? keyboard_state.current_keymap[base_scancode] : KEY_UNKNOWN;
    }

    if (kc == KEY_UNKNOWN) {
        // serial_write("[KB IRQ] Unknown scancode processed: 0x"); serial_print_hex(scancode); serial_write("\n");
        return; // Ignorer ukjente koder
    }

    // Oppdater key_states og modifiers
    if (kc < KEY_COUNT) { // Sørg for at kc er innenfor grensene til key_states arrayet
        keyboard_state.key_states[kc] = !is_break_code;
    }

    switch (kc) {
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT:
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_SHIFT) : (keyboard_state.modifiers | MOD_SHIFT);
            break;
        case KEY_CTRL: // Dekker både LCtrl (fra keymap) og RCtrl (fra E0 mapping)
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_CTRL) : (keyboard_state.modifiers | MOD_CTRL);
            break;
        case KEY_ALT: // Dekker både LAlt (fra keymap) og RAlt/AltGr (fra E0 mapping)
            keyboard_state.modifiers = is_break_code ? (keyboard_state.modifiers & ~MOD_ALT) : (keyboard_state.modifiers | MOD_ALT);
            break;
        case KEY_CAPS:   if (!is_break_code) keyboard_state.modifiers ^= MOD_CAPS; break;
        case KEY_NUM:    if (!is_break_code) keyboard_state.modifiers ^= MOD_NUM; break;
        case KEY_SCROLL: if (!is_break_code) keyboard_state.modifiers ^= MOD_SCROLL; break;
        default: break; // Ingen endring i modifier for vanlige tegn
    }

    // Buffer event
    KeyEvent event = {
        .code = kc,
        .action = is_break_code ? KEY_RELEASE : KEY_PRESS,
        .modifiers = keyboard_state.modifiers,
        .timestamp = get_pit_ticks() // Hent nåværende tick-count
    };

    uintptr_t buffer_irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock);
    uint8_t next_head = (keyboard_state.buf_head + 1) % KB_BUFFER_SIZE;
    if (next_head == keyboard_state.buf_tail) { // Buffer full
        serial_write("[KB WARNING] Keyboard event buffer overflow! Oldest event dropped.\n");
        keyboard_state.buf_tail = (keyboard_state.buf_tail + 1) % KB_BUFFER_SIZE; // "Dropp" eldste
    }
    keyboard_state.buffer[keyboard_state.buf_head] = event;
    keyboard_state.buf_head = next_head;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, buffer_irq_flags);

    // Kall registrert callback (f.eks. terminal_handle_key_event)
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
    // Kopier default keymap (US QWERTY)
    memcpy(keyboard_state.current_keymap, DEFAULT_KEYMAP_US, sizeof(DEFAULT_KEYMAP_US));
    serial_write("  [KB Init] Default US keymap loaded.\n");

    // --- Step 1: Flush KBC Output Buffer ---
    // Dette er for å fjerne eventuelle ventende bytes fra KBC før vi starter konfigurasjonen.
    serial_write("  [KB Init] Flushing KBC Output Buffer (pre-init)...\n");
    kbc_flush_output_buffer("Pre-Init");

    // --- Step 2: Disable Keyboard and Mouse Interfaces (via KBC commands) ---
    serial_write("  [KB Init] Sending 0xAD (Disable KB Interface)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_KB_IFACE); // 0xAD
    very_short_delay(); // Gi KBC tid
    kbc_flush_output_buffer("After 0xAD"); // Les eventuell respons

    serial_write("  [KB Init] Sending 0xA7 (Disable Mouse Interface)...\n");
    kbc_send_command_port(KBC_CMD_DISABLE_MOUSE_IFACE); // 0xA7
    very_short_delay();
    kbc_flush_output_buffer("After 0xA7");

    // --- Step 3: Set KBC Configuration Byte ---
    // Vi vil: KB Int Enabled (IRQ1), Mouse Int Disabled, KB Interface Enabled (Clock Active), Translation Enabled
    serial_write("  [KB Init] Reading KBC Config Byte (0x20)...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG); // Kommando 0x20
    uint8_t current_config = kbc_read_data();   // Les nåværende konfigurasjonsbyte
    serial_write("  [KB Init] Read Config = 0x"); serial_print_hex(current_config); serial_write("\n");

    // Ønsket konfigurasjon:
    // Bit 0 (KB INT): 1 (Enable)
    // Bit 1 (Mouse INT): 0 (Disable)
    // Bit 4 (KB Disable Clock): 0 (Enable Interface)
    // Bit 5 (Mouse Disable Clock): 1 (Disable Interface) - valgfritt, men bra
    // Bit 6 (Translation): 1 (Enable, Scancode Set 1 -> Set 2 via KBC)
    uint8_t desired_config = (current_config | KBC_CFG_INT_KB | KBC_CFG_TRANSLATION)
                             & ~(KBC_CFG_INT_MOUSE | KBC_CFG_DISABLE_KB | KBC_CFG_DISABLE_MOUSE);
    // KBC_CFG_SYS_FLAG (Bit 2) og KBC_CFG_OVERRIDE_INH (Bit 3) bør vanligvis ikke endres av OS.

    if (current_config != desired_config) {
        serial_write("  [KB Init] Writing KBC Config Byte 0x"); serial_print_hex(desired_config); serial_write(" (Command 0x60)...\n");
        kbc_send_command_port(KBC_CMD_WRITE_CONFIG); // Kommando 0x60
        kbc_send_data_port(desired_config);          // Send den nye konfigurasjonsbyten
        very_short_delay(); // Gi KBC tid til å prosessere
        kbc_flush_output_buffer("After 0x60");
    } else {
        serial_write("  [KB Init] KBC Config Byte 0x"); serial_print_hex(current_config); serial_write(" is already correct.\n");
    }
    // Verifiser at konfigurasjonen ble skrevet
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t readback_config = kbc_read_data();
    serial_write("  [KB Init] Config Byte Readback = 0x"); serial_print_hex(readback_config); serial_write("\n");
    if (readback_config != desired_config) {
        KERNEL_PANIC_HALT("Failed to set KBC Configuration Byte!");
    }

    // --- Step 4: KBC Self-Test ---
    serial_write("  [KB Init] Performing KBC Self-Test (0xAA)...\n");
    kbc_send_command_port(KBC_CMD_SELF_TEST); // Kommando 0xAA
    uint8_t test_result = kbc_read_data();
    serial_write("  [KB Init] KBC Test Result = 0x"); serial_print_hex(test_result);
    if (test_result != KBC_RESP_SELF_TEST_PASS) { // 0x55
        serial_write(" (FAIL!)\n");
        KERNEL_PANIC_HALT("KBC Self-Test Failed!");
    } else {
        serial_write(" (PASS)\n");
    }
    very_short_delay();
    kbc_flush_output_buffer("After 0xAA"); // Kan komme ekstra bytes

    // --- Step 5: Enable Keyboard Interface (via KBC command) ---
    // Selv om config-biten (bit 4) er satt til 0 (enabled), skader det ikke å sende 0xAE.
    serial_write("  [KB Init] Sending 0xAE (Enable KB Interface)...\n");
    kbc_send_command_port(KBC_CMD_ENABLE_KB_IFACE); // 0xAE
    very_short_delay();
    kbc_flush_output_buffer("After 0xAE"); // Enkelte KBC-er kan sende ACK her

    // Verifiser KBC config byte igjen etter Enable kommando for å være sikker
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t config_after_enable = kbc_read_data();
    serial_write("  [KB Init] Config Byte after 0xAE = 0x"); serial_print_hex(config_after_enable); serial_write("\n");
    if (config_after_enable & KBC_CFG_DISABLE_KB) { // Bit 4 skal være 0
         KERNEL_PANIC_HALT("KBC Config still shows KB Disabled after 0xAE!");
    } else {
         serial_write("  [KB Init] Confirmed Config Byte OK after 0xAE.\n");
    }

    // --- Step 6: Reset Keyboard Device ---
    serial_write("  [KB Init] Sending 0xFF (Reset) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_RESET); // Kommando 0xFF til data port 0x60
    if (!kbc_expect_ack("KB Reset (0xFF)")) {
        serial_write("  [KB Init] Warning: No ACK for KB Reset command. Keyboard may not be initialized correctly.\n");
        // Fortsett, men vær obs på at tastaturet kan være i en ukjent tilstand
    } else {
        // Vent på BAT (Basic Assurance Test) resultat etter ACK
        serial_write("  [KB Init] Waiting for KB BAT result after Reset...\n");
        uint8_t bat_result = kbc_read_data(); // Bør være 0xAA (pass) eller 0xFC/FD (fail)
        serial_write("  [KB Init] Keyboard BAT Result = 0x"); serial_print_hex(bat_result);
        if (bat_result != KB_RESP_SELF_TEST_PASS) { // 0xAA
             serial_write(" (FAIL/WARN)\n");
             // Vurder KERNEL_PANIC her hvis BAT feiler, da tastaturet er kritisk.
        } else {
             serial_write(" (PASS)\n");
        }
    }
    very_short_delay();
    kbc_flush_output_buffer("After KB Reset");


    // --- Step 7: Enable Scanning (start sending keypresses) ---
    serial_write("  [KB Init] Sending 0xF4 (Enable Scanning) to Keyboard Device...\n");
    kbc_send_data_port(KB_CMD_ENABLE_SCAN); // Kommando 0xF4
    if (!kbc_expect_ack("Enable Scanning (0xF4)")) {
         serial_write("  [KB Init] Warning: Failed to get ACK for Enable Scanning! Keyboard may not send data.\n");
    } else {
         // Viktig: Les eventuelle data som kommer etter ACK (f.eks. fra QEMU emulering)
         very_short_delay();
         kbc_flush_output_buffer("After 0xF4 ACK");
         serial_write("  [KB Init] Scanning Enabled. ACK flushed.\n");
    }

    // --- Step 8: (REMOVED) Set Scan Code Set 1 ---
    // KBC_CFG_TRANSLATION (bit 6 i KBC config) er satt, så KBC bør oversette
    // scancodes fra tastaturet (typisk Set 2 eller 3) til Set 1 for OS-et.
    // Å sende 0xF0 0x01 her kan være unødvendig eller til og med forårsake problemer.
    // serial_write("  [KB Init] SKIPPING Set Scan Code Set 1 (0xF0 0x01).\n");


    // --- Step 9: Final Verification & Handler Registration ---
    serial_write("  [KB Init] Final KBC Config Check...\n");
    kbc_send_command_port(KBC_CMD_READ_CONFIG);
    uint8_t final_config_check = kbc_read_data();
    serial_write("  [KB Init] Final Config Readback = 0x"); serial_print_hex(final_config_check); serial_write("\n");
    if (final_config_check & KBC_CFG_DISABLE_KB) { // Bit 4 bør være 0
        KERNEL_PANIC_HALT("Keyboard Init Failed: Interface Disabled in final config check!");
    }
    if (!(final_config_check & KBC_CFG_INT_KB)) { // Bit 0 bør være 1
         serial_write("  [KB Init WARNING] Keyboard Interrupt Disabled in final config check!\n");
    }

    // Registrer IRQ Handler og Callback
    register_int_handler(IRQ1_VECTOR, keyboard_irq1_handler, NULL); // IRQ1 er vanligvis vector 33
    serial_write("  [KB Init] IRQ1 handler registered.\n");
    keyboard_register_callback(terminal_handle_key_event); // Sett terminal_handle_key_event som callback
    serial_write("  [KB Init] Registered terminal handler as callback.\n");

    terminal_write("[Keyboard] Initialized (v6.3 - ACK Flush, No SetScanCode).\n");
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
    if (key >= KEY_COUNT) return false; // Sjekk grenser
    // Les key_states atomisk eller med lås hvis nødvendig i et SMP-system
    // For uniprocessor er direkte lesing OK her, da IRQ-handleren er den eneste skriveren.
    return keyboard_state.key_states[key];
}

uint8_t keyboard_get_modifiers(void) {
    // Les modifiers atomisk eller med lås hvis nødvendig i SMP.
    return keyboard_state.modifiers;
}

void keyboard_set_leds(bool scroll, bool num, bool caps) {
    uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
    kbc_send_data_port(KB_CMD_SET_LEDS); // 0xED
    if (kbc_expect_ack("Set LEDs (0xED)")) {
        kbc_send_data_port(led_state);
        kbc_expect_ack("Set LEDs Data Byte"); // Forvent ny ACK etter data
    }
}

void keyboard_set_keymap(const uint16_t* keymap) {
    KERNEL_ASSERT(keymap != NULL, "NULL keymap passed to keyboard_set_keymap");
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock); // Beskytt tilgang til current_keymap
    memcpy(keyboard_state.current_keymap, keymap, sizeof(keyboard_state.current_keymap));
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
    serial_write("[KB] Keymap updated.\n");
}

void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
    // Typematic rate/delay: delay (00=250ms, 01=500ms, 10=750ms, 11=1s)
    // speed (00000=30.0cps ... 11111=2.0cps)
    delay &= 0x03; // Masker til 2 bits
    speed &= 0x1F; // Masker til 5 bits
    kbc_send_data_port(KB_CMD_SET_TYPEMATIC); // 0xF3
    if (kbc_expect_ack("Set Typematic (0xF3)")) {
        kbc_send_data_port((delay << 5) | speed);
        kbc_expect_ack("Set Typematic Data Byte");
    }
}

void keyboard_register_callback(void (*callback)(KeyEvent)) {
    uintptr_t irq_flags = spinlock_acquire_irqsave(&keyboard_state.buffer_lock); // Beskytt callback-pekeren
    keyboard_state.event_callback = callback;
    spinlock_release_irqrestore(&keyboard_state.buffer_lock, irq_flags);
}

// --- Modifier Application ---
// Denne funksjonen oversetter KeyCode (som kan være ASCII eller spesialkode)
// til et tegn, tatt hensyn til Shift og CapsLock.
// Den er mer korrekt plassert her eller i en keymap-utility fil.
char apply_modifiers_extended(char c, uint8_t modifiers) {
    bool shift = (modifiers & MOD_SHIFT) != 0;
    bool caps  = (modifiers & MOD_CAPS) != 0;

    // Hvis c er en bokstav
    if (c >= 'a' && c <= 'z') {
        return (shift ^ caps) ? (c - 'a' + 'A') : c; // Store bokstaver hvis Shift ELLER Caps (men ikke begge)
    }
    if (c >= 'A' && c <= 'Z') {
        return (shift ^ caps) ? (c - 'A' + 'a') : c; // Små bokstaver hvis Shift ELLER Caps (men ikke begge) og c var stor
    }

    // Hvis Shift er aktiv, sjekk for spesialtegn
    if (shift) {
        switch (c) {
            // US QWERTY layout for Shift-modifikatorer
            case '1': return '!';
            case '2': return '@'; // Eller '"' på UK/NO
            case '3': return '#'; // Eller '£' på UK
            case '4': return '$';
            case '5': return '%';
            case '6': return '^'; // Eller '&' på NO
            case '7': return '&'; // Eller '/' på NO
            case '8': return '*'; // Eller '(' på NO
            case '9': return '('; // Eller ')' på NO
            case '0': return ')'; // Eller '=' på NO
            case '-': return '_';
            case '=': return '+';
            case '[': return '{';
            case ']': return '}';
            case '\\': return '|';
            case ';': return ':';
            case '\'': return '"';
            case ',': return '<';
            case '.': return '>';
            case '/': return '?';
            case '`': return '~';
            // Norske taster med shift (basert på standard norsk layout)
            // Dette avhenger av hvordan keymap_norwegian returnerer grunntegnene
            // For eksempel, hvis keymap_norwegian[0x0C] (tasten for '+') returnerer '+',
            // så vil apply_modifiers med shift gi '?'
            // Hvis keymap_norwegian[0x0C] returnerer '\'', så vil apply_modifiers med shift gi '*'
            // Det er best om keymap returnerer grunntegnet, og denne funksjonen håndterer shift.
            // Her må vi vite hva grunntegnet er for å korrekt mappe shift.
            // For '+' tasten (scancode 0x0C i norsk keymap) som er '+' uten shift, blir '?' med shift.
            // Hvis vi antar at `c` allerede er det *u-shiftede* norske tegnet:
            // Eksempel: Hvis 'c' er '+' (fra 0x0C i keymap_norwegian), og shift er på.
            // Da skal den returnere '?'.
             // For now, this will only correctly handle US-like shift mappings.
             // A more robust solution needs to know the original scancode OR the layout.
        }
    }
    return c; // Returner originalt tegn hvis ingen modifikator gjelder
}

// END OF FILE