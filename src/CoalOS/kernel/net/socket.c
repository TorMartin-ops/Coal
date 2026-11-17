/**
 * @file socket.c
 * @brief AF_INET sockets as file descriptors: a "sockfs" vnode per socket.
 *
 * Every state change happens with interrupts off (see net.h). A blocked
 * caller sleeps on the socket's wait queue; the protocol code wakes it from
 * the receive interrupt. Timeouts use drv_wait_timeout().
 */
#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/fs/vfs/vfs.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/memory/uaccess.h>
#include <kernel/process/process.h>
#include <kernel/process/signal.h>
#include <kernel/sync/spinlock.h>
#include <kernel/lib/string.h>

#define S_IFSOCK_MODE   0140000
#define EPHEMERAL_FIRST 49152
#define UDP_PAYLOAD_MAX (IP_PAYLOAD_MAX - 8)

#define FIONREAD        0x541B
#define FIONBIO         0x5421
#define SIOCGIFFLAGS    0x8913
#define SIOCGIFADDR     0x8915
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891B
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define IFF_UP_RUNNING  (0x1 | 0x2 | 0x40 | 0x1000)    /* UP BROADCAST RUNNING MULTICAST */

struct sock *net_socks[NET_MAX_SOCKS];
static uint16_t next_port = EPHEMERAL_FIRST;

/* The part of struct ifreq the SIOCGIF* requests use. */
struct ifreq {
    char name[16];
    union {
        struct sockaddr_in addr;
        struct { uint16_t family; uint8_t data[14]; } hw;
        int16_t flags;
        int32_t mtu;
    } u;
};

/* ---- waiting ------------------------------------------------------------------ */

struct waiter { bool timed; uint32_t end; };

static void waiter_init(struct waiter *w, uint32_t ms) {
    w->timed = ms != 0;
    w->end = net_now() + ms;
}

/* Sleeps until the socket changes. Interrupts are off. -EINTR for a signal, -EAGAIN at the deadline. */
static int wait_event(struct sock *s, const struct waiter *w) {
    if (w->timed) {
        int32_t left = (int32_t)(w->end - net_now());
        if (left <= 0) return -EAGAIN;
        return drv_wait_timeout(&s->wq, (uint32_t)left);
    }
    return drv_wait(&s->wq);
}

void sock_wake(struct sock *s) { drv_wake_all(&s->wq); }

/* DHCP may still be running just after boot: give it until its own fallback to finish. */
static int wait_configured(void) {
    for (int i = 0; i < 600 && net_if.present && !net_if.ip; i++) sleep_ms(10);
    return net_if.present ? 0 : -ENETDOWN;
}

static bool nonblocking(const struct sock *s, int flags) { return s->nonblock || (flags & MSG_DONTWAIT); }

/* ---- table and ports ------------------------------------------------------------ */

bool sock_port_in_use(uint8_t type, uint16_t port) {
    for (int i = 0; i < NET_MAX_SOCKS; i++)
        if (net_socks[i] && net_socks[i]->type == type && net_socks[i]->lport == port) return true;
    return false;
}

static uint16_t alloc_port(uint8_t type) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port;
        next_port = next_port == 65535 ? EPHEMERAL_FIRST : next_port + 1;
        if (!sock_port_in_use(type, p)) return p;
    }
    return 0;
}

struct sock *sock_find_listener(uint16_t port) {
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        struct sock *s = net_socks[i];
        if (s && s->listening && s->lport == port) return s;
    }
    return NULL;
}

static struct sock *sock_new(int type, int protocol) {
    struct sock *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->type = (uint8_t)type;
    s->proto = (uint8_t)protocol;
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (!net_socks[i]) { net_socks[i] = s; return s; }
    }
    kfree(s);
    return NULL;
}

