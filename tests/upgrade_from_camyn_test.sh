#!/usr/bin/env bash
# Host-side test of the upgrade path from an image built under the former name
# (Camyn) to an Akam image, end to end in a fake root:
#   - the camera already holds /etc/config/dropbear/{authorized_keys,host key}
#     persisted by the old image (that partition survives a reflash of the APP),
#   - the first-boot SD card was prepared for the old image: camyn/authorized_keys
#     and an empty CAMYN_MAINTENANCE at its root,
#   - the Akam service.sh (patched with the DEFAULT flag names) records the
#     maintenance marker, akam_ssh.sh provisions SSH, akam_lockdown.sh must leave
#     the toolkit telnet alone while the old flag is in use.
# Then the same camera without the card: telnet killed, SSH still up.
# Camera scripts run under $SH_UNDER_TEST (default: bootstrap BusyBox 1.35.0, else
# "busybox sh", else "sh"). Everything is fake; nothing touches hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TPL=$REPO/tools/firmware_patch_templates
FIXTURES=$REPO/tests/fixtures
TCBB=$REPO/tools/.toolchain/busybox-1.35.0-x86_64/busybox

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif [ -x "$TCBB" ]; then SH_UNDER_TEST="$TCBB sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
PASS=0
FAIL=0
cleanup() {
    local p
    for p in "$WORK"/pids/*; do
        [ -f "$p" ] && kill -9 "$(cat "$p")" 2>/dev/null
    done
    for p in "$WORK"/*/tmp/dropbear.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$WORK/pids"

ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}
alive() {
    local st
    st=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null) || return 1
    [ -n "$st" ] && [ "$st" != Z ]
}
dead() { ! alive "$1"; }

KEYDIR=$WORK/keys
mkdir -p "$KEYDIR"
ssh-keygen -q -t ed25519 -N '' -C test1 -f "$KEYDIR/k1"
K1=$(cat "$KEYDIR/k1.pub")

STUBS=$WORK/stubs
mkdir -p "$STUBS"
cat >"$STUBS/ps" <<'EOF'
#!/bin/sh
echo "  PID USER       VSZ STAT COMMAND"
cat "$REG"
EOF
chmod 755 "$STUBS/ps"

# The Akam service.sh as the build makes it: patched with the default flag names,
# then pointed at the fake root (paths only; the guard text is the shipped one).
PATCHED=$WORK/service.sh.patched
mkdir -p "$WORK/svc/sbin"
cp "$FIXTURES/app_fake_variants/d/sbin/service.sh" "$WORK/svc/sbin/service.sh"
AKAM_PATCH_REPORT=$WORK/svc.diff "$TPL/patch_service_sh.sh" "$WORK/svc" >/dev/null 2>&1
cp "$WORK/svc/sbin/service.sh" "$PATCHED"

