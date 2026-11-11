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

"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -DCONTROL_PORT=18091 -DCTL_IDLE_REMOTE_S=2 -DCTL_IDLE_TICK_S=1 "-DISP_TUNE_FILE=\"$WORK/picture\"" \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/control_test" \
    "$REPO/tests/native/control_test.c" "$REPO/src/ak_rtsp/control.c" "$REPO/src/ak_rtsp/ctl_gate.c" "$REPO/src/ak_rtsp/isp_tune.c" "$REPO/src/ak_rtsp/swdog.c"
# The server tees stdout (the flood of LOG lines goes through it): keep the noise in a file.
if "$WORK/control_test" >"$WORK/out.txt" 2>&1; then rc=0; else rc=$?; fi
grep -E '^(FAIL|skip:|control_test:)|ERROR|runtime error' "$WORK/out.txt" || true

# G02: the same program again with a stdout that nobody reads and that is full after 64 KiB (the
# serial console that stalls). Nothing may hang (timeout = failure) and log.dropped must count the loss.
mkfifo "$WORK/stall"
exec 4<>"$WORK/stall" # holds the read end open without ever reading it
if CTL_TEST_STALLED_STDOUT=1 timeout 120 "$WORK/control_test" >"$WORK/stall" 2>"$WORK/err2.txt"; then rc2=0; else rc2=$?; fi
exec 4>&-
grep -E '^(FAIL|skip:)|ERROR|runtime error' "$WORK/err2.txt" || true
if [ "$rc2" -ne 0 ]; then echo "FAIL: control_test with a stalled stdout exited $rc2 (124 = hang)"; rc=1; else echo "  ok   - stalled stdout: nothing blocked, loss counted"; fi
exit "$rc"
