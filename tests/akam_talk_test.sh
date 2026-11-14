#!/usr/bin/env bash
# Host test of tools/akam-talk (the PC-side client of the audio backchannel): builds cleanly
# with -Wall -Wextra -Werror, argument handling and exit codes, no password on the command
# line; with the pinned Zig, cross-builds every supported OS. The protocol itself (Digest,
# DESCRIBE/SETUP/PLAY, RTP) is tested against the real server loop in tests/rtsp_backchannel_test.sh.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "skip: no host C compiler ($CC)"
    exit 0
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ok() { echo "  ok   - $1"; }
bad() { echo "  FAIL - $1"; exit 1; }

"$CC" -std=gnu99 -Wall -Wextra -Werror -I"$REPO/src/ak_rtsp" -o "$WORK/akam-talk" \
    "$REPO/tools/akam-talk/akam_talk.c" "$REPO/src/ak_rtsp/md5.c" "$REPO/src/ak_rtsp/g711.c"
ok "builds with -Wall -Wextra -Werror"
T="$WORK/akam-talk"

"$T" --help 2>&1 | grep -q 'AKAM_RTSP_PASSWORD' || bad "--help mentions the password variable"
ok "--help"

rc=0; "$T" >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "no argument: exit 1 (got $rc)"
rc=0; "$T" http://192.0.2.1/ >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "not an rtsp:// address: exit 1 (got $rc)"
rc=0; "$T" --bogus rtsp://192.0.2.1/ >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "unknown option: exit 1 (got $rc)"
rc=0; "$T" --tone 0 rtsp://192.0.2.1/ >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "--tone 0: exit 1 (got $rc)"
rc=0; "$T" --codec opus rtsp://192.0.2.1/ >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "--codec opus: exit 1 (got $rc)"
rc=0; "$T" --password rtsp://192.0.2.1/ </dev/null >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] || bad "a --password option does not exist: exit 1 (got $rc)"
ok "usage errors exit 1"

# Nothing listens on this port of the loopback: a network error (2), quickly, no hang.
rc=0; timeout 10 "$T" --tone 1 rtsp://alice@127.0.0.1:9/x </dev/null >/dev/null 2>&1 || rc=$?
[ "$rc" = 2 ] || bad "connection refused: exit 2 (got $rc)"
ok "connection refused: exit 2"

# The password is never printed, not even with -v and a wrong server.
out=$(AKAM_RTSP_PASSWORD='hunter2-do-not-print' timeout 10 "$T" -v --tone 1 rtsp://alice@127.0.0.1:9/x </dev/null 2>&1 || true)
case $out in *hunter2-do-not-print*) bad "the password appears in the output" ;; esac
ok "the password never appears in the output"

# A password in the URL works but is warned about.
out=$(timeout 10 "$T" --tone 1 rtsp://alice:pw@127.0.0.1:9/x </dev/null 2>&1 || true)
case $out in *"prefer AKAM_RTSP_PASSWORD"*) ok "password in the URL: warned" ;; *) bad "no warning for a password in the URL" ;; esac

# Cross builds (every OS of the wizard) when the pinned Zig is installed.
if [ -f "$REPO/tools/build/env.sh" ] && ( . "$REPO/tools/build/env.sh" >/dev/null 2>&1 ); then
    ( . "$REPO/tools/build/env.sh" >/dev/null 2>&1 && bash "$REPO/tools/akam-talk/build.sh" --cross --out "$WORK/dist" >/dev/null )
    for f in x86_64-linux-musl/akam-talk aarch64-linux-musl/akam-talk x86_64-macos/akam-talk \
             aarch64-macos/akam-talk x86_64-windows-gnu/akam-talk.exe; do
        [ -s "$WORK/dist/$f" ] || bad "cross build produced no $f"
    done
    ok "cross builds: linux x86_64/aarch64, macOS x86_64/arm64, Windows x86_64"
else
    echo "  note - pinned Zig not installed: cross builds not checked"
fi
echo "akam_talk_test: all passed"
