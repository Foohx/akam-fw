#!/usr/bin/env bash
# Builds the 1080p akamd reproducibly (make -C platforms/anyka-ak3918 repro) and compares its stripped sha256 with
# tests/golden/lsc-indoor-1080p-ak3918.sha256. Exit 0 = identical. Needs zig 0.14.1 on PATH (or ZIG=/path/to/zig) and the
# SmolRTSP checkout in third_party/smolrtsp. Used to prove that a pure file move does not change the binary.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GOLDEN=$REPO/tests/golden/lsc-indoor-1080p-ak3918.sha256
want=$(grep -v '^#' "$GOLDEN" | awk 'NF{print $1; exit}')
make -C "$REPO/platforms/anyka-ak3918" repro ${ZIG:+ZIG="$ZIG"} >&2
got=$(sha256sum "$REPO/build/lsc-indoor-1080p-ak3918/akamd.repro" | cut -d' ' -f1)
echo "golden $want"
echo "built  $got"
[ "$got" = "$want" ] || { echo "repro_check: HASH DIFFERS" >&2; exit 1; }
echo "repro_check: identical"
