/**
 * @file tcp.c
 * @brief TCP: active and passive open, in-order receive, loss recovery, slow
 *        start and congestion avoidance, FIN/RST.
 *
 * Loss recovery follows RFC 8985 (RACK-TLP) in a form that needs no SACK: every
 * data segment in flight has a record with its transmit time, and a segment is
 * lost once a segment sent after it has been delivered and a reordering window
 * has passed. Duplicate ACKs tell how many later segments arrived, which bounds
 * the newest delivered transmit time. A tail loss probe (about 2 * SRTT after the
 * last transmission) retransmits or sends one segment so that tail losses are
 * found without waiting for the retransmission timeout, which is computed as in
 * RFC 6298.
 *
 * Looped-back segments are delivered right after each entry into the stack
 * (net_lo_drain) and carry no checksum.
 *
 * Deliberately small: out-of-order data is kept in the receive ring (at most
 * OOO_MAX ranges) and merged when the gap fills, there are no window scaling,
 * timestamps or SACK, and every received segment is acknowledged immediately.
 * MSS is negotiated and clamped to [536, MSS_MAX].
 */
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/net/socket.h>
#include <kernel/memory/kmalloc.h>
#include <kernel/cpu/linux_abi.h>
#include <kernel/fs/vfs/fs_errno.h>
#include <kernel/lib/string.h>

#define MAX_CONNS       64
#define OOO_MAX         4                   /* out-of-order ranges kept per connection */
#define BUF_SIZE        16384u              /* per direction; power of two */
#define MSS_MAX         (NET_MTU - IP_HLEN - 20)
#define MSS_DEFAULT     536u
#define SEG_MAX         32                  /* data segments in flight per connection; power of two */
#define RTO_INIT        1000u
#define RTO_MIN         200u
#define RTO_MAX         16000u
#define CLOCK_G         TCP_TICK_MS         /* timer granularity in the RTO formula */
#define PTO_MIN         10u                 /* tail loss probe floor, ms */
#define PTO_ACK_DELAY   40u                 /* receiver's worst-case ACK delay, added when one segment is in flight */
#define MAX_RETRIES     6
#define TIME_WAIT_MS    4000u
#define FIN_WAIT2_MS    30000u

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define SEQ_LT(a, b)  ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  off, flags;
    uint16_t win, csum, urg;
} __attribute__((packed));

/* One data segment in flight. */
struct seg {
    uint32_t seq, sent;                 /* first sequence number, transmit time of the latest copy */
    uint16_t len;
    bool     retx, lost;                /* sent more than once; presumed lost and not yet resent */
};

enum timer_kind { T_RTO, T_PTO, T_REO };

struct tcp_conn {
    bool     used;
    uint8_t  state;
    bool     fin_queued, fin_sent, fin_rcvd, ack_now, rtx_on;
    bool     in_recovery, tlp_done, rack_valid, reo_on;
    uint8_t  retries, dupacks, rtx_kind;
    int      err;
    struct sock *sock;                  /* owner; NULL once orphaned */
    struct sock *parent;                /* listener until accepted */
    uint32_t rip, laddr;                /* peer and local address (loopback connections use 127.x for both) */
    uint16_t lport, rport, mss;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint32_t cwnd, ssthresh, adv_wnd;
    uint32_t rto, srtt, rttvar, min_rtt, rtx_at, born;
    uint32_t recover;                   /* snd_nxt when the current loss recovery began */
    uint32_t rack_ts, rack_end, rack_rtt, reo_due;   /* newest delivered segment: transmit time, end; RTT of the latest sample */
    struct seg *segs;                   /* ring of SEG_MAX records, oldest first */
    uint8_t  seg_head, seg_n;
    struct { uint32_t seq, len; } ooo[OOO_MAX];  /* ranges past rcv_nxt, already stored in rx */
    uint8_t  ooo_n;
    uint8_t *tx, *rx;
    uint32_t tx_start, tx_len, rx_start, rx_len;
};

static struct tcp_conn conns[MAX_CONNS];
static uint32_t iss_counter;

/* ---- helpers ---------------------------------------------------------------- */

static void ring_put(uint8_t *ring, uint32_t pos, const uint8_t *src, size_t n) {
    uint32_t i = pos & (BUF_SIZE - 1), first = BUF_SIZE - i < n ? BUF_SIZE - i : (uint32_t)n;
    memcpy(ring + i, src, first);
    memcpy(ring, src + first, n - first);
}

