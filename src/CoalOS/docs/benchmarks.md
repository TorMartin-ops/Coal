| test                 | unit | wall-clock median |             p10 .. p90 | icount median |

> **x86_64 port.** The tables and the sysenter/`int 0x80` sections below were measured on the former i386
> kernel and are kept as history. Coal now enters the kernel with `syscall`. Instructions per operation
> (`-icount shift=0,sleep=off`, same host and arguments), main i386 against x86_64:
> null_syscall 131 -> 144, ctx_2 2987 -> 2301, fork_exit_wait 52010 -> 44984, fork_exec_wait 102602 -> 87030,
> page_fault 1431 -> 876, pipe_bw 632 -> 932 MB/s, clock_gettime 99 -> 81, pipe_rw_1b 1804 -> 1746.

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

## D. Files and disk (page cache, FAT, ATA DMA)

Numbers are deterministic instruction-count mode (`-icount shift=0,sleep=off`, `tools/bench/results/pD-before.json`
and `pD-final.json`); the wall-clock column of the table above was measured on an idle host, the host was busy
while this section was measured, so only the icount figures are comparable.

| test                 | unit | before   | after    | change |
|----------------------|------|----------|----------|--------|
| file_read_8m         | MB/s |    71.92 |   237.50 | +230%  |
| file_write_8m (sync) | MB/s |    31.61 |   188.77 | +497%  |
| file_write_8m_nosync | MB/s |    36.55 |   221.40 | +506%  |
| open_close           | ns   | 12878    |  6765    | -47.5% |
| stat                 | ns   | 13047    |  6933    | -46.9% |
| create_delete_0k     | ns   | 25114    | 18503    | -26.3% |
| create_delete_4k     | ns   | 66366    | 45015    | -32.2% |
| fork_exec_wait       | ns   | 250837   | 229026   | -8.7%  |

Nothing else moved beyond noise. Kernel text grew by 11.5 KB, bss by 16 KB (page hash).

