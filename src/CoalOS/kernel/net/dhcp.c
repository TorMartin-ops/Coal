/**
 * @file dhcp.c
 * @brief DHCP client: DISCOVER, OFFER, REQUEST, ACK at boot, a static fallback, and
 *        lease renewal (RFC 2131: unicast REQUEST at T1, broadcast at T2).
 *
 * Driven by dhcp_tick() and the UDP input path, so it never blocks. When the
 * lease runs out the address is dropped and discovery starts over.
 */
#define LOG_MODULE "dhcp"

#include <kernel/net/net.h>
#include <kernel/core/log.h>
#include <kernel/lib/string.h>

#define SERVER_PORT  67
#define CLIENT_PORT  68
#define MAGIC        0x63825363u
#define RETRY_MS     400u               /* QEMU's e1000 takes ~0.5 s to bring the link up */
#define MAX_TRIES    12

/* QEMU user networking defaults, used when no server answers. */
#define FALLBACK_IP   IP4(10, 0, 2, 15)
#define FALLBACK_MASK IP4(255, 255, 255, 0)
#define FALLBACK_GW   IP4(10, 0, 2, 2)
#define FALLBACK_DNS  IP4(10, 0, 2, 3)

enum { OPT_MASK = 1, OPT_ROUTER = 3, OPT_DNS = 6, OPT_REQ_IP = 50, OPT_LEASE = 51, OPT_TYPE = 53,
       OPT_SERVER = 54, OPT_PARAMS = 55, OPT_T1 = 58, OPT_T2 = 59, OPT_END = 255 };
enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, ACK = 5, NAK = 6 };
enum { IDLE, SELECTING, REQUESTING, BOUND, RENEWING, REBINDING };

#define LEASE_MAX_S    (20u * 86400u)   /* keeps lease times in milliseconds inside 32 bits */
#define RENEW_RETRY_MS 10000u

struct bootp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16], sname[64], file[128];
    uint32_t magic;
    uint8_t  options[64];
} __attribute__((packed));

static int      state;
static uint32_t xid, retry_at, offered_ip, server_ip;
static int      tries;
static uint32_t bound_at, t1_ms, t2_ms, lease_ms;   /* lease times, counted from bound_at */

static void send_msg(uint8_t type) {
    struct bootp m;
    uint8_t *o = m.options;

    memset(&m, 0, sizeof(m));
    m.op = 1;
    m.htype = 1;
    m.hlen = 6;
    m.xid = htonl(xid);
    if (!net_if.ip) m.flags = htons(0x8000);    /* ask for a broadcast reply: we have no address yet */
    if (state == RENEWING || state == REBINDING) m.ciaddr = htonl(net_if.ip);
    memcpy(m.chaddr, net_if.mac, 6);
    m.magic = htonl(MAGIC);
    *o++ = OPT_TYPE; *o++ = 1; *o++ = type;
    if (type == REQUEST && state != RENEWING && state != REBINDING) {
        *o++ = OPT_REQ_IP; *o++ = 4; *(uint32_t *)o = htonl(offered_ip); o += 4;
        *o++ = OPT_SERVER; *o++ = 4; *(uint32_t *)o = htonl(server_ip); o += 4;
    }
    *o++ = OPT_PARAMS; *o++ = 3; *o++ = OPT_MASK; *o++ = OPT_ROUTER; *o++ = OPT_DNS;
    *o++ = OPT_END;
    udp_send(net_if.ip, CLIENT_PORT, state == RENEWING ? server_ip : IP4_BROADCAST, SERVER_PORT, &m, sizeof(m));
}

static unsigned prefix_len(uint32_t mask) {
    unsigned n = 0;
    for (; mask & 0x80000000u; mask <<= 1) n++;
    return n;
}

static void configure(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns, uint32_t lease, uint32_t t1, uint32_t t2,
                      const char *how) {
    if (lease > LEASE_MAX_S) lease = LEASE_MAX_S;
    bound_at = net_now();
    lease_ms = lease * 1000u;                   /* 0: static fallback, never renewed */
    t1_ms = (t1 && t1 < lease ? t1 : lease / 2) * 1000u;
    t2_ms = (t2 && t2 < lease && t2 > t1_ms / 1000u ? t2 : lease - lease / 8) * 1000u;
    net_if.ip = ip;
    net_if.mask = mask;
    net_if.gw = gw;
    net_if.dns = dns;
    state = BOUND;
    LOG_INFO("%s: " IP4_FMT "/%u gw " IP4_FMT " dns " IP4_FMT " lease %us", how, IP4_ARGS(ip),
             prefix_len(mask), IP4_ARGS(gw), IP4_ARGS(dns), (unsigned)lease);
}