static void ring_get(const uint8_t *ring, uint32_t pos, uint8_t *dst, size_t n) {
    uint32_t i = pos & (BUF_SIZE - 1), first = BUF_SIZE - i < n ? BUF_SIZE - i : (uint32_t)n;
    memcpy(dst, ring + i, first);
    memcpy(dst + first, ring, n - first);
}

static bool orphan(const struct tcp_conn *c) { return !c->sock && !c->parent; }

static void wake_owner(const struct tcp_conn *c) {
    if (c->sock) sock_wake(c->sock);
    if (c->parent) sock_wake(c->parent);
}

static void release(struct tcp_conn *c) {
    kfree(c->tx);
    kfree(c->rx);
    kfree(c->segs);
    memset(c, 0, sizeof(*c));
}

static bool queued(const struct tcp_conn *c) {
    for (unsigned i = 0; i < c->parent->acc_n; i++)
        if (c->parent->accq[i] == c) return true;
    return false;
}

/* The connection is over: wake the owner and free it unless a socket still reads its state. */
static void finish(struct tcp_conn *c, int err) {
    c->state = TCP_CLOSED;
    c->rtx_on = false;
    if (err && !c->err) c->err = err;
    wake_owner(c);
    if (orphan(c) || (c->parent && !queued(c))) release(c);      /* nobody can reach it any more */
}

static void arm(struct tcp_conn *c, uint32_t ms) {
    c->rtx_on = true;
    c->rtx_kind = T_RTO;
    c->rtx_at = net_now() + ms;
}

static struct seg *seg_at(const struct tcp_conn *c, unsigned i) { return &c->segs[(c->seg_head + i) % SEG_MAX]; }

static uint32_t window(const struct tcp_conn *c) { return BUF_SIZE - c->rx_len; }

/* Builds and sends one segment. Data comes from the send ring at offset off past snd_una. */
static int emit(const struct tcp_conn *c, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                uint32_t seq, uint32_t ack, uint8_t flags, uint32_t win, uint32_t off, size_t dlen) {
    uint8_t *p = net_tx_payload();
    struct tcp_hdr *h = (struct tcp_hdr *)p;
    size_t hl = sizeof(*h);

    h->sport = htons(sport);
    h->dport = htons(dport);
    h->seq = htonl(seq);
    h->ack = htonl(ack);
    h->flags = flags;
    h->win = htons((uint16_t)win);
    h->csum = 0;
    h->urg = 0;
    if (flags & SYN) {                                  /* MSS option */
        p[hl++] = 2; p[hl++] = 4; p[hl++] = MSS_MAX >> 8; p[hl++] = MSS_MAX & 0xFF;
    }
    h->off = (uint8_t)(hl / 4 << 4);
    if (dlen) ring_get(c->tx, c->tx_start + off, p + hl, dlen);
    h->csum = net_is_local(dst) ? 0 : net_l4_csum(src, dst, IPPROTO_TCP, p, hl + dlen);
    return net_ip_send(IPPROTO_TCP, src, dst, hl + dlen);
}

static int send_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq, uint32_t off, size_t dlen) {
    c->adv_wnd = window(c);
    c->ack_now = false;
    return emit(c, c->laddr, c->rip, c->lport, c->rport, seq, (flags & ACK) ? c->rcv_nxt : 0, flags, c->adv_wnd, off, dlen);
}

/* Reset in reply to a segment that belongs to no connection. */
static void send_reset(uint32_t local, uint32_t src, const struct tcp_hdr *h, size_t seg_len) {
    uint32_t seq = ntohl(h->seq);
    if (h->flags & ACK)
        emit(NULL, local, src, ntohs(h->dport), ntohs(h->sport), ntohl(h->ack), 0, RST, 0, 0, 0);
    else
        emit(NULL, local, src, ntohs(h->dport), ntohs(h->sport), 0, seq + (uint32_t)seg_len, RST | ACK, 0, 0, 0);
}

/* ---- output ------------------------------------------------------------------ */

static uint32_t in_flight(const struct tcp_conn *c) {
    return c->snd_nxt - c->snd_una - (c->fin_sent ? 1u : 0u);
}

/* Bytes the network is believed to hold: everything in flight except segments presumed lost. */
static uint32_t pipe(const struct tcp_conn *c) {
    uint32_t n = 0;
    for (unsigned i = 0; i < c->seg_n; i++)
        if (!seg_at(c, i)->lost) n += seg_at(c, i)->len;
    return n;
}

