/*
 * agentd - the guest side of the Coal agent protocol (docs/agent-vm/protocol.md).
 *
 * Serves length-prefixed JSON frames on a byte-stream link (the serial port
 * /dev/ttyS1 by default) so a host gateway can run commands, manage background
 * jobs and move files. Requests are multiplexed: every response carries the
 * request's "id", and a slow request (exec, job.wait) does not hold up the others.
 *
 * One event loop polls the link, the output pipes of running commands and a
 * self-pipe that SIGCHLD writes to; it never blocks anywhere else.
 *
 * The daemon talks to the Linux i386 system call ABI directly and needs
 * nothing from libc, so the same source also builds and runs on a Linux host
 * (see test/).
 */

#define NULL ((void *)0)
typedef unsigned long size_t;
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

#define offsetof(t, m) __builtin_offsetof(t, m)

/* --- Linux i386 system calls ---------------------------------------------- */

enum {
    SYS_exit = 1, SYS_fork = 2, SYS_read = 3, SYS_write = 4, SYS_open = 5,
    SYS_close = 6, SYS_unlink = 10, SYS_execve = 11, SYS_chdir = 12, SYS_lseek = 19,
    SYS_kill = 37, SYS_rename = 38, SYS_mkdir = 39, SYS_rmdir = 40, SYS_pipe2 = 331,
    SYS_ioctl = 54, SYS_setsid = 66, SYS_dup2 = 63, SYS_uname = 122, SYS_fsync = 118,
    SYS_wait4 = 114, SYS_nanosleep = 162, SYS_poll = 168, SYS_munmap = 91,
    SYS_getdents64 = 220, SYS_lstat64 = 196, SYS_fstat64 = 197, SYS_fcntl64 = 221,
    SYS_clock_gettime = 265, SYS_mmap2 = 192, SYS_setrlimit = 75, SYS_rt_sigaction = 174,
    SYS_rt_sigreturn = 173,
};

enum {
    O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0x40, O_NOCTTY = 0x100,
    O_TRUNC = 0x200, O_APPEND = 0x400, O_NONBLOCK = 0x800, O_DIRECTORY = 0x10000,
    O_CLOEXEC = 0x80000,
    TCGETS = 0x5401, TCSETS = 0x5402, F_SETFL = 4,
    POLLIN = 1, POLLHUP = 0x10, WNOHANG = 1,
    SIGKILL = 9, SIGTERM = 15, SIGCHLD = 17, SIGXCPU = 24,
    SA_SIGINFO = 4, SA_RESTORER = 0x04000000,
    RLIMIT_CPU = 0, RLIMIT_NPROC = 6, RLIMIT_AS = 9,
};

static long sc(long n, long a, long b, long c, long d, long e) {
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                      : "memory", "cc");
    return r;
}

#define SYS1(n, a)          sc(n, (long)(a), 0, 0, 0, 0)
#define SYS2(n, a, b)       sc(n, (long)(a), (long)(b), 0, 0, 0)
#define SYS3(n, a, b, c)    sc(n, (long)(a), (long)(b), (long)(c), 0, 0)
#define SYS4(n, a, b, c, d) sc(n, (long)(a), (long)(b), (long)(c), (long)(d), 0)

/* mmap2 takes a sixth argument in ebp. */
static long anon_map(size_t len) {
    long r;
    __asm__ volatile ("pushl %%ebp\n\txorl %%ebp, %%ebp\n\tint $0x80\n\tpopl %%ebp"
                      : "=a"(r)
                      : "a"(SYS_mmap2), "b"(0), "c"(len), "d"(3), "S"(0x22), "D"(-1)
                      : "memory", "cc");
    return r;
}

/* Linux i386 struct stat64 as the kernel lays it out (96 bytes). */
struct stat64 {
    u64 st_dev; u32 pad1; u32 st_ino32; u32 st_mode; u32 st_nlink;
    u32 st_uid; u32 st_gid; u64 st_rdev; u32 pad2; s64 st_size;
    u32 st_blksize; u64 st_blocks; u32 atime, atime_ns, mtime, mtime_ns,
    ctime, ctime_ns; u64 st_ino;
} __attribute__((packed));
typedef char stat64_size_check[sizeof(struct stat64) == 96 ? 1 : -1];

struct dirent64 {
    u64 d_ino; s64 d_off; unsigned short d_reclen; u8 d_type; char d_name[];
} __attribute__((packed));

struct pollfd { int fd; short events, revents; };
struct timespec { long tv_sec, tv_nsec; };
struct ksigaction { void (*handler)(int); unsigned long flags; void (*restorer)(void); u32 mask[2]; };

#define S_IFMT 0170000
#define S_IFDIR 0040000

/* --- Small helpers --------------------------------------------------------- */

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Copies forwards, so it also moves data down inside one buffer (d below s). */
static void mcopy(void *d, const void *s, size_t n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
}

static void mzero(void *d, size_t n) {
    char *dd = d;
    while (n--) *dd++ = 0;
}

static u32 umin(u32 a, u32 b) { return a < b ? a : b; }

static void msleep(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    SYS2(SYS_nanosleep, &ts, 0);
}

static long now_ms(void) {
    struct timespec ts;
    if (SYS2(SYS_clock_gettime, 1, &ts) < 0 && SYS2(SYS_clock_gettime, 0, &ts) < 0)
        return 0;
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Appends s to a NUL-terminated buffer of capacity cap; false if it did not fit. */
static int cat(char *buf, size_t cap, const char *s) {
    size_t n = slen(buf), m = slen(s);
    if (n + m + 1 > cap) return 0;
    mcopy(buf + n, s, m + 1);
    return 1;
}

static void fmt_u(char *buf, unsigned long v) {
    char t[12]; int i = 0, k = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) buf[k++] = t[--i];
    buf[k] = 0;
}

/* --- Transport -------------------------------------------------------------
 * The link is one bidirectional byte stream, read on fd 0 and written on fd 1.
 * link_open() is the only place that knows what carries it: here a serial port in
 * raw mode. A virtio-vsock transport replaces link_open() (socket, bind, listen,
 * accept, then dup2 onto 0 and 1) and re-runs it when the peer disconnects; the
 * framing and everything above stay as they are.
 */

/* Raw 8N1 mode: no echo, no line editing, no CR/LF translation. */
static void make_raw(int fd) {
    unsigned int t[16] = { 0 };                 /* struct termios, 36 bytes used */
    if (SYS3(SYS_ioctl, fd, TCGETS, t) < 0) return;
    t[0] = 0;                                   /* c_iflag */
    t[1] = 0;                                   /* c_oflag */
    t[2] = (t[2] & ~0x30u) | 0x30 | 0x80;       /* c_cflag: CS8 | CREAD, keep speed */
    t[3] = 0;                                   /* c_lflag */
    ((u8 *)t)[17 + 6] = 1;                      /* c_cc[VMIN]  (offset 17 + 6) */
    ((u8 *)t)[17 + 5] = 0;                      /* c_cc[VTIME] */
    SYS3(SYS_ioctl, fd, TCSETS, t);
}

static int link_open(const char *dev) {
    long fd = SYS2(SYS_open, dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    make_raw((int)fd);
    SYS2(SYS_dup2, fd, 0);
    SYS2(SYS_dup2, fd, 1);
    if (fd > 1) SYS1(SYS_close, fd);
    return 0;
}

/* Writes everything; the serial driver may accept less than asked or nothing while its ring is full. */
static long write_all(int fd, const char *p, size_t n) {
    int stalls = 0;
    while (n) {
        long w = SYS3(SYS_write, fd, p, n);
        if (w < 0) return w;
        if (w == 0) {
            if (++stalls > 5000) return -5;      /* EIO: the peer stopped draining */
            msleep(1);
            continue;
        }
        stalls = 0;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* --- Frame builder ----------------------------------------------------------
 * A frame is a u32 little-endian length followed by that many bytes of JSON. Responses
 * are built in out[] after a 4-byte gap for the length.
 */

#define MAX_FRAME   (256 * 1024)
#define OUT_CAP     (MAX_FRAME + 4)
#define OUT_RESERVE 512            /* room kept for the closing fields */

static char out[OUT_CAP];
static size_t olen;
static int ofull;

static void put(char c) {
    if (olen < OUT_CAP) out[olen++] = c; else ofull = 1;
}

static void puts_(const char *s) { while (*s) put(*s++); }

static void put_num(long v) {
    char t[24]; int i = 0;
    unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
    if (v < 0) put('-');
    do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) put(t[--i]);
}

static void put_bool(int v) { puts_(v ? "true" : "false"); }

/* Length of the valid UTF-8 sequence at s (at most n bytes are readable), or 0. */
static size_t utf8_len(const u8 *s, size_t n) {
    u8 c = s[0];
    size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((s[i] & 0xC0) != 0x80) return 0;
    if ((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] > 0x9F) ||
        (c == 0xF0 && s[1] < 0x90) || (c == 0xF4 && s[1] > 0x8F)) return 0;
    return len;
}

/* Escape n bytes as the inside of a JSON string; invalid UTF-8 becomes U+FFFD. False if clipped for space. */
static int put_jstr(const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        if (olen + 6 + OUT_RESERVE > OUT_CAP) return 0;
        if (c >= 0x80) {
            size_t len = utf8_len((const u8 *)s + i, n - i);
            if (!len) puts_("\\ufffd");
            for (size_t k = 0; k < len; k++) put(s[i + k]);
            if (len) i += len - 1;
            continue;
        }
        if (c == '"' || c == '\\') { put('\\'); put((char)c); }
        else if (c == '\n') puts_("\\n");
        else if (c == '\r') puts_("\\r");
        else if (c == '\t') puts_("\\t");
        else if (c < 0x20) { puts_("\\u00"); put(hex[c >> 4]); put(hex[c & 15]); }
        else put((char)c);
    }
    return 1;
}

