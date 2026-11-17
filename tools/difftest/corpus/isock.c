/* AF_INET over loopback: TCP connect/accept/transfer/shutdown, UDP, options, error paths. */
#include "trace.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>

static struct sockaddr_in loop(unsigned port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}
static unsigned portof(long fd) {
    struct sockaddr_in a; unsigned l = sizeof a;
    return K(SYS_getsockname, fd, &a, &l) == 0 ? ntohs(a.sin_port) : 0;
}

int main(void) {
    init();
    ignsig(SIGPIPE);
    long ls = R("socket(TCP)", K(SYS_socket, AF_INET, SOCK_STREAM, 0));
    int one = 1;
    R("setsockopt(SO_REUSEADDR)", K(SYS_setsockopt, ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one));
    struct sockaddr_in a = loop(0);
    R("bind(port 0)", K(SYS_bind, ls, &a, sizeof a));
    unsigned port = portof(ls);
    NOTE("assigned an ephemeral port: %s", yn(port >= 1024));
    struct sockaddr_in got; unsigned gl = sizeof got;
    RF("getsockname", K(SYS_getsockname, ls, &got, &gl), "family=%d len=%u addr_is_loopback=%s", got.sin_family, gl, yn(got.sin_addr.s_addr == htonl(INADDR_LOOPBACK)));
    R("listen", K(SYS_listen, ls, 5));
    long cs = K(SYS_socket, AF_INET, SOCK_STREAM, 0);
    a = loop(port);
    R("connect", K(SYS_connect, cs, &a, sizeof a));
    struct sockaddr_in pa; unsigned pl = sizeof pa;
    long as = K(SYS_accept, ls, &pa, &pl);
    tr("accept", as >= 0 ? 0 : as, "peer_is_loopback=%s len=%u", yn(pa.sin_addr.s_addr == htonl(INADDR_LOOPBACK)), pl);
    R("write(client)", K(SYS_write, cs, "request", 7));
    rd("read(server)", as, 64);
    R("write(server)", K(SYS_write, as, "reply!", 6));
    rd("read(client)", cs, 64);
    pl = sizeof pa;
    R("getpeername(client)", K(SYS_getpeername, cs, &pa, &pl));
    NOTE("peer port is the listener's: %s", yn(ntohs(pa.sin_port) == port));
    int nodelay = 1;
    R("setsockopt(TCP_NODELAY)", K(SYS_setsockopt, cs, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay));
    int v = 0; unsigned vl = sizeof v;
    R("getsockopt(SO_TYPE)", K(SYS_getsockopt, cs, SOL_SOCKET, SO_TYPE, &v, &vl));
    NOTE("type is STREAM: %s", yn(v == SOCK_STREAM));
    R("getsockopt(SO_ERROR)", K(SYS_getsockopt, cs, SOL_SOCKET, SO_ERROR, &v, &vl));
    R("getsockopt(SO_ACCEPTCONN on listener)", K(SYS_getsockopt, ls, SOL_SOCKET, SO_ACCEPTCONN, &v, &vl));
    NOTE("SO_ACCEPTCONN=%d", v);
    R("setsockopt(SO_RCVBUF)", K(SYS_setsockopt, cs, SOL_SOCKET, SO_RCVBUF, &(int){65536}, sizeof(int)));
    R("setsockopt(SO_KEEPALIVE)", K(SYS_setsockopt, cs, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one));
    R("setsockopt(bad level)", K(SYS_setsockopt, cs, 999, 1, &one, sizeof one));

    /* a larger transfer, in both directions, through a forked peer */
    static char big[40000], in[40000];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + (i >> 8));
    long c = K(SYS_fork);
    if (c == 0) {
        long total = 0, r;
        while (total < (long)sizeof in && (r = K(SYS_read, as, in + total, sizeof in - (size_t)total)) > 0) total += r;
        K(SYS_write, as, in, (size_t)total);
        K(SYS_exit, total == (long)sizeof in ? 0 : 1);
    }
    long sent = 0, w;
    while (sent < (long)sizeof big && (w = K(SYS_write, cs, big + sent, sizeof big - (size_t)sent < 4096 ? sizeof big - (size_t)sent : 4096)) > 0) sent += w;
    R("write(40000 bytes in chunks)", sent);
    long got2 = 0, r;
    memset(in, 0, sizeof in);
    while (got2 < (long)sizeof in && (r = K(SYS_read, cs, in + got2, sizeof in - (size_t)got2)) > 0) got2 += r;
    R("read(echo of 40000)", got2);
    NOTE("echo identical: %s", yn(got2 == (long)sizeof big && !memcmp(big, in, sizeof big)));
    int st = 0; K(SYS_wait4, c, &st, 0, 0);
    R("peer exit code", WEXITSTATUS(st));
    K(SYS_close, as);
    R("read(after peer close)", K(SYS_read, cs, in, 10));
    R("write(after peer close, first)", K(SYS_write, cs, "x", 1));
    R("write(after peer close, second)", K(SYS_write, cs, "x", 1));
    R("shutdown(RDWR)", K(SYS_shutdown, cs, SHUT_RDWR));
    R("shutdown(bad how)", K(SYS_shutdown, cs, 7));
    R("close(client)", K(SYS_close, cs));
    R("close(listener)", K(SYS_close, ls));

    /* errors */
    long e = K(SYS_socket, AF_INET, SOCK_STREAM, 0);
    a = loop(port);
    R("connect(closed port)", K(SYS_connect, e, &a, sizeof a));
    K(SYS_close, e);
    e = K(SYS_socket, AF_INET, SOCK_STREAM, 0);
    R("connect(bad length)", K(SYS_connect, e, &a, 3));
    struct sockaddr_in b1 = loop(0);
    K(SYS_bind, e, &b1, sizeof b1);
    unsigned p1 = portof(e);
    long e2 = K(SYS_socket, AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in b2 = loop(p1);
    R("bind(port in use)", K(SYS_bind, e2, &b2, sizeof b2));
    struct sockaddr_in far = loop(0);
    far.sin_addr.s_addr = htonl(0x08080808);
    R("bind(non-local address)", K(SYS_bind, e2, &far, sizeof far));
    R("listen(unbound ok)", K(SYS_listen, e2, 1));
    R("accept(nonblocking empty)", ({ K(SYS_fcntl, e2, F_SETFL, O_NONBLOCK); K(SYS_accept, e2, 0, 0); }));
    R("accept(on non-listening)", K(SYS_accept, e, 0, 0));
    R("write(unconnected)", K(SYS_write, e, "x", 1));
    R("getpeername(unconnected)", K(SYS_getpeername, e, &pa, &pl));
    K(SYS_close, e); K(SYS_close, e2);
    R("socket(bad type)", K(SYS_socket, AF_INET, 77, 0));
    R("socket(raw as root)", ({ long q = K(SYS_socket, AF_INET, SOCK_RAW, IPPROTO_ICMP); if (q >= 0) K(SYS_close, q); q >= 0 ? 0 : q; }));
    R("socket(STREAM, UDP protocol)", K(SYS_socket, AF_INET, SOCK_STREAM, IPPROTO_UDP));
    R("sendto(bad socket)", K(SYS_sendto, 99, "x", 1, 0, 0, 0));

    /* UDP */
    long u1 = K(SYS_socket, AF_INET, SOCK_DGRAM, 0), u2 = K(SYS_socket, AF_INET, SOCK_DGRAM, 0);
    a = loop(0);
    R("bind(udp)", K(SYS_bind, u1, &a, sizeof a));
    unsigned up = portof(u1);
    struct sockaddr_in dst = loop(up);
    R("sendto(udp)", K(SYS_sendto, u2, "datagram", 8, 0, &dst, sizeof dst));
    char buf[32] = {0};
    struct sockaddr_in from; unsigned fl = sizeof from;
    RF("recvfrom(udp)", K(SYS_recvfrom, u1, buf, 32, 0, &from, &fl), "\"%.8s\" from_loopback=%s", buf, yn(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)));
    R("recvfrom(udp DONTWAIT empty)", K(SYS_recvfrom, u1, buf, 32, MSG_DONTWAIT, 0, 0));
    K(SYS_sendto, u2, "peek", 4, 0, &dst, sizeof dst);
    RF("recvfrom(MSG_PEEK)", K(SYS_recvfrom, u1, buf, 32, MSG_PEEK, 0, 0), "\"%.4s\"", buf);
    RF("recvfrom(after peek)", K(SYS_recvfrom, u1, buf, 32, 0, 0, 0), "\"%.4s\"", buf);
    R("connect(udp)", K(SYS_connect, u2, &dst, sizeof dst));
    R("write(connected udp)", K(SYS_write, u2, "conn", 4));
    RF("read(udp)", K(SYS_read, u1, buf, 32), "\"%.4s\"", buf);
    R("sendto(udp zero length)", K(SYS_sendto, u2, "", 0, 0, 0, 0));
    R("recvfrom(udp zero length)", K(SYS_recvfrom, u1, buf, 32, 0, 0, 0));
    static char huge[70000];
    R("sendto(udp too big)", K(SYS_sendto, u2, huge, sizeof huge, 0, 0, 0));
    K(SYS_close, u1); K(SYS_close, u2);
    return 0;
}
