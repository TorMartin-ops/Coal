/**
 * @file vsock.c
 * @brief AF_VSOCK stream sockets with virtio credit-based flow control (see vsock.h).
 *
 * Each connection owns a receive ring (RX_SIZE bytes, also its advertised buf_alloc). The peer
 * may have at most that much unread data in flight; every packet we send carries our consumed
 * count (fwd_cnt), and a CREDIT_UPDATE goes out when the application has freed half the ring.
 * Packets that cannot be sent at once because the transmit queue is full wait in a small queue.
 */
#define LOG_MODULE "vsock"

#include <kernel/net/vsock.h>
#include <kernel/net/socket.h>
#include <libc/errno.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

#define RX_SIZE            65536u        /* power of two */
#define EPHEMERAL_FIRST    49152u
#define CONNECT_TIMEOUT_MS 2000u
#define CTRL_QUEUE         16

enum { VS_NEW, VS_LISTEN, VS_CONNECTING, VS_CONNECTED, VS_CLOSED };

typedef struct vsock {
    struct sock  *sk;
    struct vsock *next;                  /* every socket of the protocol */
    uint8_t  state, peer_shut, local_shut;
    uint8_t  backlog, nacc;
    bool     bound, want_tx;
    int      err;                        /* positive errno, reported once */
    uint32_t local_port, remote_cid, remote_port;
    uint8_t *rx;
    uint32_t rx_head, rx_tail;           /* free running */
    uint32_t fwd_cnt, last_fwd;          /* bytes the application consumed; the value we last told the peer */
    uint32_t peer_alloc, peer_fwd, tx_cnt;
    struct vsock *acc[BACKLOG_MAX];      /* connections waiting for accept() */
} vsock_t;

static vsock_t *g_socks;
static uint32_t g_next_port = EPHEMERAL_FIRST;
static struct vsock_hdr g_ctrl[CTRL_QUEUE];
static unsigned g_ctrl_head, g_ctrl_n;

static const struct sock_ops vsock_ops;

/* ---- Helpers ------------------------------------------------------------------------- */

static uint32_t rx_count(const vsock_t *v) { return v->rx_head - v->rx_tail; }
static bool nonblocking(const struct sock *s, int flags) { return s->nonblock || (flags & MSG_DONTWAIT); }

static uint32_t tx_credit(const vsock_t *v) {
    uint32_t in_flight = v->tx_cnt - v->peer_fwd;
    return in_flight < v->peer_alloc ? v->peer_alloc - in_flight : 0;
}

static void fill_hdr(vsock_t *v, struct vsock_hdr *h, uint16_t op, uint32_t len, uint32_t flags) {
    *h = (struct vsock_hdr){ .src_cid = virtio_vsock_cid(), .dst_cid = v->remote_cid,
                             .src_port = v->local_port, .dst_port = v->remote_port, .len = len,
                             .type = VSOCK_TYPE_STREAM, .op = op, .flags = flags,
                             .buf_alloc = RX_SIZE, .fwd_cnt = v->fwd_cnt };
    v->last_fwd = v->fwd_cnt;
}

/* Header-only packets are never lost to a busy queue: they wait here (the oldest goes when it is full). */
static void send_ctrl(const struct vsock_hdr *h) {
    if (!g_ctrl_n && virtio_vsock_send(h, NULL, 0) == 0) return;
    if (g_ctrl_n == CTRL_QUEUE) { g_ctrl_head = (g_ctrl_head + 1) % CTRL_QUEUE; g_ctrl_n--; }
    g_ctrl[(g_ctrl_head + g_ctrl_n++) % CTRL_QUEUE] = *h;
}

static void send_op(vsock_t *v, uint16_t op, uint32_t flags) {
    struct vsock_hdr h;
    fill_hdr(v, &h, op, 0, flags);
    send_ctrl(&h);
}

