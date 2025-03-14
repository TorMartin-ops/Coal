#include "terminal.h"
#include "libc/string.h"

#define VGA_ADDRESS 0xB8000
#define VGA_COLS 80
#define VGA_ROWS 25

static uint16_t *terminal_buffer;
static int terminal_index = 0;

// Initialize terminal
void terminal_init(void) {
    terminal_buffer = (uint16_t *)VGA_ADDRESS;
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        terminal_buffer[i] = ' ' | (0x07 << 8);  // Clear screen with spaces
    }
    terminal_index = 0;
}

// Print a string to the terminal
void terminal_write(const char *string) {
    for (int i = 0; string[i] != '\0'; i++) {
        terminal_buffer[terminal_index++] = string[i] | (0x07 << 8); // White text
    }
}
