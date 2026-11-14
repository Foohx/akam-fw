#!/usr/bin/env bash
# Host-side test of the Dropbear clock patch and the ARM time64 probe:
#  - every patch in tools/dropbear/patches applies cleanly (fuzz 0) to the
#    pinned Dropbear archive and leaves no raw time syscall in the sources;
#  - the time64 probe builds with the pinned Zig and, when qemu-arm is installed,
#    runs (qemu-user on a time64 host has a vDSO and clock_gettime64, so this
#    only checks that the probe works, it does NOT reproduce the 4.4 kernel).
# Skips (exit 0) when the bootstrap has not been run. Nothing touches hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tools/build/deps.lock
. "$REPO/tools/build/deps.lock"
ARCHIVE=$REPO/tools/.cache/$(basename "$DROPBEAR_URL")
if [ ! -f "$ARCHIVE" ]; then
    echo "skip: $ARCHIVE missing (run tools/build/bootstrap.sh)"
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}

echo "== patches apply to the pinned Dropbear $DROPBEAR_VERSION"
mkdir "$WORK/src"
tar -xjf "$ARCHIVE" -C "$WORK/src" --strip-components=1
n=0
for p in "$REPO"/tools/dropbear/patches/*.patch; do
    n=$((n + 1))
    check "$(basename "$p") applies cleanly" bash -c "cd '$WORK/src' && patch -p1 --forward --fuzz=0 -s <'$p'"
done
check "at least one patch present" test "$n" -ge 1
check "tick based clock present in dbutil.c" grep -q akam_ticks_now "$WORK/src/src/dbutil.c"
check "no raw time syscall left" bash -c "! grep -nE 'syscall *\\( *SYS_(clock_gettime|gettimeofday|time)' '$WORK'/src/src/*.c"

echo "== clock probe"
if [ ! -x "$REPO/tools/.toolchain/zig-$ZIG_VERSION-x86_64-linux/zig" ]; then
    echo "  skip - zig not installed"
else
    check "probe builds" "$REPO/tests/native/build_probe.sh" "$WORK/time64_probe"
    if command -v qemu-arm >/dev/null 2>&1; then
        check "probe runs under qemu-arm: libc and raw legacy syscall agree" bash -c "
            out=\$(qemu-arm '$WORK/time64_probe') || exit 1
            a=\$(printf '%s\n' \"\$out\" | sed -n 's/^  libc  MONOTONIC .* sec=\\([0-9]*\\) .*/\\1/p' | head -1)
            b=\$(printf '%s\n' \"\$out\" | sed -n 's/^  raw263 MONOTONIC .* sec=\\([0-9]*\\) .*/\\1/p' | head -1)
            [ -n \"\$a\" ] && [ -n \"\$b\" ] && [ \$((a - b)) -le 2 ] && [ \$((b - a)) -le 2 ]"
    else
        echo "  skip - qemu-arm not installed"
    fi
fi

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" = 0 ]
