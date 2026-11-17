/* Network applets: ifconfig ping nslookup nc wget */

#include "cb.h"
#include "applets.h"
#include <libc/arpa/inet.h>
#include <libc/netdb.h>
#include <libc/poll.h>
#include <libc/syscall.h>
#include <libc/sys/time.h>
#include <libc/time.h>

#define SIOCGIFFLAGS   0x8913
#define SIOCGIFADDR    0x8915
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891B
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927

struct ifreq {
    char name[16];
    union {
        struct sockaddr_in addr;
        struct { unsigned short family; unsigned char data[14]; } hw;
        short flags;
        int mtu;
    } u;
};

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_timeouts(int fd, int seconds) {
    struct timeval tv = { seconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* Resolves host to an address, reporting failure the way the applets do. */
static int lookup(const char *host, struct in_addr *out) {
    struct hostent *h = gethostbyname(host);
    if (!h) {
        fprintf(stderr, "%s: %s: %s\n", prog, host, h_errno == TRY_AGAIN ? "temporary failure in name resolution"
                                                                           : "unknown host");
        return -1;
    }
    memcpy(out, h->h_addr, 4);
    return 0;
}

/* ---- ifconfig ------------------------------------------------------------------- */

int ifconfig_main(int argc, char **argv) {
    struct ifreq r;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) return cb_err("socket", "AF_INET");
    memset(&r, 0, sizeof r);
    strcpy(r.name, "eth0");
    if (__SC3(SYS_IOCTL, fd, SIOCGIFHWADDR, &r) < 0) {
        fprintf(stderr, "%s: no network interface\n", prog);
        return 1;
    }
    const unsigned char *m = r.u.hw.data;
    printf("eth0      Link encap:Ethernet  HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);

    struct in_addr ip = { 0 }, mask = { 0 }, bcast = { 0 };
    if (__SC3(SYS_IOCTL, fd, SIOCGIFADDR, &r) >= 0) ip = r.u.addr.sin_addr;
    if (__SC3(SYS_IOCTL, fd, SIOCGIFNETMASK, &r) >= 0) mask = r.u.addr.sin_addr;
    if (__SC3(SYS_IOCTL, fd, SIOCGIFBRDADDR, &r) >= 0) bcast = r.u.addr.sin_addr;
    if (ip.s_addr) {
        char a[16], b[16], c[16];
        inet_ntop(AF_INET, &ip, a, sizeof a);
        inet_ntop(AF_INET, &bcast, b, sizeof b);
        inet_ntop(AF_INET, &mask, c, sizeof c);
        printf("          inet addr:%s  Bcast:%s  Mask:%s\n", a, b, c);
    } else {
        printf("          no address (DHCP has not answered yet)\n");
    }
    int mtu = 0;
    if (__SC3(SYS_IOCTL, fd, SIOCGIFMTU, &r) >= 0) mtu = r.u.mtu;
    printf("          UP BROADCAST RUNNING MULTICAST  MTU:%d\n", mtu);
    close(fd);
    return 0;
}

/* ---- ping ----------------------------------------------------------------------- */

#define PING_DATA 56

static unsigned short icmp_csum(const unsigned char *p, int n) {
    unsigned long sum = 0;
    for (; n > 1; n -= 2, p += 2) sum += (unsigned long)(p[0] << 8 | p[1]);
    if (n) sum += (unsigned long)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

int ping_main(int argc, char **argv) {
    int count = 4, wait_s = 2, opt;
    struct in_addr dst;

    optind = 1;
    while ((opt = getopt(argc, argv, "c:W:")) != -1) {
        if (opt == 'c') count = atoi(optarg);
        else if (opt == 'W') wait_s = atoi(optarg);
        else return cb_usage("[-c count] [-W seconds] host");
    }
    if (optind != argc - 1 || count < 1) return cb_usage("[-c count] [-W seconds] host");
    if (lookup(argv[optind], &dst) < 0) return 1;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) return cb_err("socket", "raw ICMP");
    struct sockaddr_in to = { AF_INET, 0, dst, { 0 } };
    unsigned short id = (unsigned short)getpid();
    int sent = 0, got = 0;
    char name[16];

    inet_ntop(AF_INET, &dst, name, sizeof name);
    printf("PING %s (%s): %d data bytes\n", argv[optind], name, PING_DATA);
    for (int seq = 0; seq < count; seq++) {
        unsigned char pkt[8 + PING_DATA], rx[256];
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 8;
        pkt[4] = (unsigned char)(id >> 8); pkt[5] = (unsigned char)id;
        pkt[6] = (unsigned char)(seq >> 8); pkt[7] = (unsigned char)seq;
        for (int i = 8; i < (int)sizeof pkt; i++) pkt[i] = (unsigned char)i;
        unsigned short c = icmp_csum(pkt, sizeof pkt);
        pkt[2] = (unsigned char)(c >> 8); pkt[3] = (unsigned char)c;

        long start = now_ms(), deadline = start + wait_s * 1000L;
        if (sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) < 0) { cb_err("sendto", name); break; }
        sent++;
        for (long left; (left = deadline - now_ms()) > 0;) {
            struct timeval tv = { left / 1000, left % 1000 * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            int n = (int)recv(fd, rx, sizeof rx, 0);
            if (n < 0) break;
            int ihl = (rx[0] & 15) * 4;
            if (n < ihl + 8 || rx[ihl] != 0 || rx[ihl + 4] != (id >> 8) || rx[ihl + 5] != (id & 255) ||
                rx[ihl + 6] != (seq >> 8) || rx[ihl + 7] != (seq & 255)) continue;
            printf("%d bytes from %s: seq=%d ttl=%d time=%ld ms\n", n - ihl, name, seq, rx[8], now_ms() - start);
            got++;
            break;
        }
        if (seq + 1 < count) {
            long rest = start + 1000 - now_ms();
            if (rest > 0) { struct timespec ts = { rest / 1000, rest % 1000 * 1000000 }; nanosleep(&ts, NULL); }
        }
    }
    printf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n", argv[optind],
           sent, got, sent ? (sent - got) * 100 / sent : 100);
    close(fd);
    return got ? 0 : 1;
}

/* ---- nslookup ------------------------------------------------------------------- */

int nslookup_main(int argc, char **argv) {
    char conf[256] = "", server[32] = "10.0.2.3";
    struct in_addr a;

    if (argc != 2) return cb_usage("name");
    if (cb_slurp("/etc/resolv.conf", conf, sizeof conf) > 0) {
        char *p = strstr(conf, "nameserver");
        if (p) {
            p += 10 + strspn(p + 10, " \t");
            size_t n = strcspn(p, " \t\r\n");
            if (n && n < sizeof server) { memcpy(server, p, n); server[n] = 0; }
        }
    }
    printf("Server:\t\t%s\nAddress:\t%s#53\n\n", server, server);
    struct hostent *h = gethostbyname(argv[1]);
    if (!h) {
        printf("** server can't find %s: NXDOMAIN\n", argv[1]);
        return 1;
    }
    memcpy(&a, h->h_addr, 4);
    printf("Name:\t%s\nAddress: %s\n", argv[1], inet_ntoa(a));
    return 0;
}

/* ---- nc ------------------------------------------------------------------------- */

static int nc_idle_ms = -1;                        /* -w: give up after this long without traffic */

/* Copies stdin to the socket and the socket to stdout until the peer closes. */
static int pump(int s) {
    char buf[1024];
    struct pollfd pf[2] = { { 0, POLLIN, 0 }, { s, POLLIN, 0 } };
    int stdin_open = 1;

    for (;;) {
        pf[0].fd = stdin_open ? 0 : -1;
        int ready = poll(pf, 2, nc_idle_ms);
        if (ready == 0) return 0;
        if (ready < 0) { if (errno == EINTR) continue; return cb_err(NULL, "poll"); }
        if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = recv(s, buf, sizeof buf, 0);
            if (n <= 0) return n < 0;
            if (cb_write_all(1, buf, (size_t)n) < 0) return 1;
        }
        if (stdin_open && (pf[0].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) { stdin_open = 0; shutdown(s, SHUT_WR); continue; }
            if (cb_write_all(s, buf, (size_t)n) < 0) return 1;
        }
    }
}

