#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/leds.c (status LED state machine, saved settings, 8091 keys,
# blink thread) against a fake sysfs directory, a fake network check and a fake IR hook.
# No camera, no real LED.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/leds_test" \
    "$REPO/tests/native/leds_test.c" "$REPO/platforms/anyka-ak3918/leds.c" "$REPO/src/features/light/led_pat.c" "$REPO/src/features/light/light_logic.c"
"$WORK/leds_test"
