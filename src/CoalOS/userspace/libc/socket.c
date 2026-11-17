/* Socket system calls and the IPv4 address helpers. */

#include <libc/syscall.h>
#include <libc/errno.h>
#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/arpa/inet.h>

#define RET(x) return (int)__syscall_ret(x)

int socket(int d, int t, int p)                         { RET(__SC3(SYS_SOCKET, d, t, p)); }
int bind(int fd, const struct sockaddr *a, socklen_t l) { RET(__SC3(SYS_BIND, fd, a, l)); }
int connect(int fd, const struct sockaddr *a, socklen_t l) { RET(__SC3(SYS_CONNECT, fd, a, l)); }
int listen(int fd, int backlog)                         { RET(__SC3(SYS_LISTEN, fd, backlog, 0)); }
int shutdown(int fd, int how)                           { RET(__SC3(SYS_SHUTDOWN, fd, how, 0)); }
int getsockname(int fd, struct sockaddr *a, socklen_t *l) { RET(__SC3(SYS_GETSOCKNAME, fd, a, l)); }
int getpeername(int fd, struct sockaddr *a, socklen_t *l) { RET(__SC3(SYS_GETPEERNAME, fd, a, l)); }
int accept4(int fd, struct sockaddr *a, socklen_t *l, int flags) { RET(__sc(SYS_ACCEPT4, fd, (long)a, (long)l, flags, 0)); }
int accept(int fd, struct sockaddr *a, socklen_t *l)    { return accept4(fd, a, l, 0); }
int setsockopt(int fd, int lv, int n, const void *v, socklen_t l) { RET(__sc(SYS_SETSOCKOPT, fd, lv, (long)n, (long)v, (long)l)); }
int getsockopt(int fd, int lv, int n, void *v, socklen_t *l)      { RET(__sc(SYS_GETSOCKOPT, fd, lv, (long)n, (long)v, (long)l)); }

ssize_t sendto(int fd, const void *b, size_t n, int fl, const struct sockaddr *to, socklen_t l) {
    return __syscall_ret(__sc6(SYS_SENDTO, fd, (long)b, (long)n, fl, (long)to, l));
}

ssize_t recvfrom(int fd, void *b, size_t n, int fl, struct sockaddr *from, socklen_t *l) {
    return __syscall_ret(__sc6(SYS_RECVFROM, fd, (long)b, (long)n, fl, (long)from, (long)l));
}

ssize_t send(int fd, const void *b, size_t n, int fl) { return sendto(fd, b, n, fl, NULL, 0); }
ssize_t recv(int fd, void *b, size_t n, int fl)       { return recvfrom(fd, b, n, fl, NULL, NULL); }

int inet_aton(const char *s, struct in_addr *out) {
    uint32_t addr = 0;

    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        const char *start = s;
        while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
        if (s == start || v > 255 || s - start > 3) return 0;
        addr = addr << 8 | v;
        if (i < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    out->s_addr = htonl(addr);
    return 1;
}

in_addr_t inet_addr(const char *s) {
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : 0xFFFFFFFFu;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    const unsigned char *b = src;
    if ((size_t)snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= size) { errno = ENOSPC; return NULL; }
    return dst;
}

char *inet_ntoa(struct in_addr a) {
    static char buf[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &a, buf, sizeof buf);
}

int inet_pton(int af, const char *s, void *dst) {
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    return inet_aton(s, dst);
}

#include <libc/poll.h>
int poll(struct pollfd *fds, unsigned long n, int timeout_ms) { RET(__SC3(SYS_POLL, fds, n, timeout_ms)); }
