#!/usr/bin/env bash
# What reaches the kernel: the ISP frame-rate payloads (ae.c) and the 96-byte CREATE_ENC payload (venc.c), recorded
# by stubs around the real ae.c, venc.c and video_cfg.c. With no settings file (or an empty or all-default or
# all-invalid one) they must equal, byte for byte, the fixture produced from the build before video_cfg existed
# (commit 9ef4588). With a file only the intended words change. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
SRC=$REPO/src/ak_rtsp
fails=0
check() { if "${@:2}"; then echo "  ok   - $1"; else echo "FAIL: $1"; fails=$((fails + 1)); fi; }

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -I"$SRC" "-DVIDEO_CFG_PATH=\"$WORK/video\"" -o "$WORK/apply" \
    "$REPO/tests/native/video_apply_test.c" "$SRC/ae.c" "$SRC/venc.c" "$SRC/video_cfg.c" -pthread
rec() { "$WORK/apply" | grep -E '^(TUNNEL|CREATE_ENC)'; }

rm -f "$WORK/video"
rec >"$WORK/default.txt"
check "no file: payloads identical to the pre-video_cfg build" cmp -s "$WORK/default.txt" "$REPO/tests/fixtures/video_apply_default.txt"

: >"$WORK/video"
rec >"$WORK/empty.txt"
check "empty file: payloads identical" cmp -s "$WORK/empty.txt" "$WORK/default.txt"

printf 'bitrate=1536\nfps=15\ngop=2\n' >"$WORK/video"
rec >"$WORK/explicit.txt"
check "explicit defaults: payloads identical" cmp -s "$WORK/explicit.txt" "$WORK/default.txt"

printf 'garbage\nfps=12\nbitrate=99999\n' >"$WORK/video"
rec >"$WORK/bad.txt"
check "invalid lines only: payloads identical" cmp -s "$WORK/bad.txt" "$WORK/default.txt"

# fps 10, gop 4 s (40 frames), bitrate 900 (0x0384): ISP {10,3426,8,10} and {10,0,0,10}; CREATE_ENC differs from the
# default line only in fps (byte 0x1c), goplen (0x1e) and max_kbps (0x2c)
printf 'bitrate=900\nfps=10\ngop=4\n' >"$WORK/video"
rec >"$WORK/cfg.txt"
check "fps 10: SET_FRAME_RATE is the night pair {10,3426,8,10}" grep -qx 'TUNNEL 4004494e 0a000000620d0000080000000a000000' "$WORK/cfg.txt"
check "fps 10: SET_SENSOR_FPS is {10,0,0,10}" grep -qx 'TUNNEL 4004497a 0a00000000000000000000000a000000' "$WORK/cfg.txt"
enc=$(grep '^CREATE_ENC' "$WORK/default.txt" | cut -d' ' -f2)
want="${enc:0:56}0a002800${enc:64:24}8403${enc:92}"
check "CREATE_ENC: only fps 10, goplen 40, max_kbps 900 changed" grep -qx "CREATE_ENC $want" "$WORK/cfg.txt"
if [ "$fails" -ne 0 ]; then echo "video_apply_test: $fails failed"; exit 1; fi
echo "video_apply_test: all passed"