int nc_main(int argc, char **argv) {
    int listen_mode = 0, port = 0, opt;
    struct sockaddr_in sa = { AF_INET, 0, { INADDR_ANY }, { 0 } };

    optind = 1;
    while ((opt = getopt(argc, argv, "lp:w:")) != -1) {
        if (opt == 'l') listen_mode = 1;
        else if (opt == 'w') nc_idle_ms = atoi(optarg) * 1000;
        else if (opt == 'p') port = atoi(optarg);
        else return cb_usage("[-w secs] host port | -l [-p] port");
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return cb_err("socket", "AF_INET");
    if (listen_mode) {
        if (!port && optind < argc) port = atoi(argv[optind]);
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sa.sin_port = htons((unsigned short)port);
        if (!port || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s, 1) < 0)
            return port ? cb_err("bind", argv[optind]) : cb_usage("-l [-p] port");
        if (nc_idle_ms >= 0) {
            struct pollfd lp = { s, POLLIN, 0 };
            if (poll(&lp, 1, nc_idle_ms) <= 0) return 1;
        }
        int c = accept(s, NULL, NULL);
        if (c < 0) return cb_err(NULL, "accept");
        close(s);
        return pump(c);
    }
    if (optind != argc - 2) return cb_usage("[-w secs] host port | -l [-p] port");
    if (lookup(argv[optind], &sa.sin_addr) < 0) return 1;
    sa.sin_port = htons((unsigned short)atoi(argv[optind + 1]));
    set_timeouts(s, nc_idle_ms >= 0 ? (nc_idle_ms + 999) / 1000 : 10);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) return cb_err("connect", argv[optind]);
    struct timeval none = { 0, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    return pump(s);
}

