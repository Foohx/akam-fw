#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/onvif_cfg.c (ONVIF flag file parser and the runtime start/stop state machine) with the real
# http_srv, onvif, wsd and cred modules, ASan/UBSan. No camera.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
SRC=$REPO/platforms/anyka-ak3918

"$CC" -std=gnu99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
 -I"$SRC" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" -o "$WORK/onvif_svc_test" \
    "$REPO/tests/native/onvif_svc_test.c" "$SRC/onvif_cfg.c" "$REPO/src/net/onvif/onvif.c" "$REPO/src/net/onvif/onvif_common.c" "$REPO/firmware/lsc-indoor-1080p-ak3918/onvif_model.c" "$SRC/onvif_device.c" "$SRC/onvif_media.c" \
    "$SRC/onvif_imaging.c" "$REPO/tests/native/onvif_stubs.c" "$REPO/src/net/onvif/http_srv.c" "$REPO/src/net/onvif/wsd.c" "$REPO/src/net/onvif/soap.c" \
    "$REPO/src/net/crypto/cred.c" "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/crypto/md5.c" "$REPO/src/net/crypto/sha1.c" "$REPO/src/net/crypto/b64.c" "$SRC/video_cfg.c" -pthread
mkdir "$WORK/d"
"$WORK/onvif_svc_test" "$WORK/d"