/* RST answering a packet that belongs to no connection. */
static void reply_rst(const struct vsock_hdr *in) {
    struct vsock_hdr h = { .src_cid = in->dst_cid, .dst_cid = in->src_cid, .src_port = in->dst_port,
                           .dst_port = in->src_port, .type = VSOCK_TYPE_STREAM, .op = VSOCK_OP_RST };
    send_ctrl(&h);
}

static vsock_t *find_conn(uint32_t cid, uint32_t rport, uint32_t lport) {
    for (vsock_t *v = g_socks; v; v = v->next)
        if ((v->state == VS_CONNECTED || v->state == VS_CONNECTING || v->state == VS_CLOSED) &&
            v->remote_cid == cid && v->remote_port == rport && v->local_port == lport)
            return v;
    return NULL;
}

static vsock_t *find_listener(uint32_t port) {
    for (vsock_t *v = g_socks; v; v = v->next)
        if (v->state == VS_LISTEN && v->local_port == port) return v;
    return NULL;
}

static bool port_taken(uint32_t port) {
    for (vsock_t *v = g_socks; v; v = v->next)
        if (v->bound && v->local_port == port && v->state != VS_CONNECTED) return true;
    return false;
}

static uint32_t alloc_port(void) {
    for (unsigned tries = 0; tries < 16384; tries++) {
        uint32_t p = g_next_port;
        g_next_port = g_next_port == 0xFFFF ? EPHEMERAL_FIRST : g_next_port + 1;
        if (!port_taken(p)) return p;
    }
    return 0;
}

static vsock_t *vs_new(struct sock *sk) {
    vsock_t *v = kmalloc(sizeof(*v));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->sk = sk;
    v->next = g_socks;
    g_socks = v;
    sk->ops = &vsock_ops;
    sk->pdata = v;
    return v;
}

static void vs_free(vsock_t *v) {
    for (vsock_t **pp = &g_socks; *pp; pp = &(*pp)->next)
        if (*pp == v) { *pp = v->next; break; }
    kfree(v->rx);
    kfree(v);
}

static bool vs_alloc_rx(vsock_t *v) {
    v->rx = kmalloc(RX_SIZE);
    return v->rx != NULL;
}

/* ---- Receive path (interrupt context) ------------------------------------------------- */

static void ring_put(vsock_t *v, const uint8_t *p, uint32_t n) {
    uint32_t off = v->rx_head & (RX_SIZE - 1), first = MIN(n, RX_SIZE - off);
    memcpy(v->rx + off, p, first);
    memcpy(v->rx, p + first, n - first);
    v->rx_head += n;
}

static void on_request(vsock_t *l, const struct vsock_hdr *h) {
    struct sock *sk = l->nacc < l->backlog ? sock_new(SOCK_STREAM, 0) : NULL;
    vsock_t *v = sk ? vs_new(sk) : NULL;

    if (!v || !vs_alloc_rx(v)) {
        if (v) vs_free(v);
        if (sk) sock_free(sk);
        reply_rst(h);
        return;
    }
    v->state = VS_CONNECTED;
    v->bound = true;
    v->local_port = h->dst_port;
    v->remote_cid = (uint32_t)h->src_cid;
    v->remote_port = h->src_port;
    v->peer_alloc = h->buf_alloc;
    v->peer_fwd = h->fwd_cnt;
    send_op(v, VSOCK_OP_RESPONSE, 0);
    l->acc[l->nacc++] = v;
    sock_wake(l->sk);
}

static void on_data(vsock_t *v, const struct vsock_hdr *h, const void *payload) {
    uint32_t n = h->len;

    if (v->state != VS_CONNECTED) return;
    if (n > RX_SIZE - rx_count(v)) {                 /* the peer overran the window we advertised */
        send_op(v, VSOCK_OP_RST, 0);
        v->state = VS_CLOSED;
        v->err = ECONNRESET;
    } else if (v->local_shut & VSOCK_SHUTDOWN_RCV) {
        v->fwd_cnt += n;                             /* nobody will read it: count it as consumed */
    } else {
        ring_put(v, payload, n);
    }
}

