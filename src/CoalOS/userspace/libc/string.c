#include <libc/string.h>
#include <libc/stdlib.h>
#include <libc/errno.h>
#include <libc/ctype.h>

/* Whole dwords with rep movsl/stosl, then at most three tail bytes. */
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    size_t words = n >> 2, tail = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
    return r;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    size_t words = n >> 2, tail = n & 3;
    unsigned v = (unsigned char)c * 0x01010101u;
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(words) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(v) : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp <= sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n; n--, p++)
        if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) return NULL;
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) *s++ = 0;
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) break;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n) {
    size_t len = strlen(n);
    for (; *h; h++)
        if (!strncmp(h, n, len)) return (char *)h;
    return len ? NULL : (char *)h;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    return p ? memcpy(p, s, n) : NULL;
}

/* Messages indexed by errno, glibc wording. */
static const char messages[] =
    "Success\0"
    "Operation not permitted\0"
    "No such file or directory\0"
    "No such process\0"
    "Interrupted system call\0"
    "Input/output error\0"
    "No such device or address\0"
    "Argument list too long\0"
    "Exec format error\0"
    "Bad file descriptor\0"
    "No child processes\0"
    "Resource temporarily unavailable\0"
    "Cannot allocate memory\0"
    "Permission denied\0"
    "Bad address\0"
    "Block device required\0"
    "Device or resource busy\0"
    "File exists\0"
    "Invalid cross-device link\0"
    "No such device\0"
    "Not a directory\0"
    "Is a directory\0"
    "Invalid argument\0"
    "Too many open files in system\0"
    "Too many open files\0"
    "Inappropriate ioctl for device\0"
    "Text file busy\0"
    "File too large\0"
    "No space left on device\0"
    "Illegal seek\0"
    "Read-only file system\0"
    "Too many links\0"
    "Broken pipe\0"
    "Numerical argument out of domain\0"
    "Numerical result out of range\0"
    "Resource deadlock avoided\0"
    "File name too long\0"
    "No locks available\0"
    "Function not implemented\0"
    "Directory not empty\0"
    "Too many levels of symbolic links\0"
    "Unknown error\0"
    "No message of desired type\0"
    "Identifier removed\0"
    "Channel number out of range\0"
    "Level 2 not synchronized\0"
    "Level 3 halted\0"
    "Level 3 reset\0"
    "Link number out of range\0"
    "Protocol driver not attached\0"
    "No CSI structure available\0"
    "Level 2 halted\0"
    "Invalid exchange\0"
    "Invalid request descriptor\0"
    "Exchange full\0"
    "No anode\0"
    "Invalid request code\0"
    "Invalid slot\0"
    "Unknown error\0"
    "Bad font file format\0"
    "Device not a stream\0"
    "No data available\0"
    "Timer expired\0"
    "Out of streams resources\0"
    "Machine is not on the network\0"
    "Package not installed\0"
    "Object is remote\0"
    "Link has been severed\0"
    "Advertise error\0"
    "Srmount error\0"
    "Communication error on send\0"
    "Protocol error\0"
    "Multihop attempted\0"
    "RFS specific error\0"
    "Bad message\0"
    "Value too large for defined data type\0"
    "Name not unique on network\0"
    "File descriptor in bad state\0"
    "Remote address changed\0"
    "Can not access a needed shared library\0"
    "Accessing a corrupted shared library\0"
    ".lib section in a.out corrupted\0"
    "Attempting to link in too many shared libraries\0"
    "Cannot exec a shared library directly\0"
    "Invalid or incomplete multibyte or wide character\0"
    "Interrupted system call should be restarted\0"
    "Streams pipe error\0"
    "Too many users\0"
    "Socket operation on non-socket\0"
    "Destination address required\0"
    "Message too long\0"
    "Protocol wrong type for socket\0"
    "Protocol not available\0"
    "Protocol not supported\0"
    "Socket type not supported\0"
    "Operation not supported\0"
    "Protocol family not supported\0"
    "Address family not supported by protocol\0"
    "Address already in use\0"
    "Cannot assign requested address\0"
    "Network is down\0"
    "Network is unreachable\0"
    "Network dropped connection on reset\0"
    "Software caused connection abort\0"
    "Connection reset by peer\0"
    "No buffer space available\0"
    "Transport endpoint is already connected\0"
    "Transport endpoint is not connected\0"
    "Cannot send after transport endpoint shutdown\0"
    "Too many references: cannot splice\0"
    "Connection timed out\0"
    "Connection refused\0"
    "Host is down\0"
    "No route to host\0"
    "Operation already in progress\0"
    "Operation now in progress\0"
    "Stale file handle\0"
    "Structure needs cleaning\0"
    "Not a XENIX named type file\0"
    "No XENIX semaphores available\0"
    "Is a named type file\0"
    "Remote I/O error\0"
    "Disk quota exceeded\0"
    "No medium found\0"
    "Wrong medium type\0"
    "Operation canceled\0"
    "Required key not available\0"
    "Key has expired\0"
    "Key has been revoked\0"
    "Key was rejected by service\0"
    "Owner died\0"
    "State not recoverable\0"
    "Operation not possible due to RF-kill\0"
    "Memory page has hardware error\0"
;

char *strerror(int err) {
    if (err < 0 || err > 133) return "Unknown error";
    const char *m = messages;
    while (err--) m += strlen(m) + 1;
    return (char *)m;
}
