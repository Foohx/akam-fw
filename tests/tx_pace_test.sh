#!/usr/bin/env bash
# Host test of src/common/net/tx_pace.c (RTSP/TCP backpressure: skip frames, resume at a keyframe).
# Pure logic plus one SIOCOUTQ read on a loopback socketpair; no network, no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" -I"$REPO/src/common/net" -I"$REPO/src/common/logic" \
    -o "$WORK/tx_pace_test" \
    "$REPO/tests/native/tx_pace_test.c" "$REPO/src/common/net/tx_pace.c"
"$WORK/tx_pace_test"
