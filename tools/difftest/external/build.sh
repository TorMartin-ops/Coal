#!/bin/sh
# Builds the external test suites for Coal OS (static x86_64 ELF, musl) and caches the sources.
#
#   build.sh [OUTDIR]        OUTDIR defaults to build/difftest-external under the repository
#
# Sources are cached under $COAL_DIFFTEST_CACHE (default ~/.cache/coal-difftest) and are pinned or
# fetched with --depth 1. Needs: git, make, autoconf, automake, musl-gcc (package musl-tools), strip.
#   OUTDIR/ltp/<syscall>/<test>   LTP syscalls tests listed in ltp-dirs.txt (musl, static, stripped)
#   OUTDIR/libctest/<area>/<test> musl libc-test (only when the official repository is reachable)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
cache=${COAL_DIFFTEST_CACHE:-$HOME/.cache/coal-difftest}
out=$(mkdir -p "${1:-$repo/build/difftest-external}" && cd "${1:-$repo/build/difftest-external}" && pwd)
jobs=$(nproc 2>/dev/null || echo 2)
LTP_REV=c4be3107083cd11e4732884c4c8c5e9c6665e729
LTP_URL=https://github.com/linux-test-project/ltp
LIBCTEST_URL=https://git.musl-libc.org/git/libc-test
mkdir -p "$cache"

# musl-gcc hides the kernel headers LTP needs; /usr/include is searched last, after musl's own.
cflags="-O1 -w -idirafter /usr/include -idirafter /usr/include/$(gcc -dumpmachine)"

build_ltp() {
    src=$cache/ltp
    if [ ! -d "$src/.git" ]; then
        git init -q "$src"
        git -C "$src" remote add origin "$LTP_URL"
    fi
    git -C "$src" cat-file -e "$LTP_REV^{commit}" 2>/dev/null || git -C "$src" fetch -q --depth 1 origin "$LTP_REV"
    git -C "$src" checkout -q "$LTP_REV"
    if [ ! -f "$src/lib/libltp.a" ]; then
        (cd "$src" && make autotools >/dev/null 2>&1 &&
         CC="musl-gcc -static" CFLAGS="$cflags" LDFLAGS=-static ./configure >/dev/null 2>&1 &&
         make -C lib -j"$jobs" >/dev/null 2>&1 && make -C libs -j"$jobs" >/dev/null 2>&1) || { echo "LTP library build failed" >&2; return 1; }
    fi
    rm -rf "$out/ltp"
    for d in $(grep -v '^#' "$here/ltp-dirs.txt"); do
        dir=$src/testcases/kernel/syscalls/$d
        [ -d "$dir" ] || continue
        make -C "$dir" -k -j"$jobs" >/dev/null 2>&1 || true
        mkdir -p "$out/ltp/$d"
        for f in "$dir"/*; do
            [ -f "$f" ] && [ -x "$f" ] || continue
            case $f in *.sh|*.c|*.h|*.o|*Makefile) continue ;; esac
            strip -s -o "$out/ltp/$d/$(basename "$f")" "$f" 2>/dev/null || cp "$f" "$out/ltp/$d/"
        done
        # test data some tests read from their working directory
        [ -d "$dir/datafiles" ] && cp -r "$dir/datafiles" "$out/ltp/$d/" || true
        rmdir "$out/ltp/$d" 2>/dev/null || true
    done
    echo "ltp: $(find "$out/ltp" -type f | wc -l) test programs in $out/ltp"
}

# libc-test is compiled directly: each src/{functional,regression,math,musl} program is linked with the
# helpers in src/common (print.c and friends), static against musl.
build_libctest() {
    src=$cache/libc-test
    if [ ! -d "$src/.git" ]; then
        git clone -q --depth 1 "$LIBCTEST_URL" "$src" 2>/dev/null ||
            { echo "libc-test: cannot clone $LIBCTEST_URL (not reachable from here); skipped" >&2; return 1; }
    fi
    rm -rf "$out/libctest"
    common=$(mktemp -d)
    for f in "$src"/src/common/*.c; do
        case $f in */runtest.c) continue ;; esac
        musl-gcc -static -pthread -O1 -w -I"$src/src/common" -c "$f" -o "$common/$(basename "$f").o" 2>/dev/null || true
    done
    for area in functional regression math musl; do
        mkdir -p "$out/libctest/$area"
        for t in "$src"/src/$area/*.c; do
            [ -f "$t" ] || continue
            n=$(basename "$t" .c)
            musl-gcc -static -pthread -O1 -w -D_GNU_SOURCE -I"$src/src/common" -o "$out/libctest/$area/$n" "$t" "$common"/*.o -lm 2>/dev/null &&
                strip -s "$out/libctest/$area/$n" || rm -f "$out/libctest/$area/$n"
        done
    done
    rm -rf "$common"
    echo "libc-test: $(find "$out/libctest" -type f | wc -l) test programs in $out/libctest"
}

what=${2:-all}
status=0
case $what in all|ltp) build_ltp || status=1 ;; esac
case $what in all|libctest) build_libctest || status=1 ;; esac
exit $status
