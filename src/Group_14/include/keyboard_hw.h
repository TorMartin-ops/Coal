#ifndef KEYBOARD_HW_H
#define KEYBOARD_HW_H

/* Ports */
#define KBC_DATA_PORT   0x60
#define KBC_STATUS_PORT 0x64
#define KBC_CMD_PORT    0x64

/* Status-register bits */
#define KBC_SR_OBF      0x01
#define KBC_SR_IBF      0x02
#define KBC_SR_SYS_FLAG 0x04
#define KBC_SR_A2       0x08
#define KBC_SR_BIT4_UNKNOWN 0x10
#define KBC_SR_MOUSE_OBF 0x20
#define KBC_SR_TIMEOUT  0x40
#define KBC_SR_PARITY   0x80

/* Controller commands (to 0x64) */
#define KBC_CMD_READ_CONFIG         0x20
#define KBC_CMD_WRITE_CONFIG        0x60
#define KBC_CMD_DISABLE_MOUSE_IFACE 0xA7
#define KBC_CMD_ENABLE_MOUSE_IFACE  0xA8
#define KBC_CMD_SELF_TEST           0xAA
#define KBC_CMD_DISABLE_KB_IFACE    0xAD
#define KBC_CMD_ENABLE_KB_IFACE     0xAE

/* Config-byte bits */
#define KBC_CFG_INT_KB      0x01
#define KBC_CFG_INT_MOUSE   0x02
#define KBC_CFG_SYS_FLAG    0x04
#define KBC_CFG_DISABLE_KB  0x10
#define KBC_CFG_DISABLE_MOUSE 0x20
#define KBC_CFG_TRANSLATION 0x40   /* — we now leave this **CLEARED** */
                                    /* (bit defined for completeness) */

/* Keyboard-device commands (to 0x60) */
#define KB_CMD_SET_LEDS         0xED
#define KB_CMD_SET_SCANCODE_SET 0xF0
#define KB_CMD_ENABLE_SCAN      0xF4
#define KB_CMD_SET_TYPEMATIC    0xF3
#define KB_CMD_RESET            0xFF

/* Responses */
#define KB_RESP_ACK             0xFA
#define KB_RESP_SELF_TEST_PASS  0xAA
#define KBC_RESP_SELF_TEST_PASS 0x55

/* Scancode prefixes */
#define SCANCODE_EXTENDED_PREFIX 0xE0
#define SCANCODE_PAUSE_PREFIX    0xE1

#endif
