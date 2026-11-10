#!/usr/bin/env bash
# Host test of src/ak_rtsp/night_logic.c (the day / night decision) with simulated AE traces. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/night_logic_test" \
    "$REPO/tests/native/night_logic_test.c" "$REPO/src/ak_rtsp/night_logic.c" "$REPO/src/ak_rtsp/cfg_file.c" -lm
"$WORK/night_logic_test"