/* Sends n bytes of new data from the send ring and records the segment. */
static bool send_new(struct tcp_conn *c, uint32_t n) {
    if (c->seg_n == SEG_MAX || send_seg(c, ACK | PSH, c->snd_nxt, in_flight(c), n) < 0) return false;
    struct seg *s = seg_at(c, c->seg_n++);
    s->seq = c->snd_nxt;
    s->len = (uint16_t)n;
    s->sent = net_now();
    s->retx = s->lost = false;
    c->snd_nxt += n;
    return true;
}

/* Resends segments presumed lost while they fit in the congestion window; force sends the first regardless. */
static bool send_lost(struct tcp_conn *c, bool force) {
    uint32_t p = pipe(c);
    bool sent = false;

    for (unsigned i = 0; i < c->seg_n; i++) {
        struct seg *s = seg_at(c, i);
        if (!s->lost) continue;
        if (!(force && !sent) && p + s->len > c->cwnd) break;
        if (send_seg(c, ACK | PSH, s->seq, s->seq - c->snd_una, s->len) < 0) break;
        s->lost = false;
        s->retx = true;
        s->sent = net_now();
        p += s->len;
        sent = true;
    }
    return sent;
}

/* Schedules loss detection for the data in flight: a pending reorder wait, else a tail loss
 * probe, else the retransmission timeout. */
static void rearm(struct tcp_conn *c) {
    uint32_t ms = c->rto;
    uint8_t kind = T_RTO;

    if (c->snd_una == c->snd_nxt) {
        c->rtx_on = c->reo_on = false;
        return;
    }
    if (c->reo_on) {
        int32_t wait = (int32_t)(c->reo_due - net_now());
        if (wait <= 0) ms = 0;
        else if ((uint32_t)wait < ms) ms = (uint32_t)wait;
        kind = T_REO;
    } else if (c->srtt && c->seg_n && !c->tlp_done && !c->in_recovery && !c->fin_sent) {
        uint32_t pto = 2 * c->srtt + (c->seg_n == 1 ? PTO_ACK_DELAY : 0);
        if (pto < PTO_MIN) pto = PTO_MIN;
        if (pto < ms) { ms = pto; kind = T_PTO; }
    }
    arm(c, ms);
    c->rtx_kind = kind;
}

static void output(struct tcp_conn *c) {
    bool sent = false, fresh = false;
    bool open = c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT;

    if (c->seg_n && send_lost(c, false)) sent = true;
    if (open && !c->fin_sent) {
        uint32_t flight = in_flight(c), p = pipe(c);
        for (;;) {
            uint32_t room_c = c->cwnd > p ? c->cwnd - p : 0, room_w = c->snd_wnd > flight ? c->snd_wnd - flight : 0;
            uint32_t room = room_c < room_w ? room_c : room_w, unsent = c->tx_len - flight;
            uint32_t n = unsent < room ? unsent : room;
            if (n > c->mss) n = c->mss;
            if (!n || !send_new(c, n)) break;
            flight += n;
            p += n;
            sent = fresh = true;
        }
        if (c->fin_queued && c->tx_len == flight && send_seg(c, FIN | ACK, c->snd_nxt, flight, 0) >= 0) {
            c->snd_nxt++;
            c->fin_sent = true;
            c->state = c->state == TCP_ESTABLISHED ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
            sent = fresh = true;
        }
        if (fresh || (sent && !c->rtx_on)) rearm(c);
        if (!sent && !c->rtx_on && c->snd_wnd == 0 && c->tx_len > flight) arm(c, c->rto);   /* zero-window probe */
    }
    if (!sent && c->ack_now) send_seg(c, ACK, c->snd_nxt, 0, 0);
}

static void retransmit(struct tcp_conn *c) {
    switch (c->state) {
    case TCP_SYN_SENT: send_seg(c, SYN, c->iss, 0, 0); return;
    case TCP_SYN_RCVD: send_seg(c, SYN | ACK, c->iss, 0, 0); return;
    default: break;
    }
    if (c->seg_n) {
        send_lost(c, true);
    } else if (c->fin_sent && c->snd_una != c->snd_nxt) {
        send_seg(c, FIN | ACK, c->snd_nxt - 1, 0, 0);
    } else if (c->tx_len > in_flight(c)) {              /* window probe: one byte beyond the window */
        send_new(c, 1);
    }
}

