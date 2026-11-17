#ifndef LIBC_NETINET_IN_H
#define LIBC_NETINET_IN_H

#include <libc/stdint.h>
#include <libc/sys/socket.h>

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define INADDR_ANY       ((uint32_t)0)
#define INADDR_LOOPBACK  ((uint32_t)0x7F000001)
#define INADDR_BROADCAST ((uint32_t)0xFFFFFFFF)

#define TCP_NODELAY 1

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr { in_addr_t s_addr; };

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#endif
