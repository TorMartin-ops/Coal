# Agent VM, phase 0: measure before building

Phase 0 adds no features. It adds the instruments that show where Coal OS stands: how long a
boot takes and where the time goes, how much of everyday Linux shell work already behaves like
Linux, which syscalls real programs need that Coal lacks, and whether a host can run QEMU with
hardware acceleration. Everything here uses the Python 3 standard library, POSIX `sh` or
PowerShell 5.1.

| Tool | Answers |
| --- | --- |
| `tools/harness/boottime.py` | Milliseconds from QEMU spawn to each boot milestone |
| `tools/harness/conformance/run.py` | Which shell-level cases behave on Coal as they do on Linux |
| `tools/harness/syscall_gap.py` | Which Linux syscalls real workloads use that Coal does not implement |
| `tools/harness/report.sh` | All of the above in one short summary |
| `scripts/host/setup-windows.ps1`, `scripts/host/setup-linux.sh` | Can this PC run Coal with KVM or WHPX |

## Boot timeline

```sh
tools/harness/boottime.py                       # one boot, host milliseconds
tools/harness/boottime.py --runs 10 --json      # min / median / p90 per milestone
tools/harness/boottime.py --icount              # deterministic guest time
tools/harness/boottime.py --accel kvm           # needs /dev/kvm (auto picks kvm when present)
tools/harness/boottime.py --build-dir build/x   # reuse a CMake build directory
```

Without `--build-dir` the kernel and disk image are built with CMake into a fresh temporary
directory. The disk is attached with `-snapshot`, so every run starts from the same image.

Milestones, in boot order: QEMU's serial sockets exist; first serial byte; kernel entry (the
banner); one line per kernel log message (memory, network, disk, mounts, `starting /sbin/init`);
mcpd's ready event; the first successful ping over COM2; the DHCP lease. The harness connects to
both serial ports before the guest runs, so no byte is missed and every time is the moment the
host received it. Times are since the QEMU process was spawned.

`--icount` runs TCG with `-icount shift=0,sleep=off`: the virtual clock follows executed
instructions, not the host. The report then uses guest time (the kernel's own log timestamps and
the `up_ms` field of mcpd's ready event). Those numbers repeat to within a millisecond, so they
are the right ones to compare across commits; guest time starts at kernel entry and excludes
firmware and GRUB. Host-time numbers vary with machine load.

Measured on a 2-core container (TCG, 128 MiB, 3 runs, median host ms since spawn):

| Milestone | ms |
| --- | ---: |
| QEMU serial sockets ready | 62 |
| first serial byte = kernel entry | 527 |
| memory initialised | 532 |
| e1000 / net up | 551 |
| disk found | 554 |
| root file system mounted | 561 |
| `starting /sbin/init` | 577 |
| mcpd ready event | 651 |
| first mcpd ping reply | 653 |
| DHCP lease | 1551 |

Roughly 450 ms of the 527 ms before the first serial byte is SeaBIOS and GRUB; the kernel needs
about 50 ms from banner to init, and init to mcpd another 75 ms. The DHCP lease lands about
900 ms later and does not block mcpd. Guest-time (`--icount`) shows init starting at 6 to 7 ms
and mcpd ready at 10 to 12 ms after kernel entry.

## Conformance corpus

```sh
tools/harness/conformance/run.py                    # host and Coal; builds and boots Coal
tools/harness/conformance/run.py --target host      # validate the corpus on this Linux only
tools/harness/conformance/run.py --category text --verbose   # diff of every failure on stderr
tools/harness/conformance/run.py --json
```

Cases live in `tools/harness/conformance/cases/<category>.txt`, one file per category (files,
processes, pipes, signals, net, text):

```
== name
shell script lines (POSIX sh, relative paths, runs in an empty directory)
-- 0
expected stdout lines
```

The number after `--` is the expected exit status of the script (default 0); stderr is not
compared. Network cases use `@HOST@`, `@ECHO@`, `@UDP@` and `@HTTP@`: the runner starts a TCP
echo, UDP echo and HTTP server and substitutes 127.0.0.1 on the host and QEMU's gateway
10.0.2.2 in Coal.

The host run comes first and validates the corpus. A case that fails on Linux is reported as
"invalid on host" and excluded from Coal's rate, and the exit status is 1. Coal's run goes through
coal-mcp (`coal_boot`, then `coal_run` per case), the same path an agent uses. Results from the
first run: 113 cases, 75 pass on Coal (66.4%); text tools (sed, awk, uniq, md5sum, base64, tac,
sort -k) and file features (symlinks, hard links, chmod bits, tar, gzip) are the main gaps, and
`wc` pads its counts where GNU does not.

## Syscall gap

```sh
tools/harness/syscall_gap.py --collect /tmp/gap-logs --markdown docs/compat/syscall-gap.md
tools/harness/syscall_gap.py logs/*.strace --json        # analyse logs you recorded yourself
```

`--collect` needs `strace` and runs whichever of these exist: `python3 -c`, `node -e`, `gcc` on a
hello world, `git init/add/commit/log`, `pytest` on a two-test project, `make -j4`. Each runs under
`strace -f -o` through `sh -c`. Your own logs may be `strace -f -o FILE` output or `strace -c`
summaries; the file name before the first dot names the workload.

The tool parses Coal's table from `kernel/cpu/syscall.c`, compares by syscall name (Coal is i386,
the logs are x86_64) after folding pure ABI renames (`mmap2` and `mmap`, `stat64` and `stat`,
`getuid32` and `getuid`, `set_thread_area` and `arch_prctl`), and ranks missing syscalls by how many
workloads need them, then by call count. It also lists table entries that return `-ENOSYS` for
some arguments (`openat` with a directory fd, file-backed `mmap`). The committed result is
[docs/compat/syscall-gap.md](../compat/syscall-gap.md).

## One-shot report

```sh
tools/harness/report.sh            # conformance, then syscall gap
tools/harness/report.sh --boot     # plus one boot timeline
BUILD_DIR=/tmp/b tools/harness/report.sh
```

## Host setup

Linux:

```sh
scripts/host/setup-linux.sh        # checks CPU flags, /dev/kvm, group, QEMU, build tools; asks before installing
scripts/host/setup-linux.sh -y     # install QEMU without asking
```

Windows (elevated PowerShell, 5.1 or newer):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\host\setup-windows.ps1
powershell -ExecutionPolicy Bypass -File scripts\host\setup-windows.ps1 -Distro Ubuntu -Memory 12GB
```

The script reports the Windows build and edition, CPU virtualization, Hyper-V, Virtual Machine
Platform and Windows Hypervisor Platform, and the WSL version and default distribution. It sets
`nestedVirtualization=true` (and `memory`, unless you already set one) under `[wsl2]` in
`%UserProfile%\.wslconfig`, leaving every other line alone and writing a timestamped backup first.
After a change it asks before running `wsl --shutdown`. Inside the distribution it checks
`ls -l /dev/kvm`, `kvm-ok` and QEMU, and asks before installing `qemu-system-x86`. It also looks for
`qemu-system-x86_64.exe` and starts it with `-accel whpx` to see whether the accelerator
initialises. The end is a PASS/FAIL table and the recommended path: KVM in WSL2 first, QEMU with
WHPX on Windows as the fallback. Nested virtualization in WSL2 needs Windows 11.
