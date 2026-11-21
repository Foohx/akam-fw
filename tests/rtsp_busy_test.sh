#!/usr/bin/env bash
# Host test of src/net/rtsp/rtsp_busy.c (G06: the 503 answer for a second RTSP client) over real loopback sockets.
# Nothing blocks, no fd leaks (AddressSanitizer build).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/rtsp_busy_test" \
    "$REPO/tests/native/rtsp_busy_test.c" "$REPO/src/net/rtsp/rtsp_busy.c"
"$WORK/rtsp_busy_test"
