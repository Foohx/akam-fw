#!/usr/bin/env bash
# Host test of src/ak_rtsp/isp_blocks.c (ISP block table, sensor_isp.conf loader, live-versus-file
# comparison, ISPB records) against a synthetic conf file and a fake ISP driver. No camera.
# AKAM_ISP_CONF_REAL=/path/to/isp_gc20c3.conf also checks the table against a real file (local only).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/isp_blocks_test" \
    "$REPO/tests/native/isp_blocks_test.c" "$REPO/src/ak_rtsp/isp_blocks.c"
"$WORK/isp_blocks_test"
