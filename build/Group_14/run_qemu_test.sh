#!/bin/bash
cd /workspaces/2025-ikt218-osdev/build/Group_14
echo "Starting QEMU..."
qemu-system-i386 \
    -m 64 \
    -cdrom kernel.iso \
    -drive file=disk.img,if=ide,index=1,format=raw \
    -serial file:qemu_output_test.log \
    -display none \
    -no-reboot \
    -no-shutdown &

QEMU_PID=$!
echo "QEMU started with PID $QEMU_PID"

# Wait for QEMU to run
sleep 10

# Kill QEMU
echo "Stopping QEMU..."
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null

echo "QEMU stopped. Checking output..."
cat qemu_output_test.log