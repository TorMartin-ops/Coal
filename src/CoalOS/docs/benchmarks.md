| test                 | unit | wall-clock median |             p10 .. p90 | icount median |
|----------------------|------|-------------------|------------------------|---------------|
| null_syscall         | ns   |            647.79 |       635.69 .. 683.04 |        104.05 |
| read_zero_1b         | ns   |           1830.51 |     1561.61 .. 2164.56 |        694.35 |
| write_null_1b        | ns   |           1839.79 |     1484.47 .. 2267.28 |        689.34 |
| gettimeofday         | ns   |           1020.19 |      970.72 .. 1173.75 |        381.18 |
| clock_gettime        | ns   |            963.99 |      955.57 .. 1042.86 |        361.18 |
| open_close           | ns   |          15576.59 |   15290.39 .. 22181.81 |      14508.47 |
| stat                 | ns   |          14809.51 |   14382.98 .. 22045.28 |      14708.47 |
| pipe_rw_1b           | ns   |           3114.20 |     3060.93 .. 4834.69 |       1483.72 |
| page_fault           | ns   |           5032.10 |     4881.22 .. 5221.50 |      10797.12 |
| mmap_munmap_1m       | ns   |           2593.68 |     2512.12 .. 2698.28 |       1340.68 |
| fork_exit_wait       | ns   |          80056.91 |   78405.92 .. 93314.07 |      95176.09 |
| fork_exec_wait       | ns   |         415211.17 | 406196.90 .. 436527.06 |     250358.70 |
| ctx_2                | ns   |          12947.09 |   12755.72 .. 14076.56 |        811.03 |
| ctx_8                | ns   |          13007.16 |   12648.46 .. 19639.26 |        811.40 |
| pipe_bw              | MB/s |             58.04 |         43.74 .. 58.62 |         53.64 |
| signal_roundtrip     | ns   |           4287.17 |     4069.29 .. 4699.63 |       2536.79 |
| create_delete_0k     | ns   |          34827.66 |   34215.33 .. 37410.83 |      28397.25 |
| create_delete_4k     | ns   |          77145.20 |   75666.33 .. 79059.57 |      69599.74 |
| file_write_8m        | MB/s |              3.85 |           3.61 .. 4.06 |         50.44 |
| file_write_8m_nosync | MB/s |              4.19 |           3.82 .. 4.27 |         44.97 |
| file_read_8m         | MB/s |              6.96 |           6.69 .. 8.22 |         92.12 |
| tcp_latency          | ns   |               n/a |                        |           n/a |
| tcp_bw               | MB/s |               n/a |                        |           n/a |
| memcpy_bw            | MB/s |             29.99 |         23.82 .. 32.09 |        999.53 |
No loopback interface exists yet, so the TCP tests print `n/a`.

## Network

Measured with `-icount shift=0,sleep=off` (loopback) and with `tools/bench/netbench.py` (wget of 2 MiB from a
host `http.server` through QEMU user networking and a frame filter that drops TCP frames at random, seeded).
The wall-clock wget numbers move with host load, so the builds under comparison run interleaved.

### E1. Loopback is delivered synchronously, frames are cheaper

- Looped-back packets used to wait for the next 1 ms timer tick (two ticks per round trip) and the 16-slot queue
  silently dropped what did not fit, so a 16 KiB window overflowed it and TCP sat in RTO backoff. The transport
  now calls `net_lo_drain()` when it has finished building packets, like a softirq (Linux runs loopback through
  the backlog softirq the same way); a full queue returns `-ENOBUFS` and the sender stops instead of losing data.
- Loopback segments carry no TCP checksum and it is not checked (`net_rx_trusted`); Linux skips it on `lo` too.
- `net_csum_add` adds 32-bit words (RFC 1071 section 2(B): the sum does not depend on byte order).
- e1000: frames are built in the next free TX buffer and sent in place (no copy); the RX tail register is written
  once per batch instead of per frame; 64 RX and 64 TX descriptors (a burst of a full congestion window used to
  overflow 16 TX descriptors and lose frames); interrupt throttle (ITR) of about 100 us.

| test         | before     | after     |
|--------------|-----------:|----------:|
| tcp_latency  | 1999994 ns |   8660 ns |
| tcp_bw       | 0.16 MB/s  | 52.4 MB/s |

