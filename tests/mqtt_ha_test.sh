#!/usr/bin/env bash
# Host test of src/ak_rtsp/mqtt_ha.c (Home Assistant discovery documents, states, the command allow-list) with a fake camera:
# strict JSON check of every document, every entity against valid and hostile payloads, fuzz; ASan/UBSan. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$REPO/src/ak_rtsp" -o "$WORK/mqtt_ha_test" \
    "$REPO/tests/native/mqtt_ha_test.c" "$REPO/src/ak_rtsp/mqtt_ha.c" "$REPO/src/ak_rtsp/mqtt.c"
"$WORK/mqtt_ha_test"
