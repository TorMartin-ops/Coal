#!/bin/bash
cd /workspaces/2025-ikt218-osdev/build/Group_14
qemu-system-i386 -cdrom kernel.iso -drive file=disk.img,if=ide,index=1,format=raw -m 256M -serial file:qemu_enhanced_output.log -display none -no-reboot -no-shutdown &
QEMU_PID=$!
sleep 8
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true