void dhcp_start(void) {
    xid = net_now() * 2654435761u ^ ((uint32_t)net_if.mac[4] << 8 | net_if.mac[5]);
    state = SELECTING;
    tries = 1;
    retry_at = net_now() + RETRY_MS;
    send_msg(DISCOVER);
}

/* Next retransmission while renewing: half the time left, at least RENEW_RETRY_MS. */
static uint32_t renew_delay(uint32_t until_ms) {
    uint32_t left = until_ms - (net_now() - bound_at), half = (int32_t)left > 0 ? left / 2 : 0;
    return half > RENEW_RETRY_MS ? half : RENEW_RETRY_MS;
}

static void lease_tick(void) {
    uint32_t elapsed = net_now() - bound_at;

    if (!lease_ms) return;
    if (elapsed >= lease_ms) {
        LOG_WARN("lease expired");
        net_if.ip = net_if.mask = net_if.gw = net_if.dns = 0;
        dhcp_start();
    } else if (elapsed >= t2_ms && state != REBINDING) {
        state = REBINDING;
        xid++;
        retry_at = net_now() + renew_delay(lease_ms);
        send_msg(REQUEST);
    } else if (elapsed >= t1_ms && state == BOUND) {
        state = RENEWING;
        xid++;
        retry_at = net_now() + renew_delay(t2_ms);
        send_msg(REQUEST);
    } else if ((state == RENEWING || state == REBINDING) && (int32_t)(net_now() - retry_at) >= 0) {
        retry_at = net_now() + renew_delay(state == RENEWING ? t2_ms : lease_ms);
        send_msg(REQUEST);
    }
}

void dhcp_tick(void) {
    if (state == BOUND || state == RENEWING || state == REBINDING) { lease_tick(); return; }
    if ((state != SELECTING && state != REQUESTING) || (int32_t)(net_now() - retry_at) < 0) return;
    if (tries >= MAX_TRIES) {
        LOG_WARN("no answer, using the static QEMU defaults");
        configure(FALLBACK_IP, FALLBACK_MASK, FALLBACK_GW, FALLBACK_DNS, 0, 0, 0, "static");
        return;
    }
    tries++;
    retry_at = net_now() + RETRY_MS;
    send_msg(state == SELECTING ? DISCOVER : REQUEST);
}

void dhcp_input(const uint8_t *msg, size_t len) {
    const struct bootp *m = (const struct bootp *)msg;
    uint32_t mask = 0, gw = 0, dns = 0, lease = 0, srv = 0, t1 = 0, t2 = 0;
    int type = 0;

    if (len < offsetof(struct bootp, options) || m->op != 2 || ntohl(m->xid) != xid ||
        ntohl(m->magic) != MAGIC || state == IDLE || state == BOUND) return;
    if (memcmp(m->chaddr, net_if.mac, 6) != 0) return;

    for (size_t i = 0; i + 1 < len - offsetof(struct bootp, options);) {
        uint8_t code = m->options[i], n = code == OPT_END ? 0 : m->options[i + 1];
        const uint8_t *v = &m->options[i + 2];
        if (code == OPT_END) break;
        if (code == 0) { i++; continue; }           /* pad */
        if (i + 2 + n > len - offsetof(struct bootp, options)) break;
        switch (code) {
        case OPT_TYPE:   type = v[0]; break;
        case OPT_MASK:   if (n >= 4) mask = ntohl(*(const uint32_t *)v); break;
        case OPT_ROUTER: if (n >= 4) gw = ntohl(*(const uint32_t *)v); break;
        case OPT_DNS:    if (n >= 4) dns = ntohl(*(const uint32_t *)v); break;
        case OPT_LEASE:  if (n >= 4) lease = ntohl(*(const uint32_t *)v); break;
        case OPT_T1:     if (n >= 4) t1 = ntohl(*(const uint32_t *)v); break;
        case OPT_T2:     if (n >= 4) t2 = ntohl(*(const uint32_t *)v); break;
        case OPT_SERVER: if (n >= 4) srv = ntohl(*(const uint32_t *)v); break;
        }
        i += 2u + n;
    }
    if (type == OFFER && state == SELECTING) {
        offered_ip = ntohl(m->yiaddr);
        server_ip = srv;
        state = REQUESTING;
        tries = 1;
        retry_at = net_now() + RETRY_MS;
        send_msg(REQUEST);
    } else if (type == ACK && state != SELECTING) {
        bool renewed = state != REQUESTING;
        configure(ntohl(m->yiaddr), mask ? mask : FALLBACK_MASK, gw, dns, lease, t1, t2, renewed ? "renewed" : "lease");
    } else if (type == NAK && state != SELECTING) {             /* the server withdrew the lease */
        LOG_WARN("lease refused, starting over");
        net_if.ip = net_if.mask = net_if.gw = net_if.dns = 0;
        dhcp_start();
    }
}