/* Tail loss probe (RFC 8985 section 7): new data if the window allows, else the newest segment again. */
static void probe(struct tcp_conn *c) {
    uint32_t flight = in_flight(c), unsent = c->tx_len - flight;
    uint32_t room = c->snd_wnd > flight ? c->snd_wnd - flight : 0, n = unsent < room ? unsent : room;

    c->tlp_done = true;
    if (n > c->mss) n = c->mss;
    if (n && send_new(c, n)) return;
    struct seg *s = seg_at(c, c->seg_n - 1);
    if (send_seg(c, ACK | PSH, s->seq, s->seq - c->snd_una, s->len) >= 0) {
        s->retx = true;
        s->sent = net_now();
    }
}

/* ---- loss detection (RFC 8985) ----------------------------------------------- */

/* A segment was delivered: remember the newest transmit time seen delivered. */
static void rack_update(struct tcp_conn *c, uint32_t sent, uint32_t end) {
    int32_t d = (int32_t)(sent - c->rack_ts);
    if (!c->rack_valid || d > 0 || (d == 0 && SEQ_LT(c->rack_end, end))) {
        c->rack_valid = true;
        c->rack_ts = sent;
        c->rack_end = end;
    }
}

/* Marks segments lost that were sent before the newest delivered one and had a round trip plus
 * the reordering window (a quarter of the minimum RTT) to arrive. Sets the reorder timer for
 * those still inside it. Returns true if a segment was newly marked. */
static bool rack_detect(struct tcp_conn *c) {
    uint32_t now = net_now(), reo = c->min_rtt / 4 ? c->min_rtt / 4 : 1;
    bool lost = false;

    c->reo_on = false;
    if (!c->rack_valid) return false;
    for (unsigned i = 0; i < c->seg_n; i++) {
        struct seg *s = seg_at(c, i);
        int32_t older = (int32_t)(c->rack_ts - s->sent);
        if (s->lost || !(older > 0 || (older == 0 && SEQ_LT(s->seq + s->len, c->rack_end)))) continue;
        uint32_t due = s->sent + c->rack_rtt + reo;
        if ((int32_t)(now - due) >= 0) {
            s->lost = lost = true;
        } else if (!c->reo_on || (int32_t)(due - c->reo_due) < 0) {
            c->reo_on = true;
            c->reo_due = due;
        }
    }
    return lost;
}

/* Loss found: halve the window once per window of data (RFC 5681 equation 4) and resend. */
static void enter_recovery(struct tcp_conn *c) {
    if (!c->in_recovery) {
        uint32_t half = in_flight(c) / 2;
        c->ssthresh = half > 2u * c->mss ? half : 2u * c->mss;
        c->cwnd = c->ssthresh;
        c->recover = c->snd_nxt;
        c->in_recovery = true;
    }
    send_lost(c, true);
}

static void timeout(struct tcp_conn *c) {
    if (c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT2) {
        finish(c, 0);
        return;
    }
    if (c->rtx_kind == T_REO) {
        if (rack_detect(c)) enter_recovery(c);
        output(c);
        rearm(c);
        return;
    }
    if (c->rtx_kind == T_PTO && c->seg_n) {
        probe(c);
        arm(c, c->rto);
        return;
    }
    bool probing = c->snd_wnd == 0 && c->state != TCP_SYN_SENT && c->state != TCP_SYN_RCVD;
    c->reo_on = false;
    if (!probing && ++c->retries > MAX_RETRIES) {
        finish(c, ETIMEDOUT);
        return;
    }
    if (probing) {
        if (c->seg_n) seg_at(c, 0)->lost = true;        /* the probe is the oldest segment again */
    } else {
        if (!c->in_recovery) {
            uint32_t half = in_flight(c) / 2;
            c->ssthresh = half > 2u * c->mss ? half : 2u * c->mss;
        }
        c->cwnd = c->mss;
        c->dupacks = 0;
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        c->in_recovery = true;
        c->recover = c->snd_nxt;
        for (unsigned i = 0; i < c->seg_n; i++) seg_at(c, i)->lost = true;
    }
    retransmit(c);
    arm(c, c->rto);
}

void tcp_tick(void) {
    uint32_t now = net_now();

    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->used && c->rtx_on && (int32_t)(now - c->rtx_at) >= 0) timeout(c);
    }
    net_lo_drain();
}

/* ---- input ------------------------------------------------------------------- */

static uint16_t peer_mss(const uint8_t *opts, size_t len) {
    for (size_t i = 0; i < len;) {
        if (opts[i] == 0) break;
        if (opts[i] == 1) { i++; continue; }
        if (i + 1 >= len || opts[i + 1] < 2 || i + opts[i + 1] > len) break;
        if (opts[i] == 2 && opts[i + 1] == 4) {
            uint16_t m = (uint16_t)(opts[i + 2] << 8 | opts[i + 3]);
            return m < MSS_DEFAULT ? MSS_DEFAULT : m < MSS_MAX ? m : MSS_MAX;
        }
        i += opts[i + 1];
    }
    return MSS_DEFAULT;
}

