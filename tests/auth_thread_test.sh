#!/usr/bin/env bash
# Thread stress test of rtsp_auth.c + cred.c (the state shared by the RTSP thread and the ONVIF HTTP thread):
# ThreadSanitizer when the host toolchain supports it, else AddressSanitizer + UBSan. No camera.
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

# Does -fsanitize=thread link and run here?
printf 'int main(void){return 0;}\n' >"$WORK/probe.c"
if "$CC" -fsanitize=thread -o "$WORK/probe" "$WORK/probe.c" -pthread 2>/dev/null && "$WORK/probe" 2>/dev/null; then
    SAN=thread
    FLAGS=(-fsanitize=thread -O1 -g)
else
    SAN=address
    FLAGS=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -O1 -g)
fi
echo "sanitizer: $SAN"
"$CC" -std=gnu99 -Wall -Wextra -Werror "${FLAGS[@]}" -I"$SRC" -o "$WORK/auth_thread_test" \
    "$REPO/tests/native/auth_thread_test.c" "$SRC/cred.c" "$SRC/rtsp_auth.c" "$SRC/md5.c" "$SRC/sha1.c" "$SRC/b64.c" -pthread
mkdir "$WORK/d"
TSAN_OPTIONS="halt_on_error=1" "$WORK/auth_thread_test" "$WORK/d"