static void put_b64(const u8 *s, size_t n) {
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)s[i] << 16;
        if (i + 1 < n) v |= (u32)s[i + 1] << 8;
        if (i + 2 < n) v |= s[i + 2];
        put(t[v >> 18]); put(t[(v >> 12) & 63]);
        put(i + 1 < n ? t[(v >> 6) & 63] : '=');
        put(i + 2 < n ? t[v & 63] : '=');
    }
}

/* Decodes in place; returns the byte count or -1 on malformed input. */
static long b64_decode(char *s, size_t n) {
    size_t o = 0; u32 acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i]; int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return -1;
        acc = (acc << 6) | (u32)v; bits += 6;
        if (bits >= 8) { bits -= 8; s[o++] = (char)(acc >> bits); }
    }
    return (long)o;
}

static void put_kv_str(const char *k, const char *v, size_t n) {
    puts_(",\""); puts_(k); puts_("\":\""); put_jstr(v, n); put('"');
}

static void put_kv_num(const char *k, long v) {
    puts_(",\""); puts_(k); puts_("\":"); put_num(v);
}

static void put_kv_bool(const char *k, int v) {
    puts_(",\""); puts_(k); puts_("\":"); put_bool(v);
}

/* Fills in the length and writes the frame. */
static void send_out(void) {
    if (ofull) {
        static const char big[] =
            "{\"id\":null,\"ok\":false,\"error\":{\"code\":\"too_large\",\"message\":\"response too large\"}}";
        olen = 4; mcopy(out + 4, big, sizeof big - 1); olen += sizeof big - 1; ofull = 0;
    }
    u32 n = (u32)(olen - 4);
    out[0] = (char)n; out[1] = (char)(n >> 8); out[2] = (char)(n >> 16); out[3] = (char)(n >> 24);
    write_all(1, out, olen);
}

/* --- JSON request parser --------------------------------------------------- */

#define MAX_ENV 16

struct req {
    char id[24];                   /* raw JSON token echoed back */
    long idnum;                    /* the id if it is an integer, else -1 */
    const char *op, *cmd, *cwd, *path, *stream, *data;
    size_t data_len;
    long offset, limit, timeout_ms, mode, head, tail, spill_max, job_id, target, sig,
         mem_mb, nproc, cpu_s;
    int append, more, parents, recursive;
    const char *env_k[MAX_ENV], *env_v[MAX_ENV];
    int nenv;
};

enum { K_STR, K_INT, K_BOOL, K_DATA, K_ENV };

#define F(name, kind, field) { name, kind, offsetof(struct req, field) }
static const struct { const char *key; int kind; size_t off; } fields[] = {
    F("op", K_STR, op), F("cmd", K_STR, cmd), F("cwd", K_STR, cwd), F("path", K_STR, path),
    F("stream", K_STR, stream), F("data", K_DATA, data), F("env", K_ENV, env_k),
    F("offset", K_INT, offset), F("limit", K_INT, limit), F("timeout_ms", K_INT, timeout_ms),
    F("mode", K_INT, mode), F("head", K_INT, head), F("tail", K_INT, tail),
    F("spill_max", K_INT, spill_max), F("job_id", K_INT, job_id), F("target", K_INT, target),
    F("sig", K_INT, sig), F("mem_mb", K_INT, mem_mb), F("nproc", K_INT, nproc),
    F("cpu_s", K_INT, cpu_s),
    F("append", K_BOOL, append), F("more", K_BOOL, more), F("parents", K_BOOL, parents),
    F("recursive", K_BOOL, recursive),
};
#undef F

/* True if s holds no quote, backslash or control character and is valid UTF-8, so it can be echoed as is. */
static int plain_string(const u8 *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\' || s[i] < 0x20) return 0;
        if (s[i] >= 0x80) {
            size_t len = utf8_len(s + i, n - i);
            if (!len) return 0;
            i += len - 1;
        }
    }
    return 1;
}

/* True if s is exactly a JSON number, true, false or null. */
static int json_scalar(const char *s, size_t n) {
    size_t i = 0;
    static const char *const lits[] = { "null", "true", "false" };
    for (int k = 0; k < 3; k++) {
        size_t m = slen(lits[k]);
        if (m == n) {
            size_t j = 0;
            while (j < n && s[j] == lits[k][j]) j++;
            if (j == n) return 1;
        }
    }
    if (i < n && s[i] == '-') i++;
    if (i < n && s[i] == '0') i++;
    else if (i < n && s[i] >= '1' && s[i] <= '9') while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    else return 0;
    if (i < n && s[i] == '.') {
        size_t d = ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        size_t d = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return 0;
    }
    return i == n;
}

static void skip_ws(char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n') (*p)++;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexv(p[i]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}

/*
 * Parses a string at *p (on the opening quote), decoding escapes in place.
 * The result is never longer than the source, so it cannot overrun.
 * Returns the NUL-terminated start (length in *len) or NULL on a syntax error.
 */
static char *parse_str(char **p, size_t *len) {
    char *s = *p + 1, *w = s, *start = s;
    while (*s && *s != '"') {
        if (*s != '\\') { *w++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'n': *w++ = '\n'; s++; break;
        case 't': *w++ = '\t'; s++; break;
        case 'r': *w++ = '\r'; s++; break;
        case 'b': *w++ = '\b'; s++; break;
        case 'f': *w++ = '\f'; s++; break;
        case '/': case '\\': case '"': *w++ = *s++; break;
        case 'u': {
            int cp = hex4(s + 1);
            if (cp < 0) return NULL;
            s += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && s[0] == '\\' && s[1] == 'u') {
                int lo = hex4(s + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s += 6;
                }
            }
            if (cp < 0x80) *w++ = (char)cp;
            else if (cp < 0x800) {
                *w++ = (char)(0xC0 | cp >> 6); *w++ = (char)(0x80 | (cp & 63));
            } else if (cp < 0x10000) {
                *w++ = (char)(0xE0 | cp >> 12);
                *w++ = (char)(0x80 | ((cp >> 6) & 63));
                *w++ = (char)(0x80 | (cp & 63));
            } else {
                *w++ = (char)(0xF0 | cp >> 18);
                *w++ = (char)(0x80 | ((cp >> 12) & 63));
                *w++ = (char)(0x80 | ((cp >> 6) & 63));
                *w++ = (char)(0x80 | (cp & 63));
            }
            break;
        }
        default: return NULL;
        }
    }
    if (*s != '"') return NULL;
    *len = (size_t)(w - start);
    *w = 0;
    *p = s + 1;
    return start;
}

static int skip_value(char **p, int depth) {
    skip_ws(p);
    char c = **p;
    if (depth > 8 || !c) return -1;
    if (c == '"') { size_t n; return parse_str(p, &n) ? 0 : -1; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        (*p)++; skip_ws(p);
        if (**p == close) { (*p)++; return 0; }
        for (;;) {
            if (c == '{') {
                size_t n; skip_ws(p);
                if (**p != '"' || !parse_str(p, &n)) return -1;
                skip_ws(p);
                if (*(*p)++ != ':') return -1;
            }
            if (skip_value(p, depth + 1) < 0) return -1;
            skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == close) { (*p)++; return 0; }
            return -1;
        }
    }
    while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ' &&
           **p != '\t' && **p != '\r' && **p != '\n')
        (*p)++;
    return 0;
}

/* Reads an integer in [-2^31+1, 2^31-1]; 0 on success, -1 if it is not one or does not fit. */
static int parse_int(char **p, long *res) {
    int neg = 0, digits = 0;
    u64 v = 0;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (u64)(**p - '0');
        if (v > 0x7fffffffULL) return -1;
        digits++; (*p)++;
    }
    if (!digits) return -1;
    if (**p == '.') {                           /* 3.0 is fine, 3.5 is not */
        (*p)++;
        while (**p == '0') (*p)++;
    }
    if (**p && **p != ',' && **p != '}' && **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n') return -1;
    *res = neg ? -(long)v : (long)v;
    return 0;
}

/* "env": {"NAME":"value", ...}; values must be strings. */
static int parse_env(char **p, struct req *r) {
    size_t n;
    if (**p != '{') return skip_value(p, 0);
    (*p)++; skip_ws(p);
    if (**p == '}') { (*p)++; return 0; }
    for (;;) {
        char *k, *v;
        skip_ws(p);
        if (**p != '"' || !(k = parse_str(p, &n))) return -1;
        skip_ws(p);
        if (*(*p)++ != ':') return -1;
        skip_ws(p);
        if (**p != '"' || !(v = parse_str(p, &n))) return -1;
        if (r->nenv < MAX_ENV) { r->env_k[r->nenv] = k; r->env_v[r->nenv++] = v; }
        skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; return 0; }
        return -1;
    }
}

static const char *parse_err = "malformed JSON";

