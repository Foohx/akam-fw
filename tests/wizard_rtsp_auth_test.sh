#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for ./akam rtsp-auth (set, disable, status) against the REAL camera command
# tools/firmware_patch_templates/akam_rtsp_auth.sh run by a fake ssh in a fake root under the
# pinned BusyBox. Checks that the password and its hash are never in a command line, in the
# output or in the wizard log. The real camera is never contacted.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BB=${AKAM_BUSYBOX:-$ROOT/tools/.toolchain/busybox-1.35.0-x86_64/busybox}
if [ ! -x "$BB" ]; then
    echo "SKIP: the pinned BusyBox is required (run tools/build/bootstrap.sh)"
    exit 0
fi
AKAM="$ROOT/akam"
CAMCMD="$ROOT/tools/firmware_patch_templates/akam_rtsp_auth.sh"

T=$(mktemp -d)
trap 'chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { local n=$1; shift; if "$@"; then ok "$n"; else ko "$n"; fi; }
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

export HOME=$T/home NO_COLOR=1
mkdir -p "$HOME"
unset AKAM_STATE_DIR AKAM_SSH

# Fake camera root and the applets of the camera's BusyBox only.
export STUB_DIR=$T/stub FAKE_ROOT=$T/camroot FAKE_BB=$BB FAKE_CMD=$CAMCMD
mkdir -p "$STUB_DIR" "$FAKE_ROOT/etc/config" "$STUB_DIR/bin"
for x in md5sum sync mv chmod mkdir rm stty cat ls; do ln -s "$BB" "$STUB_DIR/bin/$x"; done
AUTHFILE=$FAKE_ROOT/etc/config/akam/rtsp_auth
PLAINFILE=$FAKE_ROOT/etc/config/akam/credentials

