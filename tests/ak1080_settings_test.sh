#!/usr/bin/env bash
# Host test of the settings registry of the 1080p (firmware/lsc-indoor-1080p-ak3918/settings.c, convergence P7): the table, every row against stubs of the
# modules it is built on (the real LED engine included), the 8091 adapter, PATCH rules, ONVIF/MQTT switches. No camera. ASan + UBSan.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" -I"$REPO/tests/native" "${SHARED_INC[@]}" \
    -o "$WORK/ak1080_settings_test" \
    "$REPO/tests/native/ak1080_settings_test.c" "$REPO/firmware/lsc-indoor-1080p-ak3918/settings.c" "$REPO/platforms/anyka-ak3918/leds.c" \
    "$REPO/src/features/light/led_pat.c" "$REPO/src/features/light/light_logic.c" "$REPO/src/core/cfg_file.c" \
    "$REPO/src/core/settings_reg.c" "$REPO/src/api/json_min.c" "$REPO/src/api/api_keys.c" "$REPO/src/net/crypto/sha256.c" "$REPO/platforms/anyka-ak3918/video_cfg.c" "$REPO/platforms/anyka-ak3918/image_orient.c"
"$WORK/ak1080_settings_test"
# the day / night switches of night.c replay the orientation after each ISP reload (both, under the ISP configuration lock they already hold)
n=$(grep -c 'image_orient_replay_locked();' "$REPO/platforms/anyka-ak3918/night.c")
if [ "$n" -ne 2 ]; then echo "FAIL night.c replays the orientation $n time(s), want 2 (night and day switch)"; exit 1; fi
echo "ok   night.c replays the orientation after the night and the day ISP reload"
