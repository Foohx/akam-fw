#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/imgq.c (image-quality switches: start-up options, 8091 img.* keys) with
# the ISP layer stubbed. No camera.
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
    -o "$WORK/imgq_test" \
    "$REPO/tests/native/imgq_test.c" "$REPO/platforms/anyka-ak3918/imgq.c" "$REPO/platforms/anyka-ak3918/isp_blocks.c" "$REPO/platforms/anyka-ak3918/isp_nr.c" "$REPO/src/core/cfg_file.c"
"$WORK/imgq_test" | grep -v -e "^\[isp.dump\]" -e "^\[imgq\]"
