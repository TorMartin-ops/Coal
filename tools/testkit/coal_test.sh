#!/bin/sh
# Build Coal OS and boot it once; the old quick check, now a thin wrapper over coaltest.
# Usage: coal_test.sh   (any arguments are passed on to coaltest after the test name)
# The boot is deterministic (icount, fixed seed, admission slot) and ends at init; for more use `coaltest`.
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/coaltest" hello "$@"
