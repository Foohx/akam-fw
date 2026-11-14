#!/usr/bin/env bash
# Host test of src/ak_rtsp/mqtt.c (the MQTT 3.1.1 packet codec): golden bytes, remaining-length edge cases, malformed
# input and a fuzz loop, under ASan/UBSan. No camera, no network.
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
    -I"$REPO/src/ak_rtsp" -o "$WORK/mqtt_codec_test" \
    "$REPO/tests/native/mqtt_codec_test.c" "$REPO/src/ak_rtsp/mqtt.c"
"$WORK/mqtt_codec_test"
