#!/usr/bin/env bash
# Host test of src/features/night/night_logic.c (the day / night decision) with simulated AE traces. No camera.
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
    -o "$WORK/night_logic_test" \
    "$REPO/tests/native/night_logic_test.c" "$REPO/src/features/night/night_logic.c" "$REPO/src/core/cfg_file.c" -lm
"$WORK/night_logic_test"
