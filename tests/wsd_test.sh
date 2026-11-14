#!/usr/bin/env bash
# Host test of src/ak_rtsp/wsd.c (WS-Discovery: matching, templates, rate limit, UDP loopback) under ASan/UBSan.
# Multicast is not needed: the responder is driven by unicast Probes on a test port. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/wsd_test" \
    "$REPO/tests/native/wsd_test.c" "$REPO/src/ak_rtsp/wsd.c" "$REPO/src/ak_rtsp/soap.c" -lpthread
"$WORK/wsd_test"
