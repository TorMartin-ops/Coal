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
