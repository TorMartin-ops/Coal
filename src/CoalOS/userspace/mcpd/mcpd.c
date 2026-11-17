/*
 * mcpd - the Model Context Protocol agent inside Coal OS.
 *
 * Serves newline-delimited JSON requests on a serial port (default
 * /dev/ttyS1, COM2) so a host-side MCP server can run commands and move files.
 * One request per line, one response per line; every response carries the
 * request's "id". Requests are handled one at a time.
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

/* --- Linux i386 system calls ---------------------------------------------- */

enum {
    SYS_exit = 1, SYS_fork = 2, SYS_read = 3, SYS_write = 4, SYS_open = 5,
    SYS_close = 6, SYS_lseek = 19, SYS_kill = 37, SYS_pipe = 42,
    SYS_ioctl = 54, SYS_setsid = 66, SYS_dup2 = 63, SYS_execve = 11, SYS_uname = 122,
    SYS_wait4 = 114, SYS_nanosleep = 162, SYS_poll = 168,
    SYS_getdents64 = 220, SYS_lstat64 = 196, SYS_fstat64 = 197,
    SYS_clock_gettime = 265,
};

enum {
    O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0x40, O_NOCTTY = 0x100,
    O_TRUNC = 0x200, O_APPEND = 0x400, O_DIRECTORY = 0x10000,
    TCGETS = 0x5401, TCSETS = 0x5402,
    POLLIN = 1, POLLHUP = 0x10, WNOHANG = 1, SIGKILL = 9,
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

#define S_IFMT 0170000

/* --- Small helpers --------------------------------------------------------- */

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void mcopy(void *d, const void *s, size_t n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
}

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

/* --- Response builder ------------------------------------------------------ */

#define OUT_CAP  (384 * 1024)
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

/* --- JSON request parser --------------------------------------------------- */

struct req {
    char id[32];                   /* raw JSON token echoed back */
    const char *op, *cmd, *path, *data;
    size_t data_len;
    long offset, max, timeout_ms, mode;
    int append, more;              /* more: keep the file open, another chunk follows */
};

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
    while (**p && **p != ',' && **p != '}' && **p != ']' && **p != ' ')
        (*p)++;
    return 0;
}

/* Reads an integer in [-2^31+1, 2^31-1]; 0 on success, -1 if it is not one or does not fit. */
static int parse_int(char **p, long *out) {
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
    if (**p && **p != ',' && **p != '}' && **p != ' ' && **p != '\t' && **p != '\r') return -1;
    *out = neg ? -(long)v : (long)v;
    return 0;
}

static const char *parse_err = "malformed JSON";

static int parse_req(char *line, struct req *r) {
    char *p = line;
    r->id[0] = 'n'; r->id[1] = 'u'; r->id[2] = 'l'; r->id[3] = 'l'; r->id[4] = 0;
    r->op = r->cmd = r->path = r->data = NULL;
    parse_err = "malformed JSON";
    r->data_len = 0; r->offset = 0; r->max = -1; r->timeout_ms = -1;
    r->mode = 0644; r->append = 0; r->more = 0;

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
                }
            }
        } else if (seq(key, "op") || seq(key, "cmd") || seq(key, "path") ||
                   seq(key, "data")) {
            if (*p != '"') { if (skip_value(&p, 0) < 0) return -1; }
            else {
                if (!(val = parse_str(&p, &vlen))) return -1;
                if (key[0] == 'o') r->op = val;
                else if (key[0] == 'c') r->cmd = val;
                else if (key[0] == 'p') r->path = val;
                else { r->data = val; r->data_len = vlen; }
            }
        } else if (seq(key, "offset") || seq(key, "max") || seq(key, "timeout_ms") ||
                   seq(key, "mode")) {
            long *dst = key[0] == 'o' ? &r->offset : key[0] == 't' ? &r->timeout_ms
                      : key[1] == 'a' ? &r->max : &r->mode;
            if (parse_int(&p, dst) < 0) { parse_err = "number out of range or not an integer"; return -1; }
        }
        else if (seq(key, "append") || seq(key, "more")) {
            if (key[0] == 'a') r->append = *p == 't'; else r->more = *p == 't';
            if (skip_value(&p, 0) < 0) return -1;
        }
        else if (skip_value(&p, 0) < 0) return -1;

        skip_ws(&p);
        if (*p == ',') { p++; continue; }
        return *p == '}' ? 0 : -1;
    }
}

/* --- Operations ------------------------------------------------------------ */

static void begin(const struct req *r) {
    olen = 0; ofull = 0;
    puts_("{\"id\":"); puts_(r->id);
}

