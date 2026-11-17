/**
 * @file net.c
 * @brief Ethernet, ARP, IPv4, ICMP echo and port unreachable, and the loopback interface.
 *
 * Packets for 127.0.0.0/8 or for our own address never reach the NIC: they are
 * queued (LO_SLOTS packets, excess is dropped like on a full NIC ring) and
 * delivered by a timer on the next millisecond. Delivering from the timer
 * rather than from net_ip_send() keeps the transport code from being re-entered
 * while it is still building the packet.
 *
 * Two kernel timers drive the stack: one delivers loopback packets, the other runs the TCP
 * and DHCP timers and polls a NIC without interrupts. Neither runs while there is nothing to do.
 */
#define LOG_MODULE "net"

#include <kernel/net/net.h>
#include <kernel/drivers/net/e1000.h>
#include <kernel/drivers/timer/timer.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/core/log.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>

#define ETH_IP      0x0800
#define ETH_ARP     0x0806
#define ARP_REQUEST 1
#define ARP_REPLY   2
#define ARP_TTL_MS  300000u
#define ARP_SLOTS   8
#define LO_SLOTS    16
#define ICMP_UNREACH      3
#define ICMP_PORT_UNREACH 3

struct eth_hdr { uint8_t dst[6], src[6]; uint16_t type; } __attribute__((packed));

struct ip_hdr {
    uint8_t  vihl, tos;
    uint16_t len, id, frag;
    uint8_t  ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

struct arp_pkt {
    uint16_t hw, proto;
    uint8_t  hlen, plen;
    uint16_t op;
    uint8_t  smac[6];
    uint32_t sip;
    uint8_t  tmac[6];
    uint32_t tip;
} __attribute__((packed));

struct icmp_hdr { uint8_t type, code; uint16_t csum; } __attribute__((packed));

struct netif net_if;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t NO_MAC[6] = { 0 };

static struct { uint32_t ip, expires; uint8_t mac[6]; bool used; } arp_cache[ARP_SLOTS];
static uint8_t  tx_frame[ETH_HLEN + NET_MTU];       /* the one outgoing frame */
static uint8_t  arp_held[ETH_HLEN + NET_MTU];       /* a frame waiting for its next hop's MAC */
static size_t   arp_held_len;
static uint32_t arp_held_hop;
static uint16_t ip_id;

static struct { size_t len; uint8_t pkt[NET_MTU]; } lo_q[LO_SLOTS];     /* loopback packets in flight */
static unsigned lo_head, lo_tail;                                      /* free-running counters */
static struct ktimer lo_timer, proto_timer;

bool net_is_loopback(uint32_t ip) { return (ip >> 24) == 127; }

static bool is_local(uint32_t ip) { return net_is_loopback(ip) || (net_if.ip && ip == net_if.ip); }

uint32_t net_src_for(uint32_t dst) { return net_is_loopback(dst) ? dst : net_if.ip; }

/* ---- checksums ------------------------------------------------------------ */

uint32_t net_csum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = data;
    for (; len > 1; len -= 2, p += 2) sum += (uint32_t)p[0] << 8 | p[1];
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t net_csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint16_t net_l4_csum(uint32_t src, uint32_t dst, uint8_t proto, const void *seg, size_t len) {
    uint32_t sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
    return net_csum_fold(net_csum_add(sum, seg, len));
}

/* ---- ARP ------------------------------------------------------------------ */

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    uint32_t now = net_now();
    int slot = -1;

    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) { slot = i; break; }
        if (slot < 0 && (!arp_cache[i].used || (int32_t)(now - arp_cache[i].expires) >= 0)) slot = i;
    }
    if (slot < 0) slot = (int)(now % ARP_SLOTS);        /* all fresh: evict an arbitrary entry */
    arp_cache[slot].used = true;
    arp_cache[slot].ip = ip;
    arp_cache[slot].expires = now + ARP_TTL_MS;
    memcpy(arp_cache[slot].mac, mac, 6);
}

static const uint8_t *arp_lookup(uint32_t ip) {
    uint32_t now = net_now();
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp_cache[i].used && arp_cache[i].ip == ip && (int32_t)(now - arp_cache[i].expires) < 0)
            return arp_cache[i].mac;
    return NULL;
}

static void eth_send(const uint8_t *dst_mac, uint16_t type, uint8_t *frame, size_t payload_len) {
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst_mac, 6);
    memcpy(e->src, net_if.mac, 6);
    e->type = htons(type);
    e1000_send(frame, ETH_HLEN + payload_len);
}

