#!/usr/bin/env bash
# Host test of src/net/rtsp/h264_nalu.c (Annex B splitter) and rtcp_sr.c (RTCP Sender Report writer), the send helpers shared by both RTSP servers.
# The SR writer links SmolRTSP's rtcp serialiser: the test skips without the checkout.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "skip: no host C compiler ($CC)"; exit 0; }
SMOL="$REPO/third_party/smolrtsp"
if [ ! -f "$SMOL/include/smolrtsp.h" ] || [ ! -d "$SMOL/build/_deps/metalang99-src/include" ]; then
    echo "skip: SmolRTSP is not checked out at third_party/smolrtsp (run tools/build/bootstrap.sh)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
INC=("${SHARED_INC[@]}" -I"$SMOL/include" -I"$SMOL/build/_deps/slice99-src" -I"$SMOL/build/_deps/datatype99-src"
     -I"$SMOL/build/_deps/interface99-src" -I"$SMOL/build/_deps/metalang99-src/include")
"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined "${INC[@]}" -o "$WORK/rtsp_tx_test" \
    "$REPO/tests/native/rtsp_tx_test.c" "$REPO/src/net/rtsp/h264_nalu.c" "$REPO/src/net/rtsp/rtcp_sr.c" "$REPO/src/net/rtsp/rtp_clock.c" \
    "$SMOL/src/types/rtcp.c" -lm
"$WORK/rtsp_tx_test"
echo "rtsp_tx_test: OK"
