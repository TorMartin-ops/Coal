/**
 * @file text_renderer.c
 * @brief Text rendering and formatting following Single Responsibility Principle
 * 
 * This module is responsible ONLY for:
 * - Text rendering and character output
 * - ANSI escape sequence processing
 * - Text formatting and color management
 * - Printf-style formatting
 */

#define LOG_MODULE "text_renderer"

#include <kernel/drivers/display/text_renderer.h>
#include <kernel/drivers/display/vga_hardware.h>
#include <kernel/interfaces/logger.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>
#include <libc/stdarg.h>

// Configuration constants
#define TAB_WIDTH 4
#define PRINTF_BUFFER_SIZE 256

// ANSI escape-code state machine
typedef enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_BRACKET,
    ANSI_STATE_PARAM
} AnsiState;

// Text renderer state
static spinlock_t renderer_lock = {0};
static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t current_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
static AnsiState ansi_state = ANSI_STATE_NORMAL;
static bool ansi_private = false;
static int ansi_params[4];
static int ansi_param_count = 0;
static bool renderer_initialized = false;

/**
 * @brief Process ANSI escape sequence parameters
 */
static void process_ansi_sgr(void) {
    for (int i = 0; i < ansi_param_count; i++) {
        int param = ansi_params[i];
        
        switch (param) {
            case 0: // Reset
                current_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
                break;
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                // Set foreground color
                current_color = (current_color & 0xF0) | (param - 30);
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                // Set background color
                current_color = (current_color & 0x0F) | ((param - 40) << 4);
                break;
            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                // Set bright foreground color
                current_color = (current_color & 0xF0) | (param - 90 + 8);
                break;
        }
    }
}

/**
 * @brief Process ANSI cursor movement
 */
static void process_ansi_cursor(char command) {
    int param = (ansi_param_count > 0) ? ansi_params[0] : 1;
    
    switch (command) {
        case 'A': // Move cursor up
            cursor_y = (cursor_y - param < 0) ? 0 : cursor_y - param;
            break;
        case 'B': // Move cursor down
            cursor_y = (cursor_y + param >= VGA_ROWS) ? VGA_ROWS - 1 : cursor_y + param;
            break;
        case 'C': // Move cursor right
            cursor_x = (cursor_x + param >= VGA_COLS) ? VGA_COLS - 1 : cursor_x + param;
            break;
        case 'D': // Move cursor left
            cursor_x = (cursor_x - param < 0) ? 0 : cursor_x - param;
            break;
        case 'H': // Set cursor position
        case 'f':
            cursor_y = (ansi_param_count > 0 && ansi_params[0] > 0) ? ansi_params[0] - 1 : 0;
            cursor_x = (ansi_param_count > 1 && ansi_params[1] > 0) ? ansi_params[1] - 1 : 0;
            if (cursor_y >= VGA_ROWS) cursor_y = VGA_ROWS - 1;
            if (cursor_x >= VGA_COLS) cursor_x = VGA_COLS - 1;
            break;
        case 'J': // Clear screen
            if (param == 2) {
                vga_clear_screen(current_color);
                cursor_x = cursor_y = 0;
            }
            break;
    }
}

/**
 * @brief Process a single character through ANSI state machine
 */
static void process_ansi_char(char c) {
    switch (ansi_state) {
        case ANSI_STATE_NORMAL:
            if (c == '\033') { // ESC
                ansi_state = ANSI_STATE_ESC;
            } else {
                text_renderer_put_char(c);
            }
            break;
            
        case ANSI_STATE_ESC:
            if (c == '[') {
                ansi_state = ANSI_STATE_BRACKET;
                ansi_param_count = 0;
                ansi_private = false;
            } else {
                ansi_state = ANSI_STATE_NORMAL;
                text_renderer_put_char(c);
            }
            break;
            
        case ANSI_STATE_BRACKET:
            if (c == '?') {
                ansi_private = true;
            } else if (c >= '0' && c <= '9') {
                ansi_state = ANSI_STATE_PARAM;
                ansi_params[0] = c - '0';
                ansi_param_count = 1;
            } else {
                ansi_state = ANSI_STATE_NORMAL;
                process_ansi_cursor(c);
            }
            break;
            
        case ANSI_STATE_PARAM:
            if (c >= '0' && c <= '9') {
                if (ansi_param_count > 0) {
                    ansi_params[ansi_param_count - 1] = 
                        ansi_params[ansi_param_count - 1] * 10 + (c - '0');
                }
            } else if (c == ';') {
                if (ansi_param_count < 4) {
                    ansi_params[ansi_param_count++] = 0;
                }
            } else {
                ansi_state = ANSI_STATE_NORMAL;
                if (c == 'm') {
                    process_ansi_sgr();
                } else {
                    process_ansi_cursor(c);
                }
            }
            break;
    }
}

