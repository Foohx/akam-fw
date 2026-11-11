#!/usr/bin/env bash
# Host test of src/ak_rtsp/onvif_cfg.c (ONVIF flag file parser and the runtime start/stop state machine) with the real
# http_srv, onvif, wsd and cred modules, ASan/UBSan. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
SRC=$REPO/src/ak_rtsp

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$SRC" -o "$WORK/onvif_svc_test" \
    "$REPO/tests/native/onvif_svc_test.c" "$SRC/onvif_cfg.c" "$SRC/onvif.c" "$SRC/onvif_device.c" "$SRC/onvif_media.c" \
    "$SRC/onvif_imaging.c" "$REPO/tests/native/onvif_stubs.c" "$SRC/http_srv.c" "$SRC/wsd.c" "$SRC/soap.c" \
    "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" "$SRC/video_cfg.c" -pthread
mkdir "$WORK/d"
"$WORK/onvif_svc_test" "$WORK/d"
