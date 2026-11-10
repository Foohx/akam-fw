#!/usr/bin/env bash
# Host test of tools/firmware_patch_templates/akam_rtsp_auth.sh, the camera command that
# manages the RTSP credentials. It runs in a fake root, under BusyBox ash (the pinned one when
# AKAM_BUSYBOX is set), with PATH reduced to the applets the camera's BusyBox 1.30.1 has
# (md5sum, sync, mv, chmod, mkdir, rm, stty, cat, ls): no tr, no mktemp, no head. Nothing here
# touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/tools/firmware_patch_templates/akam_rtsp_auth.sh

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
# env -i runs it with a reduced PATH: use an absolute path.
case ${SHCMD[0]} in /*) ;; *) SHCMD[0]=$(command -v "${SHCMD[0]}") ;; esac
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else ko "$d"; fi; }

ROOT=$WORK/root
CONF=$ROOT/etc/config
FILE=$CONF/akam/rtsp_auth
PLAIN=$CONF/akam/credentials
mkdir -p "$CONF"

# PATH with only the camera's applets (from the pinned BusyBox when available).
BIN=$WORK/bin
mkdir -p "$BIN"
if [ -n "${AKAM_BUSYBOX:-}" ] && [ -x "$AKAM_BUSYBOX" ]; then
    for a in md5sum sync mv chmod mkdir rm stty cat ls; do ln -s "$AKAM_BUSYBOX" "$BIN/$a"; done
else
    for a in md5sum sync mv chmod mkdir rm stty cat ls; do ln -s "$(command -v "$a")" "$BIN/$a"; done
    echo "note: pinned BusyBox absent, the host's applets are used (run tools/build/bootstrap.sh)"
fi

# run [stdin-file] -- args...: output in $OUT, status in RC. Fake root, reduced PATH.
OUT=$WORK/out
RC=0
run() {
    local in=$1
    shift
    env -i PATH="$BIN" AKAM_ROOT="$ROOT" "${SHCMD[@]}" "$SCRIPT" "$@" <"$in" >"$OUT" 2>&1
    RC=$?
}
feed() { printf '%s' "$1" >"$WORK/in"; }   # exact stdin content
EMPTY=$WORK/empty
: >"$EMPTY"

mode_of() { stat -c '%a' "$1"; }
ha1_of() { printf '%s' "$1" | md5sum | cut -d' ' -f1; }   # host-side reference
has() { grep -q -e "$1" "$OUT"; }
hasnt() { ! grep -q -e "$1" "$OUT"; }

# --- 1. nothing configured: disabled ---------------------------------------
run "$EMPTY" status
check "status without a file: disabled, exit 0" test "$RC" = 0 -a "$(cat "$OUT")" = disabled
run "$EMPTY" show
check "show without a file: disabled" test "$RC" = 0 -a "$(cat "$OUT")" = disabled

# --- 2. set-hash (what ./akam rtsp-auth sends) ------------------------------
H=$(ha1_of 'viewer:akam:Correct-Horse-9')
feed "viewer:akam:$H
"
run "$WORK/in" set-hash
check "set-hash accepted" test "$RC" = 0
check "file written with the line as sent" test "$(cat "$FILE")" = "viewer:akam:$H"
check "file mode is 0600" test "$(mode_of "$FILE")" = 600
check "directory mode is 0700" test "$(mode_of "$CONF/akam")" = 700
check "no temporary file left behind" test "$(ls -A "$CONF/akam")" = rtsp_auth
run "$EMPTY" status
check "status: enabled, no plain copy: not ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"
run "$EMPTY" show
check "show: the user name only" test "$(cat "$OUT")" = "user: viewer"
check "show never prints the hash" hasnt "$H"

# --- 3. set USER from a pipe (two reads, as on a terminal) ------------------
SENT='S3NTINEL-pass'
feed "$SENT
$SENT
"
run "$WORK/in" set cam_1
check "set USER accepted" test "$RC" = 0
check "set: file holds HA1 of user:akam:password" test "$(cat "$FILE")" = "cam_1:akam:$(ha1_of "cam_1:akam:$SENT")"
check "set: the password is not in the file" bash -c "! grep -q S3NTINEL '$FILE'"
check "set: the password is not in the output" hasnt S3NTINEL
check "set: the user is in the URL hint" has 'rtsp://cam_1:PASSWORD@'
check "set: file mode 0600" test "$(mode_of "$FILE")" = 600
check "set: plain-text copy written as user=/pass=" test "$(cat "$PLAIN")" = "user=cam_1
pass=$SENT"
check "set: plain-text copy mode 0600" test "$(mode_of "$PLAIN")" = 600
check "set: the output says the password is stored for ONVIF" has 'so that ONVIF can verify'
check "set: the output never prints the password" hasnt S3NTINEL-pass
run "$EMPTY" status
check "status: ONVIF-ready after set" test "$(cat "$OUT")" = "enabled (user cam_1, ONVIF-ready)"

# Awkward characters survive byte for byte: backslash, quotes, dollar, colon, spaces.
# shellcheck disable=SC2016  # literal characters on purpose
AWK=' a\b"c$d:e f'\''g  '
feed "$AWK
$AWK
"
run "$WORK/in" set odd
check "set: special characters accepted" test "$RC" = 0
check "set: special characters hashed exactly" test "$(cat "$FILE")" = "odd:akam:$(ha1_of "odd:akam:$AWK")"

# CRLF input (a Windows terminal) is stripped.
feed "$(printf 'S3NTINEL-crlf\r\nS3NTINEL-crlf\r\n')
"
run "$WORK/in" set crlf
check "set: CRLF stripped" test "$(cat "$FILE")" = "crlf:akam:$(ha1_of 'crlf:akam:S3NTINEL-crlf')"

# --- 4. refusals leave the file alone --------------------------------------
cp "$FILE" "$WORK/before"
unchanged() { cmp -s "$FILE" "$WORK/before"; }
feed "S3NTINEL-one
S3NTINEL-two
"
run "$WORK/in" set cam
check "set: different passwords refused" test "$RC" = 1
check "set: ... file unchanged" unchanged
feed "short7!
short7!
"
run "$WORK/in" set cam
check "set: 7 characters refused" test "$RC" = 1
check "set: ... file unchanged" unchanged
feed "eight888
eight888
"
run "$WORK/in" set cam
check "set: exactly 8 characters accepted" test "$RC" = 0
cp "$FILE" "$WORK/before"
for u in 'bad user' 'a:b' '' 'x"y' "$(printf 'u%.0s' $(seq 1 33))"; do
    feed "S3NTINEL-pass
S3NTINEL-pass
"
    run "$WORK/in" set "$u"
    check "set: user '$u' refused" test "$RC" -ne 0
done
check "set: refused users left the file unchanged" unchanged
run "$EMPTY" set
check "set without a user: usage, exit 2" test "$RC" = 2
check "set: no output of any password on refusal" hasnt S3NTINEL

good=$(ha1_of 'u:akam:p')
for bad in "u:other:$good" "u:akam:$(printf '%s' "$good" | cut -c1-31)" "u:akam:$(printf '%s' "$good" | sed 's/[a-f]/A/')" \
    "u:akam:${good}0" 'garbage' ':akam:'"$good" "u:akam:$good:x" "a b:akam:$good" ''; do
    feed "$bad
"
    run "$WORK/in" set-hash
    check "set-hash: '$bad' refused" test "$RC" -ne 0
done
check "set-hash: refused lines left the file unchanged" unchanged
run "$EMPTY" set-hash
check "set-hash: empty input refused" test "$RC" -ne 0
feed "u:akam:$good"   # no final newline
run "$WORK/in" set-hash
check "set-hash: line without final newline accepted" test "$RC" = 0

# --- 5b. plain-text copy for ONVIF: set-creds, set-hash, status, disable -------
rm -f "$FILE" "$PLAIN"
PW='S3NTINEL-p w=d#9:é'
feed "viewer
$PW
"
run "$WORK/in" set-creds
check "set-creds accepted" test "$RC" = 0
check "set-creds: htdigest line from user:akam:password" test "$(cat "$FILE")" = "viewer:akam:$(ha1_of "viewer:akam:$PW")"
check "set-creds: plain file is user=/pass= lines" test "$(cat "$PLAIN")" = "user=viewer
pass=$PW"
check "set-creds: plain file mode 0600" test "$(mode_of "$PLAIN")" = 600
check "set-creds: htdigest file mode 0600" test "$(mode_of "$FILE")" = 600
check "set-creds: exactly the two files" test "$(ls -A "$CONF/akam" | wc -l)" = 2
check "set-creds: output states that ONVIF needs the stored password" has 'so that ONVIF can verify'
check "set-creds: the password is not in the output" hasnt S3NTINEL
run "$EMPTY" status
check "status: ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, ONVIF-ready)"
run "$EMPTY" show
check "show: still only the user" test "$(cat "$OUT")" = "user: viewer"

# CRLF input
feed "$(printf 'crlf\r\nS3NTINEL-crlf-pw\r\n')
"
run "$WORK/in" set-creds
check "set-creds: CRLF stripped" test "$RC" = 0 -a "$(cat "$PLAIN")" = "user=crlf
pass=S3NTINEL-crlf-pw"

# refusals change nothing
cp "$FILE" "$WORK/b1"
cp "$PLAIN" "$WORK/b2"
unchanged2() { cmp -s "$FILE" "$WORK/b1" && cmp -s "$PLAIN" "$WORK/b2"; }
LONG129=$(printf 'p%.0s' $(seq 1 129))
LONG128=$(printf 'q%.0s' $(seq 1 128))
for bad in "bad user
S3NTINEL-pass1" "a:b
S3NTINEL-pass1" "viewer
short7!" "viewer
$(printf 'tab\there-S3NTINEL')" "viewer
$LONG129" "viewer" ""; do
    feed "$bad
"
    run "$WORK/in" set-creds
    check "set-creds: refused ($(printf '%s' "$bad" | head -c 14 | tr -c 'A-Za-z0-9 .' '_' 2>/dev/null || echo input))" test "$RC" -ne 0
done
check "set-creds: refusals changed neither file" unchanged2
feed "viewer
$LONG128
"
run "$WORK/in" set-creds
check "set-creds: 128 characters accepted" test "$RC" = 0 -a "$(cat "$PLAIN")" = "user=viewer
pass=$LONG128"

# set-hash cannot vouch for the plain copy: it removes it
H=$(ha1_of 'viewer:akam:Correct-Horse-9')
feed "viewer:akam:$H
"
run "$WORK/in" set-hash
check "set-hash removes the stale plain-text copy" test "$RC" = 0 -a ! -e "$PLAIN"
check "set-hash: says ONVIF cannot verify" has 'ONVIF cannot verify'
run "$EMPTY" status
check "status after set-hash: not ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"

# status notices a plain copy that does not match, has the wrong mode or is malformed
feed "viewer
Correct-Horse-9
"
run "$WORK/in" set-creds
run "$EMPTY" status
check "status: ready again" test "$(cat "$OUT")" = "enabled (user viewer, ONVIF-ready)"
printf 'user=viewer\npass=Another-pass-9\n' >"$PLAIN"
chmod 600 "$PLAIN"
run "$EMPTY" status
check "status: password differs from the hash: not ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"
printf 'user=other\npass=Correct-Horse-9\n' >"$PLAIN"
run "$EMPTY" status
check "status: other user: not ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"
printf 'user=viewer\npass=Correct-Horse-9\nextra=1\n' >"$PLAIN"
run "$EMPTY" status
check "status: extra line: not ONVIF-ready" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"
printf 'user=viewer\npass=Correct-Horse-9\n' >"$PLAIN"
chmod 644 "$PLAIN"
run "$EMPTY" status
check "status: mode 0644: not ONVIF-ready (ak_rtsp refuses it)" test "$(cat "$OUT")" = "enabled (user viewer, not ONVIF-ready)"
chmod 600 "$PLAIN"
run "$EMPTY" status
check "status: mode 0600 and matching: ready" test "$(cat "$OUT")" = "enabled (user viewer, ONVIF-ready)"

# set USER of the camera command also keeps both files, disable removes both
feed "S3NTINEL-pass9
S3NTINEL-pass9
"
run "$WORK/in" set viewer
check "set: both files" test -f "$FILE" -a -f "$PLAIN"
run "$EMPTY" disable
check "disable removes both files" test "$RC" = 0 -a ! -e "$FILE" -a ! -e "$PLAIN"
printf 'user=x\npass=y\n' >"$PLAIN"
run "$EMPTY" disable
check "disable also removes an orphan plain-text copy" test "$RC" = 0 -a ! -e "$PLAIN"
run "$EMPTY" status
check "status: disabled again" test "$(cat "$OUT")" = disabled

# --- 5. an invalid file is reported -----------------------------------------
printf 'nonsense\n' >"$FILE"
run "$EMPTY" status
check "status: INVALID, exit 3" test "$RC" = 3 -a "$(head -c 7 "$OUT")" = INVALID
run "$EMPTY" show
check "show: INVALID, exit 3" test "$RC" = 3
printf 'u:akam:%s\nv:akam:%s\n' "$good" "$good" >"$FILE"
run "$EMPTY" status
check "status: a second line makes the file INVALID (as for ak_rtsp)" test "$RC" = 3
printf 'u:akam:%s\n' "$good" >"$FILE"
run "$EMPTY" status
check "status: valid again" test "$RC" = 0 -a "$(cat "$OUT")" = "enabled (user u, not ONVIF-ready)"

# --- 6. disable ---------------------------------------------------------------
run "$EMPTY" disable
check "disable removes the file" test "$RC" = 0 -a ! -e "$FILE"
run "$EMPTY" disable
check "disable twice is fine" test "$RC" = 0
run "$EMPTY" status
check "status after disable" test "$(cat "$OUT")" = disabled
run "$EMPTY" frobnicate
check "unknown command: usage, exit 2" test "$RC" = 2
run "$EMPTY"
check "no command: usage, exit 2" test "$RC" = 2

# --- 7. the password never reaches a command line ----------------------------
# A recording md5sum stands in for the applet: everything it is given in argv is logged.
SPY=$WORK/spy
mkdir -p "$SPY"
cat >"$SPY/md5sum" <<EOF
#!/bin/sh
echo "argv: \$*" >>"$WORK/spy.log"
exec "$BIN/md5sum" "\$@"
EOF
chmod +x "$SPY/md5sum"
for a in sync mv chmod mkdir rm stty cat ls; do ln -s "$BIN/$a" "$SPY/$a"; done
feed "S3NTINEL-spy
S3NTINEL-spy
"
: >"$WORK/spy.log"
env -i PATH="$SPY" AKAM_ROOT="$ROOT" "${SHCMD[@]}" "$SCRIPT" set spyuser <"$WORK/in" >"$OUT" 2>&1
check "spy md5sum was used" grep -q '^argv:' "$WORK/spy.log"
check "the password was not in any argv" bash -c "! grep -q S3NTINEL '$WORK/spy.log'"
feed "spyuser
S3NTINEL-spy2
"
: >"$WORK/spy.log"
env -i PATH="$SPY" AKAM_ROOT="$ROOT" "${SHCMD[@]}" "$SCRIPT" set-creds <"$WORK/in" >"$OUT" 2>&1
check "set-creds: spy md5sum used and the password was not in any argv" bash -c "grep -q '^argv:' '$WORK/spy.log' && ! grep -q S3NTINEL '$WORK/spy.log'"
rm -f "$PLAIN"
rm -f "$FILE"

# --- 8. read-only CONFIG: a clear error, no partial file ----------------------
if [ "$(id -u)" != 0 ]; then
    rm -rf "$CONF/akam"
    chmod 555 "$CONF"
    feed "u:akam:$good
"
    run "$WORK/in" set-hash
    check "read-only CONFIG: exit 1" test "$RC" = 1
    check "read-only CONFIG: explicit message" has 'cannot'
    chmod 755 "$CONF"
fi

# --- 9. the file is what ak_rtsp reads: end-to-end with ffprobe ----------------
if command -v ffprobe >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    cc -std=gnu99 -I"$REPO/src/ak_rtsp" -o "$WORK/srv" "$REPO/tests/native/rtsp_auth_server.c" \
        "$REPO/src/ak_rtsp/rtsp_auth.c" "$REPO/src/ak_rtsp/md5.c" 2>/dev/null
    E2E='n0t:S3NTINEL@p/ss w0rd'
    ENC='n0t%3AS3NTINEL%40p%2Fss%20w0rd'
    feed "$E2E
$E2E
"
    run "$WORK/in" set e2e
    "$WORK/srv" "$FILE" >"$WORK/srv.out" 2>"$WORK/srv.err" &
    SRV=$!
    port=""
    for _ in $(seq 1 50); do
        port=$(sed -n 's/^PORT //p' "$WORK/srv.out" | head -1)
        [ -n "$port" ] && break
        sleep 0.1
    done
    timeout 30 ffprobe -v error -rtsp_transport tcp -i "rtsp://e2e:$ENC@127.0.0.1:$port/" >/dev/null 2>&1
    check "ffprobe logs in with the file written by the script" grep -q '^DESCRIBE 200' "$WORK/srv.out"
    kill "$SRV" 2>/dev/null
    wait "$SRV" 2>/dev/null
else
    echo "skip: ffprobe or cc missing, no end-to-end check"
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
