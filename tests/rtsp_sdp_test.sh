#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtsp_sdp.c (SDP golden bytes, backchannel negotiation: Require parsing, 551 decision, track ids).
# Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_sdp_test" \
    "$REPO/tests/native/rtsp_sdp_test.c" "$REPO/src/ak_rtsp/rtsp_sdp.c"
"$WORK/rtsp_sdp_test"
