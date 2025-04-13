#!/bin/bash
KERNEL_PATH=$1
DISK_PATH=$2

# Basic check for arguments
if [ -z "$KERNEL_PATH" ] || [ -z "$DISK_PATH" ]; then
  echo "Usage: $0 <kernel_path> <disk_path>"
  exit 1
fi

# Check if kernel/disk files exist
if [ ! -f "$KERNEL_PATH" ]; then
  echo "Error: Kernel file not found at $KERNEL_PATH"
  exit 1
fi
if [ ! -f "$DISK_PATH" ]; then
  echo "Error: Disk image not found at $DISK_PATH"
  # Optional: Create disk if it doesn't exist? Requires knowing size/format.
  # echo "Warning: Disk image not found at $DISK_PATH. (Continuing anyway)"
  # exit 1 # Or continue carefully
fi

# Start QEMU in the background, redirecting serial to a file
echo "Starting QEMU, serial output to qemu_output.log"
qemu-system-i386 -S -gdb tcp::1234 -boot d -hda "$KERNEL_PATH" -hdb "$DISK_PATH" -m 64 \
                 -audiodev sdl,id=sdl1,out.buffer-length=40000 -machine pcspk-audiodev=sdl1 \
                 -serial file:qemu_output.log &
QEMU_PID=$!

# Check if QEMU started successfully
sleep 1 # Give QEMU a moment to start or fail
if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "Error: QEMU failed to start."
    exit 1
fi
echo "QEMU started with PID $QEMU_PID"

# Function to check if gdb is running
is_gdb_running() {
    # Use pgrep with more specific matching if possible
    pgrep -f "gdb-multiarch.*1234" > /dev/null
}

# Function to handle termination signals
cleanup() {
    echo "Stopping QEMU (PID $QEMU_PID)..."
    # Send SIGTERM first, then SIGKILL if needed
    kill $QEMU_PID 2>/dev/null
    sleep 1
    if kill -0 $QEMU_PID 2>/dev/null; then
        echo "QEMU did not terminate gracefully, sending SIGKILL..."
        kill -9 $QEMU_PID 2>/dev/null
    fi
    echo "QEMU stopped."
    # Display log file content if requested
    if [ -f "qemu_output.log" ]; then
        echo "--- QEMU Output Log (qemu_output.log) ---"
        cat qemu_output.log
        echo "-----------------------------------------"
    fi
    exit 0
}

# Trap signals for cleanup
trap cleanup SIGINT SIGTERM EXIT

# Wait for gdb to connect
echo "Waiting for gdb connection (listening on tcp::1234)..."
# Instead of checking if gdb process is running,
# let's wait until QEMU resumes execution (which happens after gdb 'continue')
# This is tricky without direct QEMU interaction.
# A simpler approach is to just wait for gdb process to APPEAR.
while ! is_gdb_running; do
    # Check if QEMU exited prematurely
    if ! kill -0 $QEMU_PID 2>/dev/null; then
        echo "QEMU process exited before GDB connected."
        exit 1 # Exit script if QEMU dies
    fi
    sleep 1
done
echo "GDB process detected."

# Monitor the GDB connection / QEMU process
echo "Monitoring GDB/QEMU..."
while kill -0 $QEMU_PID 2>/dev/null; do
    # Check if GDB is still running
    if ! is_gdb_running; then
        echo "GDB process appears to have disconnected/exited."
        break # Exit the monitoring loop if GDB stops
    fi
    sleep 2 # Check less frequently
done

echo "Monitoring loop finished."
# Cleanup will be called automatically due to the EXIT trap