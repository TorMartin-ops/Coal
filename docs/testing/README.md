# Testing Coal OS

One entry point runs every test:

```sh
tools/testkit/coaltest --all                 # everything runnable on this host
tools/testkit/coaltest guest                 # one suite (name), or suite/test, or a test name glob
tools/testkit/coaltest --changed             # the suites worth running for the paths you touched
tools/testkit/coaltest --all --repeat 3      # each test three times, identical verdicts and output required
tools/testkit/coaltest --list                # suites, tests, tags, devices, cost estimate
python3 -m testkit ...                       # the same, from tools/testkit
```

Python 3 standard library, QEMU, CMake, NASM, mtools and a C compiler are all it needs. `tools/testkit/coal_test.sh` keeps
the old "build and boot once" entry point (it runs `coaltest hello`).

## Philosophy

1. **Deterministic by default.** Guest tests do not poll a host clock. QEMU runs under
   `-icount shift=6,sleep=off,align=off`: the guest's clock is the number of instructions it has executed, and idle time
   is skipped, so a `sleep 3` costs nothing and a loaded host changes how long a run takes, never what it does. The
   RTC is fixed (`-rtc base=2026-01-01T00:00:00,clock=vm`), the network is absent, and the kernel random generator is
   seeded from the kernel command line (`coal.seed=N`). Time limits are guest-time budgets enforced inside the guest
   (`coal.budget=`); the host's wall-clock watchdog exists only to catch a guest that has stopped completely, and it
   is stretched by the measured host load.
2. **No host input during a run.** `init` has a test mode (`coal.test=PROG,ARG,...`): it runs one program with its
   output on COM1, prints `@@COALTEST exit=... ns=...` and powers off. Nothing the host does while the guest runs can
   change what the guest sees. Disk writes go to a private snapshot overlay.
3. **Isolated.** Each QEMU has its own temp directory (overlay, serial log, pidfile) and its own process group. It is
   stopped only through its own pid. Nothing in the kit finds processes by name; `pkill qemu` is never the answer.
4. **Admitted.** Every QEMU start, from `coaltest` or from the gateway, first takes a slot of the host-wide
   semaphore (below). Several agents or sessions on one small host queue instead of overloading it.
5. **No retries, ever.** A test runs once per requested repetition. A failure is a failure. `--repeat N` runs each test
   N times with the same seed and demands identical verdicts and, for deterministic tests, identical normalised serial
   output (kernel time stamps and the guest clock removed). Any divergence is reported as `NONDETERMINISM` with a diff.
   `--seeds K` runs K different seeds; each must pass and output may differ between seeds only where the test says so.
6. **Every failure is replayable.** When a guest test fails, `coaltest` re-runs it under QEMU record/replay and writes a
   repro bundle (below), then verifies that the replay reproduces the recorded serial output byte for byte.

## Suites

`tools/testkit/coaltest --list` is authoritative; the registry is `tools/testkit/testkit/suites.py`.

| Suite | What | Deterministic |
| --- | --- | --- |
| `unit` | host-only: gateway codec, HTTP, agentd on a pty, MCP server, fuzzing, conformance corpus on this host, syscall numbers, the kit's own tests | verdict (frames: output too) |
| `guest` | user-space programs run by init: `abi_test syscall_test libc_test fault_test fs_stress kernel_test` (and `oom`), `memtest`, `sh_test.sh`, `applet_test.sh`, `hello`, `seed` | verdict and output |
| `virtio` | `hello`, `syscall_test`, `fs_stress` on a virtio-blk root | verdict and output |
| `microvm` | the same on `-M microvm` with virtio-mmio | verdict and output |
| `gateway` | agent protocol and agentd against the real kernel over COM2, virtio console, vsock | verdict only |
| `smoke` | coal-mcp end to end (build, boot, commands, files, jobs, snapshots, network) | verdict only |
| `conformance` | the shell corpus, one guest per category (`files pipes processes signals text`) against `tools/testkit/baselines/conformance_known_failures.txt`: a case outside the list that fails is a regression, a listed case that passes makes the list stale. The `net` category talks to servers on the host and stays host-driven | categories: verdict and output; `net`: verdict only |
| `bench` | boot timeline, `coalbench` | verdict only |

