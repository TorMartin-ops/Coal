# virtio

Coal OS speaks virtio 1.x (modern, `VERSION_1` required; legacy and transitional-only devices are
ignored) over two transports. Code: `kernel/drivers/virtio/`, public API
`include/kernel/drivers/virtio/virtio.h`.

| Layer | Files |
| --- | --- |
| Core: feature negotiation, status handshake, IRQ dispatch | `virtio.c` |
| Split virtqueues: power-of-two rings, scatter-gather, optional `EVENT_IDX`, per-queue callbacks | `virtqueue.c` |
| Transports | `virtio_pci.c` (modern capabilities), `virtio_mmio.c` (version 2) |
| Drivers | `virtio_blk.c`, `virtio_console.c`, `virtio_rng.c`, `virtio_vsock.c` (+ `kernel/net/vsock.c`) |

virtio-net is not implemented (the e1000 stays the NIC).

## Kernel command line

Boot with `-kernel kernel.bin -append "..."` (the PVH entry in `boot/boot.asm` shares the 32 to 64 bit trampoline with Multiboot2, so QEMU's direct boot works on `pc` and `microvm`) or edit the GRUB entry.

| Word | Effect |
| --- | --- |
| `root=vda` | Mount the root file system from virtio-blk (`/dev/` prefix optional; default `hda`, the ATA disk) |
| `console=hvc0` | Also send kernel log lines to the first virtio console |
| `virtio_mmio.device=<size>@<base>:<irq>` | Probe a virtio-mmio device (repeatable; `K`/`M` size suffixes; IRQ 16 and above are polled every 1 ms). The command line is limited to 255 bytes |
| `agentd.link=` | `vsock[:PORT]` or a device path such as `/dev/hvc0`; read by agentd from `/proc/cmdline` |

## Drivers

* **virtio-blk**: `vda`..`vdd`, registered like the ATA disk (`block_device_t` with `rw`/`flush`
  operations). Multi-segment requests, `seg_max`/`size_max` splitting, flush.
* **virtio-console**: `/dev/hvc0` (char major 229). Raw byte stream, `poll` support. With
  `console=hvc0` the kernel log also goes there through the emergency-write config field.
* **virtio-rng**: a 64-byte request every 10 s feeds the ChaCha20 pool behind `getrandom(2)`,
  `/dev/random` and `/dev/urandom`.
* **virtio-vsock**: `AF_VSOCK` (40), `SOCK_STREAM` only. `socket/bind/listen/accept/connect/read/write/
  close/poll`, `getsockname/getpeername`, guest CID from the device config, credit flow control
  (64 KiB receive ring per connection), RST on unknown connections and on transport reset, and small
  writes to the same connection are merged into one packet while earlier packets are in flight.
  `socket()` fails with `EAFNOSUPPORT` when no vsock device exists. Other socket types fail with `ESOCKTNOSUPPORT`.

## agentd link

agentd's link is chosen once at start: its first argument, else `agentd.link=` from `/proc/cmdline`,
else `/dev/ttyS1`. `vsock` or `vsock:PORT` listens on the port (default 1024), accepts one host
connection, and when the host disconnects accepts the next and sends a new `hello`. If the guest has no
vsock device agentd falls back to `/dev/ttyS1`. The wire protocol does not change.

## coal-gateway

```sh
coal-gateway stdio --virtio blk,console,rng,vsock=3      # or COAL_VIRTIO=...
```

`--virtio` takes any of `blk`, `console`, `rng`, `vsock[=CID]` (default CID 3). It boots the kernel with
`-kernel`, and QEMU gets a virtio-blk root, a virtconsole, virtio-rng and `vhost-vsock-pci`. The agentd link
is vsock if present, else the console, else COM2. `--attach` also takes `vsock:CID:PORT` and
`hvsock:PATH:PORT` (the unix socket of Firecracker or `vhost-device-vsock`: connect, send `CONNECT port`).
`vhost-vsock-pci` needs `/dev/vhost-vsock` on the host and KVM-class setups (WSL2 with KVM works).

## Tests

* `tests/test_protocol.py`: the full agent protocol suite on `QemuVirtioProtocol` (virtio-blk root,
  agentd on a virtio console) and `QemuVsockProtocol` (agentd over vsock through `vhost-device-vsock`;
  set `COAL_VHOST_DEVICE_VSOCK`, skipped otherwise).
* `tools/coal-mcp/tests/smoke_test.py` with `COAL_VIRTIO=blk,console,rng` is the smoke variant.
* `tools/harness/virtio_bench.py` produces the numbers below.

## Measurements

QEMU TCG on a shared 2-core container, so absolute values are noisy; ratios are what matter.

| agentd link | `exec true` median | write 1 MiB | read 1 MiB |
| --- | --- | --- | --- |
| COM2 | 11.3 ms | 139 KiB/s | 430 KiB/s |
| virtio-console | 6.5 ms | 13 981 KiB/s | 27 523 KiB/s |
| virtio-vsock | 6.7 ms | 17 070 KiB/s | 26 813 KiB/s |

| coalbench (8 MiB, 64 KiB chunks) | ATA DMA | virtio-blk |
| --- | --- | --- |
| `file_read_8m` | 614 MB/s | 758 MB/s |
| `file_write_8m_nosync` | 448 MB/s | 558 MB/s |

The read figure is mostly the page cache; it shows driver overhead, not device speed.

## Verified and not verified

Verified under QEMU 8.2 TCG: virtio-blk root and `/proc/mounts` (PCI and MMIO), virtio-console as agentd link
and kernel log sink, virtio-rng requests completing, vsock listen/accept/read/write/close and reconnect
through `vhost-user-vsock-pci` + `vhost-device-vsock`, the whole protocol suite over each, `-M microvm`
(virtio-mmio blk and console, agentd answering). Not verified: `vhost-vsock-pci` (kernel vhost; no
`/dev/vhost-vsock` here), KVM, `event_idx` negotiated off, vsock with several concurrent connections
under load, virtio on real hardware.

microvm notes: QEMU hands out its 24 virtio-mmio slots from the top, the first device at `0xfeb02e00` (slot 23, IRQ
28), then `0xfeb02c00` and down, 512 bytes each, and they need `-global virtio-mmio.force-legacy=false` (version 2).
Pass one `virtio_mmio.device=512@<base>:<irq>` word per slot in use (empty slots are skipped). Example:

```sh
qemu-system-x86_64 -M microvm -m 128 -display none -kernel kernel.bin -global virtio-mmio.force-legacy=false \
  -append "root=vda agentd.link=/dev/hvc0 virtio_mmio.device=512@0xfeb02e00:28 virtio_mmio.device=512@0xfeb02c00:27" \
  -drive file=disk.img,format=raw,if=none,id=d0 -device virtio-blk-device,drive=d0 \
  -chardev socket,id=c0,path=com2.sock,server=on,wait=off -device virtio-serial-device -device virtconsole,chardev=c0
```

IRQs of 16 and above are polled. microvm has no PCI, no i8042 and no port 0x61; the kernel skips the keyboard
controller when its status port reads 0xFF and calibrates the TSC on PIT channel 0. It boots to agentd in about
10 seconds of TCG time.

## 64-bit notes

Coal runs on x86_64 only. Drivers reach hardware through `dma_alloc_coherent`/`dma_free_coherent`/`dma_phys`/
`dma_reachable`, `ioremap` (a dedicated uncached kernel range at `0xFFFFFFFFFB000000`, 4 KiB pages), `mmio_*`,
`mb()`, `register_int_handler`/`get_int_handler`, `local_irq_save`/`local_irq_restore`, the PCI helpers and
`cmdline_init`. Rings and buffers come from the direct map, so any physical address (also above 4 GiB) is valid for
virtio; 64-bit BARs are followed by `pci_bar_base`.
