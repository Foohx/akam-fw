#!/usr/bin/env bash
# Host test of src/ak_rtsp/imgq.c (image-quality switches: start-up options, 8091 img.* keys) with
# the ISP layer stubbed. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/imgq_test" \
    "$REPO/tests/native/imgq_test.c" "$REPO/src/ak_rtsp/imgq.c" "$REPO/src/ak_rtsp/isp_blocks.c" "$REPO/src/ak_rtsp/cfg_file.c"
"$WORK/imgq_test" | grep -v -e "^\[isp.dump\]" -e "^\[imgq\]"
