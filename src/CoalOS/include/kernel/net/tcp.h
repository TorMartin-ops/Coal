/**
 * @file tcp.h
 * @brief TCP: connection state machine, in-order receive, retransmission.
 *
 * A tcp_conn lives in a fixed pool and outlives its socket while it finishes
 * closing. All functions expect interrupts off (see net.h).
 */
#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include <kernel/core/types.h>

struct sock;
struct tcp_conn;

enum tcp_state {
    TCP_CLOSED, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSING, TCP_TIME_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK
};

/** A closed connection owned by owner, or NULL when the pool or memory is exhausted. */
struct tcp_conn *tcp_open(struct sock *owner);
/** Starts the handshake. 0, or -errno when the first SYN cannot be sent. */
int  tcp_connect(struct tcp_conn *c, uint32_t addr, uint16_t port, uint16_t lport);
/** Gives the connection to its new owner (accept). */
void tcp_attach(struct tcp_conn *c, struct sock *owner);
/** Drops the owner: the connection finishes closing on its own, or is reset if data is unread. */
void tcp_close(struct tcp_conn *c);
/** Resets and frees every connection still waiting in listener's accept queue or handshake. */
void tcp_drop_children(struct sock *listener);
int  tcp_shutdown_wr(struct tcp_conn *c);
/** TCP_NODELAY: send small writes at once instead of holding them until earlier data is acknowledged. */
void tcp_set_nodelay(struct tcp_conn *c, bool on);

/** Bytes queued for sending (may be less than len, 0 if the buffer is full), or -errno. */
int  tcp_write(struct tcp_conn *c, const void *buf, size_t len);
/** Bytes read, 0 at end of stream, -EAGAIN if nothing yet, or -errno (once). */
int  tcp_read(struct tcp_conn *c, void *buf, size_t len, bool peek);

int      tcp_state(const struct tcp_conn *c);
uint32_t tcp_readable(const struct tcp_conn *c);    /* bytes waiting */
uint32_t tcp_writable(const struct tcp_conn *c);    /* free send buffer */
bool     tcp_eof(const struct tcp_conn *c);         /* peer closed or connection gone */
int      tcp_take_error(struct tcp_conn *c);        /* pending errno (positive), cleared */
uint32_t tcp_peer_addr(const struct tcp_conn *c, uint16_t *port);

#endif
