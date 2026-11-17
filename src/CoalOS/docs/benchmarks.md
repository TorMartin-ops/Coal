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

## Perf wave C: memory

Numbers below are icount medians (`-icount shift=0,sleep=off`, 5 reps) against the commit before the wave
(`ffb6890`). In this mode QEMU counts every `rep` iteration as one instruction, so byte-wise `rep movsb`/`stosb`
look four times worse than dword-wise; see the wall-clock table at the end for the real effect.

### rep movsd / rep stosd for memcpy, memset and copy_*_user

The kernel `memset`/`memcpy` were byte loops, the libc ones and `copy_*_user` used `rep movsb`/`stosb`.
All now move whole dwords with `rep movsl`/`rep stosl` and finish with at most three bytes (`rep movsb`).
`copy_*_user` keeps its exception-table fixup: a fault in the dword phase returns `ECX*4 + tail` bytes
not copied. `memmove` copies backwards (`std`, tail bytes first) only for a forward overlap.
Interrupt stubs now execute `cld`, because user mode can set DF and the kernel's `rep` strings need it clear.
The usual rationale: string instructions are microcoded and cost per iteration, so fewer, wider iterations win
(Intel SDM vol. 1, 7.3.9; Intel optimization manual, "Fast string operations").

| test | before | after |
|------|--------|-------|
| memcpy_bw (MB/s) | 999 | 3998 |
| page_fault (ns) | 10813 | 1592 |
| pipe_bw (MB/s) | 117 | 661 |
| fork_exit_wait (ns) | 97274 | 52115 |
| fork_exec_wait (ns) | 250837 | 120025 |

### Slab: free-list sharding

`slab.c` follows mimalloc's free-list sharding (Leijen, Zorn, de Moura, MSR-TR-2019-18): every page has an
allocation list (`free`) and a `local_free` list that frees push to; they are merged only when `free` runs
dry. A cache allocates from one current page, so the fast path is a load, a pop and a counter increment, and
a fresh page bumps an offset instead of threading all its objects into a list (a 16-byte page used to cost
252 stores up front). A free into a full page puts it back on the partial list; a page that empties goes back
to the buddy allocator at once unless it is the cache's current page. The per-free `%` (a division) that
validated the pointer is gone. Result in icount mode: small but consistent (0.1 to 1.7 percent fewer
instructions on syscall, pipe, mmap and fork paths) and 64 bytes less kernel text.

| test | before | after |
|------|--------|-------|
| mmap_munmap_1m (ns) | 1270.7 | 1249.2 |
| pipe_rw_1b (ns) | 1814.9 | 1800.9 |
| fork_exit_wait (ns) | 52114.9 | 52071.1 |
| open_close (ns) | 11915.4 | 11877.3 |

### Frame reference counts without a lock; page tables copied without clearing

`get_frame`/`put_frame` change the count with one `lock xaddw` instead of a spinlock with interrupt
save/restore (fork touches one count per mapped page in both directions). `paging_clone_directory` no longer
clears the new page table before filling it: every entry is written, and the table is published only when full.
fork_exit_wait 52071 -> 50072 ns (icount), fork_exec_wait 119877 -> 119246.

### Pre-zeroed frame pool

The idle loop zeroes one frame at a time into a pool of 32 (`frame_refill_zeroed`); `frame_alloc*_zeroed`
(demand-zero faults, page tables) take from it and fall back to clearing the frame themselves. The pool is
bounded (128 KiB), counts as free in `/proc/meminfo`, is refilled only while the buddy allocator holds more than
256 pages, and is drained by ordinary allocations when memory runs out. MemFree after boot is identical with and
without it (127272 kB of 129168 kB at 128 MiB). An empty pool costs one load on the allocation path.
New benchmark `page_fault_idle`: 16 first-touch faults right after a 10 ms sleep.

| test | pool off | pool on |
|------|----------|---------|
| page_fault_idle (ns) | 1700.1 | 357.9 |
| page_fault (ns), back-to-back, pool empty | 1634.6 (lock on every call) | 1594.6 |

A throughput loop never idles, so `page_fault` itself cannot gain; the pool pays off for bursts that follow idle
time (shell commands, program start-up).

### libc malloc: size classes and per-page free lists

The first-fit list with a linear scan (and coalescing on every pass) is replaced by the same scheme as the
kernel slab: 22 size classes up to 2032 bytes (16-byte steps to 128, then four per doubling), 4 KiB pages
from brk each serving one class with `free`/`local_free` lists and bump allocation, empty pages kept in a pool
every class can reuse, larger blocks mapped on their own and unmapped on `free`. The header sits at the start of
the page, so `free` and `realloc` need no per-block header and blocks stay 16-byte aligned.
Alloc/free ping-pong on one page merges the lists inline instead of taking the slow path.
Cost: one 4 KiB page per size class in use (a running `cat` holds 28 kB more than before).
New benchmarks `malloc_free_64` (malloc+free of 64 bytes) and `malloc_churn` (128 live blocks of 8 sizes, freed
out of order). The libc test that expected a freed 100-byte block to serve a 50-byte request now asks for 100.

| test | before | after |
|------|--------|-------|
| malloc_free_64 (ns) | 102.0 | 87.0 |
| malloc_churn (ns) | 86890 | 12862 |

### Fault-around for demand-zero pages

A demand-zero fault in a growing-up VMA (heap, mmap; not the stack) also maps the next three absent pages
that share the page table. Only spare memory is used (`frame_alloc_user_zeroed_spare` fails below 256 free pages)
and the OOM killer is never involved for the extra pages. Four pages is the knee: eight pages gave only
12.0 percent instead of 10.2 percent on `page_fault` because zeroing the frames dominates. ELF segments are
loaded eagerly by exec, so there is nothing to fault around for file-backed pages.
Worst case for a process that touches one page in a sparse region: three extra frames, freed on exit or munmap.

| test | before | after |
|------|--------|-------|
| page_fault (ns) | 1594.5 | 1431.7 |
| page_fault_idle (ns) | 357.9 | 195.3 |
