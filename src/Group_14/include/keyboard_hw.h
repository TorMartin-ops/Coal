// include/keyboard_hw.h
#ifndef KEYBOARD_HW_H
#define KEYBOARD_HW_H

// --- KBC Ports ---
#define KBC_DATA_PORT    0x60 // Data Register (R/W)
#define KBC_STATUS_PORT  0x64 // Status Register (R)
#define KBC_CMD_PORT     0x64 // Command Register (W)

// --- KBC Status Register Bits (Port 0x64 Read) ---
#define KBC_SR_OBF       0x01 // Bit 0: Output Buffer Full (Data available from KBC/Device)
#define KBC_SR_IBF       0x02 // Bit 1: Input Buffer Full (KBC busy, don't write cmd/data)
#define KBC_SR_SYS_FLAG  0x04 // Bit 2: System Flag (POST status)
#define KBC_SR_A2        0x08 // Bit 3: Command/Data (0=Data byte written to 0x60 was for device, 1=Cmd byte written to 0x60 was for KBC)
#define KBC_SR_INH       0x10 // Bit 4: Inhibit Switch / Keyboard Interface Disabled (0=Enabled, 1=Disabled/Inhibited)
#define KBC_SR_MOUSE_OBF 0x20 // Bit 5: Mouse Output Buffer Full (PS/2 specific)
#define KBC_SR_TIMEOUT   0x40 // Bit 6: Timeout Error
#define KBC_SR_PARITY    0x80 // Bit 7: Parity Error

// --- KBC Commands (Port 0x64 Write) ---
#define KBC_CMD_READ_CONFIG         0x20 // Read KBC "Command Byte" (Configuration)
#define KBC_CMD_WRITE_CONFIG        0x60 // Write KBC "Command Byte" (Configuration)
#define KBC_CMD_DISABLE_MOUSE_IFACE 0xA7 // Disable Mouse Interface
#define KBC_CMD_ENABLE_MOUSE_IFACE  0xA8 // Enable Mouse Interface
#define KBC_CMD_TEST_MOUSE_IFACE    0xA9 // Test Mouse Interface
#define KBC_CMD_SELF_TEST           0xAA // KBC Self-Test (Controller)
#define KBC_CMD_KB_INTERFACE_TEST   0xAB // Keyboard Interface Test
#define KBC_CMD_DISABLE_KB_IFACE    0xAD // Disable Keyboard Interface
#define KBC_CMD_ENABLE_KB_IFACE     0xAE // Enable Keyboard Interface
#define KBC_CMD_READ_INPUT_PORT     0xC0 // Read Input Port
#define KBC_CMD_READ_OUTPUT_PORT    0xD0 // Read Output Port
#define KBC_CMD_WRITE_OUTPUT_PORT   0xD1 // Write Output Port
#define KBC_CMD_READ_TEST_INPUTS    0xE0 // Read Test Inputs
#define KBC_CMD_SYSTEM_RESET        0xFE // Pulse reset line (CPU reset) - Use with caution!
#define KBC_CMD_MOUSE_PREFIX        0xD4 // Prefix for sending command byte to mouse device

// --- KBC Configuration Bits (Used with Read/Write Config 0x20/0x60) ---
#define KBC_CFG_INT_KB              0x01 // Bit 0: Keyboard Interrupt Enable (IRQ1)
#define KBC_CFG_INT_MOUSE           0x02 // Bit 1: Mouse Interrupt Enable (IRQ12)
#define KBC_CFG_SYS_FLAG            0x04 // Bit 2: System Flag (POST status)
#define KBC_CFG_OVERRIDE_INH        0x08 // Bit 3: Should be zero
#define KBC_CFG_DISABLE_KB          0x10 // Bit 4: Keyboard Interface Disable (0=Enabled, 1=Disabled) - Same as INH status bit
#define KBC_CFG_DISABLE_MOUSE       0x20 // Bit 5: Mouse Interface Disable (0=Enabled, 1=Disabled)
#define KBC_CFG_TRANSLATION         0x40 // Bit 6: Translation Enable (Set 1 -> Set 2 scancodes)
#define KBC_CFG_RESERVED            0x80 // Bit 7: Must be zero

// --- Keyboard Device Commands (Port 0x60 Write) ---
#define KB_CMD_SET_LEDS             0xED // Set Keyboard LEDs
#define KB_CMD_ECHO                 0xEE // Diagnostic Echo
#define KB_CMD_SET_SCANCODE_SET     0xF0 // Set Scancode Set
#define KB_CMD_IDENTIFY             0xF2 // Identify Keyboard
#define KB_CMD_SET_TYPEMATIC        0xF3 // Set Typematic Rate/Delay
#define KB_CMD_ENABLE_SCAN          0xF4 // Enable Scanning (Keyboard starts sending keypresses)
#define KB_CMD_DISABLE_SCAN         0xF5 // Disable Scanning (Default state)
#define KB_CMD_SET_DEFAULT          0xF6 // Set Default Parameters
#define KB_CMD_RESEND               0xFE // Resend Last Byte
#define KB_CMD_RESET                0xFF // Reset Keyboard Device

// --- Keyboard/KBC Responses (Port 0x60 Read) ---
#define KB_RESP_ERROR_OR_BUF_OVERRUN 0x00 // Keyboard Error or Buffer Overrun
#define KB_RESP_SELF_TEST_PASS      0xAA // BAT (Basic Assurance Test) Passed (sent after 0xFF Reset)
#define KB_RESP_ECHO                0xEE // Response to Echo command (0xEE)
#define KB_RESP_ACK                 0xFA // Command Acknowledged (ACK)
#define KB_RESP_SELF_TEST_FAIL1     0xFC // BAT Failed (Code 1)
#define KB_RESP_SELF_TEST_FAIL2     0xFD // BAT Failed (Code 2)
#define KB_RESP_RESEND              0xFE // Resend Last Command/Data
#define KB_RESP_ERROR               0xFF // Keyboard Error

// --- KBC Test/Command Responses (Port 0x60 Read) ---
#define KBC_RESP_SELF_TEST_PASS     0x55 // KBC Self-Test (0xAA) Passed
#define KB_RESP_INTERFACE_TEST_PASS 0x00 // KBC Interface Test (0xAB or 0xA9) Passed
                                         // Note: Other values indicate errors for tests

// --- Special Scancode Prefixes ---
#define SCANCODE_EXTENDED_PREFIX    0xE0 // Prefix for extended keys (arrows, R-Ctrl, etc.)
#define SCANCODE_PAUSE_PREFIX       0xE1 // Prefix for Pause/Break key sequence

#endif // KEYBOARD_HW_H