static int parse_req(char *line, struct req *r) {
    char *p = line;
    mzero(r, sizeof *r);
    mcopy(r->id, "null", 5);
    r->idnum = -1;
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++)
        if (fields[i].kind == K_INT) *(long *)((char *)r + fields[i].off) = -1;
    parse_err = "malformed JSON";

    skip_ws(&p);
    if (*p++ != '{') return -1;
    skip_ws(&p);
    if (*p == '}') return 0;
    for (;;) {
        size_t klen, vlen;
        char *key, *val;
        skip_ws(&p);
        if (*p != '"' || !(key = parse_str(&p, &klen))) return -1;
        skip_ws(&p);
        if (*p++ != ':') return -1;
        skip_ws(&p);

        size_t f = 0;
        while (f < sizeof fields / sizeof fields[0] && !seq(key, fields[f].key)) f++;
        if (seq(key, "id")) {
            char *b = p;
            if (*p == '"') {
                char *v = parse_str(&p, &vlen);
                if (!v) return -1;
                if (vlen < sizeof r->id - 2 && plain_string((const u8 *)v, vlen)) {
                    r->id[0] = '"'; mcopy(r->id + 1, v, vlen);
                    r->id[vlen + 1] = '"'; r->id[vlen + 2] = 0;
                }
            } else {
                if (skip_value(&p, 0) < 0) return -1;
                if (p - b < (long)sizeof r->id && json_scalar(b, (size_t)(p - b))) {
                    mcopy(r->id, b, (size_t)(p - b));
                    r->id[p - b] = 0;
                    char *q = r->id;
                    if (parse_int(&q, &r->idnum) < 0) r->idnum = -1;
                }
            }
        } else if (f == sizeof fields / sizeof fields[0]) {
            if (skip_value(&p, 0) < 0) return -1;
        } else if (fields[f].kind == K_ENV) {
            if (parse_env(&p, r) < 0) return -1;
        } else if (fields[f].kind == K_INT) {
            if (parse_int(&p, (long *)((char *)r + fields[f].off)) < 0) {
                parse_err = "number out of range or not an integer";
                return -1;
            }
        } else if (fields[f].kind == K_BOOL) {
            *(int *)((char *)r + fields[f].off) = *p == 't';
            if (skip_value(&p, 0) < 0) return -1;
        } else if (*p != '"') {
            if (skip_value(&p, 0) < 0) return -1;
        } else {
            if (!(val = parse_str(&p, &vlen))) return -1;
            *(const char **)((char *)r + fields[f].off) = val;
            if (fields[f].kind == K_DATA) r->data_len = vlen;
        }

        skip_ws(&p);
        if (*p == ',') { p++; continue; }
        return *p == '}' ? 0 : -1;
    }
}

/* --- Responses ------------------------------------------------------------- */

static void begin(const struct req *r) {
    olen = 4; ofull = 0;
    puts_("{\"id\":"); puts_(r->id);
}

static void ok_begin(const struct req *r) { begin(r); puts_(",\"ok\":true"); }

static void event_begin(const char *name) {
    olen = 4; ofull = 0;
    puts_("{\"event\":\""); puts_(name); put('"');
}

static const struct { int num; const char *name, *text; } errnos[] = {
    { 1, "EPERM", "Operation not permitted" }, { 2, "ENOENT", "No such file or directory" },
    { 3, "ESRCH", "No such process" }, { 4, "EINTR", "Interrupted system call" },
    { 5, "EIO", "Input/output error" }, { 9, "EBADF", "Bad file descriptor" },
    { 11, "EAGAIN", "Resource temporarily unavailable" }, { 12, "ENOMEM", "Out of memory" },
    { 13, "EACCES", "Permission denied" }, { 14, "EFAULT", "Bad address" },
    { 16, "EBUSY", "Device or resource busy" }, { 17, "EEXIST", "File exists" },
    { 19, "ENODEV", "No such device" }, { 20, "ENOTDIR", "Not a directory" },
    { 21, "EISDIR", "Is a directory" }, { 22, "EINVAL", "Invalid argument" },
    { 23, "ENFILE", "Too many open files in system" }, { 24, "EMFILE", "Too many open files" },
    { 27, "EFBIG", "File too large" }, { 28, "ENOSPC", "No space left on device" },
    { 29, "ESPIPE", "Illegal seek" }, { 30, "EROFS", "Read-only file system" },
    { 32, "EPIPE", "Broken pipe" }, { 36, "ENAMETOOLONG", "File name too long" },
    { 38, "ENOSYS", "Function not implemented" }, { 39, "ENOTEMPTY", "Directory not empty" },
};

/*
 * Error response. err < 0 is a negative errno: the code is its name and its text is appended to
 * msg. With err == 0, code names a protocol-level problem (bad_request, unknown_op, ...).
 */
static void fail(const struct req *r, const char *code, const char *msg, long err) {
    const char *name = code, *text = NULL;
    begin(r);
    puts_(",\"ok\":false,\"error\":{\"code\":\"");
    if (err < 0)
        for (size_t i = 0; i < sizeof errnos / sizeof errnos[0]; i++)
            if (errnos[i].num == -err) { name = errnos[i].name; text = errnos[i].text; }
    puts_(name ? name : "EIO");
    puts_("\",\"message\":\""); put_jstr(msg, slen(msg));
    if (text) { puts_(": "); put_jstr(text, slen(text)); }
    put('"');
    if (err < 0) { puts_(",\"errno\":"); put_num(-err); }
    puts_("}}");
    send_out();
}

static void bad(const struct req *r, const char *msg) { fail(r, "bad_request", msg, 0); }

/* --- Memory ---------------------------------------------------------------- */

static char *xalloc(size_t n) {
    long p = anon_map(n);
    return p < 0 && p > -4096 ? NULL : (char *)p;
}

static void xfree(char *p, size_t n) {
    if (p) SYS2(SYS_munmap, p, n);
}

/* --- Settings -------------------------------------------------------------- */

#define SPILL_DIR_DEFAULT "/tmp/agentd"
#define MAX_JOBS        32
#define MAX_EXEC        6              /* running exec calls: each holds 2 pipes and up to 2 spill files */
#define KEEP_SPILLS     32             /* spill files of the newest exec calls that are kept */
#define EXEC_TIMEOUT_MS 120000
#define MAX_TIMEOUT_MS  3600000
#define HEAD_DEFAULT    2000
#define TAIL_DEFAULT    6000
#define KEEP_MAX        (128 * 1024)   /* head + tail per stream */
#define SPILL_DEFAULT   (4 * 1024 * 1024)
#define SPILL_LIMIT     (16 * 1024 * 1024)
#define FRAME_TIMEOUT_MS 1000
#define RESYNC_QUIET_MS  50
#define FS_CHUNK        (96 * 1024)

static const char *spill_dir = SPILL_DIR_DEFAULT;

/* --- Jobs -------------------------------------------------------------------
 * One table holds exec calls (output captured through pipes, answered when the
 * command ends) and spawned jobs (output goes straight to files, polled later).
 */

enum { J_FREE, J_RUN, J_DONE };
enum { KILL_NONE, KILL_TIMEOUT, KILL_CANCEL, KILL_SIGNAL };

struct stream {
    int fd;                     /* read end of the pipe, -1 once closed */
    u32 total;                  /* bytes seen */
    u32 H, T;                   /* head and tail capacity */
    char *head, *tail;
    int spill;                  /* spill file fd, -1 if none */
    u32 spilled;                /* bytes written to it */
    int spill_cut, spill_err;   /* spill_max reached / write or open failed */
    char path[48];
};

struct job {
    int state, exec;            /* exec: answered with a response; otherwise a spawned job */
    long id, pid, exec_n;
    long start_ms, end_ms, deadline_ms;
    char req_id[24];
    long reqnum;                /* numeric request id, for cancel; -1 otherwise */
    int kill_reason, status, limited;
    u32 hits;                   /* RL_* bit mask read from /proc before the zombie was reaped */
    u32 spill_max;
    char cwd[96], cmd[96], paths[2][48];
    struct stream s[2];
    char *buf; size_t buf_size;
};

static struct job jobs[MAX_JOBS];
static long next_job_id = 1, next_exec_n = 1;

static struct job *job_find(long id) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].state != J_FREE && jobs[i].id == id) return &jobs[i];
    return NULL;
}

static void job_files_remove(struct job *j) {
    if (!j->exec) {
        SYS1(SYS_unlink, j->paths[0]);
        SYS1(SYS_unlink, j->paths[1]);
    }
}

/* A free slot; if the table is full the oldest finished spawned job makes room. */
static struct job *job_alloc(void) {
    struct job *old = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_FREE) { old = &jobs[i]; break; }
        if (jobs[i].state == J_DONE && (!old || jobs[i].end_ms < old->end_ms)) old = &jobs[i];
    }
    if (!old) return NULL;
    if (old->state == J_DONE) job_files_remove(old);
    mzero(old, sizeof *old);
    return old;
}

static int running_execs(void) {
    int n = 0;
    for (int i = 0; i < MAX_JOBS; i++) n += jobs[i].state == J_RUN && jobs[i].exec;
    return n;
}

/* --- Output capture for exec ------------------------------------------------
 * Each stream keeps the first H bytes and the last T bytes. Once more than H + T
 * bytes have been seen the whole output is also written to a spill file, so the
 * client can read the part in the middle with fs.read.
 */

static void make_spill_path(char *dst, size_t cap, const char *prefix, long n, const char *suffix) {
    char num[12];
    fmt_u(num, (unsigned long)n);
    dst[0] = 0;
    cat(dst, cap, spill_dir); cat(dst, cap, "/"); cat(dst, cap, prefix);
    cat(dst, cap, num); cat(dst, cap, suffix);
}

static void spill_write(struct stream *s, const char *p, u32 n, u32 cap) {
    if (s->spill < 0 || s->spill_err) return;
    if (s->spilled + n > cap) { n = cap > s->spilled ? cap - s->spilled : 0; s->spill_cut = 1; }
    if (n && write_all(s->spill, p, n) < 0) { s->spill_err = 1; return; }
    s->spilled += n;
}

