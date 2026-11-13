#!/usr/bin/env bash
# Host-side test of tools/flash_scripts/akam_authkeys.sh (the script the assistant runs from the camera's
# RAM to add or remove one SSH key), in a fake root. It runs under the pinned BusyBox ash when the
# toolchain has it (the camera's shell), and under the host's sh. Nothing here touches a camera.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/tools/flash_scripts/akam_authkeys.sh
# shellcheck source=tools/build/deps.lock
. "$REPO/tools/build/deps.lock"

SHELLS=("sh")
BB=$REPO/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox
if [ -x "$BB" ]; then SHELLS+=("$BB sh"); fi
if ! command -v ssh-keygen >/dev/null 2>&1; then
    echo "SKIP: ssh-keygen is not installed"
    exit 0
fi

WORK=$(mktemp -d)
trap 'chmod -R u+rwx "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else ko "$d"; fi; }

for n in a b c; do ssh-keygen -q -t ed25519 -N '' -C "key-$n" -f "$WORK/k$n"; done
ssh-keygen -q -t rsa -b 2048 -N '' -C rsa -f "$WORK/krsa"

# fields FILE: "type base64" of every key line, in order
fields() { awk '$1 == "ssh-ed25519" { print $1 " " $2 }' "$1"; }
mode_of() { stat -c '%a' "$1"; }
export -f fields