static void arp_send(uint16_t op, const uint8_t *tmac, uint32_t tip) {
    uint8_t frame[ETH_HLEN + sizeof(struct arp_pkt)];
    struct arp_pkt *a = (struct arp_pkt *)(frame + ETH_HLEN);

    a->hw = htons(1);
    a->proto = htons(ETH_IP);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(op);
    memcpy(a->smac, net_if.mac, 6);
    a->sip = htonl(net_if.ip);
    memcpy(a->tmac, op == ARP_REQUEST ? NO_MAC : tmac, 6);
    a->tip = htonl(tip);
    eth_send(op == ARP_REQUEST ? BCAST : tmac, ETH_ARP, frame, sizeof(*a));
}

static void arp_input(const uint8_t *pkt, size_t len) {
    const struct arp_pkt *a = (const struct arp_pkt *)pkt;
    if (len < sizeof(*a) || a->hlen != 6 || a->plen != 4 || a->proto != htons(ETH_IP)) return;

    uint32_t sip = ntohl(a->sip);
    if (sip) arp_learn(sip, a->smac);
    if (ntohs(a->op) == ARP_REQUEST && net_if.ip && ntohl(a->tip) == net_if.ip)
        arp_send(ARP_REPLY, a->smac, sip);
    if (arp_held_len && arp_held_hop == sip) {          /* the frame we were holding can go now */
        size_t n = arp_held_len;
        arp_held_len = 0;
        memcpy(((struct eth_hdr *)arp_held)->dst, a->smac, 6);
        e1000_send(arp_held, n);
    }
}

/* ---- IPv4 ----------------------------------------------------------------- */

uint8_t *net_tx_payload(void) { return tx_frame + ETH_HLEN + IP_HLEN; }

int net_ip_send(uint8_t proto, uint32_t src, uint32_t dst, size_t len) {
    struct ip_hdr *ip = (struct ip_hdr *)(tx_frame + ETH_HLEN);
    const uint8_t *mac;
    uint32_t hop = dst;

    if (!net_if.present && !is_local(dst)) return -ENETDOWN;
    if (dst != IP4_BROADCAST && !is_local(dst)) {
        if (!net_if.ip) return -ENETUNREACH;
        if ((dst & net_if.mask) != (net_if.ip & net_if.mask)) hop = net_if.gw;
        if (!hop) return -ENETUNREACH;
    }
    ip->vihl = 0x45;
    ip->tos = 0;
    ip->len = htons((uint16_t)(IP_HLEN + len));
    ip->id = htons(ip_id++);
    ip->frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    if (!src) src = net_src_for(dst);
    ip->src = htonl(src);
    ip->dst = htonl(dst);
    ip->csum = net_csum_fold(net_csum_add(0, ip, IP_HLEN));

    if (is_local(dst)) {
        if (lo_tail - lo_head < LO_SLOTS) {
            lo_q[lo_tail % LO_SLOTS].len = IP_HLEN + len;
            memcpy(lo_q[lo_tail % LO_SLOTS].pkt, ip, IP_HLEN + len);
            lo_tail++;
            timer_add_earlier(&lo_timer, 0);
        }
        return 0;
    }
    memcpy(((struct eth_hdr *)tx_frame)->src, net_if.mac, 6);      /* complete the header a held frame keeps */
    ((struct eth_hdr *)tx_frame)->type = htons(ETH_IP);

    mac = dst == IP4_BROADCAST ? BCAST : arp_lookup(hop);
    if (mac) {
        eth_send(mac, ETH_IP, tx_frame, IP_HLEN + len);
        return 0;
    }
    /* Next hop unknown: hold this frame, ask, and let the sender's retransmit cover a lost request. */
    memcpy(arp_held, tx_frame, ETH_HLEN + IP_HLEN + len);
    arp_held_len = ETH_HLEN + IP_HLEN + len;
    arp_held_hop = hop;
    arp_send(ARP_REQUEST, NULL, hop);
    return 0;
}

static void icmp_input(const uint8_t *ip_packet, size_t ip_len, uint32_t src, size_t hl) {
    const struct icmp_hdr *ic = (const struct icmp_hdr *)(ip_packet + hl);
    size_t len = ip_len - hl;

    if (len < sizeof(*ic) || net_csum_fold(net_csum_add(0, ic, len)) != 0) return;
    if (ic->type == 8 && ic->code == 0 && (net_if.ip || net_is_loopback(src))) {          /* echo request: answer from our address */
        uint8_t *out = net_tx_payload();
        memcpy(out, ic, len);
        ((struct icmp_hdr *)out)->type = 0;
        ((struct icmp_hdr *)out)->csum = 0;
        ((struct icmp_hdr *)out)->csum = net_csum_fold(net_csum_add(0, out, len));
        net_ip_send(IPPROTO_ICMP, 0, src, len);
    }
    if (ic->type == ICMP_UNREACH && ic->code == ICMP_PORT_UNREACH) icmp_port_unreachable((const uint8_t *)(ic + 1) + 4, len > sizeof(*ic) + 4 ? len - sizeof(*ic) - 4 : 0);
    icmp_deliver(ip_packet, ip_len);
}