void vsock_rx(const struct vsock_hdr *h, const void *payload) {
    vsock_t *v;

    if (h->type != VSOCK_TYPE_STREAM || h->dst_cid != virtio_vsock_cid()) {
        if (h->op != VSOCK_OP_RST) reply_rst(h);
        return;
    }
    v = find_conn(h->src_cid, h->src_port, h->dst_port);
    if (!v) {
        vsock_t *l = h->op == VSOCK_OP_REQUEST ? find_listener(h->dst_port) : NULL;
        if (l) on_request(l, h);
        else if (h->op != VSOCK_OP_RST) reply_rst(h);
        return;
    }
    v->peer_alloc = h->buf_alloc;
    v->peer_fwd = h->fwd_cnt;
    switch (h->op) {
    case VSOCK_OP_RESPONSE:
        if (v->state == VS_CONNECTING) v->state = VS_CONNECTED;
        break;
    case VSOCK_OP_RST:
        if (v->state != VS_CLOSED) v->err = v->state == VS_CONNECTING ? ECONNREFUSED : ECONNRESET;
        v->state = VS_CLOSED;
        break;
    case VSOCK_OP_SHUTDOWN:
        v->peer_shut |= (uint8_t)(h->flags & (VSOCK_SHUTDOWN_RCV | VSOCK_SHUTDOWN_SEND));
        break;
    case VSOCK_OP_RW:
        on_data(v, h, payload);
        break;
    case VSOCK_OP_CREDIT_REQUEST:
        send_op(v, VSOCK_OP_CREDIT_UPDATE, 0);
        break;
    default:
        break;
    }
    sock_wake(v->sk);
}

void vsock_tx_ready(void) {
    while (g_ctrl_n && virtio_vsock_send(&g_ctrl[g_ctrl_head], NULL, 0) == 0) {
        g_ctrl_head = (g_ctrl_head + 1) % CTRL_QUEUE;
        g_ctrl_n--;
    }
    for (vsock_t *v = g_socks; v; v = v->next)
        if (v->want_tx) { v->want_tx = false; sock_wake(v->sk); }
}

void vsock_transport_reset(void) {
    g_ctrl_n = 0;
    for (vsock_t *v = g_socks; v; v = v->next) {
        if (v->state == VS_CONNECTED || v->state == VS_CONNECTING) { v->state = VS_CLOSED; v->err = ECONNRESET; }
        sock_wake(v->sk);
    }
}

/* ---- Socket operations ------------------------------------------------------------------ */

static bool is_vm_addr(const void *addr, size_t len) {
    return len >= sizeof(struct sockaddr_vm) && ((const struct sockaddr_vm *)addr)->svm_family == AF_VSOCK;
}

static int vs_bind(struct sock *s, const void *addr, size_t len) {
    vsock_t *v = s->pdata;
    const struct sockaddr_vm *a = addr;
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (!is_vm_addr(addr, len)) rc = -EINVAL;
    else if (v->bound || v->state != VS_NEW) rc = -EINVAL;
    else if (a->svm_cid != VMADDR_CID_ANY && a->svm_cid != virtio_vsock_cid()) rc = -EADDRNOTAVAIL;
    else if (a->svm_port == VMADDR_PORT_ANY) rc = (v->local_port = alloc_port()) ? 0 : -EADDRINUSE;
    else if (port_taken(a->svm_port)) rc = -EADDRINUSE;
    else v->local_port = a->svm_port;
    if (!rc) v->bound = true;
    local_irq_restore(irq);
    return rc;
}

