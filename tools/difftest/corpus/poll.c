/* poll, select, pselect6, ppoll on pipes, sockets and files; timeouts and readiness reporting. */
#include "trace.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>

struct ts { long s, n; };
struct tv { long s, u; };

static void showp(const char *label, long r, struct pollfd *p, int n) {
    char o[200] = "";
    for (int i = 0; i < n; i++) {
        char t[40];
        snprintf(t, sizeof t, "[fd%d:%s%s%s%s%s]", i, p[i].revents & POLLIN ? "I" : "", p[i].revents & POLLOUT ? "O" : "",
                 p[i].revents & POLLHUP ? "H" : "", p[i].revents & POLLERR ? "E" : "", p[i].revents & POLLNVAL ? "N" : "");
        strcat(o, t);
    }
    tr(label, r, "%s", o);
}

int main(void) {
    init();
    int p[2], q[2];
    K(SYS_pipe, p);
    K(SYS_pipe, q);
    struct pollfd f[3];
    f[0] = (struct pollfd){p[0], POLLIN, 0};
    f[1] = (struct pollfd){p[1], POLLOUT, 0};
    f[2] = (struct pollfd){q[0], POLLIN, 0};
    showp("poll(timeout 0: write end ready only)", K(SYS_poll, f, 3, 0), f, 3);
    K(SYS_write, q[1], "x", 1);
    showp("poll(after write to q)", K(SYS_poll, f, 3, 0), f, 3);
    K(SYS_write, p[1], "y", 1);
    showp("poll(both readable)", K(SYS_poll, f, 3, 100), f, 3);
    K(SYS_read, p[0], scratch_, 1);
    K(SYS_read, q[0], scratch_, 1);
    f[0].revents = f[1].revents = f[2].revents = 0;
    struct ts t0, t1;
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t0);
    showp("poll(timeout 50ms, nothing ready)", K(SYS_poll, &f[0], 1, 50), f, 1);
    K(SYS_clock_gettime, CLOCK_MONOTONIC, &t1);
    long el = (t1.s - t0.s) * 1000 + (t1.n - t0.n) / 1000000;
    NOTE("waited between 45ms and 500ms: %s", yn(el >= 45 && el < 500));
    K(SYS_close, q[1]);
    f[2].revents = 0;
    showp("poll(EOF reports HUP)", K(SYS_poll, &f[2], 1, 0), &f[2], 1);
    struct pollfd bad = {77, POLLIN, 0};
    showp("poll(bad fd reports NVAL)", K(SYS_poll, &bad, 1, 0), &bad, 1);
    struct pollfd neg = {-1, POLLIN, 0x55};
    R("poll(negative fd ignored)", K(SYS_poll, &neg, 1, 0));
    NOTE("revents of negative fd cleared: %s", yn(neg.revents == 0));
    R("poll(nfds 0, timeout 10)", K(SYS_poll, 0, 0, 10));
    R("poll(bad pointer)", K(SYS_poll, 8, 1, 0));
    R("poll(nfds huge)", K(SYS_poll, f, 1000000, 0));

    /* select */
    struct tv zt = {0, 0}, us = {0, 1000};
    unsigned long rs[16] = {0}, ws[16] = {0};
    rs[0] = 1UL << p[0]; ws[0] = 1UL << p[1];
    R("select(write ready only)", K(SYS_select, 8, rs, ws, 0, &zt));
    NOTE("read set 0x%lx write set 0x%lx", rs[0], ws[0]);
    K(SYS_write, p[1], "z", 1);
    rs[0] = 1UL << p[0]; ws[0] = 0;
    R("select(read ready)", K(SYS_select, 8, rs, 0, 0, &zt));
    NOTE("read set 0x%lx", rs[0]);
    K(SYS_read, p[0], scratch_, 1);
    rs[0] = 1UL << p[0];
    struct tv tv = {0, 30000};
    R("select(timeout 30ms)", K(SYS_select, 8, rs, 0, 0, &tv));
    NOTE("read set cleared on timeout: %s", yn(rs[0] == 0));
    rs[0] = 1UL << 40;
    R("select(bad fd in set)", K(SYS_select, 64, rs, 0, 0, &zt));
    R("select(nfds negative)", K(SYS_select, -1, 0, 0, 0, 0));
    R("select(nfds 0 timeout)", K(SYS_select, 0, 0, 0, 0, &us));
    rs[0] = 1UL << p[0];
    struct ts pt = {0, 20000000};
    R("pselect6(timeout 20ms)", K(SYS_pselect6, 8, rs, 0, 0, &pt, 0));
    f[0] = (struct pollfd){p[0], POLLIN, 0};
    R("ppoll(timeout 20ms)", K(SYS_ppoll, f, 1, &pt, 0, 8));

    /* sockets: connected stream becomes writable, then readable */
    int sv[2];
    K(SYS_socketpair, AF_UNIX, SOCK_STREAM, 0, sv);
    struct pollfd sp[2] = {{sv[0], POLLIN | POLLOUT, 0}, {sv[1], POLLIN, 0}};
    showp("poll(socketpair idle)", K(SYS_poll, sp, 2, 0), sp, 2);
    K(SYS_write, sv[0], "hi", 2);
    showp("poll(socketpair after write)", K(SYS_poll, sp, 2, 0), sp, 2);
    K(SYS_close, sv[0]);
    showp("poll(peer closed)", K(SYS_poll, &sp[1], 1, 0), &sp[1], 1);

    /* regular files are always ready */
    long fd = K(SYS_open, "/etc/passwd", O_RDONLY);
    struct pollfd rf = {fd, POLLIN | POLLOUT, 0};
    showp("poll(regular file)", K(SYS_poll, &rf, 1, 0), &rf, 1);
    /* signal interrupts a long poll */
    long c = K(SYS_fork);
    if (c == 0) { struct pollfd w = {p[0], POLLIN, 0}; K(SYS_poll, &w, 1, -1); K(SYS_exit, 0); }
    struct ts sl = {0, 50000000};
    K(SYS_nanosleep, &sl, 0);
    K(SYS_kill, c, SIGKILL);
    int st = 0;
    K(SYS_wait4, c, &st, 0, 0);
    NOTE("blocked poll killed by SIGKILL: %s", yn((st & 0x7f) == SIGKILL));
    /* data written by a child wakes a blocked parent poll */
    int w[2]; K(SYS_pipe, w);
    c = K(SYS_fork);
    if (c == 0) { K(SYS_nanosleep, &sl, 0); K(SYS_write, w[1], "go", 2); K(SYS_exit, 0); }
    struct pollfd wp = {w[0], POLLIN, 0};
    showp("poll(blocking until child writes)", K(SYS_poll, &wp, 1, 5000), &wp, 1);
    K(SYS_wait4, c, &st, 0, 0);
    return 0;
}
