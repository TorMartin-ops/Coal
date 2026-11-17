/* Name resolution: numeric addresses, /etc/hosts, then DNS over UDP (A records). */

#include <libc/netdb.h>
#include <libc/arpa/inet.h>
#include <libc/sys/time.h>
#include <libc/errno.h>
#include <libc/fcntl.h>
#include <libc/stdio.h>
#include <libc/stdlib.h>
#include <libc/string.h>
#include <libc/ctype.h>
#include <libc/unistd.h>
#include <libc/time.h>

#define DNS_PORT      53
#define DNS_TIMEOUT_S 2
#define DNS_TRIES     3
#define DEFAULT_DNS   "10.0.2.3"

int h_errno;

static const struct { const char *name; int port; } services[] = {
    { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 }, { "domain", 53 },
    { "http", 80 }, { "pop3", 110 }, { "ntp", 123 }, { "imap", 143 }, { "https", 443 },
};

static long slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    long n;
    if (fd < 0) return -1;
    n = read(fd, buf, cap - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = 0;
    return n;
}

/* Cuts the next line out of *cursor, dropping comments; NULL at the end. */
static char *next_line(char **cursor) {
    char *line = *cursor, *nl;
    if (!line || !*line) return NULL;
    nl = strchr(line, '\n');
    if (nl) { *nl = 0; *cursor = nl + 1; } else { *cursor += strlen(line); }
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    return line;
}

static int hosts_lookup(const char *name, in_addr_t *out) {
    char buf[1024], *cur = buf, *line;

    if (slurp("/etc/hosts", buf, sizeof buf) < 0) return -1;
    while ((line = next_line(&cur))) {
        char *save, *addr = strtok_r(line, " \t\r", &save), *host;
        struct in_addr a;
        if (!addr || !inet_aton(addr, &a)) continue;
        while ((host = strtok_r(NULL, " \t\r", &save)))
            if (!strcasecmp(host, name)) { *out = a.s_addr; return 0; }
    }
    return -1;
}

static in_addr_t nameserver(void) {
    char buf[512], *cur = buf, *line;
    struct in_addr a;

    if (slurp("/etc/resolv.conf", buf, sizeof buf) >= 0) {
        while ((line = next_line(&cur))) {
            char *save, *key = strtok_r(line, " \t\r", &save), *val = strtok_r(NULL, " \t\r", &save);
            if (key && val && !strcmp(key, "nameserver") && inet_aton(val, &a)) return a.s_addr;
        }
    }
    inet_aton(DEFAULT_DNS, &a);
    return a.s_addr;
}

/* Builds a standard A query for name; returns its length or -1. */
static int build_query(uint8_t *q, uint16_t id, const char *name) {
    int n = 12;

    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01;                                    /* recursion desired */
    q[5] = 1;                                       /* one question */
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || n + (int)len + 6 > 512) return -1;
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, len);
        n += (int)len;
        p += len + (dot ? 1 : 0);
        if (dot && !*p) break;                      /* trailing dot */
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                         /* type A */
    q[n++] = 0; q[n++] = 1;                         /* class IN */
    return n;
}

static int skip_name(const uint8_t *r, int n, int pos) {
    while (pos < n) {
        if (r[pos] == 0) return pos + 1;
        if ((r[pos] & 0xC0) == 0xC0) return pos + 2;
        pos += r[pos] + 1;
    }
    return -1;
}

/* First A record of a reply to query `id`; 0 and *out set, or -1 (h_errno set). */
static int parse_reply(const uint8_t *r, int n, uint16_t id, in_addr_t *out) {
    if (n < 12 || r[0] != (id >> 8) || r[1] != (uint8_t)id || !(r[2] & 0x80)) return -2;   /* not ours */
    if ((r[3] & 0x0F) != 0) { h_errno = HOST_NOT_FOUND; return -1; }
    int pos = 12, qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
    while (qd-- > 0) { if ((pos = skip_name(r, n, pos)) < 0) return -1; pos += 4; }
    while (an-- > 0) {
        if ((pos = skip_name(r, n, pos)) < 0 || pos + 10 > n) break;
        int type = r[pos] << 8 | r[pos + 1], rdlen = r[pos + 8] << 8 | r[pos + 9];
        pos += 10;
        if (pos + rdlen > n) break;
        if (type == 1 && rdlen == 4) { memcpy(out, r + pos, 4); return 0; }
        pos += rdlen;
    }
    h_errno = HOST_NOT_FOUND;
    return -1;
}

