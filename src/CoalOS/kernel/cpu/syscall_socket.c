/**
 * @file syscall_socket.c
 * @brief Socket system calls: the direct socket calls.
 *
 * Marshals sockaddr, msghdr, iovec and option values across the user boundary
 * and forwards to kernel/net/socket.c, which only sees kernel buffers.
 */

#include "syscall_internal.h"
#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/lib/string.h>

#define SEND_CHUNK  4096
#define OPT_MAX     64
#define IOV_MAX     16

struct linux_msghdr {
    uintptr_t name;
    uint32_t namelen, pad0;
    uintptr_t iov;
    size_t iovlen;
    uintptr_t control;
    size_t controllen;
    uint32_t flags, pad1;
};

/* The socket behind fd, with a reference held in *sf (release with sys_file_put). */
static struct sock *grab(uint32_t fd, sys_file_t **sf, long *err) {
    *sf = sys_file_get((int)fd);
    if (!*sf) { *err = -EBADF; return NULL; }
    struct sock *s = sock_of(*sf);
    if (!s) { sys_file_put(*sf); *err = -ENOTSOCK; }
    return s;
}

static bool is_socketpair(uint32_t fd) {
    sys_file_t *sf = sys_file_get((int)fd);
    bool pair = sf && sys_is_socketpair(sf);

    if (sf) sys_file_put(sf);
    return pair;
}

static long get_addr(uintptr_t uaddr, uint32_t len, struct sockaddr_in *sa) {
    if (len < sizeof(*sa)) return -EINVAL;
    if (copy_from_user(sa, (const void *)uaddr, sizeof(*sa)) != 0) return -EFAULT;
    return sa->sin_family == AF_INET ? 0 : -EAFNOSUPPORT;
}

/* Stores sa and its length through the user's (addr, addrlen) pair; both may be NULL. */
static long put_addr(uintptr_t uaddr, uintptr_t ulen, const struct sockaddr_in *sa) {
    uint32_t len;

    if (!uaddr || !ulen) return 0;
    if (copy_from_user(&len, (const void *)ulen, sizeof(len)) != 0) return -EFAULT;
    uint32_t n = len < sizeof(*sa) ? len : sizeof(*sa), full = sizeof(*sa);
    if (copy_to_user((void *)uaddr, sa, n) != 0 || copy_to_user((void *)ulen, &full, sizeof(full)) != 0) return -EFAULT;
    return 0;
}

/* Protocols behind struct sock_ops take the sockaddr as it came from user space. */
#define RAW_ADDR_MAX 32

static long get_raw_addr(uintptr_t uaddr, uint32_t len, uint8_t *kaddr, size_t *klen) {
    if (len > RAW_ADDR_MAX) return -EINVAL;
    *klen = len;
    return copy_from_user(kaddr, (const void *)uaddr, len) ? -EFAULT : 0;
}

/* Like put_addr() for a raw sockaddr of klen bytes. */
static long put_raw_addr(uintptr_t uaddr, uintptr_t ulen, const uint8_t *kaddr, size_t klen) {
    uint32_t len, full = (uint32_t)klen;

    if (!uaddr || !ulen) return 0;
    if (copy_from_user(&len, (const void *)ulen, sizeof(len)) != 0) return -EFAULT;
    if (copy_to_user((void *)uaddr, kaddr, MIN(len, full)) != 0 || copy_to_user((void *)ulen, &full, sizeof(full)) != 0) return -EFAULT;
    return 0;
}

/* Sends len user bytes in chunks; a stream is written completely unless the socket is non-blocking. */
static long send_user(struct sock *s, uintptr_t ubuf, size_t len, int flags, const struct sockaddr_in *to) {
    long total = 0;
    size_t chunk = len < SEND_CHUNK ? len : SEND_CHUNK;
    uint8_t *kbuf = kmalloc(chunk ? chunk : 1);

    if (!kbuf) return -ENOMEM;
    do {
        size_t n = len - total < chunk ? len - total : chunk;
        long r = copy_from_user(kbuf, (const char *)ubuf + total, n) ? -EFAULT : sock_send(s, kbuf, n, flags, to);
        if (r < 0) { if (!total) total = r; break; }
        total += r;
        if ((size_t)r < n || s->type != SOCK_STREAM) break;
    } while ((size_t)total < len);
    kfree(kbuf);
    return total;
}

static long recv_user(struct sock *s, uintptr_t ubuf, size_t len, int flags, struct sockaddr_in *from) {
    size_t n = len < SEND_CHUNK ? len : SEND_CHUNK;
    uint8_t *kbuf = kmalloc(n ? n : 1);
    long r;

    if (!kbuf) return -ENOMEM;
    r = sock_recv(s, kbuf, n, flags, from);
    if (r > 0 && copy_to_user((void *)ubuf, kbuf, (size_t)r) != 0) r = -EFAULT;
    kfree(kbuf);
    return r;
}

