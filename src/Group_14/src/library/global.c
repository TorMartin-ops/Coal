// Implementation of the global functions is inspired by James Molloy's tutorial: https://archive.is/Wg1bR#selection-457.0-585.2

#include <libc/global.h>

void outb(uint16_t port, uint8_t value) {
   asm volatile ("outb %1, %0" : : "dN" (port), "a" (value));
}

uint8_t inb(uint16_t port) {
   uint8_t ret;
   asm volatile("inb %1, %0" : "=a" (ret) : "dN" (port));
   return ret;
}

uint16_t inw(uint16_t port) {
   uint16_t ret;
   asm volatile ("inw %1, %0" : "=a" (ret) : "dN" (port));
   return ret;
}