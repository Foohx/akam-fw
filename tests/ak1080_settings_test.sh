#!/usr/bin/env bash
# Host test of the settings registry of the 1080p (src/ak_rtsp/ak1080_settings.c, convergence P7): the table, every row against stubs of the
# modules it is built on (the real LED engine included), the 8091 adapter, PATCH rules, ONVIF/MQTT switches. No camera. ASan + UBSan.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$REPO/src/ak_rtsp" -I"$REPO/tests/native" -I"$REPO/src/common/net" -I"$REPO/src/common/logic" -I"$REPO/src/common/api" \
    -o "$WORK/ak1080_settings_test" \
    "$REPO/tests/native/ak1080_settings_test.c" "$REPO/src/ak_rtsp/ak1080_settings.c" "$REPO/src/ak_rtsp/leds.c" \
    "$REPO/src/common/logic/led_pat.c" "$REPO/src/common/logic/light_logic.c" "$REPO/src/common/logic/cfg_file.c" \
    "$REPO/src/common/api/settings_reg.c" "$REPO/src/common/api/json_min.c" "$REPO/src/common/api/api_keys.c" "$REPO/src/common/net/sha256.c" "$REPO/src/ak_rtsp/video_cfg.c"
"$WORK/ak1080_settings_test"
