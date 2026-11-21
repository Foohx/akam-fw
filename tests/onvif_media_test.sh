#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/onvif_media.c + onvif_imaging.c over real HTTP on a localhost port, ASan/UBSan: every
# Media 1 and Imaging 2.0 operation against golden responses (tests/fixtures/onvif_media/), access classes, the
# stream URI from getsockname, the SetImagingSettings mapping and validation, the flash write debounce with an
# injected clock. The camera side (audio_out, control) is stubbed (tests/native/onvif_stubs.c). Responses are also
# checked for well-formedness with xmllint when it is installed. No camera.
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
 -I"$SRC" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" "-DVIDEO_CFG_PATH=\"$WORK/d/video\"" -o "$WORK/onvif_media_test" \
    "$REPO/tests/native/onvif_media_test.c" "$REPO/tests/native/onvif_stubs.c" "$REPO/src/net/onvif/onvif.c" "$REPO/firmware/lsc-indoor-1080p-ak3918/onvif_model.c" "$SRC/onvif_device.c" \
    "$SRC/onvif_media.c" "$SRC/onvif_imaging.c" "$REPO/src/net/onvif/http_srv.c" "$REPO/src/net/onvif/soap.c" \
    "$REPO/src/net/crypto/cred.c" "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/crypto/md5.c" "$REPO/src/net/crypto/sha1.c" "$REPO/src/net/crypto/b64.c" "$SRC/video_cfg.c" -pthread
mkdir "$WORK/d"
"$WORK/onvif_media_test" "$WORK/d" "$REPO/tests/fixtures/onvif_media"

if command -v xmllint >/dev/null 2>&1; then
    n=0
    for f in "$WORK"/d/resp_*.xml; do
        xmllint --noout "$f" || { echo "FAIL: $f is not well-formed"; exit 1; }
        n=$((n + 1))
    done
    echo "  ok   - xmllint: $n responses are well-formed"
else
    echo "skip: xmllint not installed, well-formedness not re-checked (golden files were reviewed)"
fi
