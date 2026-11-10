#!/usr/bin/env bash
# Host test of src/ak_rtsp/isp.c picture values across a pipeline reload (day/night switch). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# no sanitizer: the test replaces ioctl()
"$CC" -std=gnu99 -Wall -Wextra -Werror -I"$REPO/src/ak_rtsp" \
    -o "$WORK/isp_effects_test" \
    "$REPO/tests/native/isp_effects_test.c" "$REPO/src/ak_rtsp/isp.c" "$REPO/src/ak_rtsp/isp_blocks.c" -lpthread
"$WORK/isp_effects_test"
