#!/bin/sh
# Builds mcpd as a static 32-bit Linux binary for testing against a pty.
set -e
cd "$(dirname "$0")"
gcc -m32 -static -nostdlib -ffreestanding -fno-pie -no-pie -fno-stack-protector \
    -O2 -Wall -Wextra -Werror -o "${1:-/tmp/mcpd-host}" ../mcpd.c host_start.c