"Verdict only" suites are driven from the host over a socket (COM2 or the network). Which guest instruction a
host byte arrives at depends on host timing, so output cannot be bit-identical and the suites are tagged `host-io`
(and `network` where they talk to servers on the host). They still run under admission control, with timeouts scaled by
host load (`COAL_TIMEOUT_SCALE`), and `--repeat` still demands the same verdict every time. Everything a guest can check
by itself is in the deterministic suites; the host-driven ones test the host side of the link.

## Admission control

`/tmp/coal-testkit/slots/0 .. N-1`, N = `max(1, nproc - 1)` (override: `COAL_TESTKIT_SLOTS`). A guest holds an
exclusive `flock` on a free slot file for the whole life of its QEMU: a non-blocking try over all slots, then a blocking
wait on slot 0. From a shell, `flock /tmp/coal-testkit/slots/0 CMD` takes part. `coaltest --slots` shows which are busy.
A process tree holds one slot: `coaltest` sets `COAL_TESTKIT_SLOT_HELD` for host tests that start QEMU themselves, and the
gateway's `Machine` (and the test fixtures in `tools/coal-gateway/tests/support.py`) take a slot only when it is not
set. `tools/testkit/testkit/slots.py` has no dependencies so other tools can import it.

## Repro bundles

A failing guest test leaves `build/testkit-results/repro/<suite>-<test>-seed<N>/`:

| File | Content |
| --- | --- |
| `manifest.json` | test, seed, verdict, kernel and disk SHA-256, kernel command line, QEMU version, whether the failure reproduced under record, whether the replay was identical |
| `kernel.bin`, `disk.img` | exactly what ran (sparse copies) |
| `rr.bin` | the QEMU record/replay log |
| `output_run*.log`, `record/serial.log`, `replay/serial.log` | serial output of the failing run, the recording, the replay |
| `gateway_runlog/` | the gateway's per-run JSONL log, for host-driven tests |
| `replay.sh` | `sh replay.sh` replays and checks the output; `sh replay.sh gdb` starts QEMU with `-icount rr=replay -s -S` and attaches gdb to `kernel.bin` |

Breakpoints, `finish` and `reverse-stepi` were checked in a replay under gdb. `coaltest --replay DIR [--gdb]` does the
same as `replay.sh`. Host-driven tests get the logs but no recording (QEMU is driven by the host there). The record run
uses `sleep=on` because QEMU's record/replay hangs with `sleep=off`; the virtual time of the recording differs from the
failing run only in how idle time is spent, which is what a replay of a recording needs.

## Outputs

`coaltest` prints a table (result, runs, cases or checks counted, guest milliseconds, guest Mi-instructions, host
seconds). `--json FILE` and `--junit FILE` write the same data; `--ci` writes both to `build/testkit-results/`. Guest
instructions are the guest time divided by the clock's nanoseconds per instruction (64): an upper bound that includes
idle time the clock skipped.

## Fast loops

`--changed [BASE]` maps the files you touched (working tree, plus commits since the merge base with BASE, default `main`)
to suites through each suite's `paths`. Examples: a change under `kernel/drivers/virtio` runs `virtio`, `microvm`,
`guest`-tagged tests of that area; a change under `tools/coal-gateway` runs `unit`, `gateway`, `smoke`.

## CI

`.github/workflows/coal.yml` (copy in `docs/testing/coal.yml.example`) builds and runs `coaltest --ci`
(the hermetic suites, `--repeat 2`) on `ubuntu-latest` with TCG, and, when the repository variable `COAL_KVM_RUNNER`
names a runner with `/dev/kvm`, runs `--ci --accel kvm`. The workflow file has to be added to the repository by hand.

## Writing a test

* A guest program: add it to `src/CoalOS/CMakeLists.txt` (`add_user_program`, and the disk image `mcopy` list) and a
  `guest(...)` line to `suites.py`. Print `NAME: N checks, M failed` (see `userspace/tests/check.h`) and exit non-zero
  on failure. `coaltest --lint` fails if a built test program is not registered.
* Do not read the wall clock to decide anything that matters, do not sleep to "let something settle": wait for the event
  (a pipe, a wait status), or for guest time with an explicit budget.
* If a test is flaky, that is a bug in the test or the kernel. Under icount a race becomes a deterministic failure you
  can replay and bisect; fix the race.
