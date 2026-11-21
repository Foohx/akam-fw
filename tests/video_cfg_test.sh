#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/video_cfg.c + video_ctl.c (saved video settings: defaults, validation, atomic save, the
# 8091 keys, apply-at-start values). ASan/UBSan. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
SRC=$REPO/platforms/anyka-ak3918

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 -I"$SRC" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" "-DVIDEO_CFG_PATH=\"$WORK/video\"" -o "$WORK/video_cfg_test" \
    "$REPO/tests/native/video_cfg_test.c" "$SRC/video_cfg.c" "$SRC/video_ctl.c" "$REPO/src/core/cfg_file.c" -pthread
"$WORK/video_cfg_test" "$WORK"