long sc_socket(uint32_t domain, uint32_t type, uint32_t protocol) {
    long fd = sock_create((int)domain, (int)type, (int)protocol);
    if (fd >= 0 && (type & SOCK_CLOEXEC)) get_current_process()->cloexec |= 1u << fd;
    return fd;
}

/* socketpair(AF_UNIX, SOCK_STREAM): two connected descriptors backed by a pair of pipes. */
long sc_socketpair(uint32_t domain, uint32_t type, uint32_t protocol, uintptr_t usv) {
    int fds[2];

    if (domain != AF_UNIX) return -EAFNOSUPPORT;
    if ((type & 0xF) != SOCK_STREAM) return -ESOCKTNOSUPPORT;
    if (protocol) return -EPROTONOSUPPORT;
    long r = sys_socketpair(fds, type & SOCK_NONBLOCK);
    if (r < 0) return r;
    if (copy_to_user((void *)usv, fds, sizeof(fds)) != 0) {
        sys_close(fds[0]);
        sys_close(fds[1]);
        return -EFAULT;
    }
    if (type & SOCK_CLOEXEC) get_current_process()->cloexec |= 1u << fds[0] | 1u << fds[1];
    return 0;
}

long sc_bind(uint32_t fd, uintptr_t uaddr, uint32_t len) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (s->ops) {
        uint8_t raw[RAW_ADDR_MAX];
        size_t rlen;
        r = get_raw_addr(uaddr, len, raw, &rlen);
        if (!r) r = s->ops->bind(s, raw, rlen);
    } else {
        r = get_addr(uaddr, len, &sa);
        if (!r) r = sock_bind(s, ntohl(sa.sin_addr), ntohs(sa.sin_port));
    }
    sys_file_put(sf);
    return r;
}

long sc_connect(uint32_t fd, uintptr_t uaddr, uint32_t len) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (s->ops) {
        uint8_t raw[RAW_ADDR_MAX];
        size_t rlen;
        r = get_raw_addr(uaddr, len, raw, &rlen);
        if (!r) r = s->ops->connect(s, raw, rlen);
    } else {
        r = get_addr(uaddr, len, &sa);
        if (!r) r = sock_connect(s, ntohl(sa.sin_addr), ntohs(sa.sin_port));
    }
    sys_file_put(sf);
    return r;
}

long sc_listen(uint32_t fd, uint32_t backlog) {
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    r = sock_listen(s, (int)backlog);
    sys_file_put(sf);
    return r;
}

long sc_accept4(uint32_t fd, uintptr_t uaddr, uintptr_t ulen, uint32_t flags) {
    struct sockaddr_in sa;
    uint32_t addr;
    uint16_t port;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    uint8_t raw[RAW_ADDR_MAX];
    size_t rlen = 0;
    if (flags & ~(uint32_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) r = -EINVAL;
    else if (s->ops) r = s->ops->accept(s, (int)flags, raw, &rlen);
    else r = sock_accept(s, &addr, &port, (int)flags);
    sys_file_put(sf);
    if (r < 0) return r;
    if (flags & SOCK_CLOEXEC) get_current_process()->cloexec |= 1u << r;
    if (rlen) {
        put_raw_addr(uaddr, ulen, raw, rlen);
    } else {
        sa = (struct sockaddr_in){ AF_INET, htons(port), htonl(addr), { 0 } };
        put_addr(uaddr, ulen, &sa);
    }
    return r;
}

static long name_of(uint32_t fd, uintptr_t uaddr, uintptr_t ulen, bool peer) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (s->ops) {
        uint8_t raw[RAW_ADDR_MAX];
        size_t rlen;
        r = s->ops->name(s, peer, raw, &rlen);
        if (!r) r = put_raw_addr(uaddr, ulen, raw, rlen);
    } else if (peer && !s->connected && !s->tcp) r = -ENOTCONN;
    else { sock_name(s, peer, &sa); r = put_addr(uaddr, ulen, &sa); }
    sys_file_put(sf);
    return r;
}

long sc_getsockname(uint32_t fd, uintptr_t uaddr, uintptr_t ulen) { return name_of(fd, uaddr, ulen, false); }
long sc_getpeername(uint32_t fd, uintptr_t uaddr, uintptr_t ulen) { return name_of(fd, uaddr, ulen, true); }

