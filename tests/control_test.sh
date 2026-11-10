#!/usr/bin/env bash
# Host test of the control port 8091 (src/ak_rtsp/control.c + ctl_gate.c): read-only gate, DEBUG with an
# injected clock, 4 client slots, golden read output, LOG forwarding that never blocks, `ak_rtsp ctl`.
# control.c is built for real against stub modules and talked to over TCP on port 18091. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -DCONTROL_PORT=18091 -DCTL_IDLE_REMOTE_S=2 -DCTL_IDLE_TICK_S=1 \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/control_test" \
    "$REPO/tests/native/control_test.c" "$REPO/src/ak_rtsp/control.c" "$REPO/src/ak_rtsp/ctl_gate.c"
# The server tees stdout (the flood of LOG lines goes through it): keep the noise in a file.
if "$WORK/control_test" >"$WORK/out.txt" 2>&1; then rc=0; else rc=$?; fi
grep -E '^(FAIL|skip:|control_test:)|ERROR|runtime error' "$WORK/out.txt" || true
exit "$rc"