static int vs_listen(struct sock *s, int backlog) {
    vsock_t *v = s->pdata;
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (v->state != VS_NEW && v->state != VS_LISTEN) rc = -EINVAL;
    else if (!v->bound && !(v->local_port = alloc_port())) rc = -EADDRINUSE;
    if (!rc) {
        v->bound = true;
        v->state = VS_LISTEN;
        v->backlog = (uint8_t)(backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog);
    }
    local_irq_restore(irq);
    return rc;
}

static void fill_addr(struct sockaddr_vm *a, uint32_t cid, uint32_t port) {
    memset(a, 0, sizeof(*a));
    a->svm_family = AF_VSOCK;
    a->svm_cid = cid;
    a->svm_port = port;
}

static int vs_connect(struct sock *s, const void *addr, size_t len) {
    vsock_t *v = s->pdata;
    const struct sockaddr_vm *a = addr;
    uintptr_t irq = local_irq_save();
    sock_wait_t w;
    int rc = 0;

    if (!is_vm_addr(addr, len)) rc = -EINVAL;
    else if (v->state == VS_CONNECTED) rc = -EISCONN;
    else if (v->state == VS_CONNECTING) rc = -EALREADY;
    else if (v->state != VS_NEW) rc = -EINVAL;
    else if (!v->bound && !(v->local_port = alloc_port())) rc = -EADDRINUSE;
    else if (!vs_alloc_rx(v)) rc = -ENOMEM;
    if (rc) { local_irq_restore(irq); return rc; }

    v->bound = true;
    v->remote_cid = a->svm_cid;
    v->remote_port = a->svm_port;
    v->state = VS_CONNECTING;
    send_op(v, VSOCK_OP_REQUEST, 0);
    if (s->nonblock) { local_irq_restore(irq); return -EINPROGRESS; }

    sock_wait_init(&w, s->snd_timeout_ms ? s->snd_timeout_ms : CONNECT_TIMEOUT_MS);
    while (v->state == VS_CONNECTING && (rc = sock_wait(s, &w)) == 0) { }
    if (rc == -EAGAIN) rc = -ETIMEDOUT;
    if (rc == 0) rc = v->state == VS_CONNECTED ? 0 : -(v->err ? v->err : ECONNREFUSED);
    if (rc && rc != -EINTR) {                        /* a failed attempt leaves the socket reusable */
        if (v->state == VS_CONNECTING) send_op(v, VSOCK_OP_RST, 0);
        v->state = VS_NEW;
        v->err = 0;
    }
    local_irq_restore(irq);
    return rc;
}

static int vs_accept(struct sock *s, int flags, void *addr, size_t *len) {
    vsock_t *l = s->pdata;
    uintptr_t irq = local_irq_save();
    sock_wait_t w;
    int rc = 0;

    if (l->state != VS_LISTEN) { local_irq_restore(irq); return -EINVAL; }
    sock_wait_init(&w, s->rcv_timeout_ms);
    while (!l->nacc) {
        rc = s->nonblock ? -EAGAIN : sock_wait(s, &w);
        if (rc) { local_irq_restore(irq); return rc; }
    }
    vsock_t *v = l->acc[0];
    memmove(l->acc, l->acc + 1, (size_t)(--l->nacc) * sizeof(l->acc[0]));
    v->sk->nonblock = (flags & SOCK_NONBLOCK) != 0;
    fill_addr(addr, v->remote_cid, v->remote_port);
    *len = sizeof(struct sockaddr_vm);
    rc = sock_install(v->sk, flags & SOCK_NONBLOCK);
    if (rc < 0) {                                    /* install() freed the sock; the connection is dropped */
        send_op(v, VSOCK_OP_RST, 0);
        vs_free(v);
    }
    local_irq_restore(irq);
    return rc;
}