static void spill_start(struct job *j, struct stream *s, const char *suffix) {
    char old[48];
    if (j->exec_n > KEEP_SPILLS) {                   /* keep the disk from filling up over a long session */
        make_spill_path(old, sizeof old, "x", j->exec_n - KEEP_SPILLS, ".stdout"); SYS1(SYS_unlink, old);
        make_spill_path(old, sizeof old, "x", j->exec_n - KEEP_SPILLS, ".stderr"); SYS1(SYS_unlink, old);
    }
    make_spill_path(s->path, sizeof s->path, "x", j->exec_n, suffix);
    long fd = SYS3(SYS_open, s->path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) { s->spill_err = 1; return; }
    s->spill = (int)fd;
    u32 hlen = umin(s->total, s->H), tlen = s->total - hlen;     /* the ring has not wrapped yet */
    spill_write(s, s->head, hlen, j->spill_max);
    spill_write(s, s->tail, tlen, j->spill_max);
}

static void stream_add(struct job *j, struct stream *s, const char *p, u32 n) {
    if (s->spill < 0 && !s->spill_err && s->total + n > s->H + s->T)
        spill_start(j, s, s == &j->s[0] ? ".stdout" : ".stderr");
    spill_write(s, p, n, j->spill_max);

    u32 i = 0;
    if (s->total < s->H) {
        i = umin(n, s->H - s->total);
        mcopy(s->head + s->total, p, i);
    }
    if (s->T) {
        u32 tail_total = s->total + i - umin(s->total + i, s->H);   /* bytes ever put into the ring */
        while (i < n) {
            u32 pos = tail_total % s->T, k = umin(n - i, s->T - pos);
            mcopy(s->tail + pos, p + i, k);
            tail_total += k; i += k;
        }
    }
    s->total += n;
}

/* Length of s[0..n) without a trailing incomplete UTF-8 sequence. */
static size_t utf8_trim_end(const char *s, size_t n) {
    for (size_t k = 1; k <= 3 && k <= n; k++) {
        u8 c = (u8)s[n - k];
        if ((c & 0xC0) == 0x80) continue;
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > k ? n - k : n;
    }
    return n;
}

static char text_buf[KEEP_MAX + 256];

/* Emits "name":{text, bytes_total, bytes_returned, truncated, path}. */
static void put_stream(const char *name, struct stream *s) {
    u32 hlen = umin(s->total, s->H);
    u32 tail_total = s->total - hlen, tlen = umin(tail_total, s->T);
    u32 omitted = tail_total - tlen;
    size_t n = 0, returned;

    if (omitted) hlen = (u32)utf8_trim_end(s->head, hlen);
    mcopy(text_buf, s->head, hlen);
    n = hlen;
    if (omitted) {
        char num[12], marker[80] = "\n[... ";
        fmt_u(num, omitted);
        cat(marker, sizeof marker, num); cat(marker, sizeof marker, " bytes omitted ...]\n");
        mcopy(text_buf + n, marker, slen(marker)); n += slen(marker);
    }
    u32 start = tail_total - tlen;                       /* ring index of the first tail byte to show */
    u32 skip = 0;
    for (u32 k = 0; omitted && k < tlen && k < 3; k++) {  /* do not start in the middle of a character */
        if ((((u8)s->tail[(start + k) % s->T]) & 0xC0) != 0x80) break;
        skip++;
    }
    for (u32 k = skip; k < tlen; k++) text_buf[n++] = s->tail[(start + k) % s->T];
    returned = hlen + (tlen - skip);

    puts_(",\""); puts_(name); puts_("\":{\"text\":\"");
    put_jstr(text_buf, n);
    puts_("\",\"bytes_total\":"); put_num(s->total);
    puts_(",\"bytes_returned\":"); put_num((long)returned);
    puts_(",\"truncated\":"); put_bool(omitted != 0);
    if (s->spill >= 0 || s->spilled) {
        puts_(",\"path\":\""); put_jstr(s->path, slen(s->path)); put('"');
        if (s->spill_cut) puts_(",\"spill_truncated\":true");
    }
    if (s->spill_err) puts_(",\"spill_error\":true");
    put('}');
}

/* --- Starting commands --------------------------------------------------------- */

static const char *const default_env[] = {
    "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/root", "TERM=dumb", "PAGER=cat", "EDITOR=true",
    "CI=1", "GIT_TERMINAL_PROMPT=0",
};
#define NDEFAULT_ENV ((int)(sizeof default_env / sizeof default_env[0]))

/* NAME=value strings for execve: the defaults, overridden or extended by the request. */
static void build_env(const struct req *r, char *buf, size_t cap, char **envp) {
    int n = 0;
    size_t used = 0;
    for (int i = 0; i < NDEFAULT_ENV; i++) envp[n++] = (char *)default_env[i];
    for (int i = 0; i < r->nenv; i++) {
        size_t kl = slen(r->env_k[i]), vl = slen(r->env_v[i]);
        if (!kl || used + kl + vl + 2 > cap) continue;
        char *e = buf + used;
        mcopy(e, r->env_k[i], kl); e[kl] = '='; mcopy(e + kl + 1, r->env_v[i], vl + 1);
        used += kl + vl + 2;
        int k = 0;
        for (; k < n; k++) {
            size_t m = 0;
            while (m < kl && envp[k][m] == e[m]) m++;
            if (m == kl && envp[k][kl] == '=') break;
        }
        if (k == n) n++;                       /* new name: MAX_ENV bounds how many can be added */
        envp[k] = e;
    }
    envp[n] = NULL;
}

static void set_limit(int res, unsigned long v, unsigned long hard) {
    unsigned long l[2] = { v, hard };
    SYS2(SYS_setrlimit, res, l);
}

static long count_processes(void) {
    static char dents[1024];
    long fd = SYS2(SYS_open, "/proc", O_RDONLY | O_DIRECTORY), n = 0;
    if (fd < 0) return 0;
    for (long got; (got = SYS3(SYS_getdents64, fd, dents, sizeof dents)) > 0; )
        for (long off = 0; off < got; ) {
            struct dirent64 *d = (struct dirent64 *)(dents + off);
            off += d->d_reclen;
            n += d->d_name[0] >= '0' && d->d_name[0] <= '9';
        }
    SYS1(SYS_close, fd);
    return n;
}

/*
 * Forks the command with stdout and stderr on out_fd and err_fd. The child is its own session
 * and process group (no controlling terminal), reads /dev/null, runs in cwd with the resource
 * limits of the request. Returns the pid or -errno.
 */
static long launch(struct job *j, const struct req *r, int out_fd, int err_fd) {
    char envbuf[2048];
    char *envp[NDEFAULT_ENV + MAX_ENV + 1];
    unsigned long nproc_limit = 0;

    build_env(r, envbuf, sizeof envbuf, envp);
    if (r->cwd && r->cwd[0]) {                              /* fail here with a proper error, not in the child */
        long d = SYS2(SYS_open, r->cwd, O_RDONLY | O_DIRECTORY);
        if (d < 0) return d;
        SYS1(SYS_close, d);
    }
    if (r->nproc > 0) nproc_limit = (unsigned long)(count_processes() + r->nproc);
    j->limited = r->mem_mb > 0 || r->cpu_s > 0 || r->nproc > 0;

    long pid = SYS1(SYS_fork, 0);
    if (pid != 0) return pid;

    char *argv[] = { "sh", "-c", (char *)r->cmd, NULL };
    SYS1(SYS_setsid, 0);
    long nul = SYS2(SYS_open, "/dev/null", O_RDONLY);
    if (nul >= 0) { SYS2(SYS_dup2, nul, 0); if (nul > 2) SYS1(SYS_close, nul); }
    SYS2(SYS_dup2, out_fd, 1);
    SYS2(SYS_dup2, err_fd, 2);
    if (r->mem_mb > 0 && r->mem_mb < 4096) set_limit(RLIMIT_AS, (unsigned long)r->mem_mb << 20, (unsigned long)r->mem_mb << 20);
    if (r->cpu_s > 0) set_limit(RLIMIT_CPU, (unsigned long)r->cpu_s, (unsigned long)r->cpu_s + 2);
    if (nproc_limit) set_limit(RLIMIT_NPROC, nproc_limit, nproc_limit);
    if (r->cwd && r->cwd[0] && SYS1(SYS_chdir, r->cwd) < 0) SYS1(SYS_exit, 126);
    SYS3(SYS_execve, "/bin/sh", argv, envp);
    SYS1(SYS_exit, 127);
    return 0;
}

static void job_describe(struct job *j, const struct req *r) {
    size_t n = 0;
    const char *c = r->cmd;
    while (c[n] && n < sizeof j->cmd - 1) { j->cmd[n] = c[n]; n++; }
    j->cmd[n] = 0;
    n = 0;
    c = r->cwd && r->cwd[0] ? r->cwd : "/";
    while (c[n] && n < sizeof j->cwd - 1) { j->cwd[n] = c[n]; n++; }
    j->cwd[n] = 0;
}

static long clamp(long v, long dflt, long lo, long hi) {
    return v < 0 ? dflt : v < lo ? lo : v > hi ? hi : v;
}

