/**
 * @file keyboard_hw.h
 * @brief i8042 controller and PS/2 keyboard constants.
 */
#ifndef KEYBOARD_HW_H
#define KEYBOARD_HW_H

#define KBC_DATA_PORT   0x60
#define KBC_STATUS_PORT 0x64
#define KBC_CMD_PORT    0x64

/* Polling limits */
#define KBC_WAIT_TIMEOUT_CYCLES 300000
#define KBC_FLUSH_MAX_ATTEMPTS  100

#define KBC_SR_OBF 0x01 /* output buffer full */
#define KBC_SR_IBF 0x02 /* input buffer full */

#define KBC_CMD_READ_CONFIG         0x20
#define KBC_CMD_WRITE_CONFIG        0x60
#define KBC_CMD_DISABLE_MOUSE_IFACE 0xA7
#define KBC_CMD_DISABLE_KB_IFACE    0xAD
#define KBC_CMD_ENABLE_KB_IFACE     0xAE

#define KBC_CFG_INT_KB      0x01
#define KBC_CFG_INT_MOUSE   0x02
#define KBC_CFG_DISABLE_KB  0x10
#define KBC_CFG_TRANSLATION 0x40 /* controller converts set 2 to set 1 */

#define KB_CMD_SET_LEDS    0xED
#define KB_LED_SCROLL      0x01
#define KB_LED_NUM         0x02
#define KB_LED_CAPS        0x04
#define KB_CMD_ENABLE_SCAN 0xF4
#define KB_CMD_RESET       0xFF
#define KB_RESP_ACK        0xFA

#define SCANCODE_EXTENDED_PREFIX 0xE0
#define SCANCODE_PAUSE_PREFIX    0xE1

#endif /* KEYBOARD_HW_H */
