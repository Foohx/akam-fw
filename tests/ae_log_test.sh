#!/usr/bin/env bash
# Log volume of ae.c: the per-second "[ae] window" line is for DEBUG mode only; by default one summary per minute
# (the first window is always logged). Real ae.c, stubbed ISP. Takes about 7 s.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
SRC=$REPO/src/ak_rtsp
fails=0
check() { if "${@:2}"; then echo "  ok   - $1"; else echo "FAIL: $1"; fails=$((fails + 1)); fi; }

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -I"$SRC" -o "$WORK/ae_log" \
    "$REPO/tests/native/ae_log_test.c" "$SRC/ae.c" "$SRC/video_cfg.c" -pthread

"$WORK/ae_log" >"$WORK/quiet.txt"
"$WORK/ae_log" debug >"$WORK/debug.txt"
q=$(grep -c '^\[ae\] window' "$WORK/quiet.txt" || true)
d=$(grep -c '^\[ae\] window' "$WORK/debug.txt" || true)
check "default: one window line in 3.4 s (got $q)" test "$q" -eq 1
check "DEBUG on: one window line per second (got $d)" test "$d" -ge 3
if [ "$fails" -ne 0 ]; then echo "ae_log_test: $fails failed"; exit 1; fi
echo "ae_log_test: all passed"
