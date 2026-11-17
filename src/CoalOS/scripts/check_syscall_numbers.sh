#!/bin/sh
# Check that include/libc/syscall_numbers.h agrees with the kernel header.
# Usage: scripts/check_syscall_numbers.sh   (from anywhere)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
extract() {
    sed -n 's|^#define[ \t]*\(SYS_[A-Z_0-9]*\)[ \t]*\([0-9]*\).*|\1 \2|p' "$1" | sort
}
extract "$root/include/kernel/cpu/syscall.h" > /tmp/coal_sys_kernel.$$
extract "$root/include/libc/syscall_numbers.h" > /tmp/coal_sys_user.$$
status=0
# Every user number must exist in the kernel header with the same value.
if comm -13 /tmp/coal_sys_kernel.$$ /tmp/coal_sys_user.$$ | grep .; then
    echo "error: the lines above are in syscall_numbers.h but not in the kernel header" >&2
    status=1
fi
rm -f /tmp/coal_sys_kernel.$$ /tmp/coal_sys_user.$$
[ $status -eq 0 ] && echo "syscall numbers match"
exit $status