cat >"$STUB_DIR/ssh" <<'EOF'
#!/usr/bin/env bash
# fake ssh: logs its argv, runs akam_rtsp_auth.sh for real, answers `true`
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
[ "${STUB_SSH_RC:-0}" = 0 ] || exit "$STUB_SSH_RC"
case $cmd in
    true) exit 0 ;;
    /usr/sbin/akam_rtsp_auth.sh*)
        # shellcheck disable=SC2086
        exec env -i PATH="$STUB_DIR/bin" AKAM_ROOT="$FAKE_ROOT" "$FAKE_BB" sh "$FAKE_CMD" ${cmd#/usr/sbin/akam_rtsp_auth.sh}
        ;;
esac
exit 0
EOF
chmod +x "$STUB_DIR/ssh"
export AKAM_SSH=$STUB_DIR/ssh
printf 'fake key\n' >"$T/key"

new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
# a STATE ARGS...: answers come from stdin; AKAM_FORCE_TTY=1 lets host_confirm read them.
a() { local st=$1; shift; AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
# a_notty: no terminal at all.
a_notty() { local st=$1; shift; bash "$AKAM" --no-color --state-dir "$st" --ip 127.0.0.1 --identity "$T/key" "$@"; }
ha1_of() { printf '%s' "$1" | md5sum | cut -d' ' -f1; }

# ---- status when nothing is set ------------------------------------------------
S=$(new_state status0)
a "$S" rtsp-auth status </dev/null >"$T/s0.out" 2>&1
check "status: succeeds on a camera without a password" test $? -eq 0
check "status: says the stream is open" has "$T/s0.out" "RTSP password: disabled"
a "$S" rtsp-auth </dev/null >"$T/s0b.out" 2>&1
check "rtsp-auth alone is status" has "$T/s0b.out" "RTSP password: disabled"
a "$S" rtsp-auth frobnicate </dev/null >"$T/s0c.out" 2>&1
check "unknown action: exit 2" test $? -eq 2

# ---- set -------------------------------------------------------------------------
PW='S3NTINEL-pass!1'
HA1=$(ha1_of "viewer:akam:$PW")
S=$(new_state set1)
: >"$STUB_DIR/ssh.log"
printf 'viewer\n%s\n%s\n' "$PW" "$PW" | a "$S" rtsp-auth set >"$T/set1.out" 2>&1
check "set: succeeds" test $? -eq 0
check "set: the camera file holds user:akam:HA1" test "$(cat "$AUTHFILE" 2>/dev/null)" = "viewer:akam:$HA1"
check "set: the file is 0600" test "$(stat -c %a "$AUTHFILE")" = 600
check "set: the camera also holds the plain-text copy (user=/pass=)" test "$(cat "$PLAINFILE" 2>/dev/null)" = "user=viewer
pass=$PW"
check "set: the plain-text copy is 0600" test "$(stat -c %a "$PLAINFILE")" = 600
check "set: the wizard tells the password is stored on the camera for ONVIF" has "$T/set1.out" "also stored on the camera"
check "set: the wizard names ONVIF" has "$T/set1.out" "ONVIF can verify it"
check "set: the camera command was set-creds" has "$STUB_DIR/ssh.log" "set-creds"
check "set: the password is not in any ssh command line" bash -c '! grep -q -e S3NTINEL -e "$2" "$1"' _ "$STUB_DIR/ssh.log" "$HA1"
check "set: the password is not in the output" hasnt "$T/set1.out" S3NTINEL
check "set: the HA1 is not in the output" hasnt "$T/set1.out" "$HA1"
check "set: the password and the HA1 are not in the wizard log" bash -c '! grep -q -e S3NTINEL -e "$2" "$1"' _ "$S/akam.log" "$HA1"
check "set: the password is not in the state directory at all" bash -c '! grep -rq -e S3NTINEL "$1"' _ "$S"
check "set: the URL hint has a placeholder, not the password" has "$T/set1.out" "rtsp://viewer:PASSWORD@127.0.0.1:554/"
check "set: URL-encoding is explained" has "$T/set1.out" "URL-encoded"
a "$S" rtsp-auth status </dev/null >"$T/set1s.out" 2>&1
check "status: enabled with the user, ONVIF-ready" has "$T/set1s.out" "enabled (user viewer, ONVIF-ready)"

# Awkward password characters are hashed byte for byte.
PW2=' a\b"c$d:e f@g/h#i?j%k'
S=$(new_state set2)
printf 'bob.1\n%s\n%s\n' "$PW2" "$PW2" | a "$S" rtsp-auth set >"$T/set2.out" 2>&1
check "set: special characters accepted" test "$(cat "$AUTHFILE")" = "bob.1:akam:$(ha1_of "bob.1:akam:$PW2")"
check "set: special characters stored byte for byte in the plain copy" test "$(cat "$PLAINFILE")" = "user=bob.1
pass=$PW2"

# Refusals change nothing and never reach the camera.
cp "$AUTHFILE" "$T/before"
cp "$PLAINFILE" "$T/before_plain"
: >"$STUB_DIR/ssh.log"
S=$(new_state set3)
printf 'bob.1\nS3NTINEL-one\nS3NTINEL-two\n' | a "$S" rtsp-auth set >"$T/set3.out" 2>&1
check "set: different passwords refused" test $? -ne 0
printf 'bob.1\nshort7!\nshort7!\n' | a "$S" rtsp-auth set >"$T/set3b.out" 2>&1
check "set: 7 characters refused" bash -c '[ "$1" -ne 0 ] && grep -q "too short" "$2"' _ $? "$T/set3b.out"
printf 'bad user\nS3NTINEL-pass1\nS3NTINEL-pass1\n' | a "$S" rtsp-auth set >"$T/set3c.out" 2>&1
check "set: a user name with a space refused" bash -c '[ "$1" -ne 0 ] && grep -q "invalid user name" "$2"' _ $? "$T/set3c.out"
printf 'a:b\nS3NTINEL-pass1\nS3NTINEL-pass1\n' | a "$S" rtsp-auth set >/dev/null 2>&1
check "set: a user name with a colon refused" test $? -ne 0
check "set: refusals left the camera file unchanged" cmp -s "$AUTHFILE" "$T/before"
LONGPW=$(printf 'x%.0s' $(seq 1 129))
printf 'bob.1\n%s\n%s\n' "$LONGPW" "$LONGPW" | a "$S" rtsp-auth set >"$T/set3d.out" 2>&1
check "set: 129 characters refused" bash -c '[ "$1" -ne 0 ] && grep -q "too long" "$2"' _ $? "$T/set3d.out"
printf 'bob.1\nS3NTINEL\tpass1\nS3NTINEL\tpass1\n' | a "$S" rtsp-auth set >"$T/set3e.out" 2>&1
check "set: a control character in the password refused" bash -c '[ "$1" -ne 0 ] && grep -q "control character" "$2"' _ $? "$T/set3e.out"
check "set: refusals left the plain-text copy unchanged" cmp -s "$PLAINFILE" "$T/before_plain"
check "set: no set-creds call was made for a refusal" bash -c '! grep -q set-creds "$1"' _ "$STUB_DIR/ssh.log"
check "set: nothing secret in the output of refusals" bash -c '! grep -q S3NTINEL "$1" "$2" "$3"' _ "$T/set3.out" "$T/set3b.out" "$T/set3c.out"

S=$(new_state set4)
printf 'viewer\nS3NTINEL-pass1\nS3NTINEL-pass1\n' | STUB_SSH_RC=255 a "$S" rtsp-auth set >"$T/set4.out" 2>&1
check "set: an unreachable camera is reported before any password is asked" bash -c '[ "$1" -ne 0 ] && grep -q "cannot log in" "$2" && ! grep -q "Password" "$2"' _ $? "$T/set4.out"

# ---- disable -------------------------------------------------------------------------
S=$(new_state dis1)
printf 'n\n' | a "$S" rtsp-auth disable >"$T/dis1.out" 2>&1
check "disable: answering n keeps the password" test -e "$AUTHFILE"
a_notty "$S" rtsp-auth disable </dev/null >"$T/dis1b.out" 2>&1
check "disable: refuses without a terminal" bash -c '[ "$1" -ne 0 ] && test -e "$2"' _ $? "$AUTHFILE"
printf 'y\n' | a "$S" rtsp-auth disable >"$T/dis1c.out" 2>&1
check "disable: answering y removes the file" bash -c '[ "$1" -eq 0 ] && ! test -e "$2"' _ $? "$AUTHFILE"
check "disable: removes the plain-text copy too" test ! -e "$PLAINFILE"
check "disable: tells the stream is open again" has "$T/dis1c.out" "open again"

# ---- dry-run: nothing is sent --------------------------------------------------------
: >"$STUB_DIR/ssh.log"
S=$(new_state dry)
for act in status set disable; do
    a "$S" --dry-run rtsp-auth "$act" </dev/null >"$T/dry_$act.out" 2>&1
    check "dry-run $act: succeeds" test $? -eq 0
    check "dry-run $act: shows a WOULD line" has "$T/dry_$act.out" WOULD
done
check "dry-run: the camera was not contacted" test ! -s "$STUB_DIR/ssh.log"
check "dry-run: the file was not created" test ! -e "$AUTHFILE"
check "dry-run set: says the password will be stored for ONVIF" has "$T/dry_set.out" "ONVIF can verify it"
check "dry-run: no plain-text copy was created" test ! -e "$PLAINFILE"

echo "wizard_rtsp_auth_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
