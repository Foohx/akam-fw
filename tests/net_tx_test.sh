#!/usr/bin/env bash
# Host test of src/common/net/net_tx.c (RTSP/TCP writer stall policy: slow client
# keeps its stream, dead client is freed after the budget) over socketpairs.
# Needs only a host C compiler; touches no network and no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -I"$REPO/src/ak_rtsp" -I"$REPO/src/common/net" -I"$REPO/src/common/logic" \
    -o "$WORK/net_tx_test" \
    "$REPO/tests/native/net_tx_test.c" "$REPO/src/common/net/net_tx.c" \
    "$REPO/src/common/net/rtp_clock.c"
"$WORK/net_tx_test"