static void sock_free(struct sock *s) {
    for (int i = 0; i < NET_MAX_SOCKS; i++)
        if (net_socks[i] == s) net_socks[i] = NULL;
    while (s->rxq) {
        struct dgram *d = s->rxq;
        s->rxq = d->next;
        kfree(d);
    }
    kfree(s);
}

void sock_queue_dgram(struct sock *s, uint32_t src, uint16_t sport, const void *data, size_t len) {
    if (s->rxq_n >= DGRAM_QUEUE_MAX) return;
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d) return;
    d->next = NULL;
    d->src = src;
    d->sport = sport;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    if (s->rxq) s->rxq_tail->next = d; else s->rxq = d;
    s->rxq_tail = d;
    s->rxq_n++;
    sock_wake(s);
}

void icmp_deliver(const uint8_t *pkt, size_t len) {
    uint32_t src = (uint32_t)pkt[12] << 24 | (uint32_t)pkt[13] << 16 | (uint32_t)pkt[14] << 8 | pkt[15];
    for (int i = 0; i < NET_MAX_SOCKS; i++)
        if (net_socks[i] && net_socks[i]->type == SOCK_RAW) sock_queue_dgram(net_socks[i], src, 0, pkt, len);
}

/* ---- sockfs vnode -------------------------------------------------------------------- */

static vfs_driver_t sockfs;

struct sock *sock_of(sys_file_t *sf) {
    vnode_t *v = sf->vfs_file->vnode;
    return v->fs_driver == &sockfs ? v->data : NULL;
}

static int install(struct sock *s, int flags) {
    int fd = sys_install_vnode(&sockfs, s, S_IFSOCK_MODE, flags);
    if (fd < 0) sock_free(s);
    return fd;
}

int sock_create(int domain, int type, int protocol) {
    int kind = type & 0xF, flags = type & SOCK_NONBLOCK;

    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (kind == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) protocol = IPPROTO_TCP;
    else if (kind == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) protocol = IPPROTO_UDP;
    else if (kind == SOCK_RAW && protocol == IPPROTO_ICMP) { }
    else return kind == SOCK_STREAM || kind == SOCK_DGRAM || kind == SOCK_RAW ? -EPROTONOSUPPORT : -ESOCKTNOSUPPORT;

    uintptr_t irq = local_irq_save();
    struct sock *s = sock_new(kind, protocol);
    local_irq_restore(irq);
    if (!s) return -ENOMEM;
    s->nonblock = flags != 0;
    irq = local_irq_save();
    int fd = install(s, flags);
    local_irq_restore(irq);
    return fd;
}

/* ---- operations (interrupts off inside) ---------------------------------------------- */

static int autobind(struct sock *s) {
    if (s->lport) return 0;
    s->lport = alloc_port(s->type);
    return s->lport ? 0 : -EADDRINUSE;
}

int sock_bind(struct sock *s, uint32_t addr, uint16_t port) {
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (s->lport) rc = -EINVAL;
    else if (addr && addr != net_if.ip && addr != IP4_BROADCAST) rc = -EADDRNOTAVAIL;
    else if (!port) rc = autobind(s);
    else {
        for (int i = 0; i < NET_MAX_SOCKS && !rc; i++) {
            struct sock *o = net_socks[i];
            if (o && o != s && o->type == s->type && o->lport == port && !(s->reuse && !o->listening)) rc = -EADDRINUSE;
        }
        if (!rc) s->lport = port;
    }
    if (!rc) s->laddr = addr;
    local_irq_restore(irq);
    return rc;
}

int sock_listen(struct sock *s, int backlog) {
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (s->type != SOCK_STREAM || s->tcp) rc = -EOPNOTSUPP;
    else if (!s->listening && (rc = autobind(s)) == 0) {
        s->backlog = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : (uint8_t)backlog;
        s->listening = true;
    } else if (s->listening) {
        s->backlog = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : (uint8_t)backlog;
    }
    local_irq_restore(irq);
    return rc;
}

