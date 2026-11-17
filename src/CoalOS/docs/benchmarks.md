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
