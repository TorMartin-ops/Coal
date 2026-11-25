# Kernel sanitizers (debug kernel)

The debug kernel is built with `-DCOAL_DEBUG_KERNEL=ON`. The `test` profile (`-DCOAL_PROFILE=test`) turns it on by
default; the release profile leaves it off and builds the same binary as before.

## What it adds

| Check | Report prefix | Cmdline switch |
|---|---|---|
| UBSan (`__ubsan_handle_*`) | `UBSAN:` | `coal.ubsan=panic` |
| Heap: red zones, poison, double free, quarantine, KFENCE | `KSAN:` | `coal.san=panic` |
| Lock checker | `LOCKDEP:` | `coal.lockdep=panic` |
| Stack protector (random canary), stack guard pages | panic | always |

Default is WARN: one report with file:line and a backtrace on the serial console, then the kernel continues.
`panic` halts after the report. The debug ISO is booted with all three set to `panic` (`COAL_CMDLINE`).

Backtraces follow frame pointers and are symbolised from a table embedded at build time (`scripts/gen_ksyms.py`;
the kernel is linked twice and a post-build step checks that no function moved).

## Heap hardening

Every slab object has a trailer holding a red zone, the requested size, a state and the allocation and free
backtraces; a front red zone follows the previous object. Free checks both red zones, the slack fill and the state
(double free, interior free). Freed memory is filled with 0x6b and checked when it is handed out again; new memory is
filled with 0xa5. Freed slab objects wait in a 256-entry quarantine; large blocks in a 32-entry one. Freed buddy
pages are poisoned and checked on the next allocation. Freeing a page that is the page directory loaded in CR3 is
reported.

KFENCE: `coal.kfence=N` samples one allocation in N into a pool of guard-paged slots (255). Overflow, underflow and
use after free fault and are reported with the allocation and free stacks. It is off by default because pool objects
are not in the direct map and cannot be used for DMA (virtio-blk refuses them); the suite and the debug ISO enable
it with `coal.kfence=100`.

## Lock checker

Spinlock classes are the init site (file:line) or the address for static locks. Reported: order inversion (cycle in
the class graph), recursive acquire, release of a lock not held, sleeping (mutex, uaccess, `schedule()`) with a
lock held or in interrupt context, locks still held at system call exit.

## Running

    flock /tmp/coal-testkit/slots/0 python3 tools/sanitizers/suite.py BUILD_DIR      # whole test set, JSON result
    flock /tmp/coal-testkit/slots/0 python3 tools/sanitizers/selftest.py BUILD_DIR   # 21 deliberate bugs

The suite runs kernel_test, fault_test, syscall_test, libc_test, fs_stress, applet_test, sh_test and memtest and
lists every sanitizer report found in the kernel log. The self-test uses the debug-only system call 0x1000 (user
program `ksan_test OP`) to trigger each detector and checks the report text.
