#ifndef PORT_IO_H
#define PORT_IO_H

#include <kernel/core/types.h>

/**
 * Read a byte from the specified port.
 * @param port The port number (0..65535).
 * @return The byte read.
 */
uint8_t inb(uint16_t port);

/**
 * Write a byte to the specified port.
 * @param port The port number.
 * @param value The byte value to write.
 */
void outb(uint16_t port, uint8_t value);

/**
 * Read a word (16 bits) from the specified port.
 * @param port The port number.
 * @return The 16-bit value read.
 */
uint16_t inw(uint16_t port);

/**
 * Write a word (16 bits) to the specified port.
 * @param port The port number.
 * @param value The 16-bit value to write.
 */
void outw(uint16_t port, uint16_t value);

/**
 * Read a doubleword (32 bits) from the specified port.
 * @param port The port number.
 * @return The 32-bit value read.
 */
uint32_t inl(uint16_t port);

/**
 * Write a doubleword (32 bits) to the specified port.
 * @param port The port number.
 * @param value The 32-bit value to write.
 */
void outl(uint16_t port, uint32_t value);

#endif // PORT_IO_H