/* Records [seq, seq+len) as stored past rcv_nxt, merging it with touching ranges. */
static void ooo_add(struct tcp_conn *c, uint32_t seq, uint32_t len) {
    uint32_t end = seq + len;

    for (unsigned i = 0; i < c->ooo_n;) {
        uint32_t os = c->ooo[i].seq, oe = os + c->ooo[i].len;
        if (SEQ_LEQ(os, end) && SEQ_LEQ(seq, oe)) {
            if (SEQ_LT(os, seq)) seq = os;
            if (SEQ_LT(end, oe)) end = oe;
            c->ooo[i] = c->ooo[--c->ooo_n];
            i = 0;
        } else {
            i++;
        }
    }
    if (c->ooo_n < OOO_MAX) {
        c->ooo[c->ooo_n].seq = seq;
        c->ooo[c->ooo_n++].len = end - seq;
    }
}

/* Moves ranges that rcv_nxt has reached into the readable part of the ring. */
static void ooo_drain(struct tcp_conn *c) {
    for (unsigned i = 0; i < c->ooo_n;) {
        uint32_t os = c->ooo[i].seq, oe = os + c->ooo[i].len;
        if (!SEQ_LEQ(os, c->rcv_nxt)) { i++; continue; }
        if (SEQ_LT(c->rcv_nxt, oe)) {
            c->rx_len += oe - c->rcv_nxt;
            c->rcv_nxt = oe;
        }
        c->ooo[i] = c->ooo[--c->ooo_n];
        i = 0;
    }
}

/* RFC 6298: smoothed RTT, variance and the retransmission timeout from one measurement (ms). */
static void rtt_update(struct tcp_conn *c, uint32_t m) {
    if (!m) m = 1;
    if (!c->min_rtt || m < c->min_rtt) c->min_rtt = m;
    c->rack_rtt = m;
    if (!c->srtt) {
        c->srtt = m;
        c->rttvar = m / 2;
    } else {
        uint32_t d = c->srtt > m ? c->srtt - m : m - c->srtt;
        c->rttvar = (3 * c->rttvar + d) / 4;
        c->srtt = (7 * c->srtt + m) / 8;
    }
    c->rto = c->srtt + (4 * c->rttvar > CLOCK_G ? 4 * c->rttvar : CLOCK_G);
    if (c->rto < RTO_MIN) c->rto = RTO_MIN;
    if (c->rto > RTO_MAX) c->rto = RTO_MAX;
}

/* Drops the records ack covers. The RTT comes from the newest segment that was sent only once (Karn);
 * every delivered segment, resent ones only if the ACK cannot belong to the earlier copy, feeds RACK. */
static void seg_ack(struct tcp_conn *c, uint32_t ack) {
    uint32_t now = net_now(), sample = 0;
    bool have = false;

    while (c->seg_n) {
        struct seg *s = seg_at(c, 0);
        uint32_t end = s->seq + s->len;
        if (SEQ_LT(ack, end)) {
            if (SEQ_LT(s->seq, ack)) { s->len = (uint16_t)(end - ack); s->seq = ack; }
            break;
        }
        if (!s->retx) { sample = now - s->sent; have = true; }
        if (!s->retx || now - s->sent >= c->min_rtt) rack_update(c, s->sent, end);
        c->seg_head = (uint8_t)((c->seg_head + 1) % SEG_MAX);
        c->seg_n--;
    }
    if (have) rtt_update(c, sample);
}

static void established(struct tcp_conn *c) {
    c->state = TCP_ESTABLISHED;
    c->retries = 0;
    c->rtx_on = false;
    c->in_recovery = false;
    c->cwnd = 4u * c->mss;
    c->ssthresh = 65535;
    if (c->parent && c->parent->acc_n < BACKLOG_MAX) c->parent->accq[c->parent->acc_n++] = c;
    wake_owner(c);
}

