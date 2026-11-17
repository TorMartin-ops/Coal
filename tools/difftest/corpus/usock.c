/* AF_UNIX: socketpair, stream and datagram, bind/listen/connect/accept in one process, address errors. */
#include "trace.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static void sun(struct sockaddr_un *u, const char *p) { memset(u, 0, sizeof *u); u->sun_family = AF_UNIX; strcpy(u->sun_path, p); }
#define SL(p) ((long)(offsetof(struct sockaddr_un, sun_path) + strlen(p) + 1))

int main(void) {
    init();
    scratch("usock");
    int sv[2] = {-1, -1};
    R("socketpair(STREAM)", K(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv));
    R("write(sv0)", K(SYS_write, sv[0], "ping", 4));
    rd("read(sv1)", sv[1], 10);
    R("write(sv1)", K(SYS_write, sv[1], "pong", 4));
    rd("read(sv0)", sv[0], 2);
    rd("read(sv0 rest)", sv[0], 10);
    R("shutdown(sv0,WR)", K(SYS_shutdown, sv[0], SHUT_WR));
    R("read(sv1 eof)", K(SYS_read, sv[1], scratch_, 1));
    ignsig(SIGPIPE);
    R("write(sv0 after SHUT_WR)", K(SYS_write, sv[0], "x", 1));
    R("close(sv0)", K(SYS_close, sv[0]));
    R("read(sv1 eof after close)", K(SYS_read, sv[1], scratch_, 1));
    R("write(sv1 peer closed)", K(SYS_write, sv[1], "x", 1));
    R("close(sv1)", K(SYS_close, sv[1]));

    R("socketpair(DGRAM)", K(SYS_socketpair, AF_UNIX, SOCK_DGRAM, 0, sv));
    R("sendto(dgram 3 bytes)", K(SYS_sendto, sv[0], "abc", 3, 0, 0, 0));
    R("sendto(dgram 2 bytes)", K(SYS_sendto, sv[0], "de", 2, 0, 0, 0));
    char b[8] = {0};
    RF("recvfrom(small buffer truncates)", K(SYS_recvfrom, sv[1], b, 2, 0, 0, 0), "\"%.2s\"", b);
    RF("recvfrom(next datagram)", K(SYS_recvfrom, sv[1], b, 8, 0, 0, 0), "\"%.2s\"", b);
    R("recvfrom(DONTWAIT empty)", K(SYS_recvfrom, sv[1], b, 8, MSG_DONTWAIT, 0, 0));
    K(SYS_close, sv[0]); K(SYS_close, sv[1]);
    R("socketpair(bad type)", K(SYS_socketpair, AF_UNIX, 99, 0, sv));
    R("socketpair(SEQPACKET)", ({ long q = K(SYS_socketpair, AF_UNIX, SOCK_SEQPACKET, 0, sv); if (q == 0) { K(SYS_close, sv[0]); K(SYS_close, sv[1]); } q; }));
    R("socketpair(NONBLOCK|CLOEXEC)", ({ long q = K(SYS_socketpair, AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv);
        long fl = q == 0 ? K(SYS_fcntl, sv[0], F_GETFL) & O_NONBLOCK : 0, fd = q == 0 ? K(SYS_fcntl, sv[0], F_GETFD) : 0;
        NOTE("flags applied nonblock=%s cloexec=%ld", yn(fl), fd); if (q == 0) { K(SYS_close, sv[0]); K(SYS_close, sv[1]); } q; }));
    R("socket(unknown family)", K(SYS_socket, 99, SOCK_STREAM, 0));
    R("socket(AF_UNIX bad protocol)", K(SYS_socket, AF_UNIX, SOCK_STREAM, 7));
    R("read(on non-socket via recvfrom)", K(SYS_recvfrom, 0, b, 1, 0, 0, 0) == -ENOTSOCK ? -ENOTSOCK : 0);

    /* listener in the same process: connect completes against the backlog */
    struct sockaddr_un u;
    sun(&u, "sock");
    long ls = R("socket(STREAM)", K(SYS_socket, AF_UNIX, SOCK_STREAM, 0));
    R("bind", K(SYS_bind, ls, &u, SL("sock")));
    STAT("stat(socket file)", "sock");
    long ls2 = K(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    R("bind(address in use)", K(SYS_bind, ls2, &u, SL("sock")));
    K(SYS_close, ls2);
    R("listen", K(SYS_listen, ls, 4));
    long cs = K(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    R("connect", K(SYS_connect, cs, &u, SL("sock")));
    struct sockaddr_un pa; unsigned len = sizeof pa;
    long as = K(SYS_accept, ls, &pa, &len);
    tr("accept", as >= 0 ? 0 : as, NULL);
    R("write(client)", K(SYS_write, cs, "hello", 5));
    rd("read(accepted)", as, 10);
    struct sockaddr_un na; len = sizeof na;
    RF("getsockname(listener)", K(SYS_getsockname, ls, &na, &len), "family=%d path=%s", na.sun_family, na.sun_path);
    len = sizeof na;
    R("getpeername(client)", K(SYS_getpeername, cs, &na, &len));
    NOTE("peer path is sock: %s", yn(!strcmp(na.sun_path, "sock")));
    R("connect(already connected)", K(SYS_connect, cs, &u, SL("sock")));
    int err = -1; unsigned el = sizeof err;
    R("getsockopt(SO_ERROR)", K(SYS_getsockopt, cs, SOL_SOCKET, SO_ERROR, &err, &el));
    NOTE("SO_ERROR value %d", err);
    int ty = 0; el = sizeof ty;
    R("getsockopt(SO_TYPE)", K(SYS_getsockopt, cs, SOL_SOCKET, SO_TYPE, &ty, &el));
    NOTE("SO_TYPE is STREAM: %s", yn(ty == SOCK_STREAM));
    R("getsockopt(bad level)", K(SYS_getsockopt, cs, 999, 1, &ty, &el));
    R("listen(on connected)", K(SYS_listen, cs, 1));
    R("accept(none pending nonblock)", ({ K(SYS_fcntl, ls, F_SETFL, O_NONBLOCK); K(SYS_accept, ls, 0, 0); }));
    sun(&u, "nosuch");
    long c2 = K(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    R("connect(no such path)", K(SYS_connect, c2, &u, SL("nosuch")));
    sun(&u, "h");
    K(SYS_close, K(SYS_creat, "h", 0644));
    R("connect(to regular file)", K(SYS_connect, c2, &u, SL("h")));
    sun(&u, "h");
    R("bind(path is a file)", K(SYS_bind, c2, &u, SL("h")));
    R("connect(bad addrlen)", K(SYS_connect, c2, &u, 1));
    R("connect(bad family)", ({ struct sockaddr_un v = u; v.sun_family = 99; K(SYS_connect, c2, &v, sizeof v); }));
    R("sendto(unconnected stream)", K(SYS_sendto, c2, "x", 1, 0, 0, 0));
    R("close(listener)", K(SYS_close, ls));
    R("connect(after listener closed)", ({ sun(&u, "sock"); K(SYS_connect, c2, &u, SL("sock")); }));
    R("unlink(socket file)", K(SYS_unlink, "sock"));
    K(SYS_close, cs); K(SYS_close, as); K(SYS_close, c2);

    /* abstract namespace and datagram bind */
    struct sockaddr_un ab; memset(&ab, 0, sizeof ab); ab.sun_family = AF_UNIX; memcpy(ab.sun_path + 1, "dtabs", 5);
    long d1 = K(SYS_socket, AF_UNIX, SOCK_DGRAM, 0), d2 = K(SYS_socket, AF_UNIX, SOCK_DGRAM, 0);
    R("bind(abstract dgram)", K(SYS_bind, d1, &ab, offsetof(struct sockaddr_un, sun_path) + 6));
    R("sendto(abstract)", K(SYS_sendto, d2, "dgram", 5, 0, &ab, offsetof(struct sockaddr_un, sun_path) + 6));
    RF("recvfrom(abstract)", K(SYS_recvfrom, d1, b, 8, 0, 0, 0), "\"%.5s\"", b);
    K(SYS_close, d1); K(SYS_close, d2);

    /* SCM_RIGHTS descriptor passing */
    R("socketpair for sendmsg", K(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv));
    long pf = K(SYS_open, "h", O_RDWR);
    K(SYS_write, pf, "passed", 6);
    struct { struct cmsghdr h; int fd; } cm = {{.cmsg_len = CMSG_LEN(sizeof(int)), .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS}, (int)pf};
    struct iovec io = {"x", 1};
    struct msghdr mh = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = &cm, .msg_controllen = sizeof cm};
    R("sendmsg(SCM_RIGHTS)", K(SYS_sendmsg, sv[0], &mh, 0));
    char rb[1]; struct iovec rio = {rb, 1};
    struct { struct cmsghdr h; int fd; } rc = {{0}, -1};
    struct msghdr rm = {.msg_iov = &rio, .msg_iovlen = 1, .msg_control = &rc, .msg_controllen = sizeof rc};
    R("recvmsg(SCM_RIGHTS)", K(SYS_recvmsg, sv[1], &rm, 0));
    NOTE("received a descriptor: %s", yn(rc.h.cmsg_type == SCM_RIGHTS && rc.fd > 2));
    if (rc.fd > 2) { K(SYS_lseek, rc.fd, 0, SEEK_SET); rd("read(passed fd)", rc.fd, 10); }
    return 0;
}
