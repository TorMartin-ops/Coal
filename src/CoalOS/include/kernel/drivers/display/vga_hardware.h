/**
 * @file vga_hardware.h
 * @brief VGA hardware abstraction interface
 */

#ifndef KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H
#define KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H

#include <kernel/core/types.h>

// VGA text mode
#define VGA_ADDRESS 0xB8000
#define VGA_COLS    80
#define VGA_ROWS    25

// CRTC ports and cursor registers
#define VGA_CMD_PORT          0x3D4
#define VGA_DATA_PORT         0x3D5
#define VGA_REG_CURSOR_HI     0x0E
#define VGA_REG_CURSOR_LO     0x0F
#define VGA_REG_CURSOR_START  0x0A
#define VGA_REG_CURSOR_END    0x0B
#define CURSOR_SCANLINE_START 14
#define CURSOR_SCANLINE_END   15

// VGA colors
#define VGA_COLOR_BLACK         0
#define VGA_COLOR_BLUE          1
#define VGA_COLOR_GREEN         2
#define VGA_COLOR_CYAN          3
#define VGA_COLOR_RED           4
#define VGA_COLOR_MAGENTA       5
#define VGA_COLOR_BROWN         6
#define VGA_COLOR_LIGHT_GREY    7
#define VGA_COLOR_DARK_GREY     8
#define VGA_COLOR_LIGHT_BLUE    9
#define VGA_COLOR_LIGHT_GREEN   10
#define VGA_COLOR_LIGHT_CYAN    11
#define VGA_COLOR_LIGHT_RED     12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_LIGHT_BROWN   14
#define VGA_COLOR_WHITE         15

// Color helper macros
#define VGA_RGB(fg,bg)   (uint8_t)(((bg)&0x0F)<<4 | ((fg)&0x0F))
#define VGA_FG(attr)     ((attr)&0x0F)
#define VGA_BG(attr)     (((attr)>>4)&0x0F)

#endif // KERNEL_DRIVERS_DISPLAY_VGA_HARDWARE_H