int sock_accept(struct sock *s, uint32_t *raddr, uint16_t *rport, int flags) {
    uintptr_t irq = local_irq_save();
    struct waiter w;
    int rc = 0;

    if (!s->listening) { local_irq_restore(irq); return -EINVAL; }
    waiter_init(&w, s->rcv_timeout_ms);
    while (!s->acc_n) {
        if (s->nonblock) rc = -EAGAIN;
        else rc = wait_event(s, &w);
        if (rc) { local_irq_restore(irq); return rc; }
    }
    struct tcp_conn *c = s->accq[0];
    memmove(s->accq, s->accq + 1, (size_t)(--s->acc_n) * sizeof(s->accq[0]));

    struct sock *n = sock_new(SOCK_STREAM, IPPROTO_TCP);
    if (!n) { tcp_close(c); local_irq_restore(irq); return -ENOMEM; }
    n->tcp = c;
    n->connected = true;
    n->nonblock = (flags & SOCK_NONBLOCK) != 0;
    n->laddr = net_if.ip;
    n->lport = s->lport;
    n->raddr = tcp_peer_addr(c, &n->rport);
    tcp_attach(c, n);
    *raddr = n->raddr;
    *rport = n->rport;
    rc = install(n, flags & SOCK_NONBLOCK);
    if (rc < 0) tcp_close(c);           /* install() freed n; the connection is orphaned */
    local_irq_restore(irq);
    return rc;
}

int sock_connect(struct sock *s, uint32_t addr, uint16_t port) {
    uintptr_t irq = local_irq_save();
    struct waiter w;
    int rc;

    if (s->type != SOCK_STREAM) {                       /* datagram: remember the default peer */
        rc = s->type == SOCK_DGRAM ? autobind(s) : 0;
        if (!rc) { s->raddr = addr; s->rport = port; s->connected = true; }
        local_irq_restore(irq);
        return rc;
    }
    if (s->listening) { local_irq_restore(irq); return -EINVAL; }
    if (s->tcp) {                                       /* a second call reports how the first one ended */
        int st = tcp_state(s->tcp);
        rc = st == TCP_SYN_SENT ? -EALREADY : st == TCP_CLOSED ? -tcp_take_error(s->tcp) : s->connected ? -EISCONN : 0;
        s->connected = s->connected || rc == 0;
        local_irq_restore(irq);
        return rc;
    }
    local_irq_restore(irq);
    if ((rc = wait_configured()) != 0) return rc;
    irq = local_irq_save();
    if ((rc = autobind(s)) != 0) { local_irq_restore(irq); return rc; }
    s->tcp = tcp_open(s);
    if (!s->tcp) { local_irq_restore(irq); return -ENOMEM; }
    s->raddr = addr;
    s->rport = port;
    s->laddr = net_if.ip;
    if ((rc = tcp_connect(s->tcp, addr, port, s->lport)) != 0) {
        tcp_close(s->tcp);
        s->tcp = NULL;
        local_irq_restore(irq);
        return rc;
    }
    if (s->nonblock) { local_irq_restore(irq); return -EINPROGRESS; }

    waiter_init(&w, s->snd_timeout_ms);
    while (tcp_state(s->tcp) == TCP_SYN_SENT && (rc = wait_event(s, &w)) == 0) { }
    if (rc == -EAGAIN) rc = -ETIMEDOUT;
    if (rc == 0) rc = tcp_state(s->tcp) == TCP_ESTABLISHED ? 0 : -tcp_take_error(s->tcp);
    if (rc == 0) s->connected = true;
    if (rc && rc != -EINTR) {                           /* a failed attempt leaves the socket reusable */
        tcp_close(s->tcp);
        s->tcp = NULL;
    }
    local_irq_restore(irq);
    return rc;
}

