#!/bin/sh
# Builds agentd as a static 64-bit Linux binary for testing against a pty.
# COAL_AGENTD_SAN="-fsanitize=undefined -fsanitize-undefined-trap-on-error" builds it with undefined-behaviour traps.
set -e
cd "$(dirname "$0")"
gcc -m64 -static -nostdlib -ffreestanding -fno-pie -no-pie ${COAL_AGENTD_SAN:--fno-stack-protector} \
    -fno-tree-loop-distribute-patterns -O2 -Wall -Wextra -Werror -I../../../include \
    -o "${1:-/tmp/agentd-host}" ../agentd.c host_start.c
