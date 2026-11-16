#!/usr/bin/env bash
# Host test of the 1080p NR / sharpening knobs (src/ak_rtsp/isp_nr.c composers, isp_nr_ctl.c senders, the SHARP order
# in isp.c) against a fake ISP driver with vendor-like tables. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# no sanitizer: the test replaces ioctl()
"$CC" -std=gnu99 -Wall -Wextra -Werror -DFIXTURE_DIR="\"$REPO/tests/fixtures\"" -I"$REPO/src/ak_rtsp" -I"$REPO/src/common/net" -I"$REPO/src/common/logic" \
    -o "$WORK/isp_nr_test" \
    "$REPO/tests/native/isp_nr_test.c" "$REPO/src/ak_rtsp/isp.c" "$REPO/src/ak_rtsp/isp_blocks.c" \
    "$REPO/src/ak_rtsp/isp_nr.c" "$REPO/src/ak_rtsp/isp_nr_ctl.c" -lpthread -lm
"$WORK/isp_nr_test"
