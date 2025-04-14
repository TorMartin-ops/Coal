#!/bin/bash
INPUT_KERNEL_PATH=$1 # Use a different name for the input argument
INPUT_DISK_PATH=$2

# Basic check for arguments
if [ -z "$INPUT_KERNEL_PATH" ] || [ -z "$INPUT_DISK_PATH" ]; then
  echo "Usage: $0 <kernel_path> <disk_path>"
  exit 1
fi

# Determine the script's own directory absolutely
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Function to resolve path (handles both relative and absolute input)
resolve_path() {
  local input_path="$1"
  local base_dir="$2"
  # Check if input is potentially an absolute path
  if [[ "$input_path" == /* ]]; then
    # Use readlink -f directly on the absolute path
    readlink -f "$input_path"
  else
    # Input is relative, resolve it relative to base_dir
    readlink -f "$base_dir/$input_path"
  fi
}

# Calculate the absolute paths using the helper function
ABS_KERNEL_PATH=$(resolve_path "$INPUT_KERNEL_PATH" "$SCRIPT_DIR")
ABS_DISK_PATH=$(resolve_path "$INPUT_DISK_PATH" "$SCRIPT_DIR")

# Check if readlink failed (returned empty string)
if [ -z "$ABS_KERNEL_PATH" ]; then
  echo "Error: Failed to resolve kernel path: $INPUT_KERNEL_PATH"
  exit 1
fi
 if [ -z "$ABS_DISK_PATH" ]; then
  echo "Error: Failed to resolve disk path: $INPUT_DISK_PATH"
  exit 1
fi


echo "DEBUG: Script directory: $SCRIPT_DIR"
echo "DEBUG: Input Kernel Path Arg: $INPUT_KERNEL_PATH"
echo "DEBUG: Calculated Absolute Kernel Path: $ABS_KERNEL_PATH"
echo "DEBUG: Input Disk Path Arg: $INPUT_DISK_PATH"
echo "DEBUG: Calculated Absolute Disk Path: $ABS_DISK_PATH"

# Check if files exist using the calculated ABSOLUTE paths
if [ ! -f "$ABS_KERNEL_PATH" ]; then
  echo "Error: Kernel file not found at calculated absolute path: $ABS_KERNEL_PATH"
  exit 1
fi
echo "DEBUG: Kernel file check passed using absolute path."

if [ ! -f "$ABS_DISK_PATH" ]; then
  echo "Error: Disk image not found at calculated absolute path: $ABS_DISK_PATH"
  exit 1
fi
echo "DEBUG: Disk file check passed using absolute path."


# Start QEMU using the ABSOLUTE paths for robustness
# Use -cdrom for the ISO image based on previous findings
echo "Starting QEMU (paused, listening for GDB on tcp::1234)..."
# Ensure you are passing the correct file (kernel.iso) as the first argument if using -cdrom
qemu-system-i386 -S -gdb tcp::1234 \
                 -cdrom "$ABS_KERNEL_PATH" \
                 -hda "$ABS_DISK_PATH" \
                 -m 64 \
                 -audiodev sdl,id=sdl1,out.buffer-length=40000 -machine pcspk-audiodev=sdl1 \
                 -serial file:"$SCRIPT_DIR/qemu_output.log" & # Log file in script dir
QEMU_PID=$!

# Check if QEMU started successfully
sleep 1 # Give QEMU a moment to start or fail
if ! kill -0 $QEMU_PID 2>/dev/null; then
    echo "Error: QEMU failed to start."
    exit 1
fi
echo "QEMU started with PID $QEMU_PID. Waiting for GDB connection and 'continue' command..."

# Function to handle termination signals (Ctrl+C, VS Code Stop)
cleanup() {
    echo "Signal received. Stopping QEMU (PID $QEMU_PID)..."
    # Send SIGTERM first, then SIGKILL if needed
    kill $QEMU_PID 2>/dev/null
    sleep 0.5 # Shorter sleep
    if kill -0 $QEMU_PID 2>/dev/null; then
        echo "QEMU sending SIGKILL..."
        kill -9 $QEMU_PID 2>/dev/null
    fi
    echo "QEMU stopped."
    # Optional: Display log
    if [ -f "$SCRIPT_DIR/qemu_output.log" ]; then
        echo "--- QEMU Output Log ($SCRIPT_DIR/qemu_output.log) ---"
        cat "$SCRIPT_DIR/qemu_output.log"
        echo "-----------------------------------------"
    fi
    exit 0
}

# Trap signals for cleanup
trap cleanup SIGINT SIGTERM EXIT # Use EXIT too

# Wait indefinitely for the QEMU process to end on its own,
# or until the script receives a signal (SIGINT/SIGTERM).
# This prevents the script from killing QEMU just because GDB disconnects.
wait $QEMU_PID
QEMU_EXIT_STATUS=$?
echo "QEMU process (PID $QEMU_PID) ended naturally with status $QEMU_EXIT_STATUS."

# The EXIT trap will call cleanup automatically here