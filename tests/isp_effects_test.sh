#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/isp.c picture values across a pipeline reload (day/night switch). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# no sanitizer: the test replaces ioctl()
"$CC" -std=gnu99 -Wall -Wextra -Werror -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/isp_effects_test" \
    "$REPO/tests/native/isp_effects_test.c" "$REPO/platforms/anyka-ak3918/isp.c" "$REPO/platforms/anyka-ak3918/isp_blocks.c" "$REPO/platforms/anyka-ak3918/isp_nr.c" "$REPO/platforms/anyka-ak3918/isp_nr_ctl.c" "$REPO/platforms/anyka-ak3918/isp_wdr.c" -lpthread
"$WORK/isp_effects_test"
