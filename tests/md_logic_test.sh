#!/usr/bin/env bash
# Host test of src/ak_rtsp/md_logic.c (motion detection: block reduction, score, state machine, presets). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/md_logic_test" \
    "$REPO/tests/native/md_logic_test.c" "$REPO/src/ak_rtsp/md_logic.c"
"$WORK/md_logic_test"
