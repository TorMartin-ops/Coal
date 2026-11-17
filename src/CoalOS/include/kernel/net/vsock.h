/**
 * @file vsock.h
 * @brief AF_VSOCK stream sockets: connections between the guest and the host (or other guests).
 *
 * The protocol (virtio 1.3, 5.10) is credit based: each side tells the other how much receive buffer
 * it has (buf_alloc) and how much it has consumed (fwd_cnt), and a sender never has more bytes in
 * flight than the peer's free space. The socket layer here is independent of the device; the
 * transport (virtio_vsock.c) calls vsock_rx() and the other hooks below.
 *
 * Like the network stack, all state is protected by running with interrupts off.
 */
#ifndef KERNEL_NET_VSOCK_H
#define KERNEL_NET_VSOCK_H

#include <kernel/core/types.h>
#include <kernel/drivers/virtio/virtio_vsock.h>

#define VMADDR_CID_HYPERVISOR 0
#define VMADDR_CID_LOCAL      1
#define VMADDR_CID_HOST       2
#define VMADDR_CID_ANY        0xFFFFFFFFu
#define VMADDR_PORT_ANY       0xFFFFFFFFu

/** struct sockaddr_vm as user space passes it. */
struct sockaddr_vm {
    uint16_t svm_family;
    uint16_t svm_reserved1;
    uint32_t svm_port;
    uint32_t svm_cid;
    uint8_t  svm_flags;
    uint8_t  svm_zero[3];
};

/** socket(AF_VSOCK, ...): a new descriptor, or a negative errno (-EAFNOSUPPORT without a vsock device). */
int vsock_create(int type, int protocol, int flags);

/* Called by the transport (interrupt context) */
void vsock_rx(const struct vsock_hdr *hdr, const void *payload);   /* a packet arrived; payload holds hdr->len bytes */
void vsock_tx_ready(void);                                          /* a transmit slot freed up */
void vsock_transport_reset(void);                                   /* the host reset the device: connections are gone */

#endif
