// Example: include/keyboard_hw.h
#ifndef KEYBOARD_HW_H
#define KEYBOARD_HW_H

#define KBC_DATA_PORT   0x60
#define KBC_STATUS_PORT 0x64
#define KBC_CMD_PORT    0x64
#define KBC_SR_OUT_BUF  0x01
// Add other KBC related commands/responses if needed for broader use
// #define KBC_CMD_ENABLE_SCAN 0xF4
// #define KBC_RESP_ACK        0xFA

#endif // KEYBOARD_HW_H