static ssize_t send_stream(struct sock *s, const uint8_t *buf, size_t len, int flags) {
    uintptr_t irq = local_irq_save();
    struct waiter w;
    size_t done = 0;
    int rc = 0;

    waiter_init(&w, s->snd_timeout_ms);
    if (!s->tcp) rc = -ENOTCONN;
    else if (tcp_state(s->tcp) == TCP_SYN_SENT || tcp_state(s->tcp) == TCP_SYN_RCVD) rc = -ENOTCONN;
    while (!rc && done < len) {
        int n = tcp_write(s->tcp, buf + done, len - done);
        if (n < 0) {
            rc = n;
            if (n == -EPIPE && !(flags & MSG_NOSIGNAL) && get_current_process())
                process_kill((int)get_current_process()->pid, SIGPIPE);
        } else if (n > 0) {
            done += (size_t)n;
        } else if (nonblocking(s, flags)) {
            rc = -EAGAIN;
        } else {
            rc = wait_event(s, &w);
        }
    }
    local_irq_restore(irq);
    return done ? (ssize_t)done : rc;
}

ssize_t sock_send(struct sock *s, const void *buf, size_t len, int flags, const struct sockaddr_in *to) {
    uint32_t dst = s->raddr;
    uint16_t dport = s->rport;
    uintptr_t irq;
    int rc;

    if (s->type == SOCK_STREAM) return send_stream(s, buf, len, flags);
    if (to) { dst = ntohl(to->sin_addr); dport = ntohs(to->sin_port); }
    else if (!s->connected) return -EDESTADDRREQ;
    if (len > (s->type == SOCK_DGRAM ? UDP_PAYLOAD_MAX : IP_PAYLOAD_MAX)) return -EMSGSIZE;

    if ((rc = wait_configured()) != 0) return rc;
    irq = local_irq_save();
    if (s->type == SOCK_DGRAM) {
        rc = autobind(s);
        if (!rc) rc = udp_send(s->laddr, s->lport, dst, dport, buf, len);
    } else {
        memcpy(net_tx_payload(), buf, len);
        rc = net_ip_send(IPPROTO_ICMP, 0, dst, len);
    }
    local_irq_restore(irq);
    return rc < 0 ? rc : (ssize_t)len;
}

static void fill_from(struct sockaddr_in *from, uint32_t addr, uint16_t port) {
    if (!from) return;
    from->sin_family = AF_INET;
    from->sin_port = htons(port);
    from->sin_addr = htonl(addr);
    memset(from->sin_zero, 0, sizeof(from->sin_zero));
}

ssize_t sock_recv(struct sock *s, void *buf, size_t len, int flags, struct sockaddr_in *from) {
    uintptr_t irq = local_irq_save();
    struct waiter w;
    ssize_t rc = 0;

    waiter_init(&w, s->rcv_timeout_ms);
    if (s->type == SOCK_STREAM) {
        if (!s->tcp || s->listening) rc = -ENOTCONN;
        else if (s->shut_rd) rc = 0;
        else {
            for (;;) {
                rc = tcp_read(s->tcp, buf, len, flags & MSG_PEEK);
                if (rc != -EAGAIN) break;
                if (nonblocking(s, flags)) break;
                if ((rc = wait_event(s, &w)) != 0) break;
            }
            if (rc >= 0) fill_from(from, s->raddr, s->rport);
        }
    } else {
        while (!s->rxq) {
            if (nonblocking(s, flags)) { rc = -EAGAIN; break; }
            if ((rc = wait_event(s, &w)) != 0) break;
        }
        if (s->rxq) {
            struct dgram *d = s->rxq;
            rc = d->len < len ? d->len : (ssize_t)len;
            memcpy(buf, d->data, (size_t)rc);
            fill_from(from, d->src, d->sport);
            if (!(flags & MSG_PEEK)) {
                s->rxq = d->next;
                s->rxq_n--;
                kfree(d);
            }
        }
    }
    local_irq_restore(irq);
    return rc;
}

