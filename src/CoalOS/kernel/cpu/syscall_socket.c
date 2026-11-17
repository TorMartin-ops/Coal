/**
 * @file syscall_socket.c
 * @brief Socket system calls: the direct i386 calls (359..373) and socketcall(2).
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
    uint32_t name, namelen, iov, iovlen, control, controllen, flags;
};

/* The socket behind fd, with a reference held in *sf (release with sys_file_put). */
static struct sock *grab(uint32_t fd, sys_file_t **sf, long *err) {
    *sf = sys_file_get((int)fd);
    if (!*sf) { *err = -EBADF; return NULL; }
    struct sock *s = sock_of(*sf);
    if (!s) { sys_file_put(*sf); *err = -ENOTSOCK; }
    return s;
}

static long get_addr(uint32_t uaddr, uint32_t len, struct sockaddr_in *sa) {
    if (len < sizeof(*sa)) return -EINVAL;
    if (copy_from_user(sa, (const void *)uaddr, sizeof(*sa)) != 0) return -EFAULT;
    return sa->sin_family == AF_INET ? 0 : -EAFNOSUPPORT;
}

/* Stores sa and its length through the user's (addr, addrlen) pair; both may be NULL. */
static long put_addr(uint32_t uaddr, uint32_t ulen, const struct sockaddr_in *sa) {
    uint32_t len;

    if (!uaddr || !ulen) return 0;
    if (copy_from_user(&len, (const void *)ulen, sizeof(len)) != 0) return -EFAULT;
    uint32_t n = len < sizeof(*sa) ? len : sizeof(*sa), full = sizeof(*sa);
    if (copy_to_user((void *)uaddr, sa, n) != 0 || copy_to_user((void *)ulen, &full, sizeof(full)) != 0) return -EFAULT;
    return 0;
}

/* Sends len user bytes in chunks; a stream is written completely unless the socket is non-blocking. */
static long send_user(struct sock *s, uint32_t ubuf, uint32_t len, int flags, const struct sockaddr_in *to) {
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
    } while ((uint32_t)total < len);
    kfree(kbuf);
    return total;
}

static long recv_user(struct sock *s, uint32_t ubuf, uint32_t len, int flags, struct sockaddr_in *from) {
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

long sc_bind(uint32_t fd, uint32_t uaddr, uint32_t len) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    r = get_addr(uaddr, len, &sa);
    if (!r) r = sock_bind(s, ntohl(sa.sin_addr), ntohs(sa.sin_port));
    sys_file_put(sf);
    return r;
}

long sc_connect(uint32_t fd, uint32_t uaddr, uint32_t len) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    r = get_addr(uaddr, len, &sa);
    if (!r) r = sock_connect(s, ntohl(sa.sin_addr), ntohs(sa.sin_port));
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

long sc_accept4(uint32_t fd, uint32_t uaddr, uint32_t ulen, uint32_t flags) {
    struct sockaddr_in sa;
    uint32_t addr;
    uint16_t port;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (flags & ~(uint32_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) r = -EINVAL;
    else r = sock_accept(s, &addr, &port, (int)flags);
    sys_file_put(sf);
    if (r < 0) return r;
    if (flags & SOCK_CLOEXEC) get_current_process()->cloexec |= 1u << r;
    sa = (struct sockaddr_in){ AF_INET, htons(port), htonl(addr), { 0 } };
    put_addr(uaddr, ulen, &sa);
    return r;
}

static long name_of(uint32_t fd, uint32_t uaddr, uint32_t ulen, bool peer) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (peer && !s->connected && !s->tcp) r = -ENOTCONN;
    else { sock_name(s, peer, &sa); r = put_addr(uaddr, ulen, &sa); }
    sys_file_put(sf);
    return r;
}

long sc_getsockname(uint32_t fd, uint32_t uaddr, uint32_t ulen) { return name_of(fd, uaddr, ulen, false); }
long sc_getpeername(uint32_t fd, uint32_t uaddr, uint32_t ulen) { return name_of(fd, uaddr, ulen, true); }

long sc_sendto(uint32_t fd, uint32_t buf, uint32_t len, uint32_t flags, uint32_t uaddr, uint32_t alen) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if (uaddr && (r = get_addr(uaddr, alen, &sa)) != 0) { sys_file_put(sf); return r; }
    r = send_user(s, buf, len, (int)flags, uaddr ? &sa : NULL);
    sys_file_put(sf);
    return r;
}

