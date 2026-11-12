#!/usr/bin/env bash
# Host integration test of the MQTT service (src/ak_rtsp/mqtt_svc.c with mqtt.c and mqtt_ha.c) against a fake broker
# that is part of the test: config parser, dormant guarantee (no thread, no socket while off), CONNECT/will/discovery/
# state/commands over the wire, keepalive, drops, refusals with backoff, clean stop, one-shot test. ASan/UBSan.
# Loopback only, no camera, no real broker. Set MQTT_TEST_DNS=1 to add the name-resolution failure case.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
SRC=$REPO/src/ak_rtsp

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$SRC" -o "$WORK/mqtt_svc_test" \
    "$REPO/tests/native/mqtt_svc_test.c" "$SRC/mqtt_svc.c" "$SRC/mqtt_ha.c" "$SRC/mqtt.c" -pthread
mkdir "$WORK/d"
timeout 120 "$WORK/mqtt_svc_test" "$WORK/d"
