#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtsp_busy.c (G06: the 503 answer for a second RTSP client) over real loopback sockets.
# Nothing blocks, no fd leaks (AddressSanitizer build).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_busy_test" \
    "$REPO/tests/native/rtsp_busy_test.c" "$REPO/src/ak_rtsp/rtsp_busy.c"
"$WORK/rtsp_busy_test"