long sc_recvfrom(uint32_t fd, uint32_t buf, uint32_t len, uint32_t flags, uint32_t uaddr, uint32_t ulen) {
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    r = recv_user(s, buf, len, (int)flags, uaddr ? &sa : NULL);
    sys_file_put(sf);
    if (r >= 0 && uaddr) put_addr(uaddr, ulen, &sa);
    return r;
}

static long load_msg(uint32_t umsg, struct linux_msghdr *m, struct linux_iovec *iov) {
    if (copy_from_user(m, (const void *)umsg, sizeof(*m)) != 0) return -EFAULT;
    if (m->iovlen > IOV_MAX) return -EMSGSIZE;
    return copy_from_user(iov, (const void *)m->iov, m->iovlen * sizeof(*iov)) ? -EFAULT : 0;
}

long sc_sendmsg(uint32_t fd, uint32_t umsg, uint32_t flags) {
    struct linux_msghdr m;
    struct linux_iovec iov[IOV_MAX];
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0, total = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if ((r = load_msg(umsg, &m, iov)) == 0 && m.name) r = get_addr(m.name, m.namelen, &sa);
    for (uint32_t i = 0; !r && i < m.iovlen; i++) {
        long n = send_user(s, iov[i].iov_base, iov[i].iov_len, (int)flags, m.name ? &sa : NULL);
        if (n < 0) { r = total ? 0 : n; if (!total) total = n; break; }
        total += n;
        if ((uint32_t)n < iov[i].iov_len || s->type != SOCK_STREAM) break;
    }
    sys_file_put(sf);
    return r ? r : total;
}

long sc_recvmsg(uint32_t fd, uint32_t umsg, uint32_t flags) {
    struct linux_msghdr m;
    struct linux_iovec iov[IOV_MAX];
    struct sockaddr_in sa;
    sys_file_t *sf;
    long r = 0;
    struct sock *s = grab(fd, &sf, &r);

    if (!s) return r;
    if ((r = load_msg(umsg, &m, iov)) == 0) {
        uint32_t first = 0;
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

    if (!s) return r;
    r = sock_shutdown(s, (int)how);
    sys_file_put(sf);
    return r;
}

long sc_setsockopt(uint32_t fd, uint32_t level, uint32_t name, uint32_t uval, uint32_t len) {
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

long sc_getsockopt(uint32_t fd, uint32_t level, uint32_t name, uint32_t uval, uint32_t ulen) {
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

/* socketcall(2): the one i386 entry point of old libcs. Arguments are an array of 32-bit words. */
long sc_socketcall(uint32_t call, uint32_t uargs) {
    static const uint8_t nargs[] = { 0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4 };
    uint32_t a[6] = { 0 };

    if (call < 1 || call >= sizeof(nargs)) return -EINVAL;
    if (copy_from_user(a, (const void *)uargs, nargs[call] * sizeof(a[0])) != 0) return -EFAULT;
    switch (call) {
    case 1:  return sc_socket(a[0], a[1], a[2]);
    case 2:  return sc_bind(a[0], a[1], a[2]);
    case 3:  return sc_connect(a[0], a[1], a[2]);
    case 4:  return sc_listen(a[0], a[1]);
    case 5:  return sc_accept4(a[0], a[1], a[2], 0);
    case 6:  return sc_getsockname(a[0], a[1], a[2]);
    case 7:  return sc_getpeername(a[0], a[1], a[2]);
    case 9:  return sc_sendto(a[0], a[1], a[2], a[3], 0, 0);
    case 10: return sc_recvfrom(a[0], a[1], a[2], a[3], 0, 0);
    case 11: return sc_sendto(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 12: return sc_recvfrom(a[0], a[1], a[2], a[3], a[4], a[5]);
    case 13: return sc_shutdown(a[0], a[1]);
    case 14: return sc_setsockopt(a[0], a[1], a[2], a[3], a[4]);
    case 15: return sc_getsockopt(a[0], a[1], a[2], a[3], a[4]);
    case 16: return sc_sendmsg(a[0], a[1], a[2]);
    case 17: return sc_recvmsg(a[0], a[1], a[2]);
    case 18: return sc_accept4(a[0], a[1], a[2], a[3]);
    default: return -EOPNOTSUPP;                    /* socketpair */
    }
}
