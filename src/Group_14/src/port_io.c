/**
 * port_io.c
 *
 * Provides normal (non-inline) functions to perform I/O port
 * operations on x86, for 8-bit, 16-bit, and 32-bit transfers.
 *
 * This avoids redefinition errors if you previously had them inline
 * in the header. Make sure you remove or comment out any
 * static inline definitions from port_io.h.
 */

 #include "port_io.h"

 /* Read a byte (8 bits) from 'port' */
 uint8_t inb(uint16_t port)
 {
     uint8_t result;
     __asm__ volatile("inb %1, %0"
                      : "=a"(result)
                      : "Nd"(port));
     return result;
 }
 
 /* Write a byte to 'port' */
 void outb(uint16_t port, uint8_t value)
 {
     __asm__ volatile("outb %0, %1"
                      :
                      : "a"(value), "Nd"(port));
 }
 
 /* Read a word (16 bits) from 'port' */
 uint16_t inw(uint16_t port)
 {
     uint16_t result;
     __asm__ volatile("inw %1, %0"
                      : "=a"(result)
                      : "Nd"(port));
     return result;
 }
 
 /* Write a word to 'port' */
 void outw(uint16_t port, uint16_t value)
 {
     __asm__ volatile("outw %0, %1"
                      :
                      : "a"(value), "Nd"(port));
 }
 
 /* Read a doubleword (32 bits) from 'port' */
 uint32_t inl(uint16_t port)
 {
     uint32_t result;
     __asm__ volatile("inl %1, %0"
                      : "=a"(result)
                      : "Nd"(port));
     return result;
 }
 
 /* Write a doubleword to 'port' */
 void outl(uint16_t port, uint32_t value)
 {
     __asm__ volatile("outl %0, %1"
                      :
                      : "a"(value), "Nd"(port));
 }