# new_cam <name>: fake root with the stubs, the ssh helper and a camera that was
# provisioned by the old image (persisted key + host key), /tmp empty (fresh boot).
new_cam() {
    R=$WORK/$1
    mkdir -p "$R/etc/config/dropbear" "$R/tmp" "$R/usr/bin" "$R/usr/sbin" "$R/mnt" "$R/sbin"
    chmod 755 "$R" "$R/etc" "$R/etc/config" "$R/tmp"
    chmod 700 "$R/etc/config/dropbear"
    printf '%s\n' "$K1" >"$R/etc/config/dropbear/authorized_keys"
    chmod 600 "$R/etc/config/dropbear/authorized_keys"
    echo "FAKE-HOSTKEY-FROM-OLD-IMAGE" >"$R/etc/config/dropbear/dropbear_ed25519_host_key"
    chmod 600 "$R/etc/config/dropbear/dropbear_ed25519_host_key"
    cp "$TPL/akam_ssh.sh" "$R/usr/sbin/akam_ssh.sh"
    cp "$TPL/akam_lockdown.sh" "$R/usr/sbin/akam_lockdown.sh"
    chmod 755 "$R/usr/sbin/"*
    cat >"$R/usr/bin/dropbearkey" <<'EOF'
#!/bin/sh
echo "FAKE-NEW-HOSTKEY $$" >"$4"
EOF
    cat >"$R/usr/bin/dropbear" <<'EOF'
#!/bin/sh
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
    sed -e "s#/mnt/#$R/mnt/#g" -e "s#/tmp/akam_maintenance#$R/tmp/akam_maintenance#g" \
        -e "s#/tmp/tf_update.sh#$R/tmp/tf_update.sh#g" \
        -e "s#^\(.*\)sh /usr/sbin/tf_mount.sh#\1:#" -e "s#cp /usr/sbin/tf_update.sh .*#:#" \
        -e 's#^\(\s*\)rm .*#\1:#' "$PATCHED" >"$R/sbin/service.sh"
    printf '#!/bin/sh\necho flashed >>"%s/flashed"\n' "$R" >"$R/tmp/tf_update.sh"
    REG=$R/pstable
    : >"$REG"
    rm -f "$WORK"/pids/*
    export AKAM_ROOT=$R AKAM_WAIT_TRIES=2
}
spawn() { # spawn <name> <ps-line>
    local pid
    sleep 600 >/dev/null 2>&1 &
    pid=$!
    echo "$pid" >"$WORK/pids/$1"
    printf '%5d root      1234 S    %s\n' "$pid" "$2" >>"$REG"
}
pid_of() { cat "$WORK/pids/$1"; }
killdb() { [ -f "$R/tmp/dropbear.pid" ] && kill "$(cat "$R/tmp/dropbear.pid")" 2>/dev/null; rm -f "$R/tmp/dropbear.pid"; }
run_service() { "${SHCMD[@]}" "$R/sbin/service.sh" >/dev/null 2>&1; }
run_ssh() { "${SHCMD[@]}" "$R/usr/sbin/akam_ssh.sh" start; }
run_lockdown() {
    env "AKAM_ROOT=$R" "REG=$REG" "PATH=$STUBS:$PATH" AKAM_FAST_ROUNDS=3 AKAM_FAST_PERIOD=0 \
        AKAM_SLOW_ROUNDS=2 AKAM_SLOW_PERIOD=0 timeout 30 "${SHCMD[@]}" "$R/usr/sbin/akam_lockdown.sh"
}
persisted_key_ok() { cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1"); }
hostkey_kept() { [ "$(cat "$R/etc/config/dropbear/dropbear_ed25519_host_key")" = "FAKE-HOSTKEY-FROM-OLD-IMAGE" ]; }

check "shipped service.sh passes sh -n under the shell under test" "${SHCMD[@]}" -n "$PATCHED"

echo "== U1. old first-boot card (camyn/ + CAMYN_MAINTENANCE), key persisted by the old image"
new_cam u1
mkdir -p "$R/mnt/camyn"
printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
: >"$R/mnt/CAMYN_MAINTENANCE"
run_service
check "service.sh: SD hook ran with the old flag" test -s "$R/flashed"
check "service.sh: RAM marker recorded" test -e "$R/tmp/akam_maintenance"
run_ssh
check "ssh: dropbear launched" test -s "$R/tmp/dropbear.pid"
check "ssh: key in place" persisted_key_ok
check "ssh: old host key kept (no new fingerprint)" hostkey_kept
check "ssh: card key found in the old folder" grep -q 'DEPRECATED.*camyn/' "$R/tmp/akam_ssh.log"
spawn telnetd "telnetd -p 24 -l /bin/sh"
spawn hack "sh /mnt/custom/scripts/telnet.sh"
spawn dropbear "dropbear -p 22 -P /tmp/dropbear.pid"
check "lockdown: exits 0" run_lockdown
check "lockdown: toolkit telnetd survives" alive "$(pid_of telnetd)"
check "lockdown: telnet.sh respawner survives" alive "$(pid_of hack)"
check "lockdown: dropbear untouched" alive "$(pid_of dropbear)"
check "lockdown: logs maintenance mode" grep -q 'maintenance mode: telnet not touched' "$R/tmp/akam_lockdown.log"
# card removed while up: the RAM marker keeps telnet alive until reboot
rm -f "$R/mnt/CAMYN_MAINTENANCE"
rm -rf "$R/mnt/camyn"
check "card removed, marker kept: telnetd still survives" run_lockdown
check "card removed, marker kept: telnetd alive" alive "$(pid_of telnetd)"
killdb

echo "== U2. old card without key file but with CAMYN_MAINTENANCE: persisted key still works"
new_cam u2
: >"$R/mnt/CAMYN_MAINTENANCE"
run_service
check "service.sh: marker recorded" test -e "$R/tmp/akam_maintenance"
run_ssh
check "ssh: dropbear launched" test -s "$R/tmp/dropbear.pid"
check "ssh: persisted key untouched" persisted_key_ok
check "ssh: host key kept" hostkey_kept
spawn telnetd "telnetd -p 24 -l /bin/sh"
check "lockdown: exits 0" run_lockdown
check "lockdown: telnetd survives" alive "$(pid_of telnetd)"
killdb

echo "== U3. reboot without the card: telnet off, SSH up"
new_cam u3
run_service
check "service.sh: SD hook not run" test ! -e "$R/flashed"
check "service.sh: no marker" test ! -e "$R/tmp/akam_maintenance"
run_ssh
check "ssh: dropbear launched" test -s "$R/tmp/dropbear.pid"
check "ssh: persisted key untouched" persisted_key_ok
check "ssh: host key kept" hostkey_kept
check "ssh: launch uses the persisted host key" grep -q -- "-r $R/etc/config/dropbear/dropbear_ed25519_host_key" "$R/tmp/dropbear_launches"
spawn telnetd "telnetd -p 24 -l /bin/sh"
spawn hack "sh /mnt/custom/scripts/telnet.sh"
spawn dropbear "dropbear -p 22 -P /tmp/dropbear.pid"
check "lockdown: exits 0" run_lockdown
check "lockdown: telnetd killed" dead "$(pid_of telnetd)"
check "lockdown: telnet.sh respawner killed" dead "$(pid_of hack)"
check "lockdown: dropbear untouched" alive "$(pid_of dropbear)"
killdb

echo "== U4. a card prepared for Akam (akam/ + AKAM_MAINTENANCE) behaves the same"
new_cam u4
mkdir -p "$R/mnt/akam"
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
: >"$R/mnt/AKAM_MAINTENANCE"
run_service
check "service.sh: SD hook ran" test -s "$R/flashed"
check "service.sh: marker recorded" test -e "$R/tmp/akam_maintenance"
run_ssh
check "ssh: no deprecation note" bash -c "! grep -q DEPRECATED '$R/tmp/akam_ssh.log'"
spawn telnetd "telnetd -p 24 -l /bin/sh"
check "lockdown: exits 0" run_lockdown
check "lockdown: telnetd survives" alive "$(pid_of telnetd)"
killdb

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
