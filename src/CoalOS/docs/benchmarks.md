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
