/**
 * @file virtio_vsock.h
 * @brief virtio-vsock transport: moves AF_VSOCK packets (see kernel/net/vsock.h) over the device queues.
 */
#ifndef KERNEL_DRIVERS_VIRTIO_VSOCK_H
#define KERNEL_DRIVERS_VIRTIO_VSOCK_H

#include <kernel/core/types.h>

#define VSOCK_HDR_SIZE 44

#define VSOCK_TYPE_STREAM 1
#define VSOCK_OP_REQUEST        1
#define VSOCK_OP_RESPONSE       2
#define VSOCK_OP_RST            3
#define VSOCK_OP_SHUTDOWN       4
#define VSOCK_OP_RW             5
#define VSOCK_OP_CREDIT_UPDATE  6
#define VSOCK_OP_CREDIT_REQUEST 7
#define VSOCK_SHUTDOWN_RCV      1
#define VSOCK_SHUTDOWN_SEND     2

/** Largest payload a single packet carries. */
#define VSOCK_MAX_PAYLOAD 8148

/** The packet header on the wire (little endian). */
struct vsock_hdr {
    uint64_t src_cid, dst_cid;
    uint32_t src_port, dst_port;
    uint32_t len;
    uint16_t type, op;
    uint32_t flags;
    uint32_t buf_alloc, fwd_cnt;
} __attribute__((packed));

/** The guest's context ID, 0 when there is no device. */
uint64_t virtio_vsock_cid(void);

/**
 * Sends a packet of `len` payload bytes behind `hdr` (len may be 0, at most VSOCK_MAX_PAYLOAD). While
 * earlier packets are still with the device, data packets of one connection are merged into one, so a
 * stream of small writes costs few notifications. Returns 0, or -ENOBUFS when every transmit slot is
 * busy; vsock_tx_ready() runs when one frees up.
 */
int virtio_vsock_send(const struct vsock_hdr *hdr, const void *payload, uint32_t len);

#endif