static void op_exec(const struct req *r) {
    if (!r->cmd) return bad(r, "missing cmd");
    if (running_execs() >= MAX_EXEC) return fail(r, "EAGAIN", "too many commands running", -11);
    struct job *j = job_alloc();
    if (!j) return fail(r, "EAGAIN", "job table is full", -11);

    u32 H = (u32)clamp(r->head, HEAD_DEFAULT, 0, KEEP_MAX);
    u32 T = (u32)clamp(r->tail, TAIL_DEFAULT, 0, KEEP_MAX);
    if (H + T > KEEP_MAX) T = KEEP_MAX - H;
    j->buf_size = 2 * (size_t)(H + T) + 16;
    j->buf = xalloc(j->buf_size);
    if (!j->buf) return fail(r, "ENOMEM", "no memory for output buffers", -12);

    int po[2], pe[2];
    long rc = SYS2(SYS_pipe2, po, O_CLOEXEC);
    if (rc >= 0) {
        rc = SYS2(SYS_pipe2, pe, O_CLOEXEC);
        if (rc < 0) { SYS1(SYS_close, po[0]); SYS1(SYS_close, po[1]); }
    }
    if (rc < 0) { xfree(j->buf, j->buf_size); return fail(r, "EMFILE", "pipe failed", rc); }

    j->exec = 1;
    j->id = next_job_id++;
    j->exec_n = next_exec_n++;
    j->spill_max = (u32)clamp(r->spill_max, SPILL_DEFAULT, 0, SPILL_LIMIT);
    job_describe(j, r);
    long pid = launch(j, r, po[1], pe[1]);
    SYS1(SYS_close, po[1]); SYS1(SYS_close, pe[1]);
    if (pid < 0) {
        SYS1(SYS_close, po[0]); SYS1(SYS_close, pe[0]);
        xfree(j->buf, j->buf_size);
        mzero(j, sizeof *j);
        return fail(r, "EAGAIN", "cannot start the command", pid);
    }
    j->pid = pid;
    j->start_ms = now_ms();
    j->deadline_ms = j->start_ms + clamp(r->timeout_ms, EXEC_TIMEOUT_MS, 1, MAX_TIMEOUT_MS);
    mcopy(j->req_id, r->id, sizeof j->req_id);
    j->reqnum = r->idnum;
    char *mem = j->buf;
    for (int i = 0; i < 2; i++) {
        struct stream *s = &j->s[i];
        s->fd = i ? pe[0] : po[0];
        s->spill = -1;
        s->H = H; s->T = T;
        s->head = mem; mem += H;
        s->tail = mem; mem += T;
    }
    j->state = J_RUN;
}

static void make_job_paths(struct job *j) {
    make_spill_path(j->paths[0], sizeof j->paths[0], "j", j->id, ".stdout");
    make_spill_path(j->paths[1], sizeof j->paths[1], "j", j->id, ".stderr");
}

static void op_spawn(const struct req *r) {
    if (!r->cmd) return bad(r, "missing cmd");
    struct job *j = job_alloc();
    if (!j) return fail(r, "EAGAIN", "job table is full", -11);
    j->id = next_job_id++;
    make_job_paths(j);
    long fo = SYS3(SYS_open, j->paths[0], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    long fe = fo < 0 ? fo : SYS3(SYS_open, j->paths[1], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fe < 0) {
        if (fo >= 0) SYS1(SYS_close, fo);
        mzero(j, sizeof *j);
        return fail(r, "EIO", "cannot create the job's output files", fe);
    }
    job_describe(j, r);
    long pid = launch(j, r, (int)fo, (int)fe);
    SYS1(SYS_close, fo); SYS1(SYS_close, fe);
    if (pid < 0) {
        SYS1(SYS_unlink, j->paths[0]); SYS1(SYS_unlink, j->paths[1]);
        mzero(j, sizeof *j);
        return fail(r, "EAGAIN", "cannot start the command", pid);
    }
    j->pid = pid;
    j->start_ms = now_ms();
    j->deadline_ms = r->timeout_ms > 0 ? j->start_ms + clamp(r->timeout_ms, 0, 1, MAX_TIMEOUT_MS * 24L) : 0;
    j->state = J_RUN;
    ok_begin(r);
    put_kv_num("job_id", j->id); put_kv_num("pid", pid);
    puts_(",\"stdout_path\":\""); put_jstr(j->paths[0], slen(j->paths[0]));
    puts_("\",\"stderr_path\":\""); put_jstr(j->paths[1], slen(j->paths[1])); puts_("\"}");
    send_out();
}

/* --- Job state ------------------------------------------------------------------ */

static const char *const hit_names[] = { "cpu", "nproc", "as" };    /* order of RL_* in the kernel */
#define NHITS 3

static void kill_job(struct job *j, int sig, int reason) {
    if (!j->kill_reason) j->kill_reason = reason;
    SYS2(SYS_kill, -j->pid, sig);               /* the whole group; the leader may not have created it yet */
    SYS2(SYS_kill, j->pid, sig);
}

static int term_signal(const struct job *j) { return j->status & 0x7f; }

static int exit_code(const struct job *j) {
    return term_signal(j) ? 128 + term_signal(j) : (j->status >> 8) & 0xff;
}

static const char *killed_by(const struct job *j) {
    if (j->kill_reason == KILL_TIMEOUT) return "timeout";
    if (j->kill_reason == KILL_CANCEL) return "cancel";
    if (j->kill_reason == KILL_SIGNAL) return "signal";
    if ((j->hits & 1) && (term_signal(j) == SIGXCPU || term_signal(j) == SIGKILL)) return "rlimit_cpu";
    return NULL;
}

static void put_hits(const struct job *j) {
    puts_(",\"rlimit_hits\":[");
    for (int i = 0, first = 1; i < NHITS; i++)
        if (j->hits & (1u << i)) {
            if (!first) put(',');
            first = 0;
            put('"'); puts_(hit_names[i]); put('"');
        }
    put(']');
}

/* Fields shared by exec responses, job.poll and job.wait. */
static void put_job_state(const struct job *j) {
    long end = j->state == J_RUN ? now_ms() : j->end_ms;
    const char *kb = j->state == J_RUN ? NULL : killed_by(j);
    put_kv_num("job_id", j->id); put_kv_num("pid", j->pid);
    puts_(",\"state\":\""); puts_(j->state == J_RUN ? "running" : "exited"); put('"');
    if (j->state == J_DONE) { put_kv_num("exit_code", exit_code(j)); put_kv_num("signal", term_signal(j)); }
    put_kv_num("duration_ms", end - j->start_ms);
    put_kv_bool("timed_out", j->kill_reason == KILL_TIMEOUT);
    puts_(",\"killed_by\":"); if (kb) { put('"'); puts_(kb); put('"'); } else puts_("null");
    put_kv_str("cwd", j->cwd, slen(j->cwd));
    put_hits(j);
}

static void emit_hit_events(const struct job *j) {
    for (int i = 0; i < NHITS; i++)
        if (j->hits & (1u << i)) {
            event_begin("rlimit");
            put_kv_num("job_id", j->id); put_kv_str("resource", hit_names[i], slen(hit_names[i]));
            put('}');
            send_out();
        }
}

/* --- Waiters (job.wait) -------------------------------------------------------- */

struct waiter { int used; char id[24]; long job_id, deadline_ms; };
static struct waiter waiters[MAX_JOBS];

static void reply_job_state(const char *id, const struct job *j) {
    struct req r = { .id = "" };
    mcopy(r.id, id, slen(id) + 1);
    ok_begin(&r); put_job_state(j); put('}');
    send_out();
}

static void wake_waiters(const struct job *j, long now) {
    for (int i = 0; i < MAX_JOBS; i++) {
        struct waiter *w = &waiters[i];
        if (!w->used || w->job_id != j->id) continue;
        if (j->state != J_DONE && now < w->deadline_ms) continue;
        w->used = 0;
        reply_job_state(w->id, j);
    }
}

/* --- Finishing -------------------------------------------------------------------- */

static void drain_pipes(struct job *j) {
    static char chunk[4096];
    for (int round = 0; round < 64; round++) {
        struct pollfd fds[2] = { { j->s[0].fd, POLLIN, 0 }, { j->s[1].fd, POLLIN, 0 } };
        int any = 0;
        if (SYS3(SYS_poll, fds, 2, 0) <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (j->s[i].fd < 0 || !fds[i].revents) continue;
            long n = SYS3(SYS_read, j->s[i].fd, chunk, sizeof chunk);
            if (n > 0) { stream_add(j, &j->s[i], chunk, (u32)n); any = 1; }
            else { SYS1(SYS_close, j->s[i].fd); j->s[i].fd = -1; }
        }
        if (!any) break;
    }
}

static void finish_exec(struct job *j) {
    struct req r;
    mzero(&r, sizeof r);
    mcopy(r.id, j->req_id, sizeof r.id);
    drain_pipes(j);
    for (int i = 0; i < 2; i++) {
        if (j->s[i].fd >= 0) SYS1(SYS_close, j->s[i].fd);
        if (j->s[i].spill >= 0) SYS1(SYS_close, j->s[i].spill);
    }
    emit_hit_events(j);
    ok_begin(&r);
    put_job_state(j);
    put_stream("stdout", &j->s[0]);
    put_stream("stderr", &j->s[1]);
    if (!j->s[0].total && !j->s[1].total) puts_(",\"placeholder\":\"(no output)\"");
    put('}');
    send_out();
    xfree(j->buf, j->buf_size);
    mzero(j, sizeof *j);                       /* an exec call leaves nothing behind */
}

static void job_exited(struct job *j, int status) {
    j->status = status;
    j->end_ms = now_ms();
    j->state = J_DONE;
    if (j->exec) {
        wake_waiters(j, j->end_ms);           /* job.wait on an exec call gets the state before the result */
        finish_exec(j);
        return;
    }
    emit_hit_events(j);
    event_begin("job.exit");
    put_job_state(j); put('}');
    send_out();
    wake_waiters(j, j->end_ms);
}

/* Reads the RlimitHits line of /proc/<pid>/status; returns false while the process is not a zombie yet. */
static int zombie_hits(struct job *j) {
    char path[32] = "/proc/", num[12], buf[320];
    fmt_u(num, (unsigned long)j->pid);
    cat(path, sizeof path, num); cat(path, sizeof path, "/status");
    long fd = SYS2(SYS_open, path, O_RDONLY);
    if (fd < 0) return 1;                        /* no procfs (host test): reap and report no hits */
    long n = SYS3(SYS_read, fd, buf, sizeof buf - 1);
    SYS1(SYS_close, fd);
    if (n <= 0) return 1;
    buf[n] = 0;
    int zombie = 0;
    for (long i = 0; i + 8 < n; i++) {
        if (buf[i] == 'S' && buf[i + 1] == 't' && buf[i + 2] == 'a' && buf[i + 3] == 't' &&
            buf[i + 4] == 'e' && buf[i + 5] == ':') {
            long k = i + 6;
            while (buf[k] == ' ' || buf[k] == '\t') k++;
            zombie = buf[k] == 'Z';
        }
        if (buf[i] == 'R' && buf[i + 1] == 'l' && buf[i + 2] == 'i' && buf[i + 3] == 'm' && buf[i + 4] == 'i' &&
            buf[i + 5] == 't' && buf[i + 6] == 'H') {
            long k = i;
            while (k < n && buf[k] != '\t' && buf[k] != '\n') k++;
            j->hits = 0;
            for (k++; k < n && buf[k] != '\n'; ) {
                for (int h = 0; h < NHITS; h++) {
                    size_t l = slen(hit_names[h]);
                    size_t m = 0;
                    while (m < l && buf[k + (long)m] == hit_names[h][m]) m++;
                    if (m == l && (buf[k + (long)l] == ',' || buf[k + (long)l] == '\n')) j->hits |= 1u << h;
                }
                while (k < n && buf[k] != ',' && buf[k] != '\n') k++;
                if (buf[k] == ',') k++;
            }
        }
    }
    return zombie;
}

static void reap_children(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &jobs[i];
        int status = 0;
        if (j->state != J_RUN) continue;
        if (j->limited && !zombie_hits(j)) continue;
        if (SYS4(SYS_wait4, j->pid, &status, WNOHANG, 0) == j->pid) job_exited(j, status);
    }
}

