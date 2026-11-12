#!/usr/bin/env bash
# Host test of the RTSP/TCP video send path as rtsp.c composes it: tx_pace_decide() with the room of net_tx,
# whole-packet writes into the private queue, ENOBUFS as "skip to the next keyframe", over a real loopback
# TCP socket with a slow reader (virtual time, no sleeps). No network, no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/pace_compose_test" \
    "$REPO/tests/native/pace_compose_test.c" "$REPO/src/ak_rtsp/tx_pace.c" "$REPO/src/ak_rtsp/net_tx.c" \
    "$REPO/src/ak_rtsp/rtp_clock.c"
"$WORK/pace_compose_test"
