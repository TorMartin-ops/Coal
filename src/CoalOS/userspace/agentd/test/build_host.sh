#!/bin/sh
# Builds agentd as a static 32-bit Linux binary for testing against a pty.
set -e
cd "$(dirname "$0")"
gcc -m32 -static -nostdlib -ffreestanding -fno-pie -no-pie -fno-stack-protector \
    -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra -Werror \
    -o "${1:-/tmp/agentd-host}" ../agentd.c host_start.c