static ssize_t vs_send(struct sock *s, const void *buf, size_t len, int flags) {
    vsock_t *v = s->pdata;
    uintptr_t irq = local_irq_save();
    sock_wait_t w;
    size_t done = 0;
    bool asked = false;
    int rc = 0;

    sock_wait_init(&w, s->snd_timeout_ms);
    while (!rc && done < len) {
        if (v->state == VS_CLOSED && v->err) { rc = -v->err; v->err = 0; }
        else if (v->state == VS_CLOSED || (v->peer_shut & VSOCK_SHUTDOWN_RCV) || (v->local_shut & VSOCK_SHUTDOWN_SEND)) {
            rc = -EPIPE;
            if (!(flags & MSG_NOSIGNAL) && get_current_process()) process_kill((int)get_current_process()->pid, SIGPIPE);
        } else if (v->state != VS_CONNECTED) rc = -ENOTCONN;
        else {
            uint32_t credit = tx_credit(v);
            if (!credit) {
                if (!asked) { send_op(v, VSOCK_OP_CREDIT_REQUEST, 0); asked = true; }
                rc = nonblocking(s, flags) ? -EAGAIN : sock_wait(s, &w);
                continue;
            }
            uint32_t n = (uint32_t)MIN(MIN((size_t)credit, len - done), (size_t)VSOCK_MAX_PAYLOAD);
            struct vsock_hdr h;
            fill_hdr(v, &h, VSOCK_OP_RW, n, 0);
            if (virtio_vsock_send(&h, (const uint8_t *)buf + done, n) == 0) {
                v->tx_cnt += n;
                done += n;
                asked = false;
            } else {                                 /* every transmit slot is busy */
                v->want_tx = true;
                rc = nonblocking(s, flags) ? -EAGAIN : sock_wait(s, &w);
            }
        }
    }
    local_irq_restore(irq);
    return done ? (ssize_t)done : rc;
}

static ssize_t vs_recv(struct sock *s, void *buf, size_t len, int flags) {
    vsock_t *v = s->pdata;
    uintptr_t irq = local_irq_save();
    sock_wait_t w;
    ssize_t rc;

    sock_wait_init(&w, s->rcv_timeout_ms);
    for (;;) {
        uint32_t avail = rx_count(v);
        if (v->state == VS_NEW || v->state == VS_LISTEN || v->state == VS_CONNECTING) { rc = -ENOTCONN; break; }
        if (avail && !(v->local_shut & VSOCK_SHUTDOWN_RCV)) {
            uint32_t n = (uint32_t)MIN((size_t)avail, len), off = v->rx_tail & (RX_SIZE - 1), first = MIN(n, RX_SIZE - off);
            memcpy(buf, v->rx + off, first);
            memcpy((uint8_t *)buf + first, v->rx, n - first);
            if (!(flags & MSG_PEEK)) {
                v->rx_tail += n;
                v->fwd_cnt += n;
                if (v->state == VS_CONNECTED && v->fwd_cnt - v->last_fwd >= RX_SIZE / 2) send_op(v, VSOCK_OP_CREDIT_UPDATE, 0);
            }
            rc = n;
            break;
        }
        if (v->state == VS_CLOSED) { rc = -v->err; v->err = 0; break; }          /* the error once, then end of file */
        if ((v->peer_shut & VSOCK_SHUTDOWN_SEND) || (v->local_shut & VSOCK_SHUTDOWN_RCV)) { rc = 0; break; }
        if (nonblocking(s, flags)) { rc = -EAGAIN; break; }
        if ((rc = sock_wait(s, &w)) != 0) break;
    }
    local_irq_restore(irq);
    return rc;
}

static int vs_shutdown(struct sock *s, int how) {
    vsock_t *v = s->pdata;
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (how < SHUT_RD || how > SHUT_RDWR) rc = -EINVAL;
    else if (v->state != VS_CONNECTED) rc = -ENOTCONN;
    else {
        uint8_t flags = (uint8_t)((how != SHUT_WR ? VSOCK_SHUTDOWN_RCV : 0) | (how != SHUT_RD ? VSOCK_SHUTDOWN_SEND : 0));
        v->local_shut |= flags;
        send_op(v, VSOCK_OP_SHUTDOWN, flags);
        sock_wake(s);
    }
    local_irq_restore(irq);
    return rc;
}

