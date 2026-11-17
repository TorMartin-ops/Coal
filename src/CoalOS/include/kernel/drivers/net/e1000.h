/**
 * @file e1000.h
 * @brief Intel 82540EM (e1000) Ethernet driver, the NIC QEMU emulates by default.
 */
#ifndef KERNEL_DRIVERS_NET_E1000_H
#define KERNEL_DRIVERS_NET_E1000_H

#include <kernel/core/types.h>

/** Finds the adapter, brings the link up and starts receiving. Fills mac. False if there is none. */
bool e1000_init(uint8_t mac[6]);

/** A transmit buffer the next e1000_send() can use without copying, or NULL while the ring is full. */
uint8_t *e1000_tx_buffer(void);

/** Queues one frame (without FCS) for transmission: 0, or -ENOBUFS when the ring is full.
 *  A frame built in e1000_tx_buffer() is sent in place; any other frame is copied. */
int e1000_send(const void *frame, size_t len);

/** Hands every completed receive descriptor to net_rx_frame(). Interrupts must be off. */
void e1000_poll(void);

#endif
