/**
 * @file serial.h
 * @brief 16550 UART driver: COM1 is the kernel log, COM2 is for the MCP daemon.
 *
 * Kernel log (COM1): serial_write, synchronous.
 *
 * Port API (any port): interrupt-driven RX and TX ring buffers, raw bytes.
 * The first call to serial_port_read/serial_port_write on a port installs its
 * IRQ handler, so it must happen after idt_init().
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <kernel/core/types.h>
#include <libc/stdarg.h>

#define SERIAL_COM1 0
#define SERIAL_COM2 1

/** Programs the UART for 115200 8N1 with FIFOs. Polled output works right away. */
void serial_init(int port);

/** Queues up to len bytes for transmission. Never blocks; returns bytes accepted. */
size_t serial_port_write(int port, const void *buf, size_t len);

/** Blocks until at least one byte has arrived, then returns up to len bytes. */
size_t serial_port_read(int port, void *buf, size_t len);


/** Kernel log on COM1: synchronous, '\n' is sent as "\r\n". */
void serial_write(const char *str);

#endif /* SERIAL_H */