/* ---- wget ----------------------------------------------------------------------- */

struct url { char host[128], path[512]; int port; };

static int parse_url(const char *u, struct url *out) {
    if (!strncmp(u, "https://", 8)) {
        fprintf(stderr, "%s: https:// is not supported yet (Coal has no TLS); use http:// or download on the host with coal_download\n", prog);
        return -1;
    }
    if (strncmp(u, "http://", 7) != 0) {
        fprintf(stderr, "%s: %s: only http:// URLs are supported\n", prog, u);
        return -1;
    }
    u += 7;
    size_t hl = strcspn(u, "/:");
    if (!hl || hl >= sizeof out->host) return -1;
    memcpy(out->host, u, hl);
    out->host[hl] = 0;
    u += hl;
    out->port = 80;
    if (*u == ':') out->port = (int)strtol(u + 1, (char **)&u, 10);
    if (strlen(*u ? u : "/") >= sizeof out->path || out->port <= 0 || out->port > 65535) return -1;
    strcpy(out->path, *u ? u : "/");
    return 0;
}

/* Buffered reader over the socket. */
struct reader { int fd, pos, len; char buf[2048]; };

static int rd_fill(struct reader *r) {
    if (r->pos < r->len) return 1;
    ssize_t n = recv(r->fd, r->buf, sizeof r->buf, 0);
    r->pos = 0;
    r->len = n > 0 ? (int)n : 0;
    return n > 0 ? 1 : n < 0 ? -1 : 0;
}

static int rd_line(struct reader *r, char *line, size_t cap) {
    size_t n = 0;
    for (;;) {
        int f = rd_fill(r);
        if (f <= 0) return f == 0 && n ? (line[n] = 0, 1) : f;
        char c = r->buf[r->pos++];
        if (c == '\n') { line[n] = 0; if (n && line[n - 1] == '\r') line[n - 1] = 0; return 1; }
        if (n + 1 < cap) line[n++] = c;
    }
}

static ssize_t rd_data(struct reader *r, char *dst, size_t want) {
    int f = rd_fill(r);
    if (f <= 0) return f;
    size_t n = (size_t)(r->len - r->pos) < want ? (size_t)(r->len - r->pos) : want;
    memcpy(dst, r->buf + r->pos, n);
    r->pos += (int)n;
    return (ssize_t)n;
}

static int caseprefix(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != prefix[i]) return 0;
    return 1;
}

/* Progress line on a terminal only; at most one update per 16 KiB. */
static void progress(long done, long total, int quiet, int final) {
    static long shown;

    if (quiet || !isatty(2) || (!final && done - shown < 16384)) return;
    shown = done;
    if (total > 0) fprintf(stderr, "\r  %ld / %ld bytes (%ld%%)", done, total, total >= 100 ? done / (total / 100) : done * 100 / total);
    else fprintf(stderr, "\r  %ld bytes", done);
    fflush(stderr);
}

