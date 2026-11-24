#!/usr/bin/env bash
# Host test of the 1080p NR / sharpening knobs (platforms/anyka-ak3918/isp_nr.c composers, isp_nr_ctl.c senders, the SHARP order
# in isp.c) against a fake ISP driver with vendor-like tables. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# no sanitizer: the test replaces ioctl()
"$CC" -std=gnu99 -Wall -Wextra -Werror -DFIXTURE_DIR="\"$REPO/tests/fixtures\"" -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/isp_nr_test" \
    "$REPO/tests/native/isp_nr_test.c" "$REPO/platforms/anyka-ak3918/isp.c" "$REPO/platforms/anyka-ak3918/isp_blocks.c" \
    "$REPO/platforms/anyka-ak3918/isp_nr.c" "$REPO/platforms/anyka-ak3918/isp_nr_ctl.c" "$REPO/platforms/anyka-ak3918/isp_wdr.c" -lpthread -lm
"$WORK/isp_nr_test"
