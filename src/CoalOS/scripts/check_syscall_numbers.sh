#!/bin/sh
# Check include/libc/syscall_numbers.h against the Linux i386 syscall numbers.
# Every SYS_FOO below 0x1000 must equal __NR_foo from asm/unistd_32.h; Coal-only
# numbers must be >= 0x1000. Usage: scripts/check_syscall_numbers.sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
ref=$(for d in /usr/include/asm /usr/include/x86_64-linux-gnu/asm /usr/include/i386-linux-gnu/asm; do
          [ -f "$d/unistd_32.h" ] && echo "$d/unistd_32.h" && break
      done || true)
[ -n "$ref" ] || { echo "skip: asm/unistd_32.h not found" >&2; exit 0; }
status=0
while read -r name num; do
    [ -n "$num" ] && [ "$num" -lt 4096 ] || continue
    lower=$(echo "${name#SYS_}" | tr 'A-Z' 'a-z')
    want=$(sed -nE "s,^#define __NR_?_?$lower[ \t]+([0-9]+).*,\1,p" "$ref" | head -n 1)
    if [ "$want" != "$num" ]; then
        echo "error: $name is $num, Linux has ${want:-nothing}" >&2
        status=1
    fi
done <<EOT
$(sed -nE 's,^#define[ \t]+(SYS_[A-Z_0-9]+)[ \t]+(0x[0-9a-fA-F]+|[0-9]+).*,\1 \2,p' "$root/include/libc/syscall_numbers.h" | while read -r n v; do echo "$n $((v))"; done)
EOT
[ $status -eq 0 ] && echo "syscall numbers match Linux"
exit $status