static int vs_name(const struct sock *s, bool peer, void *addr, size_t *len) {
    const vsock_t *v = s->pdata;

    if (peer && v->state != VS_CONNECTED && v->state != VS_CLOSED) return -ENOTCONN;
    if (peer) fill_addr(addr, v->remote_cid, v->remote_port);
    else fill_addr(addr, (uint32_t)virtio_vsock_cid(), v->local_port);
    *len = sizeof(struct sockaddr_vm);
    return 0;
}

static uint32_t vs_poll(struct sock *s) {
    vsock_t *v = s->pdata;
    uintptr_t irq = local_irq_save();
    uint32_t ev = 0;

    switch (v->state) {
    case VS_NEW:        ev = POLLOUT | POLLHUP; break;
    case VS_LISTEN:     ev = v->nacc ? POLLIN : 0; break;
    case VS_CONNECTING: break;
    default:
        if (rx_count(v) || v->state == VS_CLOSED || (v->peer_shut & VSOCK_SHUTDOWN_SEND) || (v->local_shut & VSOCK_SHUTDOWN_RCV)) ev |= POLLIN;
        if (v->state == VS_CONNECTED && tx_credit(v) && !(v->peer_shut & VSOCK_SHUTDOWN_RCV) && !(v->local_shut & VSOCK_SHUTDOWN_SEND)) ev |= POLLOUT;
        if (v->state == VS_CLOSED) ev |= POLLHUP | POLLOUT | (v->err ? POLLERR : 0);
        if (v->peer_shut == (VSOCK_SHUTDOWN_RCV | VSOCK_SHUTDOWN_SEND)) ev |= POLLHUP;
    }
    local_irq_restore(irq);
    return ev;
}

/* Ends the connection toward the peer: a graceful SHUTDOWN, or RST if the peer already shut down both ways. */
static void vs_hangup(vsock_t *v) {
    if (v->state == VS_CONNECTED && v->peer_shut != (VSOCK_SHUTDOWN_RCV | VSOCK_SHUTDOWN_SEND))
        send_op(v, VSOCK_OP_SHUTDOWN, VSOCK_SHUTDOWN_RCV | VSOCK_SHUTDOWN_SEND);
    else if (v->state == VS_CONNECTED || v->state == VS_CONNECTING)
        send_op(v, VSOCK_OP_RST, 0);
}

static void vs_close(struct sock *s) {
    vsock_t *v = s->pdata;

    while (v->nacc) {                                /* connections nobody accepted */
        vsock_t *c = v->acc[--v->nacc];
        send_op(c, VSOCK_OP_RST, 0);
        struct sock *csk = c->sk;
        vs_free(c);
        sock_free(csk);
    }
    vs_hangup(v);
    vs_free(v);
}

static const struct sock_ops vsock_ops = {
    .bind = vs_bind, .connect = vs_connect, .listen = vs_listen, .accept = vs_accept,
    .send = vs_send, .recv = vs_recv, .shutdown = vs_shutdown, .name = vs_name,
    .poll = vs_poll, .close = vs_close,
};

int vsock_create(int type, int protocol, int flags) {
    if (!virtio_vsock_cid()) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM) return -ESOCKTNOSUPPORT;
    if (protocol) return -EPROTONOSUPPORT;

    uintptr_t irq = local_irq_save();
    struct sock *s = sock_new(SOCK_STREAM, 0);
    vsock_t *v = s ? vs_new(s) : NULL;
    int fd;

    if (!v) {
        if (s) sock_free(s);
        local_irq_restore(irq);
        return -ENOMEM;
    }
    s->nonblock = flags != 0;
    fd = sock_install(s, flags);
    if (fd < 0) vs_free(v);
    local_irq_restore(irq);
    return fd;
}