/* Handles the ACK field of a segment on a synchronized connection. Returns false if the segment is unusable. */
static bool process_ack(struct tcp_conn *c, uint32_t ack, uint16_t win, size_t dlen, uint8_t flags) {
    if (SEQ_LT(c->snd_nxt, ack)) { c->ack_now = true; output(c); return false; }   /* acks something we never sent */

    if (SEQ_LT(c->snd_una, ack)) {
        uint32_t acked = ack - c->snd_una;
        bool fin_acked = c->fin_sent && ack == c->snd_nxt;
        uint32_t data = acked - (fin_acked ? 1u : 0u);

        if (data > c->tx_len) data = c->tx_len;
        c->tx_start += data;
        c->tx_len -= data;
        c->snd_una = ack;
        c->retries = 0;
        c->dupacks = 0;
        c->tlp_done = false;
        seg_ack(c, ack);
        if (c->in_recovery && SEQ_LEQ(c->recover, ack)) c->in_recovery = false;
        if (c->cwnd < c->ssthresh) c->cwnd += acked < 2u * c->mss ? acked : 2u * c->mss;      /* RFC 3465, L=2 */
        else if (!c->in_recovery) c->cwnd += c->mss * c->mss / (c->cwnd ? c->cwnd : c->mss) + 1;
        if (c->cwnd < c->mss) c->cwnd = c->mss;
        if (c->cwnd > 2 * BUF_SIZE) c->cwnd = 2 * BUF_SIZE;
        if (rack_detect(c)) enter_recovery(c);
        rearm(c);
        if (fin_acked) {
            if (c->state == TCP_FIN_WAIT1) {
                c->state = TCP_FIN_WAIT2;
                if (orphan(c)) arm(c, FIN_WAIT2_MS);
            } else if (c->state == TCP_CLOSING) {
                c->state = TCP_TIME_WAIT;
                arm(c, TIME_WAIT_MS);
            } else if (c->state == TCP_LAST_ACK) {
                finish(c, 0);
                return false;
            }
        }
        wake_owner(c);
    } else if (ack == c->snd_una && !dlen && !(flags & (SYN | FIN)) && c->seg_n && win == c->snd_wnd) {
        /* Each duplicate ACK means one more segment arrived beyond the hole. */
        struct seg *first = seg_at(c, 0);
        if (c->dupacks < 255) c->dupacks++;
        if (c->dupacks < c->seg_n && !seg_at(c, c->dupacks)->retx)
            rack_update(c, seg_at(c, c->dupacks)->sent, seg_at(c, c->dupacks)->seq + seg_at(c, c->dupacks)->len);
        bool lost = rack_detect(c);
        if (c->dupacks == 3 && !first->retx && !first->lost) first->lost = lost = true;   /* classic fast retransmit */
        if (lost) enter_recovery(c);
        if (lost || c->reo_on) rearm(c);
    }
    c->snd_wnd = win;
    return true;
}

