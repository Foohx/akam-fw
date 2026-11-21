#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/isp_tune.c (saved picture settings: load, validation, atomic save). No camera.
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
    -o "$WORK/isp_tune_test" \
    "$REPO/tests/native/isp_tune_test.c" "$REPO/platforms/anyka-ak3918/isp_tune.c"
"$WORK/isp_tune_test" "$WORK"
