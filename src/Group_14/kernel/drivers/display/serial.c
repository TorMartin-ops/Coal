#include <kernel/drivers/display/serial.h>
#include <kernel/lib/port_io.h> // For inb/outb
#include <kernel/drivers/display/terminal.h>
#include <libc/stdarg.h>

// LSR (Line Status Register) flags
#define LSR_TX_EMPTY 0x20 // Transmitter Holding Register Empty

// Wait until the serial port is ready to send
static int is_transmit_empty() {
  return inb(SERIAL_COM1_BASE + 5) & LSR_TX_EMPTY;
}

// Send a single character
void serial_putchar(char c) {
  while (is_transmit_empty() == 0); // Wait until ready
  outb(SERIAL_COM1_BASE, c);

  // If sending newline, also send carriage return for compatibility
  if (c == '\n') {
    while (is_transmit_empty() == 0);
    outb(SERIAL_COM1_BASE, '\r');
  }
}

// Send a null-terminated string
void serial_write(const char *str) {
  for (size_t i = 0; str[i] != '\0'; i++) {
    serial_putchar(str[i]);
  }
}

void serial_print_hex(uint32_t n) {
  char buf[9];
  buf[8] = '\0'; // Null terminator
  const char *digits = "0123456789ABCDEF";
  for (int i = 7; i >= 0; i--) {
      buf[i] = digits[n & 0xF]; // Get last nibble
      n >>= 4;                 // Shift right by 4 bits
  }
  serial_write(buf); // Write the hex string
}

// Optional: Basic Initialization (can set baud rate, etc., but often works with defaults in QEMU)
// Forward declaration for _vsnprintf from terminal.c
extern int _vsnprintf(char *str, size_t size, const char *fmt, va_list args);

void serial_printf(const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = _vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    for (int i = 0; i < len; ++i) {
        serial_putchar(buf[i]);
    }
}

void serial_init() {
   // Basic init - QEMU often defaults work fine
   // Example: Set baud rate divisor (requires accessing other registers)
   // outb(SERIAL_COM1_BASE + 3, 0x80); // Enable DLAB (set baud rate divisor)
   // outb(SERIAL_COM1_BASE + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
   // outb(SERIAL_COM1_BASE + 1, 0x00); //                  (hi byte)
   // outb(SERIAL_COM1_BASE + 3, 0x03); // 8 bits, no parity, one stop bit (8N1)
   // outb(SERIAL_COM1_BASE + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
   // outb(SERIAL_COM1_BASE + 4, 0x0B); // IRQs enabled, RTS/DSR set
   terminal_write("[Serial] COM1 Initialized (basic).\n"); // Log to screen
   serial_write("[Serial] COM1 output working.\n"); // Test serial output itself
}