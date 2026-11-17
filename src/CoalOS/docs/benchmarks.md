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

