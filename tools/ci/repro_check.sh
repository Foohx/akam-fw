#!/usr/bin/env bash
# Builds the 1080p ak_rtsp reproducibly (make -C src/ak_rtsp repro) and compares its stripped sha256 with
# tests/golden/ak_rtsp_1080p.sha256. Exit 0 = identical. Needs zig 0.14.1 on PATH (or ZIG=/path/to/zig) and the
# SmolRTSP checkout in src/smolrtsp. Used to prove that a pure file move does not change the binary.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GOLDEN=$REPO/tests/golden/ak_rtsp_1080p.sha256
want=$(grep -v '^#' "$GOLDEN" | awk 'NF{print $1; exit}')
make -C "$REPO/src/ak_rtsp" repro ${ZIG:+ZIG="$ZIG"} >&2
got=$(sha256sum "$REPO/src/ak_rtsp/ak_rtsp.repro" | cut -d' ' -f1)
echo "golden $want"
echo "built  $got"
[ "$got" = "$want" ] || { echo "repro_check: HASH DIFFERS" >&2; exit 1; }
echo "repro_check: identical"