static void check_deadlines(long now) {
    for (int i = 0; i < MAX_JOBS; i++) {
        struct job *j = &jobs[i];
        if (j->state == J_RUN && j->deadline_ms && now >= j->deadline_ms && !j->kill_reason)
            kill_job(j, SIGKILL, KILL_TIMEOUT);
        if (waiters[i].used) {
            struct job *w = job_find(waiters[i].job_id);
            if (!w) waiters[i].used = 0;
            else wake_waiters(w, now);
        }
    }
}

static long next_deadline(void) {
    long d = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].state == J_RUN && jobs[i].deadline_ms && !jobs[i].kill_reason &&
            (!d || jobs[i].deadline_ms < d)) d = jobs[i].deadline_ms;
        if (waiters[i].used && (!d || waiters[i].deadline_ms < d)) d = waiters[i].deadline_ms;
    }
    return d;
}

/* --- Job operations ------------------------------------------------------------- */

static struct job *need_job(const struct req *r) {
    if (r->job_id < 0) { bad(r, "missing job_id"); return NULL; }
    struct job *j = job_find(r->job_id);
    if (!j) fail(r, "ESRCH", "no such job", -3);
    return j;
}

static void op_job_poll(const struct req *r) {
    struct job *j = need_job(r);
    if (!j) return;
    ok_begin(r); put_job_state(j);
    if (!j->exec) {
        put_kv_str("stdout_path", j->paths[0], slen(j->paths[0]));
        put_kv_str("stderr_path", j->paths[1], slen(j->paths[1]));
    }
    put_kv_str("cmd", j->cmd, slen(j->cmd));
    put('}');
    send_out();
}

static void op_job_wait(const struct req *r) {
    struct job *j = need_job(r);
    if (!j) return;
    long t = clamp(r->timeout_ms, 30000, 0, MAX_TIMEOUT_MS);
    if (j->state == J_DONE || !t) return op_job_poll(r);
    for (int i = 0; i < MAX_JOBS; i++)
        if (!waiters[i].used) {
            waiters[i].used = 1; waiters[i].job_id = j->id;
            waiters[i].deadline_ms = now_ms() + t;
            mcopy(waiters[i].id, r->id, sizeof waiters[i].id);
            return;
        }
    fail(r, "EAGAIN", "too many waiters", -11);
}

static void op_job_signal(const struct req *r) {
    struct job *j = need_job(r);
    if (!j) return;
    long sig = r->sig < 0 ? SIGTERM : r->sig;
    if (sig < 1 || sig > 31) return bad(r, "sig must be 1..31");
    if (j->state != J_RUN) return fail(r, "ESRCH", "job has exited", -3);
    kill_job(j, (int)sig, KILL_SIGNAL);
    ok_begin(r); put('}');
    send_out();
}

static void op_job_list(const struct req *r) {
    long now = now_ms();
    ok_begin(r);
    puts_(",\"jobs\":[");
    for (int i = 0, first = 1; i < MAX_JOBS; i++) {
        struct job *j = &jobs[i];
        if (j->state == J_FREE || olen + 400 + OUT_RESERVE > OUT_CAP) continue;
        if (!first) put(',');
        first = 0;
        puts_("{\"job_id\":"); put_num(j->id);
        put_kv_num("pid", j->pid);
        puts_(",\"state\":\""); puts_(j->state == J_RUN ? "running" : "exited"); put('"');
        put_kv_bool("exec", j->exec);
        put_kv_num("age_ms", now - j->start_ms);
        put_kv_str("cmd", j->cmd, slen(j->cmd));
        put('}');
    }
    puts_("]}");
    send_out();
}

static void op_job_release(const struct req *r) {
    struct job *j = need_job(r);
    if (!j) return;
    if (j->state == J_RUN) return fail(r, "EBUSY", "job is still running", -16);
    job_files_remove(j);
    mzero(j, sizeof *j);
    ok_begin(r); put('}');
    send_out();
}

/* Reads up to limit bytes of path at offset into file_buf; returns the count, or -errno. size gets the file size. */
#define FILE_CAP FS_CHUNK
static u8 file_buf[FILE_CAP];

static long read_at(const char *path, long offset, long limit, long *size) {
    long fd = SYS2(SYS_open, path, O_RDONLY);
    if (fd < 0) return fd;
    struct stat64 st;
    *size = SYS2(SYS_fstat64, fd, &st) == 0 ? (long)st.st_size : -1;
    if (offset > 0) {
        long rc = SYS3(SYS_lseek, fd, offset, 0);
        if (rc < 0) { SYS1(SYS_close, fd); return rc; }
    }
    long got = 0;
    while (got < limit) {
        long n = SYS3(SYS_read, fd, file_buf + got, limit - got);
        if (n < 0 && got == 0) { SYS1(SYS_close, fd); return n; }
        if (n <= 0) break;
        got += n;
    }
    SYS1(SYS_close, fd);
    return got;
}

static void op_job_tail(const struct req *r) {
    struct job *j = need_job(r);
    if (!j) return;
    if (j->exec) return bad(r, "exec output comes with the result");
    int err = r->stream && seq(r->stream, "stderr");
    if (r->stream && !err && !seq(r->stream, "stdout")) return bad(r, "stream must be stdout or stderr");
    long offset = r->offset < 0 ? 0 : r->offset, size = -1;
    long want = clamp(r->limit, 8000, 0, 32 * 1024);   /* escaping can grow text sixfold: stay inside one frame */
    long got = read_at(j->paths[err], offset, want, &size);
    if (got < 0) return fail(r, NULL, "read failed", got);
    ok_begin(r);
    put_kv_num("offset", offset); put_kv_num("len", got); put_kv_num("next_offset", offset + got);
    put_kv_num("size", size);
    put_kv_bool("eof", j->state == J_DONE && offset + got >= size);
    puts_(",\"text\":\""); put_jstr((char *)file_buf, (size_t)got);
    puts_("\"}");
    send_out();
}

static void op_cancel(const struct req *r) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].state == J_RUN && jobs[i].exec && r->target >= 0 && jobs[i].reqnum == r->target) {
            kill_job(&jobs[i], SIGKILL, KILL_CANCEL);
            ok_begin(r); put_kv_bool("cancelled", 1); put('}');
            return send_out();
        }
    ok_begin(r); put_kv_bool("cancelled", 0); put('}');      /* already finished: not an error */
    send_out();
}

/* --- File system operations ---------------------------------------------------------- */

static const char *type_name(u32 mode) {
    switch (mode & S_IFMT) {
    case 0040000: return "dir";
    case 0100000: return "file";
    case 0120000: return "symlink";
    case 0020000: return "chr";
    case 0060000: return "blk";
    case 0010000: return "fifo";
    case 0140000: return "sock";
    default: return "other";
    }
}

static void put_stat(const struct stat64 *st) {
    puts_(",\"type\":\""); puts_(type_name(st->st_mode));
    puts_("\",\"size\":"); put_num((long)st->st_size);
    puts_(",\"mode\":"); put_num(st->st_mode & 07777);
    puts_(",\"uid\":"); put_num(st->st_uid);
    puts_(",\"gid\":"); put_num(st->st_gid);
    puts_(",\"mtime\":"); put_num(st->mtime);
}

static void op_fs_stat(const struct req *r) {
    struct stat64 st;
    if (!r->path) return bad(r, "missing path");
    long rc = SYS2(SYS_lstat64, r->path, &st);
    if (rc < 0) return fail(r, NULL, "stat failed", rc);
    ok_begin(r); put_stat(&st); put('}');
    send_out();
}

static void op_fs_read(const struct req *r) {
    long size = -1;
    if (!r->path) return bad(r, "missing path");
    long want = clamp(r->limit, 64 * 1024, 0, FILE_CAP);
    long offset = r->offset < 0 ? 0 : r->offset;
    long got = read_at(r->path, offset, want, &size);
    if (got < 0) return fail(r, NULL, "read failed", got);
    ok_begin(r);
    put_kv_num("size", size); put_kv_num("offset", offset); put_kv_num("len", got);
    put_kv_bool("eof", size >= 0 && offset + got >= size);
    puts_(",\"data\":\""); put_b64(file_buf, (size_t)got); puts_("\"}");
    send_out();
}

