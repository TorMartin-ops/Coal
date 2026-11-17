/**
 * @file e1000.h
 * @brief Intel 82540EM (e1000) Ethernet driver, the NIC QEMU emulates by default.
 */
#ifndef KERNEL_DRIVERS_NET_E1000_H
#define KERNEL_DRIVERS_NET_E1000_H

#include <kernel/core/types.h>

/** Finds the adapter, brings the link up and starts receiving. Fills mac. False if there is none. */
bool e1000_init(uint8_t mac[6]);

/** Queues one frame (without FCS) for transmission. Dropped when the ring is full. */
void e1000_send(const void *frame, size_t len);

/** True if the adapter has no usable interrupt line and must be polled with e1000_poll() every few ms. */
bool e1000_polled(void);

/** Hands every completed receive descriptor to net_rx_frame(). Interrupts must be off. */
void e1000_poll(void);

#endif
