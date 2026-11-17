/**
 * @file socket.h
 * @brief Kernel sockets (AF_INET: TCP streams, UDP datagrams, raw ICMP) behind file descriptors.
 *
 * Operations return a value >= 0 or a negative errno. They block with
 * interrupts off on the socket's wait queue, so callers must pass kernel
 * buffers; the system call layer copies to and from user space around them.
 */
#ifndef KERNEL_NET_SOCKET_H
#define KERNEL_NET_SOCKET_H

#include <kernel/core/types.h>
#include <kernel/drivers/wait.h>
#include <kernel/fs/vfs/sys_file.h>

#define AF_UNIX         1
#define AF_INET         2
#define AF_VSOCK        40
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_NONBLOCK   O_NONBLOCK
#define SOCK_CLOEXEC    02000000

#define MSG_PEEK        0x02
#define MSG_DONTWAIT    0x40
#define MSG_NOSIGNAL    0x4000
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_LINGER       13
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21
#define TCP_NODELAY     1

#define NET_MAX_SOCKS   64
#define BACKLOG_MAX     32
#define DGRAM_QUEUE_MAX 16

/** The Linux sockaddr_in: port and address in network byte order. */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

struct dgram {
    struct dgram *next;
    uint32_t src;
    uint16_t sport, len;
    uint8_t  data[];
};

struct tcp_conn;
struct sock;

/**
 * Operations of a protocol other than the built-in AF_INET ones (AF_VSOCK). A socket with `ops` set is
 * handled entirely by them. Addresses arrive as the raw sockaddr copied from user space.
 */
struct sock_ops {
    int     (*bind)(struct sock *s, const void *addr, size_t len);
    int     (*connect)(struct sock *s, const void *addr, size_t len);
    int     (*listen)(struct sock *s, int backlog);
    int     (*accept)(struct sock *s, int flags, void *addr, size_t *len);     /* new descriptor */
    ssize_t (*send)(struct sock *s, const void *buf, size_t len, int flags);
    ssize_t (*recv)(struct sock *s, void *buf, size_t len, int flags);
    int     (*shutdown)(struct sock *s, int how);
    int     (*name)(const struct sock *s, bool peer, void *addr, size_t *len);
    uint32_t (*poll)(struct sock *s);
    void    (*close)(struct sock *s);                                          /* not the struct sock itself */
};

struct sock {
    uint8_t  type, proto;
    bool     nonblock, listening, connected, reuse;
    bool     shut_rd;
    int      err;                           /* pending error, positive errno, reported once */
    uint32_t laddr, raddr;                  /* host order; laddr 0 = any */
    uint16_t lport, rport;
    uint32_t rcv_timeout_ms, snd_timeout_ms; /* 0 = block forever */
    drv_waitq_t wq;
    const struct sock_ops *ops;             /* non-NULL: a protocol outside AF_INET */
    void *pdata;                            /* its state */
    struct tcp_conn *tcp;
    struct tcp_conn *accq[BACKLOG_MAX];     /* established, not yet accepted */
    uint8_t  acc_n, backlog;
    struct dgram *rxq, *rxq_tail;
    uint8_t  rxq_n;
};

extern struct sock *net_socks[NET_MAX_SOCKS];

/* Descriptor level API used by the system call layer */
int  sock_create(int domain, int type, int protocol);          /* new descriptor */
struct sock *sock_of(sys_file_t *sf);                          /* NULL if sf is not a socket */
int  sock_bind(struct sock *s, uint32_t addr, uint16_t port);
int  sock_listen(struct sock *s, int backlog);
int  sock_accept(struct sock *s, uint32_t *raddr, uint16_t *rport, int flags);   /* new descriptor */
int  sock_connect(struct sock *s, uint32_t addr, uint16_t port);
ssize_t sock_send(struct sock *s, const void *buf, size_t len, int flags, const struct sockaddr_in *to);
ssize_t sock_recv(struct sock *s, void *buf, size_t len, int flags, struct sockaddr_in *from);
int  sock_shutdown(struct sock *s, int how);
int  sock_setopt(struct sock *s, int level, int name, const void *val, size_t len);
int  sock_getopt(struct sock *s, int level, int name, void *val, size_t *len);
void sock_name(const struct sock *s, bool peer, struct sockaddr_in *out);

/* For protocol code: sleeping on a socket, and socket objects */
typedef struct { bool timed; uint32_t end; } sock_wait_t;
void sock_wait_init(sock_wait_t *w, uint32_t ms);          /* ms == 0: no deadline */
int  sock_wait(struct sock *s, const sock_wait_t *w);      /* interrupts off; -EINTR, or -EAGAIN at the deadline */
struct sock *sock_new(int type, int protocol);             /* registered, interrupts off */
int  sock_install(struct sock *s, int flags);              /* new descriptor; frees s on failure */
void sock_free(struct sock *s);

/* Internal hooks for the protocol code */
struct sock *sock_find_listener(uint16_t port);
void sock_wake(struct sock *s);

void sock_queue_dgram(struct sock *s, uint32_t src, uint16_t sport, const void *data, size_t len);
bool sock_port_in_use(uint8_t type, uint16_t port);

#endif
