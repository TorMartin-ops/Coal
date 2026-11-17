#!/bin/sh
# Runs the conformance corpus and the syscall gap analysis and prints a short summary.
# Usage: report.sh [--boot]     --boot adds one boot timeline run.
# BUILD_DIR selects the CMake build directory (default /tmp/coal-harness-build).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-/tmp/coal-harness-build}
LOGS=$(mktemp -d)
trap 'rm -rf "$LOGS"' EXIT

echo "== conformance (host vs Coal) =="
python3 "$HERE/conformance/run.py" --build-dir "$BUILD_DIR"

echo
echo "== syscall gap (strace on this host vs Coal's table) =="
python3 "$HERE/syscall_gap.py" --collect "$LOGS" --top 10

if [ "${1:-}" = "--boot" ]; then
    echo
    echo "== boot timeline =="
    python3 "$HERE/boottime.py" --build-dir "$BUILD_DIR"
fi
