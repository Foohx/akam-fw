#!/usr/bin/env bash
# Host test of src/ak_rtsp/h264_params.c (SPS/PPS extraction, base64, SDP
# fmtp builder). Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/h264_params_test" \
    "$REPO/tests/native/h264_params_test.c" "$REPO/src/ak_rtsp/h264_params.c"
"$WORK/h264_params_test"