int sock_shutdown(struct sock *s, int how) {
    uintptr_t irq = local_irq_save();
    int rc = 0;

    if (how < SHUT_RD || how > SHUT_RDWR) rc = -EINVAL;
    else if (s->type == SOCK_STREAM && !s->tcp) rc = -ENOTCONN;
    else {
        if (how != SHUT_WR) s->shut_rd = true;
        if (how != SHUT_RD && s->type == SOCK_STREAM) rc = tcp_shutdown_wr(s->tcp);
        sock_wake(s);
    }
    local_irq_restore(irq);
    return rc;
}

void sock_name(const struct sock *s, bool peer, struct sockaddr_in *out) {
    if (peer) fill_from(out, s->raddr, s->rport);
    else fill_from(out, s->laddr ? s->laddr : net_if.ip, s->lport);
}

/* ---- options -------------------------------------------------------------------------- */

int sock_setopt(struct sock *s, int level, int name, const void *val, size_t len) {
    int32_t v = len >= sizeof(int32_t) ? *(const int32_t *)val : 0;

    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_REUSEADDR: s->reuse = v != 0; return 0;
        case SO_RCVTIMEO:
        case SO_SNDTIMEO: {
            if (len < sizeof(struct linux_timeval)) return -EINVAL;
            const struct linux_timeval *tv = val;
            uint32_t ms = (uint32_t)tv->tv_sec * 1000u + (uint32_t)tv->tv_usec / 1000u;
            if (ms == 0 && (tv->tv_sec || tv->tv_usec)) ms = 1;
            if (name == SO_RCVTIMEO) s->rcv_timeout_ms = ms; else s->snd_timeout_ms = ms;
            return 0;
        }
        case SO_BROADCAST: case SO_KEEPALIVE: case SO_SNDBUF: case SO_RCVBUF: case SO_LINGER: return 0;
        }
    } else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
        return 0;                                       /* segments are never delayed */
    }
    return -ENOPROTOOPT;
}

int sock_getopt(struct sock *s, int level, int name, void *val, size_t *len) {
    int32_t v;

    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    switch (name) {
    case SO_TYPE:  v = s->type; break;
    case SO_ERROR: {
        uintptr_t irq = local_irq_save();
        v = s->tcp ? tcp_take_error(s->tcp) : 0;
        if (!v) { v = s->err; s->err = 0; }
        local_irq_restore(irq);
        break;
    }
    case SO_REUSEADDR: v = s->reuse; break;
    case SO_RCVTIMEO:
    case SO_SNDTIMEO: {
        uint32_t ms = name == SO_RCVTIMEO ? s->rcv_timeout_ms : s->snd_timeout_ms;
        struct linux_timeval tv = { (int32_t)(ms / 1000), (int32_t)(ms % 1000 * 1000) };
        if (*len < sizeof(tv)) return -EINVAL;
        memcpy(val, &tv, sizeof(tv));
        *len = sizeof(tv);
        return 0;
    }
    default: return -ENOPROTOOPT;
    }
    if (*len < sizeof(v)) return -EINVAL;
    memcpy(val, &v, sizeof(v));
    *len = sizeof(v);
    return 0;
}

/* ---- vfs driver ----------------------------------------------------------------------- */

static ssize_t sockfs_read(vnode_t *v, off_t pos, void *buf, size_t len) {
    (void)pos;
    return sock_recv(v->data, buf, len, 0, NULL);
}

static ssize_t sockfs_write(vnode_t *v, off_t pos, const void *buf, size_t len) {
    (void)pos;
    return sock_send(v->data, buf, len, 0, NULL);
}

static int sockfs_open(void *fs, const char *path, int flags, mode_t mode, vnode_t *v) {
    (void)fs; (void)path; (void)flags; (void)mode; (void)v;
    return -ENOENT;                                     /* only socket() creates sockets */
}

static int sockfs_close(vnode_t *v) {
    struct sock *s = v->data;
    uintptr_t irq = local_irq_save();

    if (s->listening) tcp_drop_children(s);
    if (s->tcp) tcp_close(s->tcp);
    sock_free(s);
    local_irq_restore(irq);
    return 0;
}