/* Answers a datagram for a port nobody listens on: the ICMP error carries the offending IP header and 8 bytes. */
static void icmp_send_port_unreachable(const uint8_t *pkt, size_t total, size_t hl) {
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    uint8_t *out = net_tx_payload();
    struct icmp_hdr *ic = (struct icmp_hdr *)out;
    size_t quoted = total < hl + 8 ? total : hl + 8;

    ic->type = ICMP_UNREACH;
    ic->code = ICMP_PORT_UNREACH;
    ic->csum = 0;
    memset(out + sizeof(*ic), 0, 4);
    memcpy(out + sizeof(*ic) + 4, pkt, quoted);
    ic->csum = net_csum_fold(net_csum_add(0, out, sizeof(*ic) + 4 + quoted));
    net_ip_send(IPPROTO_ICMP, ntohl(ip->dst), ntohl(ip->src), sizeof(*ic) + 4 + quoted);
}

static void ip_input(const uint8_t *pkt, size_t len) {
    const struct ip_hdr *ip = (const struct ip_hdr *)pkt;
    size_t hl = (ip->vihl & 0xF) * 4u, total = ntohs(ip->len);
    uint32_t src = ntohl(ip->src), dst = ntohl(ip->dst);

    if (len < IP_HLEN || (ip->vihl >> 4) != 4 || hl < IP_HLEN || total < hl || total > len || total > NET_MTU) return;
    if (net_csum_fold(net_csum_add(0, pkt, hl)) != 0) return;
    if (ntohs(ip->frag) & 0x3FFF) return;               /* fragments are not reassembled */
    bool unicast = net_is_loopback(dst) || (net_if.ip && dst == net_if.ip);
    if (net_if.ip && !unicast && dst != IP4_BROADCAST && dst != (net_if.ip | ~net_if.mask)) return;

    switch (ip->proto) {
    case IPPROTO_ICMP: icmp_input(pkt, total, src, hl); break;
    case IPPROTO_UDP:
        if (!udp_input(src, dst, pkt + hl, total - hl) && unicast) icmp_send_port_unreachable(pkt, total, hl);
        break;
    case IPPROTO_TCP:  if (net_if.ip || net_is_loopback(dst)) tcp_input(src, dst, pkt + hl, total - hl); break;
    }
}

void net_rx_frame(const uint8_t *frame, size_t len) {
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    if (len < ETH_HLEN) return;
    switch (ntohs(e->type)) {
    case ETH_ARP: arp_input(frame + ETH_HLEN, len - ETH_HLEN); break;
    case ETH_IP:  ip_input(frame + ETH_HLEN, len - ETH_HLEN); break;
    }
}

/* ---- init and timers -------------------------------------------------------- */

/* Delivers the loopback packets queued so far; replies queued meanwhile wait for the next millisecond. */
static void lo_deliver(struct ktimer *t) {
    (void)t;
    for (unsigned n = lo_tail - lo_head; n; n--) {
        ip_input(lo_q[lo_head % LO_SLOTS].pkt, lo_q[lo_head % LO_SLOTS].len);
        lo_head++;
    }
}

#define NIC_POLL_MS 10u

static void proto_tick(struct ktimer *t) {
    (void)t;
    if (net_if.present && e1000_polled()) {
        e1000_poll();
        net_timer_kick(NIC_POLL_MS);
    }
    uint32_t dhcp = dhcp_tick(), tcp = tcp_tick();
    if (dhcp) net_timer_kick(dhcp);
    if (tcp) net_timer_kick(tcp);
}

void net_timer_kick(uint32_t ms) {
    timer_add_earlier(&proto_timer, ms);
}

void net_init(void) {
    lo_timer.fn = lo_deliver;           /* loopback works without a NIC */
    proto_timer.fn = proto_tick;
    if (!e1000_init(net_if.mac)) {
        LOG_WARN("no network adapter found");
        return;
    }
    net_if.present = true;
    if (e1000_polled()) net_timer_kick(NIC_POLL_MS);
    LOG_INFO("e1000 %02x:%02x:%02x:%02x:%02x:%02x", net_if.mac[0], net_if.mac[1], net_if.mac[2],
             net_if.mac[3], net_if.mac[4], net_if.mac[5]);
    dhcp_start();
}
