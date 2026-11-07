#!/usr/bin/env bash
# Build the static ARM clock probe with the pinned Zig toolchain.
#   tests/native/build_probe.sh [output-file]
# Default output: build/native/time_probe (git-ignored).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# shellcheck source=tools/build/env.sh
. "$root/tools/build/env.sh"

out="${1:-$root/build/native/time_probe}"
mkdir -p "$(dirname "$out")"
# shellcheck disable=SC2086  # CAMYN_CC is a command line by design
$CAMYN_CC -static -Os -mfloat-abi=soft -Wall -Wextra -o "$out" "$here/time_probe.c"
"$ZIG" objcopy --strip-all "$out" "$out.stripped"
mv -f "$out.stripped" "$out"
echo "build_probe: built $out ($(stat -c %s "$out") bytes)"
