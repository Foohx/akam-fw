#!/usr/bin/env bash
# Build the static ARM time64 probe with the pinned Zig toolchain.
#   tests/native/build_probe.sh [output-file]
# Default output: build/native/time64_probe (git-ignored).
# The probe is linked with src/common/time32_shim.c, like every Akam binary.
# PROBE_SHIM=0 builds the comparison binary with stock musl behaviour instead
# (its "LIBC" checks then show what the camera did before the shim existed).
# PROBE_CFLAGS adds compiler flags (e.g. -DPROBE_SIMULATE_NOOP64).
# The output is stripped; PROBE_KEEP_SYMBOLS=1 keeps the symbols (for the
# disassembly checks in tests/time32_shim_test.sh).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# shellcheck source=tools/build/env.sh
. "$root/tools/build/env.sh"

out="${1:-$root/build/native/time64_probe}"
mkdir -p "$(dirname "$out")"
srcs=("$here/time64_probe.c")
if [ "${PROBE_SHIM:-1}" != 0 ]; then
    srcs+=("$root/src/common/time32_shim.c")
fi
# shellcheck disable=SC2086  # AKAM_CC is a command line by design
$AKAM_CC -static -Os -mfloat-abi=soft -Wall -Wextra ${PROBE_CFLAGS:-} -o "$out" "${srcs[@]}"
if [ "${PROBE_KEEP_SYMBOLS:-0}" != 1 ]; then
    "$ZIG" objcopy --strip-all "$out" "$out.stripped"
    mv -f "$out.stripped" "$out"
fi
echo "build_probe: built $out ($(stat -c %s "$out") bytes, shim $([ "${PROBE_SHIM:-1}" != 0 ] && echo linked || echo NOT linked))"
