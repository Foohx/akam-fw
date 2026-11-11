#!/usr/bin/env bash
# Host test of src/ak_rtsp/onvif.c + onvif_device.c over real HTTP on a localhost port, ASan/UBSan: the
# authentication gate (WS-Security, HTTP Digest, password off, invalid file, limiter delay), every Device operation
# against golden responses (tests/fixtures/onvif/), UUID stability. Responses are also checked for well-formedness
# with xmllint when it is installed. No camera.
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
    -I"$SRC" -o "$WORK/onvif_test" \
    "$REPO/tests/native/onvif_test.c" "$SRC/onvif.c" "$SRC/onvif_device.c" "$SRC/onvif_media.c" "$SRC/onvif_imaging.c" \
    "$REPO/tests/native/onvif_stubs.c" "$SRC/http_srv.c" "$SRC/soap.c" \
    "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" "$SRC/video_cfg.c" -pthread
mkdir "$WORK/d"
"$WORK/onvif_test" "$WORK/d" "$REPO/tests/fixtures/onvif"

# Every response body the test saw must be well-formed XML.
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

# Independent check of the version-5 UUID construction (python3, hashlib).
if command -v python3 >/dev/null 2>&1; then
    read -r mac uuid <"$WORK/d/uuid.txt"
    want=$(python3 -I - "$mac" <<'PY'
import hashlib, sys
ns = bytes([0x4f,0x1b,0x8c,0x62,0xe3,0x7a,0x4d,0x05,0xa9,0x21,0x7c,0xd3,0x0e,0x58,0xb6,0x94])
h = bytearray(hashlib.sha1(ns + sys.argv[1].encode()).digest()[:16])
h[6] = (h[6] & 0x0f) | 0x50
h[8] = (h[8] & 0x3f) | 0x80
x = h.hex()
print("urn:uuid:%s-%s-%s-%s-%s" % (x[:8], x[8:12], x[12:16], x[16:20], x[20:]))
PY
)
    if [ "$want" = "$uuid" ]; then
        echo "  ok   - UUID equals the independent SHA-1 construction ($uuid)"
    else
        echo "FAIL: UUID $uuid, expected $want"
        exit 1
    fi
else
    echo "skip: python3 not installed, UUID construction not re-derived"
fi
