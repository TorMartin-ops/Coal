#!/bin/bash
INPUT_ISO_PATH=$1 # Take ISO path as argument

# --- Determine path to disk.img relative to ISO path ---
ISO_DIR=$(dirname "$INPUT_ISO_PATH")
INPUT_DISK_PATH="$ISO_DIR/disk.img"
if [ "$ISO_DIR" == "." ]; then
    INPUT_DISK_PATH="./disk.img"
fi

# Basic check for arguments
if [ -z "$INPUT_ISO_PATH" ]; then
  echo "Usage: $0 <path/to/kernel.iso>"
  exit 1
fi

# Determine the script's own directory absolutely
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Function to resolve path (handles both relative and absolute input)
resolve_path() {
  local input_path="$1"
  local base_dir="$2"
  if [[ "$input_path" == /* ]]; then
    readlink -f "$input_path"
  else
    readlink -f "$base_dir/$input_path"
  fi
}

# Calculate the absolute paths for ISO and DISK
ABS_ISO_PATH=$(resolve_path "$INPUT_ISO_PATH" "$SCRIPT_DIR")
ABS_DISK_PATH=$(resolve_path "$INPUT_DISK_PATH" "$SCRIPT_DIR")

# Check if files exist
if [ ! -f "$ABS_ISO_PATH" ]; then
  echo "Error: ISO file not found at: $ABS_ISO_PATH"
  exit 1
fi
if [ ! -f "$ABS_DISK_PATH" ]; then
  echo "Error: Disk image file not found at: $ABS_DISK_PATH"
  exit 1
fi

# Create log file path in the same directory as the ISO
LOG_FILE="$ISO_DIR/qemu_output_no_debug.log"

echo "Starting QEMU (no debug) with ISO: $ABS_ISO_PATH and Disk: $ABS_DISK_PATH"
echo "Serial output will be logged to: $LOG_FILE"
echo "Press Ctrl+A then X to exit QEMU"

# Start QEMU without debugging flags
qemu-system-i386 -boot d \
                 -cdrom "$ABS_ISO_PATH" \
                 -drive file="$ABS_DISK_PATH",format=raw,index=1,media=disk \
                 -m 1024 \
                 -serial file:"$LOG_FILE" \
                 -monitor stdio \
                 -audiodev none,id=none1 -machine pcspk-audiodev=none1