long sc_sendto(uint32_t fd, uintptr_t buf, size_t len, uint32_t flags, uintptr_t uaddr, uint32_t alen) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r == -ENOTSOCK && is_socketpair(fd) ? sc_write(fd, buf, len) : r;
    if (uaddr && !s->ops && (r = get_addr(uaddr, alen, &sa)) != 0) { sys_file_put(sf); return r; }
    r = send_user(s, buf, len, (int)flags, uaddr && !s->ops ? &sa : NULL);
    sys_file_put(sf);
    return r;
}

long sc_recvfrom(uint32_t fd, uintptr_t buf, size_t len, uint32_t flags, uintptr_t uaddr, uintptr_t ulen) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r == -ENOTSOCK && is_socketpair(fd) ? sc_read(fd, buf, len) : r;
    r = recv_user(s, buf, len, (int)flags, uaddr && !s->ops ? &sa : NULL);
    sys_file_put(sf);
    if (r >= 0 && uaddr && !s->ops) put_addr(uaddr, ulen, &sa);
    return r;
}

static long load_msg(uintptr_t umsg, struct linux_msghdr *m, struct linux_iovec *iov) {
    if (copy_from_user(m, (const void *)umsg, sizeof(*m)) != 0) return -EFAULT;
    if (m->iovlen > IOV_MAX) return -EMSGSIZE;
    return copy_from_user(iov, (const void *)m->iov, m->iovlen * sizeof(*iov)) ? -EFAULT : 0;
}

long sc_sendmsg(uint32_t fd, uintptr_t umsg, uint32_t flags) {
    struct linux_msghdr m;
    struct linux_iovec iov[IOV_MAX];
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0, total = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if ((r = load_msg(umsg, &m, iov)) == 0 && m.name) r = get_addr(m.name, m.namelen, &sa);
    for (size_t i = 0; !r && i < m.iovlen; i++) {
        long n = send_user(s, iov[i].iov_base, iov[i].iov_len, (int)flags, m.name ? &sa : NULL);
        if (n < 0) { r = total ? 0 : n; if (!total) total = n; break; }
        total += n;
        if ((size_t)n < iov[i].iov_len || s->type != SOCK_STREAM) break;
    }
    sys_file_put(sf);
    return r ? r : total;
}

long sc_recvmsg(uint32_t fd, uintptr_t umsg, uint32_t flags) {
    struct linux_msghdr m;
    struct linux_iovec iov[IOV_MAX];
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if ((r = load_msg(umsg, &m, iov)) == 0) {
        size_t first = 0;
        while (first < m.iovlen && !iov[first].iov_len) first++;
        r = first < m.iovlen ? recv_user(s, iov[first].iov_base, iov[first].iov_len, (int)flags, &sa) : 0;
    }
    sys_file_put(sf);
    if (r >= 0) {                                   /* the message ends in the first buffer: no scatter */
        uint32_t zero = 0;
        if (m.name) put_addr(m.name, umsg + offsetof(struct linux_msghdr, namelen), &sa);
        if (copy_to_user((char *)umsg + offsetof(struct linux_msghdr, flags), &zero, sizeof(zero)) != 0) return -EFAULT;
    }
    return r;
}

long sc_shutdown(uint32_t fd, uint32_t how) {
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) {
        if (r != -ENOTSOCK || !(sf = sys_file_get((int)fd))) return r;
        r = sys_is_socketpair(sf) ? sys_socketpair_shutdown(sf, (int)how) : -ENOTSOCK;
        sys_file_put(sf);
        return r;
    }
    r = sock_shutdown(s, (int)how);
    sys_file_put(sf);
    return r;
}

long sc_setsockopt(uint32_t fd, int level, int name, uintptr_t uval, uint32_t len) {
    uint8_t val[OPT_MAX];
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (len > OPT_MAX) r = -EINVAL;
    else if (len && copy_from_user(val, (const void *)uval, len) != 0) r = -EFAULT;
    else r = sock_setopt(s, (int)level, (int)name, val, len);
    sys_file_put(sf);
    return r;
}

long sc_getsockopt(uint32_t fd, int level, int name, uintptr_t uval, uintptr_t ulen) {
    uint8_t val[OPT_MAX];
    uint32_t len;
    size_t n;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (copy_from_user(&len, (const void *)ulen, sizeof(len)) != 0) r = -EFAULT;
    else {
        n = len < OPT_MAX ? len : OPT_MAX;
        r = sock_getopt(s, (int)level, (int)name, val, &n);
        len = (uint32_t)n;
        if (!r && (copy_to_user((void *)uval, val, n) != 0 || copy_to_user((void *)ulen, &len, sizeof(len)) != 0)) r = -EFAULT;
    }
    sys_file_put(sf);
    return r;
}

long sc_accept(uint32_t fd, uintptr_t uaddr, uintptr_t ulen) { return sc_accept4(fd, uaddr, ulen, 0); }
