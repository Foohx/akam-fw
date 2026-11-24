#!/usr/bin/env bash
# Host test of isp.wdr of the 1080p (platforms/anyka-ak3918/isp_wdr.c): composer, refusals, sender, replay. A fake isp_tunnel(), no camera.
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
    -o "$WORK/isp_wdr_test" \
    "$REPO/tests/native/isp_wdr_test.c" "$REPO/platforms/anyka-ak3918/isp_wdr.c"
"$WORK/isp_wdr_test"
