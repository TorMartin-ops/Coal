/**
 * @file vga_hardware.h
 * @brief VGA text mode (80x25, colour) constants.
 */
#ifndef KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H
#define KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H

#include <kernel/core/types.h>

#define VGA_ADDRESS 0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25

/* CRTC index/data ports and the registers used for the hardware cursor */
#define VGA_CMD_PORT         0x3D4
#define VGA_DATA_PORT        0x3D5
#define VGA_REG_CURSOR_START 0x0A
#define VGA_REG_CURSOR_END   0x0B
#define VGA_REG_CURSOR_HI    0x0E
#define VGA_REG_CURSOR_LO    0x0F

#define VGA_SEQ_INDEX        0x3C4
#define VGA_SEQ_DATA         0x3C5
#define VGA_GC_INDEX         0x3CE
#define VGA_GC_DATA          0x3CF
#define VGA_FONT_MEM         0xA0000
#define VGA_FONT_STRIDE      32        /* bytes per glyph in font plane 2 */
#define VGA_FONT_HEIGHT      16

#define VGA_COLOR_BLACK       0
#define VGA_COLOR_LIGHT_GREY  7
#define VGA_COLOR_WHITE       15

/** Attribute byte: background in the high nibble, foreground in the low one. */
#define VGA_RGB(fg, bg) ((uint8_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)))

#endif /* KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H */