/**
 * @brief Scroll screen if cursor is at bottom
 */
static void check_scroll(void) {
    if (cursor_y >= VGA_ROWS) {
        vga_scroll_up(current_color);
        cursor_y = VGA_ROWS - 1;
    }
}

/**
 * @brief Initialize text renderer
 */
error_t text_renderer_init(void) {
    spinlock_init(&renderer_lock);
    
    cursor_x = 0;
    cursor_y = 0;
    current_color = VGA_RGB(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    ansi_state = ANSI_STATE_NORMAL;
    ansi_param_count = 0;
    
    // Clear screen
    vga_clear_screen(current_color);
    
    renderer_initialized = true;
    LOGGER_INFO(LOG_MODULE, "Text renderer initialized");
    return E_SUCCESS;
}

/**
 * @brief Put a single character without ANSI processing
 */
void text_renderer_put_char(char c) {
    if (!renderer_initialized) {
        return;
    }
    
    uintptr_t flags = spinlock_acquire_irqsave(&renderer_lock);
    
    switch (c) {
        case '\n':
            cursor_x = 0;
            cursor_y++;
            check_scroll();
            break;
            
        case '\r':
            cursor_x = 0;
            break;
            
        case '\t':
            cursor_x = (cursor_x + TAB_WIDTH) & ~(TAB_WIDTH - 1);
            if (cursor_x >= VGA_COLS) {
                cursor_x = 0;
                cursor_y++;
                check_scroll();
            }
            break;
            
        case '\b':
            if (cursor_x > 0) {
                cursor_x--;
                vga_write_char_at(cursor_x, cursor_y, ' ', current_color);
            }
            break;
            
        default:
            if (c >= 32 && c <= 126) { // Printable ASCII
                vga_write_char_at(cursor_x, cursor_y, c, current_color);
                cursor_x++;
                if (cursor_x >= VGA_COLS) {
                    cursor_x = 0;
                    cursor_y++;
                    check_scroll();
                }
            }
            break;
    }
    
    // Update hardware cursor
    vga_set_cursor_position(cursor_x, cursor_y);
    
    spinlock_release_irqrestore(&renderer_lock, flags);
}

/**
 * @brief Write a string with ANSI processing
 */
void text_renderer_write_string(const char *str) {
    if (!str || !renderer_initialized) {
        return;
    }
    
    while (*str) {
        process_ansi_char(*str);
        str++;
    }
}

/**
 * @brief Write a string without ANSI processing
 */
void text_renderer_write_string_raw(const char *str) {
    if (!str || !renderer_initialized) {
        return;
    }
    
    while (*str) {
        text_renderer_put_char(*str);
        str++;
    }
}

/**
 * @brief Set text color
 */
void text_renderer_set_color(uint8_t color) {
    uintptr_t flags = spinlock_acquire_irqsave(&renderer_lock);
    current_color = color;
    spinlock_release_irqrestore(&renderer_lock, flags);
}

/**
 * @brief Get current text color
 */
uint8_t text_renderer_get_color(void) {
    return current_color;
}

/**
 * @brief Set cursor position
 */
void text_renderer_set_cursor_position(int x, int y) {
    uintptr_t flags = spinlock_acquire_irqsave(&renderer_lock);
    
    cursor_x = (x < 0) ? 0 : (x >= VGA_COLS) ? VGA_COLS - 1 : x;
    cursor_y = (y < 0) ? 0 : (y >= VGA_ROWS) ? VGA_ROWS - 1 : y;
    
    vga_set_cursor_position(cursor_x, cursor_y);
    
    spinlock_release_irqrestore(&renderer_lock, flags);
}

/**
 * @brief Get cursor position
 */
void text_renderer_get_cursor_position(int *x, int *y) {
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}

/**
 * @brief Clear screen
 */
void text_renderer_clear_screen(void) {
    uintptr_t flags = spinlock_acquire_irqsave(&renderer_lock);
    
    vga_clear_screen(current_color);
    cursor_x = 0;
    cursor_y = 0;
    vga_set_cursor_position(cursor_x, cursor_y);
    
    spinlock_release_irqrestore(&renderer_lock, flags);
}

/**
 * @brief Printf-style formatted output
 */
int text_renderer_printf(const char *format, ...) {
    if (!format || !renderer_initialized) {
        return 0;
    }
    
    va_list args;
    va_start(args, format);
    terminal_vprintf(format, args);
    va_end(args);
    
    // We don't know the exact number of characters written
    // Return a reasonable estimate
    return strlen(format);
}

/**
 * @brief Cleanup text renderer
 */
void text_renderer_cleanup(void) {
    renderer_initialized = false;
    LOGGER_INFO(LOG_MODULE, "Text renderer cleaned up");
}