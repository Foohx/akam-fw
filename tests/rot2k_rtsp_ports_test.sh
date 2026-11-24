#!/usr/bin/env bash
# Host test: the 2K RTSP server (real rtsp_session.c + SmolRTSP over loopback) serves /stream0 and /stream1 only and
# caps its clients (tests/rot2k_rtsp_ports_unit.c). Skips without SmolRTSP; no hardware.
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
INC=("${INC_2K[@]}" "${SHARED_INC[@]}" -I"$SMOL/include"
     -I"$SMOL/build/_deps/slice99-src" -I"$SMOL/build/_deps/datatype99-src"
     -I"$SMOL/build/_deps/interface99-src" -I"$SMOL/build/_deps/metalang99-src/include")
SMOL_OBJS=()
for f in "$SMOL"/src/*.c "$SMOL"/src/types/*.c "$SMOL"/src/writer/*.c "$SMOL"/src/transport/*.c "$SMOL"/src/nal/h264.c; do
    o="$WORK/$(echo "${f#"$SMOL"/src/}" | tr / _).o"
    "$CC" -std=gnu99 -O1 -w "${INC[@]}" -DSMOLRTSP_WITH_H264 -c -o "$o" "$f"
    SMOL_OBJS+=("$o")
done
run_unit() { # <unit.c> <binary name> <pass line>
    "$CC" -std=gnu99 -Wall -Wextra -Wno-unused-parameter -pthread "${INC[@]}" -DSMOLRTSP_WITH_H264 \
        -DRTSP_AUTH_PATH="\"$WORK/rtsp_auth\"" -o "$WORK/$2" \
        "$HERE/$1" "$REPO/platforms/anyka-ak3918av130/rtsp_session.c" "$REPO/platforms/anyka-ak3918av130/stream.c" "$REPO/platforms/anyka-ak3918av130/aring.c" \
        "$REPO/src/net/rtsp/h264_params.c" "$REPO/src/net/rtsp/h264_nalu.c" "$REPO/src/net/rtsp/rtcp_sr.c" "$REPO/src/net/rtsp/rtp_clock.c" "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/rtsp/rtsp_idle.c" "$REPO/src/net/rtsp/rtsp_in.c" "$REPO/src/net/rtsp/rtsp_sdp.c" "$REPO/src/net/rtsp/net_tx.c" \
        "$REPO/src/net/rtsp/tx_pace.c" "$REPO/src/net/crypto/md5.c" "$REPO/src/features/audio/talk.c" "$REPO/src/net/rtsp/rtp_in.c" "$REPO/src/features/audio/audio_vad.c" "$REPO/src/features/audio/g711.c" "${SMOL_OBJS[@]}" -lm 2>"$WORK/cc.log" || { cat "$WORK/cc.log"; exit 1; }
    local rc=0
    timeout 90 "$WORK/$2" >"$WORK/$2.out" 2>&1 || rc=$?
    cat "$WORK/$2.out"
    echo "exit $rc"
    grep -q "$3" "$WORK/$2.out"
}
run_unit rot2k_rtsp_ports_unit.c ports 'rot2k rtsp ports unit: 0 failed'
run_unit rot2k_rtsp_talk_unit.c talk 'rot2k rtsp talk unit: 0 failed'