static int sockfs_getattr(vnode_t *v, struct kstat64 *st) {
    (void)v;
    st->st_mode = S_IFSOCK_MODE | 0777;
    st->st_nlink = 1;
    return 0;
}

static void sockfs_set_nonblock(vnode_t *v, bool on) {
    ((struct sock *)v->data)->nonblock = on;
}

static uint32_t sockfs_poll(vnode_t *v) {
    struct sock *s = v->data;
    uintptr_t irq = local_irq_save();
    uint32_t ev = 0;

    if (s->type != SOCK_STREAM) {
        ev = POLLOUT | (s->rxq ? POLLIN : 0) | (s->err ? POLLERR : 0);
    } else if (s->listening) {
        ev = s->acc_n ? POLLIN : 0;
    } else if (!s->tcp) {
        ev = POLLOUT | POLLHUP;
    } else {
        int st = tcp_state(s->tcp);
        if (tcp_readable(s->tcp) || tcp_eof(s->tcp)) ev |= POLLIN;
        if ((st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) && tcp_writable(s->tcp)) ev |= POLLOUT;
        if (st == TCP_CLOSED) ev |= POLLHUP | POLLOUT | (s->connected ? 0 : POLLERR);
    }
    local_irq_restore(irq);
    return ev;
}

static int ifreq_ioctl(uint32_t cmd, void *uarg) {
    struct ifreq r;

    if (copy_from_user(&r, uarg, sizeof(r)) != 0) return -EFAULT;
    if (strcmp(r.name, "eth0") != 0 || !net_if.present) return -ENODEV;
    memset(&r.u, 0, sizeof(r.u));
    switch (cmd) {
    case SIOCGIFADDR:    fill_from(&r.u.addr, net_if.ip, 0); break;
    case SIOCGIFNETMASK: fill_from(&r.u.addr, net_if.mask, 0); break;
    case SIOCGIFBRDADDR: fill_from(&r.u.addr, net_if.ip | ~net_if.mask, 0); break;
    case SIOCGIFFLAGS:   r.u.flags = IFF_UP_RUNNING; break;
    case SIOCGIFMTU:     r.u.mtu = NET_MTU; break;
    case SIOCGIFHWADDR:  r.u.hw.family = 1; memcpy(r.u.hw.data, net_if.mac, 6); break;
    default: return -EINVAL;
    }
    return copy_to_user(uarg, &r, sizeof(r)) ? -EFAULT : 0;
}

static int sockfs_ioctl(vnode_t *v, uint32_t cmd, void *arg) {
    struct sock *s = v->data;
    int32_t val;

    switch (cmd) {
    case FIONBIO:
        if (copy_from_user(&val, arg, sizeof(val)) != 0) return -EFAULT;
        s->nonblock = val != 0;
        return 0;
    case FIONREAD: {
        uintptr_t irq = local_irq_save();
        val = s->type == SOCK_STREAM ? (s->tcp ? (int32_t)tcp_readable(s->tcp) : 0) : s->rxq ? s->rxq->len : 0;
        local_irq_restore(irq);
        return copy_to_user(arg, &val, sizeof(val)) ? -EFAULT : 0;
    }
    case SIOCGIFFLAGS: case SIOCGIFADDR: case SIOCGIFBRDADDR: case SIOCGIFNETMASK:
    case SIOCGIFMTU: case SIOCGIFHWADDR:
        return ifreq_ioctl(cmd, arg);
    }
    return -ENOTTY;
}

static vfs_driver_t sockfs = {
    .fs_name = "sockfs",
    .open = sockfs_open,
    .close = sockfs_close,
    .read = sockfs_read,
    .write = sockfs_write,
    .getattr = sockfs_getattr,
    .ioctl = sockfs_ioctl,
    .set_nonblock = sockfs_set_nonblock,
    .poll = sockfs_poll,
};
