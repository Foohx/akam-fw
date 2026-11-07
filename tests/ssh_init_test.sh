#!/usr/bin/env bash
# Host-side test of camyn_ssh.sh and the ak_rtsp_wrapper.sh hook, run in a fake
# root with stub dropbear/dropbearkey. Camera scripts are run with $SH_UNDER_TEST
# (default: "busybox sh" if installed, else "sh"). Nothing here touches hardware.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TPL=$REPO/tools/firmware_patch_templates
FIXTURE=$REPO/tests/fixtures/app_fake

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"; else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
PASS=0
FAIL=0
cleanup() {
    local p
    for p in "$WORK"/*/tmp/dropbear.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
    done
    pkill -f "$WORK" 2>/dev/null || true
    chmod -R u+rwx "$WORK" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { # check <description> <command...>
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}
mode_of() { stat -c '%a' "$1"; }

# Test keys are generated now; no key material is stored in the repository.
KEYDIR=$WORK/keys
mkdir -p "$KEYDIR"
ssh-keygen -q -t ed25519 -N '' -C test1 -f "$KEYDIR/k1"
ssh-keygen -q -t ed25519 -N '' -C test2 -f "$KEYDIR/k2"
ssh-keygen -q -t rsa -b 2048 -N '' -C rsa -f "$KEYDIR/rsa"
K1=$(cat "$KEYDIR/k1.pub")
K2=$(cat "$KEYDIR/k2.pub")
KRSA=$(cat "$KEYDIR/rsa.pub")

# new_root <name>: build a fake root with stubs, wrapper and the fixture.
new_root() {
    R=$WORK/$1
    mkdir -p "$R/etc/config" "$R/tmp" "$R/usr/bin" "$R/usr/sbin" "$R/mnt/camyn" "$R/app"
    chmod 755 "$R" "$R/etc" "$R/etc/config" "$R/tmp"
    cp -a "$FIXTURE/." "$R/app/"
    cp "$TPL/camyn_ssh.sh" "$R/usr/sbin/camyn_ssh.sh"
    chmod 755 "$R/usr/sbin/camyn_ssh.sh"
    # Stub dropbearkey: writes a unique fake key file.
    cat >"$R/usr/bin/dropbearkey" <<'EOF'
#!/bin/sh
# stub: dropbearkey -t ed25519 -f FILE
[ "$1" = "-t" ] && [ "$2" = "ed25519" ] && [ "$3" = "-f" ] || exit 2
[ -n "${STUB_KEY_DELAY:-}" ] && sleep "$STUB_KEY_DELAY"
echo "FAKE-HOSTKEY $$ $(date +%s%N)" >"$4"
echo x >>"$(dirname "$4")/../keygen_count" 2>/dev/null || true
exit 0
EOF
    # Stub dropbear: records args, "daemonizes" via a background sleep.
    cat >"$R/usr/bin/dropbear" <<'EOF'
#!/bin/sh
# stub: parse -P pidfile, record args, detach a sleeper.
pf=
prev=
for a in "$@"; do
    [ "$prev" = "-P" ] && pf=$a
    prev=$a
done
echo "$*" >>"$(dirname "$pf")/dropbear_launches"
( sh -c 'sleep 300; :' dropbear </dev/null >/dev/null 2>&1 & echo $! >"$pf" )
exit 0
EOF
    chmod 755 "$R/usr/bin/dropbear" "$R/usr/bin/dropbearkey"
    cat >"$R/usr/bin/ak_rtsp" <<'EOF'
#!/bin/sh
echo "ak_rtsp ran" >>"$(dirname "$0")/../../tmp/ak_rtsp_ran"
exit 7
EOF
    chmod 755 "$R/usr/bin/ak_rtsp"
    export CAMYN_ROOT=$R CAMYN_WAIT_TRIES=2
    unset STUB_KEY_DELAY || true
}
ssh_run() { "${SHCMD[@]}" "$R/usr/sbin/camyn_ssh.sh" "$@"; }
killdb() { [ -f "$R/tmp/dropbear.pid" ] && kill "$(cat "$R/tmp/dropbear.pid")" 2>/dev/null || true; rm -f "$R/tmp/dropbear.pid"; }
launches() { [ -f "$R/tmp/dropbear_launches" ] && wc -l <"$R/tmp/dropbear_launches" || echo 0; }

# Wrapper copy that points at the fake root (only the helper/ak_rtsp/wpa paths).
make_wrapper() {
    sed -e "s#/usr/sbin/camyn#$R/usr/sbin/camyn#g" \
        -e "s#/usr/bin/ak_rtsp#$R/usr/bin/ak_rtsp#g" \
        -e "s#^WPA_CONF=/etc#WPA_CONF=$R/etc#" \
        "$TPL/ak_rtsp_wrapper.sh" >"$R/wrapper.sh"
}

echo "== 1. first boot: key from SD, host key generated"
new_root t1
printf '# comment\n%s\n\n' "$K1" >"$R/mnt/camyn/authorized_keys"
ssh_run start
D=$R/etc/config/dropbear
check "host key exists" test -s "$D/dropbear_ed25519_host_key"
check "host key mode 600" test "$(mode_of "$D/dropbear_ed25519_host_key")" = 600
check "dropbear dir mode 700" test "$(mode_of "$D")" = 700
check "authorized_keys mode 600" test "$(mode_of "$D/authorized_keys")" = 600
check "authorized_keys == SD key" cmp -s "$D/authorized_keys" <(printf '%s\n' "$K1")
check "SD file untouched" test -f "$R/mnt/camyn/authorized_keys"
check "dropbear launched once" test "$(launches)" = 1
check "launch args: -D dir, -r key, no -s/-g/-j/-k/-w" bash -c "
  a=\$(cat '$R/tmp/dropbear_launches')
  case \"\$a\" in *\"-D $D\"*\"-r $D/dropbear_ed25519_host_key\"*|*\"-r $D/dropbear_ed25519_host_key\"*\"-D $D\"*) ;; *) exit 1;; esac
  case \" \$a \" in *' -s '*|*' -g '*|*' -j '*|*' -k '*|*' -w '*) exit 1;; esac"
check "status reports running" ssh_run status
check "log mentions install" grep -q 'authorized_keys installed from SD' "$R/tmp/camyn_ssh.log"
check "log contains no key material" bash -c "! grep -q AAAAC3 '$R/tmp/camyn_ssh.log'"
check "lock released" test ! -d "$R/tmp/camyn_ssh.lock"
HK1=$(cat "$D/dropbear_ed25519_host_key")

echo "== 2. second boot: no regeneration, no duplicate daemon"
ssh_run start
check "still one launch (no duplicate)" test "$(launches)" = 1
check "host key unchanged" test "$(cat "$D/dropbear_ed25519_host_key")" = "$HK1"
check "keygen ran once" test "$(wc -l <"$R/etc/config/keygen_count")" = 1
# simulate reboot (/tmp cleared, daemon gone): key must persist, daemon restarts
killdb
rm -rf "$R/tmp"/* "$R/etc/config/keygen_count"
ssh_run start
check "daemon relaunched after reboot" test -s "$R/tmp/dropbear.pid"
check "host key unchanged after reboot" test "$(cat "$D/dropbear_ed25519_host_key")" = "$HK1"
check "no keygen after reboot" test ! -e "$R/etc/config/keygen_count"
check "log says unchanged" grep -q 'authorized_keys unchanged' "$R/tmp/camyn_ssh.log"
# key replaced on SD -> CONFIG follows
printf '%s\n' "$K2" >"$R/mnt/camyn/authorized_keys"
killdb
rm -rf "$R/tmp"/*
ssh_run start
check "authorized_keys replaced by new SD content" cmp -s "$D/authorized_keys" <(printf '%s\n' "$K2")
killdb

echo "== 3. concurrent start: one daemon"
new_root t3
printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
export STUB_KEY_DELAY=1
ssh_run start &
P1=$!
sleep 0.3
ssh_run start &
P2=$!
wait "$P1" "$P2"
unset STUB_KEY_DELAY
check "exactly one launch" test "$(launches)" = 1
check "second instance logged the lock" grep -q 'holds the lock' "$R/tmp/camyn_ssh.log"
killdb
# stale lock from a dead process is taken over
mkdir "$R/tmp/camyn_ssh.lock"
echo 999999 >"$R/tmp/camyn_ssh.lock/pid"
ssh_run start
check "stale lock taken over" test "$(launches)" = 2
killdb
# fresh lock whose pid file is not written yet must NOT be taken over
rm -f "$R/tmp/dropbear_launches"
mkdir "$R/tmp/camyn_ssh.lock"
sleep 30 &
HOLDER=$!
( sleep 0.5; echo "$HOLDER" >"$R/tmp/camyn_ssh.lock/pid" ) &
ssh_run start
check "lock without pid yet is respected" test "$(launches)" = 0
kill "$HOLDER" 2>/dev/null || true
wait 2>/dev/null || true
rm -rf "$R/tmp/camyn_ssh.lock"

echo "== 4. missing SD / missing file"
new_root t4
rm -rf "${R:?}/mnt"
ssh_run start
check "dropbear still started" test -s "$R/tmp/dropbear.pid"
check "host key generated" test -s "$R/etc/config/dropbear/dropbear_ed25519_host_key"
check "no authorized_keys created" test ! -e "$R/etc/config/dropbear/authorized_keys"
check "warning logged" grep -q 'WARNING: no authorized key' "$R/tmp/camyn_ssh.log"
killdb

echo "== 5. SD key validation"
new_root t5
{
    echo "$KRSA"
    echo "command=\"/bin/id\" $K1"
    echo "from=\"1.2.3.4\" $K1"
    echo "ssh-ed25519 AAAAshort"
    printf '%s\r\n' "$K2"
} >"$R/mnt/camyn/authorized_keys"
ssh_run start
check "only the plain ed25519 line kept" test "$(wc -l <"$R/etc/config/dropbear/authorized_keys")" = 1
check "kept key is K2 (CR stripped)" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K2")
check "4 rejected logged" grep -q '1 accepted, 4 rejected' "$R/tmp/camyn_ssh.log"
killdb
# all-invalid SD file must not wipe an existing good file
echo "$KRSA" >"$R/mnt/camyn/authorized_keys"
rm -rf "$R/tmp"/*
ssh_run start
check "existing keys kept when SD has none valid" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K2")
killdb

echo "== 6. missing dropbear binary: launcher returns 0, wrapper still runs ak_rtsp"
new_root t6
rm "$R/usr/bin/dropbear"
ssh_run start && rc=0 || rc=$?
check "camyn_ssh exits 0" test "$rc" = 0
check "no daemon" test ! -e "$R/tmp/dropbear.pid"
check "reason logged" grep -q 'missing/not executable' "$R/tmp/camyn_ssh.log"
make_wrapper
"${SHCMD[@]}" "$R/wrapper.sh" >/dev/null 2>&1 && rc=0 || rc=$?
check "wrapper returned ak_rtsp status (7)" test "$rc" = 7
check "ak_rtsp executed" test -f "$R/tmp/ak_rtsp_ran"
# no camyn_ssh.sh at all: legacy behaviour
rm "$R/usr/sbin/camyn_ssh.sh" "$R/tmp/ak_rtsp_ran"
"${SHCMD[@]}" "$R/wrapper.sh" >/dev/null 2>&1 && rc=0 || rc=$?
check "wrapper without camyn_ssh.sh: status 7, ak_rtsp ran" test "$rc" = 7 -a -f "$R/tmp/ak_rtsp_ran"

echo "== 7. wrapper does not wait for a slow/blocked camyn_ssh.sh"
new_root t7
printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
export STUB_KEY_DELAY=5
make_wrapper
t0=$(date +%s%N)
"${SHCMD[@]}" "$R/wrapper.sh" >/dev/null 2>&1 && rc=0 || rc=$?
t1=$(date +%s%N)
ms=$(((t1 - t0) / 1000000))
echo "  wrapper took ${ms} ms while dropbearkey was blocking 5 s"
check "wrapper status 7" test "$rc" = 7
check "ak_rtsp ran" test -f "$R/tmp/ak_rtsp_ran"
check "wrapper returned in < 2 s" test "$ms" -lt 2000
unset STUB_KEY_DELAY
sleep 6
check "background launcher finished later" test -s "$R/tmp/dropbear.pid"
killdb

echo "== 8. wrong permissions are corrected"
new_root t8
printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
ssh_run start
killdb
D=$R/etc/config/dropbear
chmod 777 "$D"
chmod 666 "$D/authorized_keys" "$D/dropbear_ed25519_host_key"
rm -rf "$R/tmp"/*
ssh_run start
check "dir back to 700" test "$(mode_of "$D")" = 700
check "authorized_keys back to 600" test "$(mode_of "$D/authorized_keys")" = 600
check "host key back to 600" test "$(mode_of "$D/dropbear_ed25519_host_key")" = 600
check "modes of /etc/config, /etc, / logged" bash -c "grep -c 'mode of' '$R/tmp/camyn_ssh.log' | grep -q 3"
killdb

echo "== 9. /etc/config not writable: ephemeral host key"
new_root t9
if [ "$(id -u)" = 0 ]; then
    echo "  skip - running as root, chmod cannot make a dir read-only"
else
    chmod 555 "$R/etc/config"
    printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
    ssh_run start
    check "dropbear started anyway" test -s "$R/tmp/dropbear.pid"
    check "ephemeral key in tmp" test -s "$R/tmp/camyn_dropbear/dropbear_ed25519_host_key"
    check "nothing persisted in CONFIG" test ! -e "$R/etc/config/dropbear"
    check "fallback logged" grep -q 'not writable' "$R/tmp/camyn_ssh.log"
    check "SD key provisioned into the ephemeral dir" test -s "$R/tmp/camyn_dropbear/authorized_keys"
    killdb
    chmod 755 "$R/etc/config"
fi

echo "== 9a. pidfile pointing at a non-dropbear process is stale"
new_root t9a
sleep 300 </dev/null >/dev/null 2>&1 &
other=$!
echo "$other" >"$R/tmp/dropbear.pid"
check "status: not running" bash -c '! "$@"' _ "${SHCMD[@]}" "$R/usr/sbin/camyn_ssh.sh" status
ssh_run start
check "dropbear launched despite stale pidfile" test "$(launches)" = 1
kill "$other" 2>/dev/null
killdb

echo "== 9b. /etc/shells without /bin/sh: bind mount of a private list"
new_root t9b
mkdir -p "$R/stubbin"
cat >"$R/stubbin/mount" <<'EOF'
#!/bin/sh
echo "$*" >>"$CAMYN_ROOT/tmp/mount_calls"
exit 0
EOF
chmod 755 "$R/stubbin/mount"
printf '/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "mount --bind called over /etc/shells" grep -qx -- "--bind $R/tmp/camyn/shells $R/etc/shells" "$R/tmp/mount_calls"
check "private shells file lists /bin/sh" grep -qx /bin/sh "$R/tmp/camyn/shells"
check "private shells file mode 644, dir 755" test "$(mode_of "$R/tmp/camyn/shells")" = 644 -a "$(mode_of "$R/tmp/camyn")" = 755
check "original /etc/shells untouched" test "$(cat "$R/etc/shells")" = /bin/bash
check "bind mount logged" grep -q 'bind-mounted' "$R/tmp/camyn_ssh.log"
check "dropbear still launched" test -s "$R/tmp/dropbear.pid"
killdb
new_root t9c
mkdir -p "$R/stubbin"
# shellcheck disable=SC2016
printf '#!/bin/sh\necho x >>"$CAMYN_ROOT/tmp/mount_calls"\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
printf '/bin/sh\n/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "/bin/sh already listed: no mount" test ! -e "$R/tmp/mount_calls"
killdb
new_root t9d
mkdir -p "$R/stubbin"
# shellcheck disable=SC2016
printf '#!/bin/sh\necho x >>"$CAMYN_ROOT/tmp/mount_calls"\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
PATH="$R/stubbin:$PATH" ssh_run start
check "no /etc/shells: no mount" test ! -e "$R/tmp/mount_calls"
killdb
new_root t9e
mkdir -p "$R/stubbin"
printf '#!/bin/sh\nexit 1\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
printf '/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "mount failure is not fatal: dropbear launched" test -s "$R/tmp/dropbear.pid"
check "mount failure logged" grep -q 'bind mount failed' "$R/tmp/camyn_ssh.log"
killdb

echo "== 9c. camera scripts only use applets present in the stock ROOTFS BusyBox"
# The stock BusyBox 1.30.1 has no tr, uniq, gzip, mktemp, flock, nohup, wget.
# shellcheck disable=SC2016
check "no tr/uniq/gzip/mktemp/flock/nohup/wget command in camera scripts" bash -c '
    ! grep -nE "(^|[|;&(]|\\$\\()[[:space:]]*(tr|uniq|gzip|mktemp|flock|nohup|wget)[[:space:]]" \
        "$1"/camyn_ssh.sh "$1"/camyn_lockdown.sh "$1"/ak_rtsp_wrapper.sh' _ "$TPL"

echo "== 10. fixture sanity"
check "fixture has telnetd/ftpd lines" grep -q telnetd "$FIXTURE/sbin/service.sh"
check "fixture symlink present" test -L "$FIXTURE/lib/libfake.so"

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
