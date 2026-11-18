# System call fuzzer

`tools/fuzz/` is a coverage-guided fuzzer for the Coal OS system call interface, run against the debug kernel.

## Parts

- `plugin/coverage.c`: QEMU TCG plugin. Every translated block of kernel text bumps `map[cur ^ prev]` in a 64 KiB
  shared-memory bitmap (AFL style). Build with `plugin/build.sh` (needs `qemu-plugin.h`).
- `userspace/tests/fuzz_executor.c`: static guest program that replaces `/sbin/agentd` on a copy of the disk. It
  reads serialized programs ("CFZ1") over `/dev/hvc0`, runs each in a fresh child with rlimits and a timeout and
  answers ("CFR1").
- `syscalls.py`: descriptions generated from the kernel system call table plus hand-written argument types.
- `prog.py`: program model, generation, mutation, wire format and C reproducer output.
- `vm.py`: QEMU driver. The disk is a qcow2 overlay; a clean snapshot is taken after boot and restored with
  `loadvm` (about 0.1 s).
- `fuzz.py`: main loop, corpus, edge-count bucket novelty, crash detection and dedup, minimisation.

## Running

    flock /tmp/coal-testkit/slots/0 python3 tools/fuzz/fuzz.py --build BUILD_DIR --out OUT --seconds 540

BUILD_DIR is a `COAL_PROFILE=test` build. State (corpus, virgin map, stats, crashes) persists in OUT, so repeated
chunks continue the same campaign; keep each chunk at 10 minutes or less per slot. `OUT/coverage.csv` records
seconds, execs, edges, corpus, crashes.

## Findings

A finding is a sanitizer report, a kernel panic, or a hang (no answer from the executor; the guest RIP is
symbolised). The signature is a hash of the report kind, the normalised message and the top non-internal stack
frames. `OUT/crashes/SIG/` holds `report.txt`, `prog.json`, `prog.txt`, and after minimisation `min.json`, `min.txt`
and `repro.c`, a small C program that runs the calls.