for SH in "${SHELLS[@]}"; do
    echo "shell under test: $SH"
    read -r -a SHCMD <<<"$SH"
    R=$WORK/root-${#SHCMD[@]}-$RANDOM
    DB=$R/etc/config/dropbear
    HOMEK=$R/tmp/akam_home/authorized_keys
    mkdir -p "$DB" "$R/tmp/akam_home"
    cp "$WORK/ka.pub" "$DB/authorized_keys"
    chmod 600 "$DB/authorized_keys"
    cp "$WORK/ka.pub" "$HOMEK"
    # keys FILE KEYMODE: run the script with the key line on stdin, print stdout; status in $RC
    run() { # mode keyfile -> out in $OUT, status in $RC
        OUT=$(AKAM_ROOT=$R "${SHCMD[@]}" "$SCRIPT" "$1" <"$2" 2>"$WORK/err")
        RC=$?
    }

    run add "$WORK/kb.pub"
    check "add: appends the key, says added" bash -c '[ "$1" = "AUTHKEYS=added" ] && [ "$2" = 0 ]' _ "$OUT" "$RC"
    check "add: both keys are listed, in order" bash -c '[ "$(cat "$1")" = "$(cat "$2" "$3" | awk "NF>=2{print \$1\" \"\$2}")" ]' _ <(fields "$DB/authorized_keys") "$WORK/ka.pub" "$WORK/kb.pub"
    check "add: the file keeps mode 600" test "$(mode_of "$DB/authorized_keys")" = 600
    check "add: the login copy Dropbear reads is refreshed" cmp -s "$DB/authorized_keys" "$HOMEK"
    check "add: the login copy is mode 600" test "$(mode_of "$HOMEK")" = 600
    check "add: no temp file is left" bash -c '! ls -A "$1" "$2" | grep -q "authorized_keys.new"' _ "$DB" "$R/tmp/akam_home"

    run add "$WORK/kb.pub"
    check "add again: idempotent (present, nothing duplicated)" bash -c '[ "$1" = "AUTHKEYS=present" ] && [ "$(grep -c "" "$2")" = 2 ]' _ "$OUT" "$DB/authorized_keys"

    # a comment on the input never makes a second line, and a key in the list with another comment is the same key
    sed 's/key-b$/another-comment/' "$WORK/kb.pub" >"$WORK/kb2.pub"
    run add "$WORK/kb2.pub"
    check "add: the comment is ignored when comparing" bash -c '[ "$1" = "AUTHKEYS=present" ]' _ "$OUT"

    run remove "$WORK/ka.pub"
    check "remove: takes out exactly that key" bash -c '[ "$1" = "AUTHKEYS=removed" ] && [ "$(fields "$2")" = "$(fields "$3")" ]' _ "$OUT" "$DB/authorized_keys" "$WORK/kb.pub"
    check "remove: the login copy follows" cmp -s "$DB/authorized_keys" "$HOMEK"

    run remove "$WORK/ka.pub"
    check "remove: a key that is not there is reported absent, exit 0" bash -c '[ "$1" = "AUTHKEYS=absent" ] && [ "$2" = 0 ]' _ "$OUT" "$RC"

    before=$(cat "$DB/authorized_keys")
    run remove "$WORK/kb.pub"
    check "remove: the last key is refused (exit 3), the list is unchanged" bash -c '[ "$1" = "AUTHKEYS=REFUSED_LAST_KEY" ] && [ "$2" = 3 ] && [ "$(cat "$3")" = "$4" ]' _ "$OUT" "$RC" "$DB/authorized_keys" "$before"
    check "remove: the refusal leaves the login copy alone" cmp -s "$DB/authorized_keys" "$HOMEK"

    # unrelated lines (comments, other keys) are kept; the file without a final newline is read fully
    { echo "# my note"; printf '%s' "$(cat "$WORK/kb.pub")"; } >"$DB/authorized_keys"
    run add "$WORK/kc.pub"
    check "add: comment lines and a last line without newline are kept" bash -c 'head -1 "$1" | grep -q "^# my note$" && [ "$(grep -c "ssh-ed25519" "$1")" = 2 ]' _ "$DB/authorized_keys"
    run remove "$WORK/kb.pub"
    check "remove: other lines are kept" bash -c 'head -1 "$1" | grep -q "^# my note$" && [ "$(fields "$1")" = "$(fields "$2")" ]' _ "$DB/authorized_keys" "$WORK/kc.pub"

    # refusals before anything is read or written
    cp "$DB/authorized_keys" "$WORK/frozen"
    run add "$WORK/krsa.pub"
    check "add: an RSA key is refused (BAD_KEY, exit 2)" bash -c '[ "$1" = "AUTHKEYS=BAD_KEY" ] && [ "$2" = 2 ]' _ "$OUT" "$RC"
    printf 'ssh-ed25519 AAAA\n' >"$WORK/short"
    run add "$WORK/short"
    check "add: a truncated key is refused" bash -c '[ "$1" = "AUTHKEYS=BAD_KEY" ]' _ "$OUT"
    printf 'ssh-ed25519 %s$(touch /tmp/x)\n' "$(awk '{print $2}' "$WORK/kb.pub")" >"$WORK/inj"
    run add "$WORK/inj"
    check "add: shell metacharacters in the key are refused" bash -c '[ "$1" = "AUTHKEYS=BAD_KEY" ]' _ "$OUT"
    : >"$WORK/empty"
    run add "$WORK/empty"
    check "add: empty input is refused" bash -c '[ "$1" = "AUTHKEYS=BAD_KEY" ]' _ "$OUT"
    check "refusals change nothing" cmp -s "$DB/authorized_keys" "$WORK/frozen"
    run frob "$WORK/kb.pub"
    check "an unknown mode is a usage error (exit 2)" test "$RC" = 2

    # without a tmpfs login copy (the ephemeral case) the persistent file is still updated
    rm -rf "$R/tmp/akam_home"
    run add "$WORK/kb.pub"
    check "add: works when there is no login copy directory" bash -c '[ "$1" = "AUTHKEYS=added" ] && [ ! -e "$2" ]' _ "$OUT" "$HOMEK"
    # a missing key directory is a clean failure
    OUT=$(AKAM_ROOT=$WORK/none "${SHCMD[@]}" "$SCRIPT" add <"$WORK/kb.pub" 2>/dev/null); RC=$?
    check "no dropbear directory: WRITE_FAILED, exit 1" bash -c '[ "$1" = "AUTHKEYS=WRITE_FAILED" ] && [ "$2" = 1 ]' _ "$OUT" "$RC"
done

echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
