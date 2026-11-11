#!/usr/bin/env bash
# Host test of src/ak_rtsp/onvif_media.c + onvif_imaging.c over real HTTP on a localhost port, ASan/UBSan: every
# Media 1 and Imaging 2.0 operation against golden responses (tests/fixtures/onvif_media/), access classes, the
# stream URI from getsockname, the SetImagingSettings mapping and validation, the flash write debounce with an
# injected clock. The camera side (audio_out, control) is stubbed (tests/native/onvif_stubs.c). Responses are also
# checked for well-formedness with xmllint when it is installed. No camera.
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
    -I"$SRC" -o "$WORK/onvif_media_test" \
    "$REPO/tests/native/onvif_media_test.c" "$REPO/tests/native/onvif_stubs.c" "$SRC/onvif.c" "$SRC/onvif_device.c" \
    "$SRC/onvif_media.c" "$SRC/onvif_imaging.c" "$SRC/http_srv.c" "$SRC/soap.c" \
    "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" -pthread
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