static void new_connection(struct sock *l, uint32_t src, uint32_t dst, const struct tcp_hdr *h, const uint8_t *opts, size_t olen) {
    unsigned pending = l->acc_n;
    struct tcp_conn *c = NULL;

    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].parent == l && conns[i].state == TCP_SYN_RCVD) pending++;
    if (pending >= l->backlog) {                        /* SYN flood: make room by dropping the oldest half-open */
        struct tcp_conn *old = NULL;
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i].used && conns[i].parent == l && conns[i].state == TCP_SYN_RCVD &&
                (!old || SEQ_LT(conns[i].born, old->born))) old = &conns[i];
        if (!old) return;
        release(old);
    }
    c = tcp_open(NULL);
    if (!c) return;
    c->parent = l;
    c->born = net_now();
    c->state = TCP_SYN_RCVD;
    c->rip = src;
    c->laddr = dst;
    c->lport = ntohs(h->dport);
    c->rport = ntohs(h->sport);
    c->mss = peer_mss(opts, olen);
    c->rcv_nxt = ntohl(h->seq) + 1;
    c->snd_wnd = ntohs(h->win);
    c->iss = net_now() * 4000u + (iss_counter += 64000u);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    send_seg(c, SYN | ACK, c->iss, 0, 0);
    arm(c, c->rto);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    const struct tcp_hdr *h = (const struct tcp_hdr *)seg;
    size_t hl;
    struct tcp_conn *c = NULL;

    if (len < sizeof(*h) || (!net_rx_trusted && net_l4_csum(src, dst, IPPROTO_TCP, seg, len) != 0)) return;
    hl = (size_t)(h->off >> 4) * 4;
    if (hl < sizeof(*h) || hl > len) return;

    const uint8_t *data = seg + hl;
    size_t dlen = len - hl;
    uint8_t flags = h->flags;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport), win = ntohs(h->win);
    size_t seg_len = dlen + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0);

    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].state != TCP_CLOSED && conns[i].lport == dport &&
            conns[i].rport == sport && conns[i].rip == src) { c = &conns[i]; break; }

    if (!c) {
        struct sock *l = (flags & (SYN | ACK | RST)) == SYN ? sock_find_listener(dport) : NULL;
        if (l) new_connection(l, src, dst, h, seg + sizeof(*h), hl - sizeof(*h));
        else if (!(flags & RST)) send_reset(dst, src, h, seg_len);
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & ACK) && ack != c->iss + 1) {
            if (!(flags & RST)) send_reset(dst, src, h, seg_len);
            return;
        }
        if (flags & RST) {
            if (flags & ACK) finish(c, ECONNREFUSED);
            return;
        }
        if ((flags & (SYN | ACK)) != (SYN | ACK)) return;       /* simultaneous open is not supported */
        c->rcv_nxt = seq + 1;
        c->mss = peer_mss(seg + sizeof(*h), hl - sizeof(*h));
        c->snd_una = ack;
        c->snd_wnd = win;
        if (!c->retries) rtt_update(c, net_now() - c->born);
        established(c);
        c->ack_now = true;
        output(c);
        return;
    }

    if (flags & RST) {
        if (seq == c->rcv_nxt || (SEQ_LEQ(c->rcv_nxt, seq) && SEQ_LT(seq, c->rcv_nxt + window(c))))
            finish(c, c->state == TCP_CLOSE_WAIT ? EPIPE : ECONNRESET);
        return;
    }
    if (flags & SYN) {                                          /* retransmitted SYN of a half-open connection */
        if (c->state == TCP_SYN_RCVD) retransmit(c);
        return;
    }
    if (!(flags & ACK)) return;

    if (c->state == TCP_SYN_RCVD) {
        if (ack != c->iss + 1) { send_reset(dst, src, h, seg_len); return; }
        c->snd_una = ack;
        if (!c->retries) rtt_update(c, net_now() - c->born);
        established(c);
    }
    if (!process_ack(c, ack, win, dlen, flags)) return;
    if (c->state == TCP_CLOSED) return;

    /* Payload and FIN: accept only what continues the stream. */
    if (SEQ_LT(seq, c->rcv_nxt)) {
        uint32_t skip = c->rcv_nxt - seq;
        if (skip >= seg_len) { c->ack_now = true; output(c); return; }      /* duplicate */
        if (skip > dlen) skip = (uint32_t)dlen;
        data += skip;
        dlen -= skip;
    } else if (seq != c->rcv_nxt) {                              /* out of order: keep the data, ask for the gap again */
        uint32_t off = seq - c->rcv_nxt;
        if (dlen && !orphan(c) && off <= window(c) && dlen <= window(c) - off &&
            (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT1 || c->state == TCP_FIN_WAIT2)) {
            ring_put(c->rx, c->rx_start + c->rx_len + off, data, dlen);
            ooo_add(c, seq, (uint32_t)dlen);
        }
        c->ack_now = true;
        output(c);
        return;
    }
    bool fin = (flags & FIN) != 0;
    bool receiving = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT1 || c->state == TCP_FIN_WAIT2;

    if (dlen && orphan(c) && receiving) {                        /* data for a closed socket */
        emit(NULL, dst, src, dport, sport, c->snd_nxt, 0, RST, 0, 0, 0);
        finish(c, ECONNRESET);
        return;
    }
    if (dlen > window(c)) { dlen = window(c); fin = false; }
    if (dlen && receiving) {
        ring_put(c->rx, c->rx_start + c->rx_len, data, dlen);
        c->rx_len += (uint32_t)dlen;
        c->rcv_nxt += (uint32_t)dlen;
        ooo_drain(c);
        c->ack_now = true;
    }
    if (fin && receiving && !c->fin_rcvd) {
        c->rcv_nxt++;
        c->fin_rcvd = true;
        c->ack_now = true;
        if (c->state == TCP_ESTABLISHED) c->state = TCP_CLOSE_WAIT;
        else if (c->state == TCP_FIN_WAIT1) c->state = TCP_CLOSING;
        else { c->state = TCP_TIME_WAIT; arm(c, TIME_WAIT_MS); }
    }
    if (dlen || c->fin_rcvd) wake_owner(c);
    output(c);
}

/* ---- API ---------------------------------------------------------------------- */

