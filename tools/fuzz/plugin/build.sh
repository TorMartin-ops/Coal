#!/bin/sh
# Build the coverage plugin against QEMU's plugin header (fetched once; it is GPL, so it is not kept in the tree).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
version=$(qemu-system-x86_64 --version | sed -n 's/.*version \([0-9][0-9.]*\).*/\1/p' | head -n 1)
if [ ! -f "$here/qemu-plugin.h" ]; then
    curl -fsSL -o "$here/qemu-plugin.h" "https://raw.githubusercontent.com/qemu/qemu/v${version%+*}/include/qemu/qemu-plugin.h"
fi
${CC:-gcc} -O2 -shared -fPIC -Wall -Wextra -Wno-unused-parameter -Werror -I"$here" -o "$here/libcoverage.so" "$here/coverage.c"
echo "$here/libcoverage.so"
