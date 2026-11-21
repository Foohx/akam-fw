#!/usr/bin/env bash
# Host test of src/net/rtsp/rtsp_sdp.c (SDP golden bytes, backchannel negotiation: Require parsing, 551 decision, track ids).
# Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -fno-sanitize-recover=undefined -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/rtsp_sdp_test" \
    "$REPO/tests/native/rtsp_sdp_test.c" "$REPO/src/net/rtsp/rtsp_sdp.c"
"$WORK/rtsp_sdp_test"