struct tcp_conn *tcp_open(struct sock *owner) {
    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->used) continue;
        c->tx = kmalloc(BUF_SIZE);
        c->rx = kmalloc(BUF_SIZE);
        c->segs = kmalloc(SEG_MAX * sizeof(struct seg));
        if (!c->tx || !c->rx || !c->segs) {
            kfree(c->tx);
            kfree(c->rx);
            kfree(c->segs);
            c->tx = c->rx = NULL;
            c->segs = NULL;
            return NULL;
        }
        c->used = true;
        c->sock = owner;
        c->state = TCP_CLOSED;
        c->mss = MSS_DEFAULT;
        c->rto = RTO_INIT;
        c->snd_wnd = 0;
        return c;
    }
    return NULL;
}

int tcp_connect(struct tcp_conn *c, uint32_t addr, uint16_t port, uint16_t lport) {
    c->rip = addr;
    c->laddr = net_src_for(addr);
    c->rport = port;
    c->lport = lport;
    c->mss = MSS_DEFAULT;
    c->iss = net_now() * 4000u + (iss_counter += 64000u);
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->born = net_now();
    c->state = TCP_SYN_SENT;
    int rc = send_seg(c, SYN, c->iss, 0, 0);
    if (rc < 0) { c->state = TCP_CLOSED; return rc; }
    arm(c, c->rto);
    net_lo_drain();
    return 0;
}

void tcp_attach(struct tcp_conn *c, struct sock *owner) {
    c->sock = owner;
    c->parent = NULL;
}

static void abort_conn(struct tcp_conn *c) {
    if (c->state >= TCP_ESTABLISHED && c->state != TCP_TIME_WAIT)
        emit(NULL, c->laddr, c->rip, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, RST | ACK, 0, 0, 0);
    release(c);
}

void tcp_close(struct tcp_conn *c) {
    c->sock = NULL;
    if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT) {
        release(c);
    } else if (c->state == TCP_SYN_RCVD || c->rx_len) {         /* unread data: RFC 1122 says reset */
        abort_conn(c);
    } else if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        c->fin_queued = true;
        output(c);
    } else if (c->state == TCP_FIN_WAIT2) {
        arm(c, FIN_WAIT2_MS);
    }
    net_lo_drain();
}

void tcp_drop_children(struct sock *l) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].parent == l) abort_conn(&conns[i]);
    l->acc_n = 0;
    net_lo_drain();
}

int tcp_shutdown_wr(struct tcp_conn *c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) return -ENOTCONN;
    c->fin_queued = true;
    output(c);
    net_lo_drain();
    return 0;
}

int tcp_write(struct tcp_conn *c, const void *buf, size_t len) {
    if (c->err) return -c->err;
    if ((c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) || c->fin_queued) return -EPIPE;
    size_t n = BUF_SIZE - c->tx_len;
    if (n > len) n = len;
    if (n) {
        ring_put(c->tx, c->tx_start + c->tx_len, buf, n);
        c->tx_len += (uint32_t)n;
        output(c);
        net_lo_drain();
    }
    return (int)n;
}

int tcp_read(struct tcp_conn *c, void *buf, size_t len, bool peek) {
    if (c->rx_len) {
        size_t n = c->rx_len < len ? c->rx_len : len;
        ring_get(c->rx, c->rx_start, buf, n);
        if (!peek) {
            c->rx_start += (uint32_t)n;
            c->rx_len -= (uint32_t)n;
            uint32_t half = c->mss < BUF_SIZE / 2 ? c->mss : BUF_SIZE / 2;
            if ((int32_t)(window(c) - c->adv_wnd) >= (int32_t)half && c->state != TCP_CLOSED) {
                c->ack_now = true;                              /* the window reopened: tell the sender */
                output(c);
                net_lo_drain();
            }
        }
        return (int)n;
    }
    if (c->err) return -tcp_take_error(c);
    return tcp_eof(c) ? 0 : -EAGAIN;
}

int tcp_state(const struct tcp_conn *c) { return c->state; }
uint32_t tcp_readable(const struct tcp_conn *c) { return c->rx_len; }
uint32_t tcp_writable(const struct tcp_conn *c) { return c->fin_queued ? 0 : BUF_SIZE - c->tx_len; }
bool tcp_eof(const struct tcp_conn *c) { return c->fin_rcvd || c->state == TCP_CLOSED; }

int tcp_take_error(struct tcp_conn *c) {
    int e = c->err;
    c->err = 0;
    return e;
}

uint32_t tcp_peer_addr(const struct tcp_conn *c, uint16_t *port) {
    *port = c->rport;
    return c->rip;
}