### E2. RACK-TLP loss recovery and RFC 6298 timers

- Every data segment in flight has a record (sequence, length, transmit time of the latest copy, retransmitted, lost).
  Loss is inferred from time, not only from three duplicate ACKs (RFC 8985, Cheng et al.): a segment is lost once a
  segment sent after it was delivered and a reordering window (a quarter of the minimum RTT) plus one RTT has
  passed. The stack has no SACK, so duplicate ACKs stand in for it: the n-th duplicate ACK means n later segments
  arrived, which bounds the newest delivered transmit time. The classic third-duplicate-ACK rule stays as well.
- Tail loss probe: about 2 * SRTT after the last transmission (plus 40 ms when one segment is outstanding, RFC 8985
  section 7.2) the newest segment is sent again, or one new segment if there is data and window. A lost tail is
  found in about 10 ms instead of the 200 ms RTO minimum plus backoff.
- Retransmission timeout as in RFC 6298 (SRTT, RTTVAR, `RTO = SRTT + max(G, 4 * RTTVAR)`, Karn's rule through the
  per-segment records, a sample from the handshake); after a timeout every segment in flight is marked lost and
  resent as the window allows (go-back-N, not one segment per RTO). One window reduction per recovery (RFC 5681
  equation 4). Slow start counts ACKed bytes up to 2 * MSS per ACK (RFC 3465).
- TCP timers run every 2 ms (`TCP_TICK_MS`) instead of every 10 ms.

2 MiB sent by the guest (`netbench.py --upload`, `yes | head | nc`, median of 4; the generator costs about 0.4 s):

| loss | before | after |
|-----:|-------:|------:|
|   0% |  0.55 s | 0.45 s |
|   1% |  0.71 s | 0.51 s |
|   2% |  2.99 s | 0.85 s |
|   5% | 10.53 s | 1.41 s |

### E3. Delayed ACKs, Nagle, TCP_NODELAY

- ACK every second full segment or after 40 ms (RFC 1122 4.2.3.2, RFC 5681 4.2); at once for out-of-order data, a
  filled gap, a duplicate and a FIN, and for the first 16 segments of a connection and after a gap (Linux's quick-ack
  idea, so slow start is not halved). A pending ACK rides on any data segment we send.
- Nagle (RFC 896): a sub-MSS write waits while earlier data is unacknowledged. `TCP_NODELAY` (setsockopt/getsockopt,
  inherited by accepted sockets) turns it off and releases held data. `kernel_test smallwrites` covers the option and
  ordered delivery of held bytes.
- Loopback (icount): tcp_latency 9233 ns -> 7844 ns (the reply carries the ACK), tcp_bw 51.6 -> 52.9 MB/s.
- Tried and dropped: holding the advertised window constant while a hole is open (so duplicate ACKs are "pure" in the
  RFC 5681 sense). QEMU's slirp sender needs the window updates; 24-run median at 1% loss went from 1.6 s to 10.5 s.
  Also not done: SACK. slirp, the sender in the wget test, does not negotiate it (SYN-ACK carries MSS only).

### E4. Receive buffer autotuning

A connection whose application keeps reading but leaves the buffer more than half full gets a buffer twice as large,
16 KiB up to 64 KiB, with 1 MiB of growth allowed over all connections (Linux does the same from measured read rate,
Fisk and Feng 2001 / Linux tcp_rcv_space_adjust). The window field is 16 bits; window scaling (RFC 7323) was not added
because slirp does not offer it, so it would never be used in the test setup.

### Results, 2 MiB through the lossy link (median of N interleaved runs, wall clock)

| test | loss | before | after |
|------|-----:|-------:|------:|
| wget (guest receives) | 0% | 0.16 s | 0.14 s |
| wget | 1% | 2.34 s | 2.34 s (mean 2.84 -> 2.21) |
| wget | 2% | 9.06 s | 6.05 s |
| upload (guest sends) | 5% | 10.5 s | 1.4 s |

Downloads are limited by slirp's own sender (BSD stack, RTO of about 1.5 s whenever fewer than three duplicate ACKs
follow a loss); the guest can only keep its window open.
