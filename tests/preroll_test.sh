#!/usr/bin/env bash
# Host test of src/ak_rtsp/preroll.c (lead-in buffer: ring of encoded frames, playout order, overwrite). No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/preroll_test" \
    "$REPO/tests/native/preroll_test.c" "$REPO/src/ak_rtsp/preroll.c"
"$WORK/preroll_test"
