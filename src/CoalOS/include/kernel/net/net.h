/**
 * @file net.h
 * @brief Network core: interface state, Ethernet/ARP/IPv4/ICMP, and the hooks the
 *        transport protocols (UDP, TCP, DHCP) plug into.
 *
 * The whole stack runs with interrupts disabled: receive processing happens in
 * the NIC interrupt and the timer tick, and system calls enter it through
 * local_irq_save(). That single rule replaces all locking, and it lets every
 * layer share one static transmit frame. IPv4 addresses are kept in host byte
 * order; the wire format is produced only when a header is written.
 */
#ifndef KERNEL_NET_NET_H
#define KERNEL_NET_NET_H

#include <kernel/core/types.h>
#include <kernel/drivers/timer/pit.h>

#define ETH_HLEN        14
#define IP_HLEN         20
#define NET_MTU         1500
#define IP_PAYLOAD_MAX  (NET_MTU - IP_HLEN)

#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define IP4_BROADCAST   0xFFFFFFFFu
#define IP4_FMT         "%u.%u.%u.%u"
#define IP4_ARGS(ip)    (unsigned)((ip) >> 24), (unsigned)((ip) >> 16 & 255), (unsigned)((ip) >> 8 & 255), (unsigned)((ip) & 255)

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
#define ntohs htons
#define ntohl htonl

struct netif {
    uint8_t  mac[6];
    uint32_t ip, mask, gw, dns;    /* 0 until DHCP (or the static fallback) configured them */
    bool     present;              /* a NIC was found and started */
};
extern struct netif net_if;

/** Starts the protocol timers and the loopback; finds the NIC and starts DHCP. Call once with interrupts able to run later. */
void net_init(void);

/** Milliseconds since boot, wrapping. Compare with (int32_t)(a - b). */
static inline uint32_t net_now(void) { return (uint32_t)timer_ticks(); }

/** Internet checksum of len bytes, folded and complemented, added to a running sum. */
uint32_t net_csum_add(uint32_t sum, const void *data, size_t len);
uint16_t net_csum_fold(uint32_t sum);
/** Checksum over the TCP/UDP pseudo header plus the segment. */
uint16_t net_l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *seg, size_t len);

/** True for 127.0.0.0/8. Packets to it, and to our own address, are looped back inside the stack. */
bool net_is_loopback(uint32_t ip);
/** The source address a packet to dst leaves with: dst itself for loopback, else the interface address. */
uint32_t net_src_for(uint32_t dst);

/** The IP payload area of the shared transmit frame. Fill up to IP_PAYLOAD_MAX bytes, then net_ip_send(). */
uint8_t *net_tx_payload(void);

/** Sends the payload in net_tx_payload() to dst (src 0 means net_src_for(dst)).
 *  Returns 0, or -ENETDOWN / -ENETUNREACH. A packet waiting for ARP is sent once the peer answers. */
int net_ip_send(uint8_t proto, uint32_t src, uint32_t dst, size_t len);

/** Called by the NIC driver for every received frame (interrupt context). */
void net_rx_frame(const uint8_t *frame, size_t len);

/** Makes the protocol timers (tcp_tick, dhcp_tick, NIC poll) run within ms milliseconds; an earlier request stays. */
void net_timer_kick(uint32_t ms);

/** Sends one UDP datagram (payload up to IP_PAYLOAD_MAX - 8 bytes) from sport. src 0 = the interface address. */
int udp_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);

/* Transport hooks. src/dst are host order, seg points at the transport header. */
bool udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);   /* false: no socket, send port unreachable */
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
/** Retransmits what is overdue. Returns the milliseconds until it has work again, 0 if no timer is armed. */
uint32_t tcp_tick(void);
void icmp_deliver(const uint8_t *ip_packet, size_t len);   /* to raw sockets */
/** An ICMP port unreachable arrived; quoted is the IP header and first 8 bytes of our datagram. */
void icmp_port_unreachable(const uint8_t *quoted, size_t len);

/* DHCP client (UDP port 68). */
void dhcp_start(void);
void dhcp_input(const uint8_t *msg, size_t len);
/** Returns the milliseconds until dhcp_tick() has work again, 0 if it needs none. */
uint32_t dhcp_tick(void);

#endif
