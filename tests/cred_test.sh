#!/usr/bin/env bash
# Host test of src/ak_rtsp/cred.c (ASan/UBSan): credentials file format and permissions, reload,
# WS-Security PasswordDigest (WS-Security profile vector, an openssl-checked vector, replay cache,
# Created window with an injected clock), the shared failure limiter and the HTTP Digest hook.
# Needs a host C compiler; touches no hardware.
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

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$SRC" \
    -o "$WORK/cred_test" \
    "$REPO/tests/native/cred_test.c" "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" -pthread
mkdir "$WORK/d"
"$WORK/cred_test" "$WORK/d"

# Same test with an 8-slot nonce cache: eviction of the oldest Created and the replay floor.
"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -I"$SRC" -DCRED_NONCE_CACHE=8 -DCRED_EVICT_TEST \
    -o "$WORK/cred_test8" \
    "$REPO/tests/native/cred_test.c" "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" -pthread
mkdir "$WORK/d8"
"$WORK/cred_test8" "$WORK/d8"

# The password must never reach a log: the module does not print at all.
if grep -nE '\b(printf|fprintf|puts|fputs|perror|syslog)\b' "$SRC/cred.c"; then
    echo "FAIL: cred.c prints"
    exit 1
fi
echo "  ok   - cred.c never prints anything"

# Independent check of the PasswordDigest vector with openssl (optional).
if command -v openssl >/dev/null 2>&1; then
    got=$({ printf 'LKqI6G/AikKCQrN0zqZFlg=='  | openssl base64 -d -A; printf '2010-09-16T07:50:45Zuserpassword'; } | openssl dgst -sha1 -binary | openssl base64)
    if [ "$got" = "tuOSpGlFlIXsozq4HFNeeGeFLEI=" ]; then
        echo "  ok   - openssl reproduces the vector used by the test"
    else
        echo "FAIL: openssl disagrees with the test vector ($got)"
        exit 1
    fi
else
    echo "skip: openssl not installed, the vector was not re-derived"
fi