### File page cache (+ scatter/gather ATA commands)
Regular file data is cached in 4 KiB pages keyed by (volume, first cluster, page index); the first cluster is
unique while a file exists and survives open/close and rename. Reads and writes copy from/to the pages, a miss
reads a readahead window with one disk command (4 pages, doubling to 32 while the access stays sequential, 1
page for random access), dirty pages are written back in runs by the existing flusher/sync/fsync path, data
before the FAT and directory sectors that describe it. Clean pages are evicted LRU when the cache is over half of
lowmem or lowmem is short, and from the OOM path before any process is killed; the dirty page count is capped
(1/8 of lowmem), after which the writer flushes. Bytes of a page past EOF are always zero, so growing a file needs
no extra zeroing and a freshly allocated cluster is not zeroed any more (directory clusters still are).
Why: the buffer cache is per 512 B sector and every sector was one ATA command, so a file read was
a hash lookup plus a command per sector. Method: Linux page cache and readahead (Wu, "Linux readahead: less
tricks for more", OLS 2007) in a much simplified form (no async trigger page). The ATA driver takes a
list of memory segments for one command (`block_device_rw`), so a window is one command even though the
pages are separate allocations.
Effect: file_read 72 to 237 MB/s (all hits; now limited by the syscall bounce copy), file_write_8m_nosync x6,
fork_exec_wait -8% (the ELF is read through the cache).

### FAT free-cluster bitmap and contiguous appends
A bitmap of free clusters is built at mount and kept current by the one function that changes the table;
allocation scans it a word at a time (`ctz`), the free count for statfs is a counter instead of a scan of the
FAT. An append first tries the cluster right behind the previous one, so files stay contiguous and the
write-back runs and readahead windows become single commands. Source: Microsoft FAT specification
(allocation is free to choose any free cluster); extent-friendly allocation as in ext4 (Mathur et al., OLS 2007).

### FAT directory entry cache
A 256-entry direct-mapped cache maps (parent cluster, name) to the slot position of the entry. A hit reads the
slot from the buffer cache and checks that it still holds the same short name, so size and cluster are never stale;
entries are dropped when their slot is freed (unlink, rmdir, rename) and replaced when a name is created, which also
covers mkdir. The DOS timestamps stay raw in the lookup result and are converted only when a node is opened.
Effect: a component costs a few hundred instructions (measured about 600 per hit, mostly hashing, the
buffer lookup and name compare) instead of a directory scan; open_close -47% together with the stack path buffer
in the VFS (no 4 KiB allocation per open) and the raw timestamps.
`fs_stress` has name tests (long names, case, rename, slot reuse, rmdir and recreate).

### ATA bus-master DMA
On an Intel PIIX3/PIIX4 IDE controller (QEMU's default) transfers use READ/WRITE DMA (EXT) with a PRD table
(SFF-8038i): one table per channel in a heap page, regions split at 64 KiB boundaries, any number of memory
segments per command, up to 256 sectors. Completion is polled (drive interrupt masked), with `yield()` in the
loop. A transfer whose buffer is not in the lowmem direct map, any other controller, and any DMA failure (the
controller is then not used again) go through the existing PIO code.
Effect: file_write_8m (sync) 79 to 183 MB/s on top of the page cache.

### Tried and not kept
Nothing was reverted. Not done: READ/WRITE MULTIPLE (the PIO path already uses multi-sector READ/WRITE SECTORS,
and DMA made it moot), an async readahead trigger page, a negative name cache.

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

### Not done: rep movsb with ERMS

CPUID leaf 7 reports ERMS, and `rep movsb` is then as fast as `rep movsd` for long copies, with no tail. It was not
added: QEMU's TCG executes one iteration per byte, so under emulation (the only place these numbers can be
measured) it is four times slower than the dword form, and on hardware fast-string microcode already covers
`rep movsd`. A CPUID probe and a branch in every copy would buy nothing measurable here.

### Wall-clock summary (TCG, host load average about 4 on 2 cores, 21 reps, one run)

Wall-clock numbers on a loaded host move by tens of percent between runs (one earlier run measured
`mmap_munmap_1m` 53 percent worse, the next 4 percent better); only large effects are meaningful.
Before is `ffb6890`, after is the end of the wave.

| test | unit | before | after |
|------|------|--------|-------|
| memcpy_bw | MB/s | 31.1 | 1681.6 |
| pipe_bw | MB/s | 37.1 | 201.8 |
| page_fault | ns | 5179 | 2406 |
| fork_exit_wait | ns | 104609 | 65932 |
| fork_exec_wait | ns | 594941 | 327354 |
| signal_roundtrip | ns | 24908 | 6533 |
| create_delete_4k | ns | 133042 | 80804 |
| mmap_munmap_1m | ns | 2942 | 2818 |
| file_write_8m | MB/s | 2.08 | 2.64 |

RAM after boot (`/proc/meminfo` MemFree at 128 MiB, a `cat` running): 127268 kB before, 127224 kB after (44 kB less:
one more kernel text page and the 28 kB of size-class pages that the new libc malloc touches in that process).
The zero pool is counted as free, so it does not show.
## System call and time fast path

Numbers are instructions per operation (`-icount shift=0,sleep=off`, `tools/bench/run.py`) on the
baseline above (ffb6890) and after the five changes below. icount counts instructions, so it cannot
show what `sysenter` saves in the CPU (no descriptor loads, no microcoded `int`/`iret`); it only shows
that the new entry costs nothing in instructions. Real hardware saves cycles there: Linus measured
gettimeofday at about 1280 against 2415 cycles on a P4 (LWN, "How to speed up system calls", 2002),
and Vlasenko puts `int 0x80` about 250 cycles above `sysenter` (Denys Vlasenko, 2015).

| test | before | after | change |
|---|---:|---:|---:|
| null_syscall | 133.1 | 131.0 | -1.5% |
| gettimeofday | 415.2 | 104.0 | -74.9% |
| clock_gettime | 395.2 | 99.0 | -74.9% |
| read_zero_1b / write_null_1b | 723 / 718 | 721 / 716 | -0.3% |
| pipe_rw_1b | 1806.9 | 1802.6 | -0.2% |
| ctx_2 / ctx_8 | 861.2 / 861.6 | 861.0 / 861.4 | 0% |
| open_close / stat | 12878 / 13047 | 12376 / 12546 | -3.9% |
| fork_exit_wait / fork_exec_wait | 97274 / 250837 | 97855 / 251978 | +0.6% / +0.5% |
| kernel_text | 170951 B | 175290 B | +2.5% |

The fork and exec cost is the extra VMA and two PTEs per address space. kernel_text includes the 4 KiB vDSO image.

1. **Dispatch.** `syscall_dispatch` called `signal_return_to_user` and `signal_mask_restore` on every call.
   Both return at once when nothing is pending, so the caller now tests `pending_signals` and `sig_restore`
   first (two loads). It saves about 33 instructions per call, which pays for the 34 instructions of the
   `sysenter` stub below. Kept.
2. **Path chunk.** `copy_path_from_user` copied the rest of the user page for every path, so its cost
   depended on where the string sat in the page (open_close moved by 20% when an unrelated change shifted
   the layout). It now copies 128-byte chunks until the NUL. open_close and stat -4%. Kept.
3. **TSC clock.** `CLOCK_MONOTONIC` and `CLOCK_REALTIME` in ns from the TSC, calibrated against the PIT
   (exact count, not a rounded kHz) and fixed after boot, with the RTC read once. The tick-based clock
   only had 1 ms resolution. The scale is `ns = delta * mult >> shift` (the Linux clocksource mult/shift scheme).
   Without a believable TSC (`-cpu qemu32,-tsc`) `mult` is 0 and the clock stays tick-based. Kept.
4. **sysenter and vDSO.** The Linux i386 `__kernel_vsyscall` convention (vdso(7), Intel SDM vol. 2 SYSENTER/SYSEXIT): the stub saves ecx, edx and ebp, passes the stack pointer in ebp and the
   sixth argument on the stack. The return lands on the stub's tail, which sits behind an `int 0x80`, so a
   call restarted after a signal (`eip -= 2`) runs through the old entry. `sysexit` is used only when the
   frame still returns to that landing pad and TF is clear; handlers, execve and sigreturn use `iret`.
   TF and NT are cleared on entry (NT would turn the `iret` into a task return), and a single-step trap
   at the entry is filtered in `isr1`. Without SEP the stub is patched to `int 0x80`. `int 0x80` is
   unchanged and static binaries that ignore AT_SYSINFO keep working (abi_test). Instruction count neutral
   (see above); the gain is cycles on hardware. Kept.
5. **vDSO time and libc.** `__vdso_clock_gettime`, `__vdso_gettimeofday` and `__vdso_time` read a seqlock
   page (`struct vdso_data`) with `rdtsc`, falling back to the system call for other clocks or when
   `mult` is 0. libc looks the symbols up in AT_SYSINFO_EHDR and calls through AT_SYSINFO, with `int 0x80`
   when they are absent. gettimeofday and clock_gettime -75%. Kept.

Nothing was reverted.

### Wall-clock

Five alternating runs of base and new under plain TCG, median of the run medians, on a machine shared
with other jobs (about 2x noise, treat as indicative): null_syscall 1036 -> 621 ns, gettimeofday 1682 -> 105 ns,
clock_gettime 1258 -> 97 ns, read_zero_1b 2601 -> 2048 ns, ctx_2 21406 -> 17144 ns, signal_roundtrip 14156 -> 9097 ns.

pipe_rw_1b is slower under TCG (about 3.5 -> 17 us) and is not a cost of this change: the commit before the vDSO
shows the same 12 to 17 us as soon as two extra frames are allocated at boot. Where the pipe ring page
lands in physical memory changes how QEMU's translator treats it. The icount number above is unchanged.

## Scheduler, timers and tickless idle

Numbers below are `python3 tools/bench/run.py --qemu-args "-icount shift=0,sleep=off"` (deterministic,
1 instruction = 1 ns), old kernel and new kernel both running the same `coalbench`. Wall-clock runs on
the shared host varied by 2x between repeats and are only used as a sanity check.

### EEVDF scheduler

Earliest Eligible Virtual Deadline First (Stoica and Abdel-Wahab, "Earliest Eligible Virtual Deadline
First", 1995; Linux 6.6 `kernel/sched/fair.c`). Linux `prio_to_weight` nice weights, virtual time
`v` advances by `ran * 1024 / weight`, a task is eligible while `v <= V` (the weighted mean of `v`,
checked without dividing), and among the eligible tasks the earliest `v + slice / weight` runs. A task
keeps its lag while it sleeps and keeps the CPU until its slice ends (run to parity). Slices are 3 ms,
0.75 ms for tasks that give the CPU back early. Two red-black trees (by deadline, and by `v` for tasks
that are not eligible yet) replace the O(1) bitmap queues. New syscalls: `nice`, `getpriority`,
`setpriority`; coalbox has a `nice` applet.

| test          | unit | before (O(1)) | after (EEVDF) |
|---------------|------|--------------:|--------------:|
| wakeup_loaded | ns   |    20 000 641 |         4 688 |
| fairness_dev  | %    |        327.84 |          2.18 |
| wakeup_idle   | ns   |         2 170 |         4 608 |
| ctx_2         | ns   |           861 |         2 349 |
| ctx_8         | ns   |           862 |         2 344 |
| pipe_rw_1b    | ns   |         1 807 |         1 808 |

`wakeup_loaded` is the time from a wake-up to the task running while a CPU hog is running; the old
kernel made it wait for the hog's whole 20 ms slice. `fairness_dev` is the largest deviation of three
hogs (nice 0, 5, 10) from their weight share (1024:335:110); the old kernel has no nice levels, so the
hogs share equally. **The price is the context switch**: every block and wake-up now updates the
virtual-time sums, one `rb_tree` insert/remove and a 64-bit `V` division, about 600 ns per
block+wake-up pair against about 100 ns before (`ctx_*` and `wakeup_idle` regress by that, in
instructions). Wall-clock `ctx_2` stays at the old 17 to 22 us because QEMU's own switch cost dominates.
The change was kept for the fairness and the wake-up latency under load; it is the one regression of
this work and sits in `scheduler.c` only.

### Timer wheel

Hierarchical timing wheel (Varghese and Lauck, "Hashed and Hierarchical Timing Wheels", 1987; the
design of Linux `kernel/time/timer.c`): 5 levels of 64 slots at 1 ms resolution, a bitmap per
level, cascading at block starts, O(1) add/delete. Sleeps, TCP, DHCP and the e1000
poll timer all use it (`timer_add`, `timer_del`). The next expiry is found from the bitmaps without
scanning, so a timer 5 s away costs one interrupt, not one per tick.

### Tickless idle

`NO_HZ` as in Linux (`kernel/time/tick-sched.c`): there is no periodic tick. `clock_ns()` counts the
TSC (calibrated against PIT channel 2). The one clock event device is a one-shot, armed for the
earliest of: the next wheel expiry and, only when another task is runnable, the end of the running
task's slice. Preference order: local APIC TSC-deadline timer, local APIC one-shot (calibrated against
the TSC), PIT mode 0 one-shot; without a TSC the PIT stays periodic at 1 kHz. The TSC-deadline path
cannot be tested under QEMU's TCG (it has no such CPUID bit); the other three boot and pass the
tests (`-cpu qemu32,-apic`, `-cpu qemu32,-tsc`).

| test     | unit  | before (1 kHz PIT) | after |
|----------|-------|-------------------:|------:|
| idle_irq | irq/s |               1000 |     4 |

`idle_irq` counts timer interrupts over 1 s of `nanosleep` using `/proc/interrupts` (new); the 4/s
left are the MCP daemon's own polling, a truly idle system takes about 0.5/s. The old number is the
fixed PIT rate, the old kernel had no `/proc/interrupts`. `gettimeofday` and `clock_gettime` cost
about 36 ns more than before (+8.6 %, TSC read and a 96-bit multiply instead of a tick counter);
the clock is exact to the nanosecond now and no longer a 1 ms step.


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

After merging main (timer wheel, tickless idle, EEVDF), same harness, 3 seeds each, medians: download 0% 0.14 s,
1% 1.71 s, 2% 8.3 s (mean 9.3), 5% 44.7 s; upload 0% 0.50 s, 1% 0.55 s, 2% 0.92 s, 5% 1.52 s (before: 3.0 s at 2%,
10.5 s at 5%). The downloads are slirp-bound and vary a lot from run to run on a loaded host, so the 2% figure
sits between the earlier before and after values; the uploads are stable. Loopback (icount) on the merged tree:
tcp_latency 13.8 us, tcp_bw 254 MB/s; latency is higher than before the merge because each round trip now
costs two EEVDF context switches (ctx_2 went from 0.9 to 3.0 us in the same run), the TCP path itself is unchanged.

Downloads are limited by slirp's own sender (BSD stack, RTO of about 1.5 s whenever fewer than three duplicate ACKs
follow a loss); the guest can only keep its window open.
