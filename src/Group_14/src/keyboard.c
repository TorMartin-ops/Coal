/**
 * keyboard.c
 *
 * Updated to use isr_frame_t for the interrupt handler.
 */

 #include "types.h"
 #include "keyboard.h"
 #include "idt.h"            // For interrupt handler registration
 #include <isr_frame.h>      // <<< ADDED: Include the frame definition
 #include "terminal.h"       // For echo output (if desired)
 #include "port_io.h"        // For inb/outb
 #include "pit.h"            // For get_pit_ticks() timestamp
 #include "string.h"         // For memset and memcpy
 
 /* I/O port definitions for the keyboard controller */
 #define KEYBOARD_DATA_PORT 0x60
 #define KEYBOARD_CMD_PORT  0x64
 
 /* Buffer size for circular event buffer */
 #define KB_BUFFER_SIZE 256
 
 /* Internal structure to hold keyboard state */
 static struct {
     bool key_states[KEY_COUNT];       // Tracks whether each key is pressed.
     uint8_t modifiers;                // Bitmask of active modifier keys.
     KeyEvent buffer[KB_BUFFER_SIZE];  // Circular buffer for key events.
     uint8_t buf_head;                 // Next write index.
     uint8_t buf_tail;                 // Next read index.
     uint16_t current_keymap[128];     // Active keymap for scancode translation.
     bool extended;                    // True if an extended scancode prefix (0xE0) is encountered.
     bool break_code;                  // True if current scancode indicates key release.
     void (*callback)(KeyEvent);       // Optional callback for each key event.
 } keyboard;
 
 // Forward declaration for the keyboard handler
 static void keyboard_handler(isr_frame_t *frame); // <<< UPDATED: Use isr_frame_t and add declaration
 
 /**
  * apply_modifiers_extended - Adjusts a raw key character based on active modifiers.
  * (Function body assumed unchanged)
  */
 char apply_modifiers_extended(char c, uint8_t modifiers) {
     bool shift = (modifiers & MOD_SHIFT) != 0;
     bool caps  = (modifiers & MOD_CAPS) != 0;
     bool altgr = (modifiers & MOD_ALT_GR) != 0;
 
     /* Handle alphabetic letters */
     if (c >= 'a' && c <= 'z') {
         if (shift ^ caps)
             return c - 'a' + 'A';
         return c;
     }
     if (c >= 'A' && c <= 'Z') {
         if (shift ^ caps)
             return c - 'A' + 'a';
         return c;
     }
 
     /* Handle digits and punctuation when Shift is active */
     if (shift) {
         switch (c) {
             case '1': return '!';
             case '2': return '@';
             case '3': return '#';
             case '4': return '$';
             case '5': return '%';
             case '6': return '^';
             case '7': return '&';
             case '8': return '*';
             case '9': return '(';
             case '0': return ')';
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
             default: break;
         }
     }
 
     /* Handle AltGr mappings (sample mappings; adjust as needed) */
     if (altgr) {
         switch (c) {
             case 'q': return '@';    // Example: AltGr+q => '@'
             case 'e': return '?';    // Fallback for AltGr+e (ideally '€', but limited to char)
             case '2': return '?';    // Fallback for AltGr+2 (ideally superscript 2)
             default: break;
         }
     }
 
     /* Return the original character if no modifiers affect it */
     return c;
 }
 
 /**
  * DEFAULT_KEYMAP
  * (Constant definition assumed unchanged)
  */
 static const uint16_t DEFAULT_KEYMAP[128] = {
     [0x00] = KEY_UNKNOWN,
     [0x01] = KEY_ESC,
     [0x02] = '1',  // Was KEY_1
     [0x03] = '2',  // Was KEY_2
     [0x04] = '3',  // Was KEY_3
     [0x05] = '4',  // Was KEY_4
     [0x06] = '5',  // Was KEY_5
     [0x07] = '6',  // Was KEY_6
     [0x08] = '7',  // Was KEY_7
     [0x09] = '8',  // Was KEY_8
     [0x0A] = '9',  // Was KEY_9
     [0x0B] = '0',  // Was KEY_0
     [0x0C] = '-',
     [0x0D] = '=',            // '=' key.
      [0x0E] = '\b',
      [0x0F] = '\t',
      [0x10] = 'q',
      [0x11] = 'w',
      [0x12] = 'e',
      [0x13] = 'r',
      [0x14] = 't',
      [0x15] = 'y',
      [0x16] = 'u',
      [0x17] = 'i',
      [0x18] = 'o',
      [0x19] = 'p',
      [0x1A] = '[',
      [0x1B] = ']',
      [0x1C] = '\n',           // Enter key.
      [0x1D] = KEY_CTRL,       // Left Control.
      [0x1E] = 'a',
      [0x1F] = 's',
      [0x20] = 'd',
      [0x21] = 'f',
      [0x22] = 'g',
      [0x23] = 'h',
      [0x24] = 'j',
      [0x25] = 'k',
      [0x26] = 'l',
      [0x27] = ';',
      [0x28] = '\'',
      [0x29] = '`',
      [0x2A] = KEY_LEFT_SHIFT,
      [0x2B] = '\\',
      [0x2C] = 'z',
      [0x2D] = 'x',
      [0x2E] = 'c',
      [0x2F] = 'v',
      [0x30] = 'b',
      [0x31] = 'n',
      [0x32] = 'm',
      [0x33] = ',',
      [0x34] = '.',
      [0x35] = '/',
      [0x36] = KEY_RIGHT_SHIFT,
      [0x37] = KEY_UNKNOWN,    // Typically keypad '*'
      [0x38] = KEY_ALT,        // Left Alt.
      [0x39] = ' ',            // Space bar.
      [0x3A] = KEY_CAPS,       // Caps Lock.
      // Remaining scancodes default to KEY_UNKNOWN.
      [0x3B ... 0x7F] = KEY_UNKNOWN
 };
 
 /**
  * keyboard_handler
  *
  * IRQ handler for the keyboard (typically connected to IRQ1, mapped to vector 33).
  * Reads the scancode, processes special prefixes, updates modifier keys,
  * translates the scancode into a KeyCode using the active keymap, and enqueues a KeyEvent.
  * The default callback (if registered) will handle echoing and further processing.
  *
  * @param frame Pointer to the interrupt stack frame.
  */
  static void keyboard_handler(isr_frame_t *frame) { // <<< UPDATED: Use isr_frame_t*
     (void)frame; // Mark frame as unused for now (might use int_no or err_code later)
 
     uint8_t scancode = inb(KEYBOARD_DATA_PORT);
 
     /* Handle extended scancode prefix */
     if (scancode == 0xE0) {
         keyboard.extended = true;
         return;
     }
     if (scancode == 0xE1) {
         // Future support for Pause/Break can be added here.
         return;
     }
 
     keyboard.break_code = (scancode & 0x80) != 0;
     uint8_t code = scancode & 0x7F;
 
     /* If an extended scancode was in effect, clear the flag.
        Optionally, adjust the code if your keymap needs to differentiate.
     */
     if (keyboard.extended) {
         keyboard.extended = false;
         // Adjust code if needed for extended keys (e.g., Right Alt, Arrow keys)
         // Example (needs specific KeyCodes defined):
         // if (code == 0x1D) kc = KEY_RIGHT_CTRL; // Example
         // if (code == 0x38) kc = KEY_ALT_GR;     // Example
     }
 
     KeyCode kc = keyboard.current_keymap[code];
     if (kc == KEY_UNKNOWN)
         return;
 
     // Update key state.
     keyboard.key_states[kc] = !keyboard.break_code;
 
     // Update modifiers.
     switch (kc) {
         case KEY_LEFT_SHIFT:
         case KEY_RIGHT_SHIFT:
             if (!keyboard.break_code)
                 keyboard.modifiers |= MOD_SHIFT;
             else
                 keyboard.modifiers &= ~MOD_SHIFT;
             break;
         case KEY_CTRL:
         // case KEY_RIGHT_CTRL: // If you add extended key handling
             if (!keyboard.break_code)
                 keyboard.modifiers |= MOD_CTRL;
             else
                 keyboard.modifiers &= ~MOD_CTRL;
             break;
         case KEY_ALT:
         // case KEY_ALT_GR: // If you add extended key handling
             if (!keyboard.break_code)
                 keyboard.modifiers |= MOD_ALT; // Or MOD_ALT_GR if ALT_GR keycode
             else
                 keyboard.modifiers &= ~(MOD_ALT | MOD_ALT_GR); // Clear both if needed
             break;
         case KEY_CAPS:
             if (!keyboard.break_code) {
                 // Toggle Caps Lock on key press.
                 keyboard.modifiers ^= MOD_CAPS;
             }
             break;
         default:
             break;
     }
 
     KeyEvent event = {
         .code = kc,
         .action = keyboard.break_code ? KEY_RELEASE : KEY_PRESS,
         .modifiers = keyboard.modifiers,
         .timestamp = get_pit_ticks() // Assuming get_pit_ticks exists and is suitable
     };
 
     /* Enqueue event into the circular buffer */
     keyboard.buffer[keyboard.buf_head] = event;
     keyboard.buf_head = (keyboard.buf_head + 1) % KB_BUFFER_SIZE;
     if (keyboard.buf_head == keyboard.buf_tail) {
         // Buffer overflow: discard oldest event.
         keyboard.buf_tail = (keyboard.buf_tail + 1) % KB_BUFFER_SIZE;
     }
 
     if (keyboard.callback)
         keyboard.callback(event);
 }
 
 /**
  * default_keyboard_callback - Default callback to echo key events.
  * (Function body assumed unchanged)
  */
 static void default_keyboard_callback(KeyEvent event) {
     if (event.action == KEY_PRESS) {
         char raw = (char)event.code;
         /* For control characters, do not process printable character conversion */
         if (raw >= ' ' && raw <= '~') {
             char adjusted = apply_modifiers_extended(raw, event.modifiers);
             terminal_write_char(adjusted);
         } else {
             // For control keys (newline, tab, backspace), echo as-is.
             // Special handling might be needed here depending on terminal logic
             if (raw == '\n') {
                 terminal_write_char('\n');
             } else if (raw == '\b') {
                 terminal_backspace(); // Assuming terminal has backspace function
             } else if (raw == '\t') {
                 terminal_write_char('\t'); // Or expand to spaces
             }
             // Ignore other non-printable keycodes in this default callback
         }
     }
 }
 
 /**
  * keyboard_init - Initializes the keyboard driver.
  *
  * Clears internal state, loads the default keymap, configures the keyboard controller,
  * registers the IRQ handler, and installs a default callback to echo key events.
  * Must be called during kernel initialization before interrupts are enabled.
  */
 void keyboard_init(void) {
     memset(&keyboard, 0, sizeof(keyboard));
     memcpy(keyboard.current_keymap, DEFAULT_KEYMAP, sizeof(DEFAULT_KEYMAP));
 
     // It's often better to wait for input buffer to be clear before sending commands
     // uint8_t status_reg;
     // do { status_reg = inb(KEYBOARD_CMD_PORT); } while (status_reg & 0x02);
 
     /* Configure keyboard controller (minimal example) */
     // Might need more robust init depending on hardware/BIOS state
     // e.g., disable/enable scanning, set scancode set, etc.
 
     // Enable the keyboard port (usually done by BIOS, but can be explicit)
     // outb(KEYBOARD_CMD_PORT, 0xAE); // Enable Keyboard Interface command
 
     // Optional: Read Controller Configuration Byte (CCB)
     // outb(KEYBOARD_CMD_PORT, 0x20); // Command: Read CCB
     // uint8_t ccb = inb(KEYBOARD_DATA_PORT);
     // ccb |= 0x01; // Ensure Keyboard Interrupt (IRQ1) is enabled
     // ccb &= ~0x10; // Ensure Keyboard Clock is enabled (if disabled)
     // ccb &= ~0x20; // Ensure PC/XT Translation is disabled (use Set 1)
     // outb(KEYBOARD_CMD_PORT, 0x60); // Command: Write CCB
     // outb(KEYBOARD_DATA_PORT, ccb);
 
     /* Register the IRQ handler for the keyboard (IRQ1 -> vector 33) */
     // Note: register_int_handler expects a void(*)(isr_frame_t*) now
     register_int_handler(33, keyboard_handler, NULL); // <<< This now matches the function definition
 
     /* Register default callback for echoing key events */
     keyboard_register_callback(default_keyboard_callback);
 
     terminal_write("[Keyboard] Initialized.\n"); // Add init message
 }
 
 /**
  * keyboard_poll_event - Retrieves the next pending key event from the circular buffer.
  * (Function body assumed unchanged)
  */
 bool keyboard_poll_event(KeyEvent* event) {
     if (keyboard.buf_head == keyboard.buf_tail)
         return false;
     *event = keyboard.buffer[keyboard.buf_tail];
     keyboard.buf_tail = (keyboard.buf_tail + 1) % KB_BUFFER_SIZE;
     return true;
 }
 
 /**
  * keyboard_is_key_down - Checks if a specific key is currently pressed.
  * (Function body assumed unchanged)
  */
 bool keyboard_is_key_down(KeyCode key) {
     if (key >= KEY_COUNT)
         return false;
     return keyboard.key_states[key];
 }
 
 /**
  * keyboard_get_modifiers - Retrieves the current state of modifier keys.
  * (Function body assumed unchanged)
  */
 uint8_t keyboard_get_modifiers(void) {
     return keyboard.modifiers;
 }
 
 /**
  * keyboard_set_leds - Sets the state of the keyboard LEDs (Scroll Lock, Num Lock, Caps Lock).
  * (Function body assumed unchanged)
  */
 void keyboard_set_leds(bool scroll, bool num, bool caps) {
     // Might need delay/ack checking for robustness
     uint8_t led_state = (scroll ? 1 : 0) | (num ? 2 : 0) | (caps ? 4 : 0);
     outb(KEYBOARD_DATA_PORT, 0xED); // Command to set LED state.
     // Wait for ACK (0xFA) ? Requires reading data port. Simplified here.
     outb(KEYBOARD_DATA_PORT, led_state);
 }
 
 /**
  * keyboard_set_keymap - Updates the active keymap used for scancode translation.
  * (Function body assumed unchanged)
  */
 void keyboard_set_keymap(const uint16_t* keymap) {
     memcpy(keyboard.current_keymap, keymap, sizeof(keyboard.current_keymap));
 }
 
 /**
  * keyboard_set_repeat_rate - Configures key repeat delay and speed.
  * (Function body assumed unchanged)
  */
 void keyboard_set_repeat_rate(uint8_t delay, uint8_t speed) {
     // Might need delay/ack checking
     outb(KEYBOARD_DATA_PORT, 0xF3); // Command to set repeat rate.
     outb(KEYBOARD_DATA_PORT, (delay << 5) | speed); // Combine delay/speed into one byte for PS/2
 }
 
 /**
  * keyboard_register_callback - Registers a callback function for key events.
  * (Function body assumed unchanged)
  */
 void keyboard_register_callback(void (*callback)(KeyEvent)) {
     keyboard.callback = callback;
 }