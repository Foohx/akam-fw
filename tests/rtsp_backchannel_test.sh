#!/usr/bin/env bash
# End-to-end host test of the ONVIF audio backchannel through the REAL rtsp.c client loop
# (SmolRTSP, Digest, rtsp_in, talk.c, rtp_in.c, audio_vad.c) over loopback TCP and UDP:
# plain SDP unchanged, Require handling, 401/551 gating, talk-only and combined sessions,
# half-duplex mute of the microphone track, UDP transport. Only the hardware is faked.
# Needs a host C compiler and the SmolRTSP checkout (src/smolrtsp, see tools/build/bootstrap.sh);
# skips without them. Touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
SMOL="$REPO/src/smolrtsp"
if [ ! -f "$SMOL/include/smolrtsp.h" ] || [ ! -d "$SMOL/build/_deps/metalang99-src/include" ]; then
    echo "skip: SmolRTSP is not checked out at src/smolrtsp (run tools/build/bootstrap.sh)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SRC="$REPO/src/ak_rtsp"
INC=(-I"$SRC" -I"$SMOL/include"
     -I"$SMOL/build/_deps/slice99-src" -I"$SMOL/build/_deps/datatype99-src"
     -I"$SMOL/build/_deps/interface99-src" -I"$SMOL/build/_deps/metalang99-src/include")

# SmolRTSP itself (third party, built quietly, no sanitizers).
mkdir -p "$WORK/smol"
SMOL_OBJS=()
for f in "$SMOL"/src/*.c "$SMOL"/src/types/*.c "$SMOL"/src/writer/*.c "$SMOL"/src/transport/*.c "$SMOL"/src/nal/h264.c; do
    o="$WORK/smol/$(echo "${f#"$SMOL"/src/}" | tr / _).o"
    "$CC" -std=gnu99 -O1 -w "${INC[@]}" -DSMOLRTSP_WITH_H264 -c -o "$o" "$f"
    SMOL_OBJS+=("$o")
done

"$CC" -std=gnu99 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -pthread "${INC[@]}" -DSMOLRTSP_WITH_H264 \
    -DRTSP_AUTH_PATH="\"$WORK/rtsp_auth\"" \
    -Wl,--wrap=ioctl \
    -o "$WORK/rtsp_backchannel_test" \
    "$REPO/tests/native/rtsp_backchannel_test.c" \
    "$SRC/rtsp.c" "$SRC/rtsp_in.c" "$SRC/rtsp_idle.c" "$SRC/rtsp_sdp.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" \
    "$SRC/net_tx.c" "$SRC/rtp_clock.c" "$SRC/h264_params.c" "$SRC/talk.c" "$SRC/rtp_in.c" \
    "$SRC/audio_vad.c" "$SRC/g711.c" "${SMOL_OBJS[@]}" 2>"$WORK/cc.log" || { cat "$WORK/cc.log"; exit 1; }

"$WORK/rtsp_backchannel_test" | grep -v '^\[ak_rtsp\]\|^\[audio_out\]'
