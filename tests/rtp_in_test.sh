#!/usr/bin/env bash
# Host test of src/ak_rtsp/rtp_in.c (RTP receive unpacker of the audio backchannel:
# header bounds, padding, CSRC, extension, payload types, sequence handling, fuzz).
# Needs only a host C compiler; touches no hardware.
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

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtp_in_test" \
    "$REPO/tests/native/rtp_in_test.c" "$REPO/src/ak_rtsp/rtp_in.c" "$REPO/src/ak_rtsp/g711.c"
"$WORK/rtp_in_test"