static const char *errno_text(long e) {
    switch (e) {
    case 1: return "Operation not permitted";
    case 2: return "No such file or directory";
    case 3: return "No such process";
    case 4: return "Interrupted system call";
    case 5: return "Input/output error";
    case 9: return "Bad file descriptor";
    case 11: return "Resource temporarily unavailable";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 14: return "Bad address";
    case 16: return "Device or resource busy";
    case 17: return "File exists";
    case 19: return "No such device";
    case 20: return "Not a directory";
    case 21: return "Is a directory";
    case 22: return "Invalid argument";
    case 23: return "Too many open files in system";
    case 24: return "Too many open files";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 30: return "Read-only file system";
    case 32: return "Broken pipe";
    case 36: return "File name too long";
    case 38: return "Function not implemented";
    case 39: return "Directory not empty";
    default: return NULL;
    }
}

/* err < 0 is a negative errno; its text is appended to msg. */
static void fail(const struct req *r, const char *msg, long err) {
    begin(r);
    puts_(",\"ok\":false,\"error\":\""); put_jstr(msg, slen(msg));
    if (err < 0) {
        const char *t = errno_text(-err);
        if (t) { puts_(": "); put_jstr(t, slen(t)); }
    }
    put('"');
    if (err < 0) { puts_(",\"errno\":"); put_num(-err); }
    put('}');
}

static void ok_begin(const struct req *r) { begin(r); puts_(",\"ok\":true"); }

static void put_kv_str(const char *k, const char *v, size_t n) {
    puts_(",\""); puts_(k); puts_("\":\""); put_jstr(v, n); put('"');
}

#define RUN_CAP (64 * 1024)
static char run_out[RUN_CAP], run_err[RUN_CAP];
static char *const run_env[] = {
    "PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/root", "TERM=dumb", NULL };

/*
 * Reads what is available on fd (it is known to be readable) into buf. Output
 * beyond the cap is read and discarded so the writer never blocks on a full
 * pipe. Returns false at end of file.
 */
static int drain(int fd, char *buf, size_t *len, int *clipped) {
    char tmp[2048];
    size_t room = RUN_CAP - *len;
    long n = room ? SYS3(SYS_read, fd, buf + *len, room < 4096 ? room : 4096)
                  : SYS3(SYS_read, fd, tmp, sizeof tmp);
    if (n <= 0) return 0;
    if (room) *len += (size_t)n; else *clipped = 1;
    return 1;
}

/* One poll round over the output pipes; returns false once both are closed. */
static int pump(struct pollfd *fds, long wait_ms, size_t *nout, size_t *nerr, int *clipped) {
    if (fds[0].fd < 0 && fds[1].fd < 0) return 0;
    long rcp = SYS3(SYS_poll, fds, 2, wait_ms);
    for (int i = 0; i < 2 && rcp > 0; i++) {
        if (fds[i].fd < 0 || !fds[i].revents) continue;
        int more = i == 0 ? drain(fds[0].fd, run_out, nout, clipped)
                          : drain(fds[1].fd, run_err, nerr, clipped);
        if (!more) { SYS1(SYS_close, fds[i].fd); fds[i].fd = -1; }
    }
    return rcp > 0;
}