static int dns_lookup(const char *name, in_addr_t *out) {
    uint8_t q[512], r[512];
    int len, fd, rc = -1;
    struct sockaddr_in ns = { AF_INET, htons(DNS_PORT), { nameserver() }, { 0 } };
    struct timeval tv = { DNS_TIMEOUT_S, 0 };
    uint16_t id = (uint16_t)(getpid() * 31 + (unsigned)time(NULL));

    h_errno = TRY_AGAIN;
    if ((len = build_query(q, id, name)) < 0) { h_errno = HOST_NOT_FOUND; return -1; }
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int attempt = 0; attempt < DNS_TRIES && rc == -1; attempt++) {
        if (sendto(fd, q, (size_t)len, 0, (struct sockaddr *)&ns, sizeof ns) < 0) break;
        for (;;) {
            int n = (int)recv(fd, r, sizeof r, 0);
            if (n < 0) break;                       /* timeout: ask again */
            int p = parse_reply(r, n, id, out);
            if (p == -2) continue;
            if (p == 0) rc = 0; else rc = -3;       /* -3: definitive "no such name" */
            break;
        }
    }
    close(fd);
    return rc == 0 ? 0 : -1;
}

/* Network-order address of name, or -1 with h_errno set. */
static int resolve(const char *name, in_addr_t *out) {
    struct in_addr a;

    if (inet_aton(name, &a)) { *out = a.s_addr; return 0; }
    if (!strcasecmp(name, "localhost")) { *out = htonl(INADDR_LOOPBACK); return 0; }
    if (hosts_lookup(name, out) == 0) return 0;
    return dns_lookup(name, out);
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent h;
    static in_addr_t addr;
    static char *addrs[2], *aliases[1], hostname[256];

    if (resolve(name, &addr) < 0) return NULL;
    strncpy(hostname, name, sizeof hostname - 1);
    addrs[0] = (char *)&addr;
    h = (struct hostent){ hostname, aliases, AF_INET, 4, addrs };
    return &h;
}

static int service_port(const char *service) {
    char *end;
    long v = strtol(service, &end, 10);
    if (!*end && end != service) return (int)v;
    for (unsigned i = 0; i < sizeof services / sizeof services[0]; i++)
        if (!strcmp(service, services[i].name)) return services[i].port;
    return -1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    in_addr_t addr = 0;
    int port = 0, socktype = hints && hints->ai_socktype ? hints->ai_socktype : SOCK_STREAM;
    int flags = hints ? hints->ai_flags : 0;

    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;
    if (service && (port = service_port(service)) < 0) return EAI_NONAME;
    if (!node) {
        addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    } else if ((flags & AI_NUMERICHOST) ? !inet_pton(AF_INET, node, &addr) : resolve(node, &addr) < 0) {
        return h_errno == TRY_AGAIN ? EAI_AGAIN : EAI_NONAME;
    }
    struct { struct addrinfo ai; struct sockaddr_in sa; } *blk = calloc(1, sizeof *blk);
    if (!blk) return EAI_MEMORY;
    blk->sa = (struct sockaddr_in){ AF_INET, htons((uint16_t)port), { addr }, { 0 } };
    blk->ai = (struct addrinfo){ 0, AF_INET, socktype, socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP,
                                 sizeof blk->sa, (struct sockaddr *)&blk->sa, NULL, NULL };
    *res = &blk->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res) {
    free(res);                                      /* the address lives in the same block */
}

const char *gai_strerror(int err) {
    switch (err) {
    case EAI_NONAME: return "Name or service not known";
    case EAI_AGAIN:  return "Temporary failure in name resolution";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_MEMORY: return "Out of memory";
    default:         return "Unknown error";
    }
}
