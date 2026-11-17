/**
 * @file udp.c
 * @brief UDP: checksums, delivery to bound datagram sockets and transmission.
 */
#include <kernel/net/net.h>
#include <kernel/net/socket.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/lib/string.h>

#define DHCP_CLIENT_PORT 68

struct udp_hdr { uint16_t sport, dport, len, csum; } __attribute__((packed));

bool udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    const struct udp_hdr *u = (const struct udp_hdr *)seg;
    size_t ulen;

    if (len < sizeof(*u)) return true;
    ulen = ntohs(u->len);
    if (ulen < sizeof(*u) || ulen > len) return true;
    if (u->csum && net_l4_csum(src, dst, IPPROTO_UDP, seg, ulen) != 0) return true;

    uint16_t dport = ntohs(u->dport);
    const uint8_t *data = seg + sizeof(*u);
    size_t dlen = ulen - sizeof(*u);

    if (dport == DHCP_CLIENT_PORT) {
        dhcp_input(data, dlen);
        return true;
    }
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        struct sock *s = net_socks[i];
        if (s && s->type == SOCK_DGRAM && s->lport == dport && (!s->laddr || s->laddr == dst)) {
            sock_queue_dgram(s, src, ntohs(u->sport), data, dlen);
            return true;
        }
    }
    return false;
}

int udp_send(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    struct udp_hdr *u = (struct udp_hdr *)net_tx_payload();
    size_t total = sizeof(*u) + len;

    if (total > IP_PAYLOAD_MAX) return -EMSGSIZE;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)total);
    u->csum = 0;
    memcpy(u + 1, data, len);
    if (!src) src = net_src_for(dst);
    uint16_t c = net_l4_csum(src, dst, IPPROTO_UDP, u, total);
    u->csum = c ? c : 0xFFFF;                   /* zero means "no checksum" on the wire */
    return net_ip_send(IPPROTO_UDP, src, dst, total);
}
