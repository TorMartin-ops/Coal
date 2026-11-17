#!/bin/bash
# Start Coal OS in QEMU from a built ISO and disk image.
#
# Usage: start_qemu.sh [--debug] <kernel.iso> [disk.img]
#
# Relative paths are taken relative to the current directory. If disk.img is
# not given, the file of that name next to the ISO is used. Serial output is
# written to qemu_output.log next to the ISO.
#
# With --debug, QEMU waits for gdb on localhost:1234 before it starts.
# For the normal case, "cmake --build <dir> --target run" does the same job.

set -eu

debug=0
if [ "${1:-}" = "--debug" ]; then
    debug=1
    shift
fi

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "Usage: $0 [--debug] <kernel.iso> [disk.img]" >&2
    exit 1
fi

iso=$(realpath -m -- "$1")
if [ $# -eq 2 ]; then
    disk=$(realpath -m -- "$2")
else
    disk="$(dirname "$iso")/disk.img"
fi
log="$(dirname "$iso")/qemu_output.log"

[ -f "$iso" ]  || { echo "Error: ISO not found: $iso" >&2; exit 1; }
[ -f "$disk" ] || { echo "Error: disk image not found: $disk" >&2; exit 1; }

args=(-boot d -cdrom "$iso"
      -drive "file=$disk,format=raw,index=1,media=disk"
      -m 128M -serial "file:$log")
if [ $debug -eq 1 ]; then
    args+=(-S -gdb tcp::1234 -nographic)
fi

echo "Starting QEMU: iso=$iso disk=$disk log=$log"
exec qemu-system-x86_64 "${args[@]}"
