#!/usr/bin/env bash
# Host test: the 2K RTSP server (real akam2k_rtsp.c + SmolRTSP over loopback) serves /stream0 and /stream1 only and
# caps its clients (tests/rot2k_rtsp_ports_unit.c). Skips without SmolRTSP; no hardware.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "skip: no host C compiler ($CC)"; exit 0; }
SMOL="$REPO/src/smolrtsp"
if [ ! -f "$SMOL/include/smolrtsp.h" ] || [ ! -d "$SMOL/build/_deps/metalang99-src/include" ]; then
    echo "skip: SmolRTSP is not checked out at src/smolrtsp (run tools/build/bootstrap.sh)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FW="$REPO/firmware/lsc-rotatable-2k-ak3918av130/app"
N="$REPO/src/common/net"
INC=(-I"$FW" -I"$REPO/src/ak_rtsp" -I"$N" -I"$REPO/src/common/logic" -I"$SMOL/include"
     -I"$SMOL/build/_deps/slice99-src" -I"$SMOL/build/_deps/datatype99-src"
     -I"$SMOL/build/_deps/interface99-src" -I"$SMOL/build/_deps/metalang99-src/include")
SMOL_OBJS=()
for f in "$SMOL"/src/*.c "$SMOL"/src/types/*.c "$SMOL"/src/writer/*.c "$SMOL"/src/transport/*.c "$SMOL"/src/nal/h264.c; do
    o="$WORK/$(echo "${f#"$SMOL"/src/}" | tr / _).o"
    "$CC" -std=gnu99 -O1 -w "${INC[@]}" -DSMOLRTSP_WITH_H264 -c -o "$o" "$f"
    SMOL_OBJS+=("$o")
done
"$CC" -std=gnu99 -Wall -Wextra -Wno-unused-parameter -pthread "${INC[@]}" -DSMOLRTSP_WITH_H264 \
    -DRTSP_AUTH_PATH="\"$WORK/rtsp_auth\"" -o "$WORK/ports" \
    "$HERE/rot2k_rtsp_ports_unit.c" "$FW/akam2k_rtsp.c" "$FW/akam2k_stream.c" "$FW/akam2k_aring.c" \
    "$N/h264_params.c" "$N/rtp_clock.c" "$N/rtsp_auth.c" "$N/rtsp_idle.c" "$N/rtsp_in.c" "$N/rtsp_sdp.c" "$N/net_tx.c" \
    "$N/tx_pace.c" "$N/md5.c" "${SMOL_OBJS[@]}" 2>"$WORK/cc.log" || { cat "$WORK/cc.log"; exit 1; }
rc=0; timeout 60 "$WORK/ports" >"$WORK/out" 2>&1 || rc=$?; cat "$WORK/out"; echo "exit $rc"
grep -q 'rot2k rtsp ports unit: 0 failed' "$WORK/out"
