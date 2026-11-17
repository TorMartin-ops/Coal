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