/*
 * Replaces a file atomically: the data goes to a temporary file in the same directory, which is
 * renamed over the target when the last chunk (more:false) has arrived. One chunked upload can be
 * open at a time; a write to another path abandons it. append:true appends in place instead.
 */
static struct { long fd; char path[256], tmp[272]; } upload = { -1, "", "" };

static void upload_abort(void) {
    if (upload.fd < 0) return;
    SYS1(SYS_close, upload.fd);
    SYS1(SYS_unlink, upload.tmp);
    upload.fd = -1;
}

static void op_fs_write(const struct req *r) {
    if (!r->path || !r->data) return bad(r, "missing path or data");
    long n = b64_decode((char *)r->data, r->data_len);
    if (n < 0) return bad(r, "data is not base64");
    long mode = r->mode < 0 ? 0644 : r->mode;
    size_t plen = slen(r->path);
    if (plen == 0 || plen >= sizeof upload.path) return fail(r, "ENAMETOOLONG", "path too long", -36);

    if (r->append) {
        long fd = SYS3(SYS_open, r->path, O_WRONLY | O_CREAT | O_APPEND, mode);
        if (fd < 0) return fail(r, NULL, "open failed", fd);
        long rc = write_all((int)fd, r->data, (size_t)n);
        long crc = SYS1(SYS_close, fd);
        if (rc < 0 || crc < 0) return fail(r, NULL, "write failed", rc < 0 ? rc : crc);
        ok_begin(r); put_kv_num("written", n); put_kv_bool("committed", 1); put('}');
        return send_out();
    }
    if (upload.fd >= 0 && !seq(upload.path, r->path)) upload_abort();
    if (upload.fd < 0) {
        mcopy(upload.path, r->path, plen + 1);
        mcopy(upload.tmp, r->path, plen);
        mcopy(upload.tmp + plen, ".agentd-tmp", 12);
        upload.fd = SYS3(SYS_open, upload.tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
        if (upload.fd < 0) { long e = upload.fd; upload.fd = -1; return fail(r, NULL, "open failed", e); }
    }
    long rc = write_all((int)upload.fd, r->data, (size_t)n);
    if (rc < 0) { upload_abort(); return fail(r, NULL, "write failed", rc); }
    if (!r->more) {
        SYS1(SYS_fsync, upload.fd);
        long crc = SYS1(SYS_close, upload.fd);
        upload.fd = -1;
        if (crc >= 0) crc = SYS2(SYS_rename, upload.tmp, upload.path);
        if (crc < 0) { SYS1(SYS_unlink, upload.tmp); return fail(r, NULL, "commit failed", crc); }
    }
    ok_begin(r); put_kv_num("written", n); put_kv_bool("committed", !r->more); put('}');
    send_out();
}

static char dent_buf[4096];

static void op_fs_list(const struct req *r) {
    char full[512];
    if (!r->path) return bad(r, "missing path");
    size_t plen = slen(r->path);
    if (plen + 258 > sizeof full) return fail(r, "ENAMETOOLONG", "path too long", -36);
    long fd = SYS2(SYS_open, r->path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail(r, NULL, "open failed", fd);
    mcopy(full, r->path, plen);
    if (plen && full[plen - 1] != '/') full[plen++] = '/';

    ok_begin(r);
    puts_(",\"entries\":[");
    long skip = r->offset < 0 ? 0 : r->offset, seen = 0;
    int first = 1, clipped = 0;
    for (;;) {
        long n = SYS3(SYS_getdents64, fd, dent_buf, sizeof dent_buf);
        if (n <= 0) break;
        for (long off = 0; off < n && !clipped;) {
            struct dirent64 *d = (struct dirent64 *)(dent_buf + off);
            off += d->d_reclen;
            if (seq(d->d_name, ".") || seq(d->d_name, "..")) continue;
            if (seen++ < skip) continue;
            if (olen + 600 + OUT_RESERVE > OUT_CAP) { clipped = 1; break; }
            size_t nl = slen(d->d_name);
            if (nl > 255) continue;
            mcopy(full + plen, d->d_name, nl + 1);
            if (!first) put(',');
            first = 0;
            puts_("{\"name\":\""); put_jstr(d->d_name, nl); put('"');
            struct stat64 st;
            if (SYS2(SYS_lstat64, full, &st) == 0) put_stat(&st);
            put('}');
        }
        if (clipped) break;
    }
    SYS1(SYS_close, fd);
    puts_("],\"truncated\":"); put_bool(clipped); put('}');
    send_out();
}

static long mkdir_parents(char *path) {
    long rc = 0;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        rc = SYS2(SYS_mkdir, path, 0755);
        *p = '/';
        if (rc < 0 && rc != -17) return rc;
    }
    return 0;
}

static void op_fs_mkdir(const struct req *r) {
    char path[512];
    if (!r->path) return bad(r, "missing path");
    size_t plen = slen(r->path);
    if (!plen || plen >= sizeof path) return fail(r, "ENAMETOOLONG", "path too long", -36);
    mcopy(path, r->path, plen + 1);
    if (r->parents) {
        long rc = mkdir_parents(path);
        if (rc < 0) return fail(r, NULL, "mkdir failed", rc);
    }
    long rc = SYS2(SYS_mkdir, path, 0755);
    if (rc < 0 && !(r->parents && rc == -17)) return fail(r, NULL, "mkdir failed", rc);
    ok_begin(r); put_kv_bool("created", rc == 0); put('}');
    send_out();
}

/* Removes path; directories only if recursive. Returns 0 or -errno. */
static long remove_tree(const char *path, int recursive, int depth) {
    struct stat64 st;
    long rc = SYS2(SYS_lstat64, path, &st);
    if (rc < 0) return rc;
    if ((st.st_mode & S_IFMT) != S_IFDIR) return SYS1(SYS_unlink, path);
    if (!recursive) return SYS1(SYS_rmdir, path);
    if (depth > 12) return -36;
    char child[512], dents[1024];
    size_t plen = slen(path);
    if (plen + 258 > sizeof child) return -36;
    mcopy(child, path, plen);
    if (plen && child[plen - 1] != '/') child[plen++] = '/';
    for (int pass = 0; pass < 100000; pass++) {          /* removing entries moves the directory, so rescan until empty */
        long fd = SYS2(SYS_open, path, O_RDONLY | O_DIRECTORY), removed = 0;
        if (fd < 0) return fd;
        long n = SYS3(SYS_getdents64, fd, dents, sizeof dents);
        for (long off = 0; off < n; ) {
            struct dirent64 *d = (struct dirent64 *)(dents + off);
            off += d->d_reclen;
            if (seq(d->d_name, ".") || seq(d->d_name, "..")) continue;
            if (slen(d->d_name) > 255) continue;
            mcopy(child + plen, d->d_name, slen(d->d_name) + 1);
            rc = remove_tree(child, 1, depth + 1);
            if (rc < 0) { SYS1(SYS_close, fd); return rc; }
            removed++;
        }
        SYS1(SYS_close, fd);
        if (n < 0) return n;
        if (!removed) break;
    }
    return SYS1(SYS_rmdir, path);
}

static void op_fs_remove(const struct req *r) {
    if (!r->path) return bad(r, "missing path");
    if (seq(r->path, "/")) return fail(r, "EPERM", "refusing to remove /", -1);
    long rc = remove_tree(r->path, r->recursive, 0);
    if (rc < 0) return fail(r, NULL, "remove failed", rc);
    ok_begin(r); put('}');
    send_out();
}

/* --- System information ------------------------------------------------------------- */

static long slurp(const char *path, char *buf, size_t cap) {
    long fd = SYS2(SYS_open, path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got + 1 < cap) {
        long n = SYS3(SYS_read, fd, buf + got, cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    SYS1(SYS_close, fd);
    buf[got] = 0;
    return (long)got;
}

/* Finds "Key:" at a line start; returns the value text (leading blanks skipped). */
static const char *field(const char *text, const char *key) {
    size_t kl = slen(key);
    for (const char *p = text; *p; ) {
        size_t i = 0;
        while (i < kl && p[i] == key[i]) i++;
        if (i == kl && p[kl] == ':') {
            p += kl + 1;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return NULL;
}

static size_t line_len(const char *s) {
    size_t n = 0;
    while (s[n] && s[n] != '\n') n++;
    return n;
}

static char proc_buf[2048];

static void op_sys_ps(const struct req *r) {
    long fd = SYS2(SYS_open, "/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail(r, NULL, "/proc is not available", fd);
    ok_begin(r);
    puts_(",\"processes\":[");
    int first = 1, clipped = 0;
    for (;;) {
        long n = SYS3(SYS_getdents64, fd, dent_buf, sizeof dent_buf);
        if (n <= 0) break;
        for (long off = 0; off < n && !clipped;) {
            struct dirent64 *d = (struct dirent64 *)(dent_buf + off);
            off += d->d_reclen;
            const char *name = d->d_name;
            if (*name < '0' || *name > '9') continue;
            size_t nl = slen(name);
            if (nl > 10) continue;
            size_t k = 0;
            while (k < nl && name[k] >= '0' && name[k] <= '9') k++;
            if (k != nl) continue;
            if (olen + 400 + OUT_RESERVE > OUT_CAP) { clipped = 1; break; }
            char path[32] = "/proc/";
            mcopy(path + 6, name, nl);
            mcopy(path + 6 + nl, "/status", 8);
            if (slurp(path, proc_buf, sizeof proc_buf) < 0) continue;   /* exited */
            const char *nm = field(proc_buf, "Name"), *stt = field(proc_buf, "State");
            const char *pp = field(proc_buf, "PPid");
            if (!first) put(',');
            first = 0;
            puts_("{\"pid\":"); puts_(name);
            puts_(",\"name\":\""); if (nm) put_jstr(nm, line_len(nm)); put('"');
            puts_(",\"state\":\""); if (stt) put_jstr(stt, line_len(stt)); put('"');
            puts_(",\"ppid\":");
            long ppid = 0;
            if (pp) { char *q = (char *)pp; if (parse_int(&q, &ppid) < 0) ppid = 0; }
            put_num(ppid);
            put('}');
        }
        if (clipped) break;
    }
    SYS1(SYS_close, fd);
    puts_("],\"truncated\":"); put_bool(clipped); put('}');
    send_out();
}

static void op_sys_info(const struct req *r) {
    char un[6][65];
    static const char *const keys[] = { "sysname", "nodename", "release", "version", "machine" };
    ok_begin(r);
    if (SYS1(SYS_uname, un) == 0)
        for (int i = 0; i < 5; i++) put_kv_str(keys[i], un[i], slen(un[i]));
    long n = slurp("/proc/meminfo", proc_buf, sizeof proc_buf);
    if (n >= 0) put_kv_str("meminfo", proc_buf, (size_t)n);
    n = slurp("/proc/uptime", proc_buf, sizeof proc_buf);
    if (n >= 0) put_kv_str("uptime", proc_buf, (size_t)n);
    put('}');
    send_out();
}

/* --- Dispatch --------------------------------------------------------------------- */

#define PROTO_VERSION 1

static void put_hello(void) {
    puts_(",\"proto\":"); put_num(PROTO_VERSION);
    puts_(",\"agent\":\"agentd\",\"max_frame\":"); put_num(MAX_FRAME);
    put_kv_num("max_jobs", MAX_JOBS); put_kv_num("max_exec", MAX_EXEC);
    put_kv_num("fs_chunk", FS_CHUNK);
    put_kv_str("spill_dir", spill_dir, slen(spill_dir));
    puts_(",\"rlimits\":[\"cpu_s\",\"mem_mb\",\"nproc\"]");
    put_kv_num("up_ms", now_ms());
}

static const struct { const char *op; void (*fn)(const struct req *); } ops[] = {
    { "exec", op_exec }, { "spawn", op_spawn }, { "cancel", op_cancel },
    { "job.poll", op_job_poll }, { "job.wait", op_job_wait }, { "job.tail", op_job_tail },
    { "job.signal", op_job_signal }, { "job.list", op_job_list }, { "job.release", op_job_release },
    { "fs.stat", op_fs_stat }, { "fs.read", op_fs_read }, { "fs.write", op_fs_write },
    { "fs.list", op_fs_list }, { "fs.mkdir", op_fs_mkdir }, { "fs.remove", op_fs_remove },
    { "sys.ps", op_sys_ps }, { "sys.info", op_sys_info },
};

static void dispatch(char *payload) {
    struct req r;
    if (parse_req(payload, &r) < 0) { fail(&r, "bad_request", parse_err, 0); return; }
    if (!r.op) { bad(&r, "missing op"); return; }
    if (seq(r.op, "ping")) { ok_begin(&r); puts_(",\"pong\":true}"); send_out(); return; }
    if (seq(r.op, "hello")) { ok_begin(&r); put_hello(); put('}'); send_out(); return; }
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (seq(r.op, ops[i].op)) { ops[i].fn(&r); return; }
    fail(&r, "unknown_op", "unknown op", 0);
}

/* --- Main loop -------------------------------------------------------------------- */

static u8 rx[MAX_FRAME + 8];
static size_t rx_len;
static long rx_deadline;                 /* a partial frame is dropped if it stalls this long; 0 = none */
static int resync;                       /* discarding input until the line has been quiet for RESYNC_QUIET_MS */

/* Linux builds the old frame (returning through sigreturn) unless SA_SIGINFO is set; Coal always builds the rt frame. */
static int chld_r = -1, chld_w = -1;

static void on_sigchld(int sig) {
    char c = 1;
    (void)sig;
    SYS3(SYS_write, chld_w, &c, 1);
}

__attribute__((naked)) static void sigreturn_trampoline(void) {
    __asm__ volatile ("movl $173, %eax\n\tint $0x80");
}

static void link_error(const char *msg) {
    event_begin("link_error");
    put_kv_str("message", msg, slen(msg));
    put('}');
    send_out();
}

static void start_resync(const char *msg) {
    link_error(msg);
    resync = 1;
    rx_len = 0;
    rx_deadline = now_ms() + RESYNC_QUIET_MS;
}

/* Takes what the link has and runs every complete frame. */
static void link_input(void) {
    long room = (long)(MAX_FRAME + 5 - rx_len);
    long n = SYS3(SYS_read, 0, rx + rx_len, room);
    if (n <= 0) { msleep(10); return; }        /* nothing there, or no carrier yet */
    if (resync) { rx_deadline = now_ms() + RESYNC_QUIET_MS; return; }
    rx_len += (size_t)n;
    while (rx_len >= 4) {
        u32 len = (u32)rx[0] | (u32)rx[1] << 8 | (u32)rx[2] << 16 | (u32)rx[3] << 24;
        if (len > MAX_FRAME) return start_resync("frame too large");
        if (rx_len < 4 + (size_t)len) break;
        u8 next = rx[4 + len];
        rx[4 + len] = 0;                       /* the parser wants a terminated string */
        if (len) dispatch((char *)rx + 4);
        rx[4 + len] = next;
        mcopy(rx, rx + 4 + len, rx_len - 4 - len);
        rx_len -= 4 + len;
    }
    rx_deadline = rx_len ? now_ms() + FRAME_TIMEOUT_MS : 0;
}

static void clean_spill_dir(void) {
    long rc = SYS2(SYS_mkdir, spill_dir, 0755);
    if (rc < 0 && rc != -17) return;
    long fd = SYS2(SYS_open, spill_dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return;
    char path[128];
    for (int pass = 0; pass < 1000; pass++) {          /* unlinking moves entries: rescan until empty */
        long n = SYS3(SYS_getdents64, fd, dent_buf, sizeof dent_buf), removed = 0;
        if (n <= 0) break;
        for (long off = 0; off < n; ) {
            struct dirent64 *d = (struct dirent64 *)(dent_buf + off);
            off += d->d_reclen;
            if (seq(d->d_name, ".") || seq(d->d_name, "..") || slen(d->d_name) > 40) continue;
            path[0] = 0;
            cat(path, sizeof path, spill_dir); cat(path, sizeof path, "/"); cat(path, sizeof path, d->d_name);
            removed += SYS1(SYS_unlink, path) == 0;
        }
        SYS3(SYS_lseek, fd, 0, 0);
        if (!removed) break;
    }
    SYS1(SYS_close, fd);
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 && argv && argv[1] ? argv[1] : "/dev/ttyS1";
    if (argc > 2 && argv[2]) spill_dir = argv[2];
    if (link_open(dev) < 0) return 1;

    int p[2];
    if (SYS2(SYS_pipe2, p, O_NONBLOCK | O_CLOEXEC) < 0) return 1;
    chld_r = p[0]; chld_w = p[1];
    struct ksigaction sa = { on_sigchld, SA_SIGINFO | SA_RESTORER, sigreturn_trampoline, { 0, 0 } };
    SYS4(SYS_rt_sigaction, SIGCHLD, &sa, 0, 8);
    clean_spill_dir();

    event_begin("hello");
    put_hello();
    put('}');
    send_out();

    for (;;) {
        struct pollfd fds[2 + 2 * MAX_JOBS];
        struct job *ojob[2 + 2 * MAX_JOBS];
        struct stream *ostream[2 + 2 * MAX_JOBS];
        int nfds = 0;
        long now = now_ms(), timeout = -1, d = next_deadline();

        fds[nfds] = (struct pollfd){ 0, POLLIN, 0 }; ojob[nfds++] = NULL;
        fds[nfds] = (struct pollfd){ chld_r, POLLIN, 0 }; ojob[nfds++] = NULL;
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobs[i].state != J_RUN || !jobs[i].exec) continue;
            for (int k = 0; k < 2; k++)
                if (jobs[i].s[k].fd >= 0) {
                    fds[nfds] = (struct pollfd){ jobs[i].s[k].fd, POLLIN, 0 };
                    ojob[nfds] = &jobs[i];
                    ostream[nfds++] = &jobs[i].s[k];
                }
        }
        if (d) timeout = d > now ? d - now : 0;
        if (rx_deadline && (timeout < 0 || rx_deadline - now < timeout))
            timeout = rx_deadline > now ? rx_deadline - now : 0;
        for (int i = 0; i < MAX_JOBS; i++)       /* safety net: a lost SIGCHLD must not hang a job */
            if (jobs[i].state == J_RUN && (timeout < 0 || timeout > 1000)) timeout = 1000;

        long ready = SYS3(SYS_poll, fds, nfds, timeout);
        now = now_ms();

        if (ready > 0) {
            char junk[64];
            if (fds[1].revents) while (SYS3(SYS_read, chld_r, junk, sizeof junk) > 0) { }
            for (int i = 2; i < nfds; i++) {
                static char chunk[4096];
                if (!fds[i].revents) continue;
                long n = SYS3(SYS_read, ostream[i]->fd, chunk, sizeof chunk);
                if (n > 0) {
                    stream_add(ojob[i], ostream[i], chunk, (u32)n);
                } else {
                    SYS1(SYS_close, ostream[i]->fd);
                    ostream[i]->fd = -1;
                }
            }
        }
        reap_children();
        check_deadlines(now);
        if (rx_deadline && now >= rx_deadline) {
            if (resync) resync = 0;
            else link_error("incomplete frame dropped");
            rx_len = 0; rx_deadline = 0;
        }
        if (ready > 0 && fds[0].revents) link_input();
    }
}