/* One request. Returns 0 on success, 1 for a redirect (next URL in loc), -1 on failure. */
static int fetch(const struct url *u, int out_fd, int quiet, char *loc, size_t loc_cap) {
    struct in_addr ip;
    struct sockaddr_in sa = { AF_INET, htons((unsigned short)u->port), { 0 }, { 0 } };
    char line[1024];

    if (lookup(u->host, &ip) < 0) return -1;
    sa.sin_addr = ip;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return cb_err("socket", "AF_INET"), -1;
    set_timeouts(s, 15);
    if (!quiet) fprintf(stderr, "Connecting to %s (%s:%d)\n", u->host, inet_ntoa(ip), u->port);
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) { cb_err("connect", u->host); close(s); return -1; }

    int n = snprintf(line, sizeof line, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: coal-wget\r\n"
                     "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", u->path, u->host);
    if (cb_write_all(s, line, (size_t)n) < 0) { cb_err("send", u->host); close(s); return -1; }

    struct reader *r = malloc(sizeof *r);
    int status = 0, chunked = 0, rc = -1;
    long length = -1, done = 0;
    if (!r) { close(s); return -1; }
    r->fd = s; r->pos = r->len = 0;
    loc[0] = 0;
    if (rd_line(r, line, sizeof line) <= 0 || strncmp(line, "HTTP/1.", 7) != 0) {
        fprintf(stderr, "%s: bad response from %s\n", prog, u->host);
        goto out;
    }
    status = atoi(line + 9);
    if (!quiet) fprintf(stderr, "HTTP request sent, awaiting response... %s\n", line + 9);
    while (rd_line(r, line, sizeof line) > 0 && line[0]) {
        if (caseprefix(line, "content-length:")) length = strtol(line + 15, NULL, 10);
        else if (caseprefix(line, "transfer-encoding:") && strstr(line, "chunked")) chunked = 1;
        else if (caseprefix(line, "location:")) {
            const char *v = line + 9 + strspn(line + 9, " \t");
            strncpy(loc, v, loc_cap - 1);
            loc[loc_cap - 1] = 0;
        }
    }
    if (status >= 300 && status < 400 && loc[0]) { rc = 1; goto out; }
    if (status < 200 || status >= 300) { fprintf(stderr, "%s: server returned error: HTTP %d\n", prog, status); goto out; }
    if (!quiet && !chunked) fprintf(stderr, "Length: %ld\n", length);

    char data[1024];
    long left = chunked ? 0 : length;        /* bytes left in the current chunk, or in the body */
    int last_chunk = 0;
    for (;;) {
        if (chunked && left == 0) {
            if (last_chunk) break;
            if (rd_line(r, line, sizeof line) <= 0) goto truncated;
            if (!line[0] && rd_line(r, line, sizeof line) <= 0) goto truncated;      /* CRLF after the data */
            left = strtol(line, NULL, 16);
            if (left == 0) { last_chunk = 1; break; }
        } else if (!chunked && length >= 0 && left == 0) {
            break;
        }
        size_t want = sizeof data;
        if ((chunked || length >= 0) && (long)want > left) want = (size_t)left;
        ssize_t got = rd_data(r, data, want);
        if (got < 0) { cb_err("recv", u->host); goto out; }
        if (got == 0) {
            if (chunked || length >= 0) goto truncated;
            break;                           /* no length: the end of the stream ends the body */
        }
        if (cb_write_all(out_fd, data, (size_t)got) < 0) { cb_err("write", "output"); goto out; }
        done += got;
        if (chunked || length >= 0) left -= got;
        progress(done, length, quiet, 0);
    }
    progress(done, length, quiet, 1);
    if (!quiet) fprintf(stderr, "%s%ld bytes received\n", isatty(2) ? "\n" : "", done);
    rc = 0;
    goto out;
truncated:
    fprintf(stderr, "\n%s: connection closed before the whole body arrived\n", prog);
out:
    free(r);
    close(s);
    return rc;
}

/* Resolves a Location against the URL it came from. */
static int next_url(const struct url *cur, const char *loc, struct url *out) {
    if (!strncmp(loc, "http", 4)) return parse_url(loc, out);
    *out = *cur;
    if (loc[0] != '/' || strlen(loc) >= sizeof out->path) return -1;
    strcpy(out->path, loc);
    return 0;
}

int wget_main(int argc, char **argv) {
    const char *outname = NULL;
    int quiet = 0, opt;
    struct url u;
    char loc[512], name[128];

    optind = 1;
    while ((opt = getopt(argc, argv, "O:q")) != -1) {
        if (opt == 'O') outname = optarg;
        else if (opt == 'q') quiet = 1;
        else return cb_usage("[-q] [-O file] http://host[:port]/path");
    }
    if (optind != argc - 1) return cb_usage("[-q] [-O file] http://host[:port]/path");
    if (parse_url(argv[optind], &u) < 0) return 1;
    if (!outname) {
        const char *b = strrchr(u.path, '/');
        snprintf(name, sizeof name, "%s", b && b[1] && b[1] != '?' ? b + 1 : "index.html");
        outname = name;
    }
    int out = !strcmp(outname, "-") ? 1 : open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) return cb_err("cannot write", outname);

    int rc = -1;
    for (int hops = 0; hops < 8; hops++) {
        rc = fetch(&u, out, quiet, loc, sizeof loc);
        if (rc != 1) break;
        struct url next;
        if (!quiet) fprintf(stderr, "Redirected to %s\n", loc);
        if (next_url(&u, loc, &next) < 0) { rc = -1; break; }
        u = next;
        if (out != 1) { close(out); out = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) return cb_err("cannot write", outname); }
        rc = -1;
    }
    if (out != 1) close(out);
    if (rc != 0 && out != 1) unlink(outname);
    return rc == 0 ? 0 : 1;
}
