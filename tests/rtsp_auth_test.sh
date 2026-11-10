#!/usr/bin/env bash
# Host test of src/ak_rtsp/md5.c and rtsp_auth.c (MD5 and Digest vectors, the
# authentication state machine, credentials file reload, rate limiting, no
# secret in the logs), then a cross-check with a real Digest client (ffprobe)
# when it is installed. Needs only a host C compiler; touches no hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
SRV=""
cleanup() {
    if [ -n "$SRV" ]; then kill "$SRV" 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_auth_test" \
    "$REPO/tests/native/rtsp_auth_test.c" "$REPO/src/ak_rtsp/rtsp_auth.c" "$REPO/src/ak_rtsp/md5.c" -pthread
"$WORK/rtsp_auth_test"

# The journal line of rtsp.c must print only what rtsp_auth.c hands out (r.log),
# never a header, a user name or a password.
if grep -n 'printf' "$REPO/src/ak_rtsp/rtsp.c" | grep -i -e 'authorization' -e 'username' -e 'ha1' -e 'passw'; then
    echo "FAIL: rtsp.c prints authentication data"
    exit 1
fi
echo "  ok   - rtsp.c never prints the Authorization header, user or password"

# ---- cross-check with a real Digest client (ffmpeg's RTSP demuxer) ----------
# Optional: skipped when ffprobe is missing. The server is tests/native/rtsp_auth_server.c,
# which runs the same rtsp_auth.c in front of a canned SDP. SETUP is not served (461): the
# check is that ffprobe answers the 401 challenge and gets DESCRIBE through.
if ! command -v ffprobe >/dev/null 2>&1; then
    echo "skip: ffprobe not installed, no real-client cross-check"
    exit 0
fi
"$CC" -std=gnu99 -Wall -Wextra -Werror -I"$REPO/src/ak_rtsp" \
    -o "$WORK/rtsp_auth_server" \
    "$REPO/tests/native/rtsp_auth_server.c" "$REPO/src/ak_rtsp/rtsp_auth.c" "$REPO/src/ak_rtsp/md5.c" -pthread

AUTHFILE=$WORK/rtsp_auth
USERNAME=viewer
PASS='p@ss:w/rd%1 x?#'            # characters that must be URL-encoded
ENC='p%40ss%3Aw%2Frd%251%20x%3F%23'
ha1=$(printf '%s' "$USERNAME:akam:$PASS" | md5sum | cut -d' ' -f1)
printf '%s:akam:%s\n' "$USERNAME" "$ha1" >"$AUTHFILE"
chmod 600 "$AUTHFILE"

"$WORK/rtsp_auth_server" "$AUTHFILE" >"$WORK/server.out" 2>"$WORK/server.err" &
SRV=$!
port=""
for _ in $(seq 1 50); do
    port=$(sed -n 's/^PORT //p' "$WORK/server.out" 2>/dev/null | head -1)
    [ -n "$port" ] && break
    sleep 0.1
done
[ -n "$port" ] || { echo "FAIL: the test server did not start"; exit 1; }

probe() { # url
    timeout 30 ffprobe -v error -rtsp_transport tcp -i "$1" >"$WORK/ffprobe.out" 2>&1 || true
}

probe "rtsp://$USERNAME:$ENC@127.0.0.1:$port/"
if grep -q '^DESCRIBE 401' "$WORK/server.out" && grep -q '^DESCRIBE 200' "$WORK/server.out"; then
    echo "  ok   - ffprobe answers the Digest challenge (URL-encoded password with @ : / % space ? #)"
else
    echo "  FAIL - ffprobe did not get DESCRIBE through:"
    cat "$WORK/server.out" "$WORK/server.err" "$WORK/ffprobe.out"
    exit 1
fi
if grep -q 'authenticated' "$WORK/server.err"; then
    echo "  ok   - the server journaled the successful login"
else
    echo "  FAIL - no login line in the server journal"
    cat "$WORK/server.err"
    exit 1
fi

before=$(grep -c '^DESCRIBE 200' "$WORK/server.out")
probe "rtsp://$USERNAME:wrong-password@127.0.0.1:$port/"
after=$(grep -c '^DESCRIBE 200' "$WORK/server.out")
if [ "$before" = "$after" ] && grep -q 'bad credentials' "$WORK/server.err"; then
    echo "  ok   - ffprobe with a wrong password is refused"
else
    echo "  FAIL - wrong password was not refused"
    cat "$WORK/server.out" "$WORK/server.err"
    exit 1
fi
if grep -F -e "$PASS" -e "$ENC" -e "$ha1" "$WORK/server.err" "$WORK/server.out" >/dev/null; then
    echo "  FAIL - a secret reached the server log"
    exit 1
fi
echo "  ok   - no secret in the server log"