static void op_run(const struct req *r) {
    int po[2], pe[2];
    if (!r->cmd) return fail(r, "missing cmd", 0);
    long limit = r->timeout_ms < 0 ? 10000 : r->timeout_ms;
    if (limit > 600000) limit = 600000;
    long rc = SYS1(SYS_pipe, po);
    if (rc < 0) return fail(r, "pipe failed", rc);
    rc = SYS1(SYS_pipe, pe);
    if (rc < 0) {
        SYS1(SYS_close, po[0]); SYS1(SYS_close, po[1]);
        return fail(r, "pipe failed", rc);
    }
    long pid = SYS1(SYS_fork, 0);
    if (pid < 0) {
        SYS1(SYS_close, po[0]); SYS1(SYS_close, po[1]);
        SYS1(SYS_close, pe[0]); SYS1(SYS_close, pe[1]);
        return fail(r, "fork failed", pid);
    }
    if (pid == 0) {
        char *argv[] = { "sh", "-c", (char *)r->cmd, NULL };
        SYS1(SYS_setsid, 0);                   /* own process group: a timeout kills the whole tree */
        long nul = SYS2(SYS_open, "/dev/null", O_RDONLY);
        if (nul >= 0) { SYS2(SYS_dup2, nul, 0); if (nul > 2) SYS1(SYS_close, nul); }
        SYS2(SYS_dup2, po[1], 1);
        SYS2(SYS_dup2, pe[1], 2);
        SYS1(SYS_close, po[0]); SYS1(SYS_close, po[1]);
        SYS1(SYS_close, pe[0]); SYS1(SYS_close, pe[1]);
        SYS3(SYS_execve, "/bin/sh", argv, run_env);
        SYS1(SYS_exit, 127);
    }
    SYS1(SYS_close, po[1]); SYS1(SYS_close, pe[1]);

    size_t nout = 0, nerr = 0;
    int clipped = 0, timed_out = 0, status = 0, reaped = 0;
    struct pollfd fds[2] = { { po[0], POLLIN, 0 }, { pe[0], POLLIN, 0 } };
    long start = now_ms();

    /* Done when the shell exits, not when every writer closes: a background job
       (cmd &) keeps the pipes open for as long as it lives. */
    for (;;) {
        pump(fds, 10, &nout, &nerr, &clipped);
        long w = SYS4(SYS_wait4, pid, &status, WNOHANG, 0);
        if (w == pid || w < 0) { reaped = 1; break; }
        if (now_ms() - start >= limit) { timed_out = 1; break; }
    }
    if (!reaped) {
        SYS2(SYS_kill, -pid, SIGKILL);         /* the whole group, pid is its leader */
        SYS2(SYS_kill, pid, SIGKILL);
        SYS4(SYS_wait4, pid, &status, 0, 0);
    }
    /* Collect what was already written; stop at the first quiet moment or after
       a short grace period (a background job may keep writing). */
    for (long t0 = now_ms(); now_ms() - t0 < 200 && pump(fds, 5, &nout, &nerr, &clipped); )
        ;
    for (int i = 0; i < 2; i++)
        if (fds[i].fd >= 0) SYS1(SYS_close, fds[i].fd);

    int code = (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
    ok_begin(r);
    puts_(",\"exit\":"); put_num(code);
    puts_(",\"stdout\":\""); if (!put_jstr(run_out, nout)) clipped = 1; put('"');
    puts_(",\"stderr\":\""); if (!put_jstr(run_err, nerr)) clipped = 1; put('"');
    puts_(",\"timed_out\":"); puts_(timed_out ? "true" : "false");
    puts_(",\"truncated\":"); puts_(clipped ? "true" : "false");
    put('}');
}

#define FILE_CAP (128 * 1024)
static u8 file_buf[FILE_CAP];

static void op_read_file(const struct req *r) {
    if (!r->path) return fail(r, "missing path", 0);
    long want = r->max < 0 || r->max > FILE_CAP ? 64 * 1024 : r->max;
    long fd = SYS2(SYS_open, r->path, O_RDONLY);
    if (fd < 0) return fail(r, "open failed", fd);
    struct stat64 st;
    long size = SYS2(SYS_fstat64, fd, &st) == 0 ? (long)st.st_size : -1;
    if (r->offset > 0) {
        long rc = SYS3(SYS_lseek, fd, r->offset, 0);
        if (rc < 0) { SYS1(SYS_close, fd); return fail(r, "seek failed", rc); }
    }
    long got = 0;
    while (got < want) {
        long n = SYS3(SYS_read, fd, file_buf + got, want - got);
        if (n < 0 && got == 0) { SYS1(SYS_close, fd); return fail(r, "read failed", n); }
        if (n <= 0) break;
        got += n;
    }
    SYS1(SYS_close, fd);
    ok_begin(r);
    puts_(",\"size\":"); put_num(size);
    puts_(",\"offset\":"); put_num(r->offset);
    puts_(",\"len\":"); put_num(got);
    puts_(",\"data\":\""); put_b64(file_buf, (size_t)got); puts_("\"}");
}

/* A multi-chunk upload keeps its file open between requests (opening and closing
   dominate the cost of a small write). Any other request closes it first. */
static long wfd = -1;
static char wpath[256];

static long close_write(void) {
    long rc = 0;
    if (wfd >= 0) rc = SYS1(SYS_close, wfd);
    wfd = -1;
    return rc;
}

static void op_write_file(const struct req *r) {
    if (!r->path || !r->data) return fail(r, "missing path or data", 0);
    long n = b64_decode((char *)r->data, r->data_len);
    if (n < 0) return fail(r, "data is not base64", 0);
    if (wfd >= 0 && !(r->append && seq(wpath, r->path))) {
        long crc = close_write();
        if (crc < 0) return fail(r, "close failed", crc);
    }
    if (wfd < 0) {
        long flags = O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
        wfd = SYS3(SYS_open, r->path, flags, r->mode);
        if (wfd < 0) { long e = wfd; wfd = -1; return fail(r, "open failed", e); }
        if (slen(r->path) < sizeof wpath) mcopy(wpath, r->path, slen(r->path) + 1);
        else wpath[0] = 0;
    }
    long rc = write_all((int)wfd, r->data, (size_t)n);
    long crc = 0;
    if (rc < 0 || !r->more) crc = close_write();
    if (rc < 0) { close_write(); return fail(r, "write failed", rc); }
    if (crc < 0) return fail(r, "close failed", crc);
    ok_begin(r);
    puts_(",\"written\":"); put_num(n); put('}');
}

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

static void op_stat(const struct req *r) {
    struct stat64 st;
    if (!r->path) return fail(r, "missing path", 0);
    long rc = SYS2(SYS_lstat64, r->path, &st);
    if (rc < 0) return fail(r, "stat failed", rc);
    ok_begin(r); put_stat(&st); put('}');
}

static char dent_buf[4096];

static void op_list_dir(const struct req *r) {
    char full[512];
    if (!r->path) return fail(r, "missing path", 0);
    size_t plen = slen(r->path);
    if (plen + 258 > sizeof full) return fail(r, "path too long", 0);
    long fd = SYS2(SYS_open, r->path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail(r, "open failed", fd);
    mcopy(full, r->path, plen);
    if (plen && full[plen - 1] != '/') full[plen++] = '/';

    ok_begin(r);
    puts_(",\"entries\":[");
    int first = 1, clipped = 0;
    for (;;) {
        long n = SYS3(SYS_getdents64, fd, dent_buf, sizeof dent_buf);
        if (n <= 0) break;
        for (long off = 0; off < n && !clipped;) {
            struct dirent64 *d = (struct dirent64 *)(dent_buf + off);
            off += d->d_reclen;
            if (seq(d->d_name, ".") || seq(d->d_name, "..")) continue;
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
    puts_("],\"truncated\":"); puts_(clipped ? "true" : "false"); put('}');
}

/* Reads a small file into buf (NUL-terminated); returns length or -1. */
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

static void op_ps(const struct req *r) {
    long fd = SYS2(SYS_open, "/proc", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail(r, "/proc is not available", fd);
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
    puts_("],\"truncated\":"); puts_(clipped ? "true" : "false"); put('}');
}

static void op_sysinfo(const struct req *r) {
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
}

static void dispatch(char *line) {
    struct req r;
    if (parse_req(line, &r) < 0) { fail(&r, parse_err, 0); return; }
    if (r.op && !seq(r.op, "write_file") && wfd >= 0) close_write();
    if (!r.op) fail(&r, "missing op", 0);
    else if (seq(r.op, "ping")) { ok_begin(&r); puts_(",\"pong\":true}"); }
    else if (seq(r.op, "run")) op_run(&r);
    else if (seq(r.op, "read_file")) op_read_file(&r);
    else if (seq(r.op, "write_file")) op_write_file(&r);
    else if (seq(r.op, "list_dir")) op_list_dir(&r);
    else if (seq(r.op, "stat")) op_stat(&r);
    else if (seq(r.op, "ps")) op_ps(&r);
    else if (seq(r.op, "sysinfo")) op_sysinfo(&r);
    else fail(&r, "unknown op", 0);
}

static void send_out(void) {
    if (ofull) {
        static const char big[] = "{\"id\":null,\"ok\":false,\"error\":\"response too large\"}";
        olen = 0; mcopy(out, big, sizeof big - 1); olen = sizeof big - 1;
    }
    put('\n');
    write_all(1, out, olen);
}

/* --- Main loop -------------------------------------------------------------- */

#define LINE_CAP (192 * 1024)
static char line[LINE_CAP];

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

int main(int argc, char **argv) {
    const char *dev = argc > 1 && argv && argv[1] ? argv[1] : "/dev/ttyS1";
    long fd = SYS2(SYS_open, dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return 1;
    make_raw((int)fd);
    SYS2(SYS_dup2, fd, 0);
    SYS2(SYS_dup2, fd, 1);
    if (fd > 1) SYS1(SYS_close, fd);

    olen = 0; ofull = 0;
    puts_("{\"event\":\"ready\",\"version\":1}");
    send_out();

    static char rx[4096];
    size_t len = 0;
    int skipping = 0;
    for (;;) {
        long n = SYS3(SYS_read, 0, rx, sizeof rx);
        if (n <= 0) { msleep(100); continue; }     /* no carrier yet; keep waiting */
        for (long i = 0; i < n; i++) {
            char c = rx[i];
            if (c != '\n') {
                if (len + 1 < LINE_CAP) line[len++] = c; else skipping = 1;
                continue;
            }
            if (skipping) {
                struct req r = { .id = "null" };
                fail(&r, "request too large", 0);
                send_out();
            } else if (len) {
                line[len] = 0;
                dispatch(line);
                send_out();
            }
            len = 0; skipping = 0;
        }
    }
}
