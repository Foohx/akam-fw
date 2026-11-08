#!/usr/bin/env bash
# Host-side test of akam_ssh.sh and the ak_rtsp_wrapper.sh hook, run in a fake
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
        { [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; } || true
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
    mkdir -p "$R/etc/config" "$R/tmp" "$R/usr/bin" "$R/usr/sbin" "$R/mnt/akam" "$R/app"
    chmod 755 "$R" "$R/etc" "$R/etc/config" "$R/tmp"
    cp -a "$FIXTURE/." "$R/app/"
    cp "$TPL/akam_ssh.sh" "$R/usr/sbin/akam_ssh.sh"
    chmod 755 "$R/usr/sbin/akam_ssh.sh"
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
    export AKAM_ROOT=$R AKAM_WAIT_TRIES=2
    unset STUB_KEY_DELAY || true
}
ssh_run() { "${SHCMD[@]}" "$R/usr/sbin/akam_ssh.sh" "$@"; }
killdb() { { [ -f "$R/tmp/dropbear.pid" ] && kill "$(cat "$R/tmp/dropbear.pid")" 2>/dev/null; } || true; rm -f "$R/tmp/dropbear.pid"; }
launches() { [ -f "$R/tmp/dropbear_launches" ] && wc -l <"$R/tmp/dropbear_launches" || echo 0; }

# Wrapper copy that points at the fake root (only the helper/ak_rtsp/wpa paths).
make_wrapper() {
    sed -e "s#/usr/sbin/akam#$R/usr/sbin/akam#g" \
        -e "s#/usr/bin/ak_rtsp#$R/usr/bin/ak_rtsp#g" \
        -e "s#^WPA_CONF=/etc#WPA_CONF=$R/etc#" \
        "$TPL/ak_rtsp_wrapper.sh" >"$R/wrapper.sh"
}

echo "== 1. first boot: key from SD, host key generated"
new_root t1
printf '# comment\n%s\n\n' "$K1" >"$R/mnt/akam/authorized_keys"
ssh_run start
D=$R/etc/config/dropbear
check "host key exists" test -s "$D/dropbear_ed25519_host_key"
check "host key mode 600" test "$(mode_of "$D/dropbear_ed25519_host_key")" = 600
check "dropbear dir mode 700" test "$(mode_of "$D")" = 700
check "authorized_keys mode 600" test "$(mode_of "$D/authorized_keys")" = 600
check "authorized_keys == SD key" cmp -s "$D/authorized_keys" <(printf '%s\n' "$K1")
check "SD file untouched" test -f "$R/mnt/akam/authorized_keys"
check "dropbear launched once" test "$(launches)" = 1
check "launch args: -D dir, -r key, no -s/-g/-j/-k/-w" bash -c "
  a=\$(cat '$R/tmp/dropbear_launches')
  case \"\$a\" in *\"-D $D\"*\"-r $D/dropbear_ed25519_host_key\"*|*\"-r $D/dropbear_ed25519_host_key\"*\"-D $D\"*) ;; *) exit 1;; esac
  case \" \$a \" in *' -s '*|*' -g '*|*' -j '*|*' -k '*|*' -w '*) exit 1;; esac"
check "status reports running" ssh_run status
check "log mentions install" grep -q 'authorized_keys installed from SD' "$R/tmp/akam_ssh.log"
check "log contains no key material" bash -c "! grep -q AAAAC3 '$R/tmp/akam_ssh.log'"
check "lock released" test ! -d "$R/tmp/akam_ssh.lock"
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
check "log says unchanged" grep -q 'authorized_keys unchanged' "$R/tmp/akam_ssh.log"
# key replaced on SD -> CONFIG follows
printf '%s\n' "$K2" >"$R/mnt/akam/authorized_keys"
killdb
rm -rf "$R/tmp"/*
ssh_run start
check "authorized_keys replaced by new SD content" cmp -s "$D/authorized_keys" <(printf '%s\n' "$K2")
killdb

echo "== 3. concurrent start: one daemon"
new_root t3
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
export STUB_KEY_DELAY=1
ssh_run start &
P1=$!
sleep 0.3
ssh_run start &
P2=$!
wait "$P1" "$P2"
unset STUB_KEY_DELAY
check "exactly one launch" test "$(launches)" = 1
check "second instance logged the lock" grep -q 'holds the lock' "$R/tmp/akam_ssh.log"
killdb
# stale lock from a dead process is taken over
mkdir "$R/tmp/akam_ssh.lock"
echo 999999 >"$R/tmp/akam_ssh.lock/pid"
ssh_run start
check "stale lock taken over" test "$(launches)" = 2
killdb
# fresh lock whose pid file is not written yet must NOT be taken over
rm -f "$R/tmp/dropbear_launches"
mkdir "$R/tmp/akam_ssh.lock"
sleep 30 &
HOLDER=$!
( sleep 0.5; echo "$HOLDER" >"$R/tmp/akam_ssh.lock/pid" ) &
ssh_run start
check "lock without pid yet is respected" test "$(launches)" = 0
kill "$HOLDER" 2>/dev/null || true
wait 2>/dev/null || true
rm -rf "$R/tmp/akam_ssh.lock"

echo "== 4. missing SD / missing file"
new_root t4
rm -rf "${R:?}/mnt"
ssh_run start
check "dropbear still started" test -s "$R/tmp/dropbear.pid"
check "host key generated" test -s "$R/etc/config/dropbear/dropbear_ed25519_host_key"
check "no authorized_keys created" test ! -e "$R/etc/config/dropbear/authorized_keys"
check "warning logged" grep -q 'WARNING: no authorized key' "$R/tmp/akam_ssh.log"
killdb

echo "== 4b. SD not mounted at boot (stock unmounts /mnt): read-only private mount"
new_root t4b
rm -rf "${R:?}/mnt"
mkdir -p "$R/stubbin" "$R/dev"
: >"$R/dev/mmcblk0p1"
cat >"$R/stubbin/mount" <<'EOF'
#!/bin/sh
# stub: mount -t <fs> -o ro <dev> <dir>; vfat "fails", exfat "succeeds" and exposes a key
echo "$*" >>"$AKAM_ROOT/tmp/mount_calls"
[ "$2" = exfat ] || exit 1
mkdir -p "$6/akam" && cp "$AKAM_ROOT/sdkey" "$6/akam/authorized_keys"
EOF
cat >"$R/stubbin/umount" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/umount_calls"
rm -rf "$1/akam"
EOF
chmod 755 "$R/stubbin/mount" "$R/stubbin/umount"
echo "$K1" >"$R/sdkey"
PATH="$R/stubbin:$PATH" ssh_run start
check "tried vfat then exfat on p1, read-only" test "$(grep -v '^-o bind' "$R/tmp/mount_calls" | cut -d' ' -f1-3 | tr '\n' ,)" = "-t vfat -o,-t exfat -o,"
check "key installed from the private mount" test "$(wc -l <"$R/etc/config/dropbear/authorized_keys")" = 1
check "private mount unmounted" grep -q akam_sd "$R/tmp/umount_calls"
killdb

# The card is found where it is ALREADY mounted (first-boot report: the toolkit
# mounts it on /tmp/sd with file bind-mounts, /mnt is an empty tmpfs).
# sd_root <name> <mount-point>: fake root, card content under <mount-point>,
# a /proc/mounts with unrelated lines and file bind-mounts from the card.
sd_root() {
    new_root "$1"
    rm -rf "${R:?}/mnt"
    mkdir -p "$R/stubbin" "$R/dev" "$R$2/akam" "$R/etc/somefile"
    rmdir "$R/etc/somefile"
    : >"$R/etc/somefile"
    : >"$R/dev/mmcblk0p1"
    echo "$K1" >"$R$2/akam/authorized_keys"
    cat >"$R/proc_mounts" <<EOM
rootfs / squashfs ro,relatime 0 0
tmpfs /mnt tmpfs rw,relatime 0 0
/dev/mmcblk0p1 /etc/somefile vfat rw,relatime,fmask=0022 0 0
/dev/mmcblk0p1 /etc/missing_target vfat rw,relatime 0 0
/dev/mmcblk0p1 $2 vfat rw,relatime,fmask=0022 0 0
/dev/mtdblock5 /etc/config jffs2 rw,relatime 0 0
EOM
    export AKAM_PROC_MOUNTS=$R/proc_mounts
    # mount/umount stubs that record every call and would FAIL the test if used
    # shellcheck disable=SC2016  # $* and $AKAM_ROOT must expand in the generated stub, not here
    printf '#!/bin/sh\necho "$*" >>"$AKAM_ROOT/tmp/mount_calls"\nexit 1\n' >"$R/stubbin/mount"
    # shellcheck disable=SC2016
    printf '#!/bin/sh\necho "$*" >>"$AKAM_ROOT/tmp/umount_calls"\n' >"$R/stubbin/umount"
    chmod 755 "$R/stubbin/mount" "$R/stubbin/umount"
}

echo "== 4c. SD already mounted on /tmp/sd (toolkit) with file bind-mounts"
sd_root t4c /tmp/sd
PATH="$R/stubbin:$PATH" ssh_run start
check "key installed from /tmp/sd/akam" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1")
check "no SD mount attempted" bash -c "! grep -q -e '-t vfat' -e '-t exfat' '$R/tmp/mount_calls' 2>/dev/null"
check "log names the mount point" grep -q 'already mounted on /tmp/sd' "$R/tmp/akam_ssh.log"
check "SD file untouched" test -f "$R/tmp/sd/akam/authorized_keys"
killdb

echo "== 4d. SD already mounted on /mnt"
sd_root t4d /mnt
PATH="$R/stubbin:$PATH" ssh_run start
check "key installed from /mnt/akam" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1")
check "no SD mount attempted" bash -c "! grep -q -e '-t vfat' -e '-t exfat' '$R/tmp/mount_calls' 2>/dev/null"
killdb

echo "== 4e. SD mounted, file bind-mount lines only match a file: not a card mount"
sd_root t4e /tmp/sd
# only the file bind-mount lines remain: the card is NOT mounted on a directory
grep -v ' /tmp/sd ' "$R/proc_mounts" >"$R/proc_mounts.new"
mv "$R/proc_mounts.new" "$R/proc_mounts"
rm -rf "$R/tmp/sd"
PATH="$R/stubbin:$PATH" ssh_run start
check "falls back to a private read-only mount attempt" grep -q -- '-o ro' "$R/tmp/mount_calls"
check "no key without the card" test ! -e "$R/etc/config/dropbear/authorized_keys"
killdb

echo "== 4f. SD mounted but without akam/authorized_keys: no second mount"
sd_root t4f /tmp/sd
rm -f "$R/tmp/sd/akam/authorized_keys"
PATH="$R/stubbin:$PATH" ssh_run start
check "no SD mount attempted" bash -c "! grep -q -e '-t vfat' -e '-t exfat' '$R/tmp/mount_calls' 2>/dev/null"
check "reason logged" grep -q 'no akam/authorized_keys there' "$R/tmp/akam_ssh.log"
check "warning logged" grep -q 'WARNING: no authorized key' "$R/tmp/akam_ssh.log"
killdb

echo "== 4g. SD not mounted and the mount fails: error is logged"
new_root t4g
rm -rf "${R:?}/mnt"
mkdir -p "$R/stubbin" "$R/dev"
: >"$R/dev/mmcblk0p1"
printf 'tmpfs /mnt tmpfs rw 0 0\n' >"$R/proc_mounts"
export AKAM_PROC_MOUNTS=$R/proc_mounts
printf '#!/bin/sh\necho "mount: wrong fs type, bad option" >&2\nexit 32\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
PATH="$R/stubbin:$PATH" ssh_run start
check "mount error text logged" grep -q 'failed: mount: wrong fs type' "$R/tmp/akam_ssh.log"
check "both filesystems tried" test "$(grep -c 'failed:' "$R/tmp/akam_ssh.log")" = 2
check "dropbear still started" test -s "$R/tmp/dropbear.pid"
killdb
# no device node at all: said so
rm -f "$R/dev/mmcblk0p1" "$R/tmp/akam_ssh.log"
PATH="$R/stubbin:$PATH" ssh_run start
check "missing device node logged" grep -q 'no /dev/mmcblk0p1 or /dev/mmcblk0 device node' "$R/tmp/akam_ssh.log"
killdb
unset AKAM_PROC_MOUNTS

echo "== 5. SD key validation"
new_root t5
{
    echo "$KRSA"
    echo "command=\"/bin/id\" $K1"
    echo "from=\"1.2.3.4\" $K1"
    echo "ssh-ed25519 AAAAshort"
    printf '%s\r\n' "$K2"
} >"$R/mnt/akam/authorized_keys"
ssh_run start
check "only the plain ed25519 line kept" test "$(wc -l <"$R/etc/config/dropbear/authorized_keys")" = 1
check "kept key is K2 (CR stripped)" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K2")
check "4 rejected logged" grep -q '1 accepted, 4 rejected' "$R/tmp/akam_ssh.log"
killdb
# all-invalid SD file must not wipe an existing good file
echo "$KRSA" >"$R/mnt/akam/authorized_keys"
rm -rf "$R/tmp"/*
ssh_run start
check "existing keys kept when SD has none valid" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K2")
killdb

echo "== 6. missing dropbear binary: launcher returns 0, wrapper still runs ak_rtsp"
new_root t6
rm "$R/usr/bin/dropbear"
ssh_run start && rc=0 || rc=$?
check "akam_ssh exits 0" test "$rc" = 0
check "no daemon" test ! -e "$R/tmp/dropbear.pid"
check "reason logged" grep -q 'missing/not executable' "$R/tmp/akam_ssh.log"
make_wrapper
"${SHCMD[@]}" "$R/wrapper.sh" >/dev/null 2>&1 && rc=0 || rc=$?
check "wrapper returned ak_rtsp status (7)" test "$rc" = 7
check "ak_rtsp executed" test -f "$R/tmp/ak_rtsp_ran"
# no akam_ssh.sh at all: legacy behaviour
rm "$R/usr/sbin/akam_ssh.sh" "$R/tmp/ak_rtsp_ran"
"${SHCMD[@]}" "$R/wrapper.sh" >/dev/null 2>&1 && rc=0 || rc=$?
check "wrapper without akam_ssh.sh: status 7, ak_rtsp ran" test "$rc" = 7 -a -f "$R/tmp/ak_rtsp_ran"

echo "== 7. wrapper does not wait for a slow/blocked akam_ssh.sh"
new_root t7
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
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
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
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
check "modes of /etc/config, /etc, / logged" bash -c "grep -c 'mode of' '$R/tmp/akam_ssh.log' | grep -q 3"
killdb

echo "== 9. /etc/config not writable: ephemeral host key"
new_root t9
if [ "$(id -u)" = 0 ]; then
    echo "  skip - running as root, chmod cannot make a dir read-only"
else
    chmod 555 "$R/etc/config"
    printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
    ssh_run start
    check "dropbear started anyway" test -s "$R/tmp/dropbear.pid"
    check "ephemeral key in tmp" test -s "$R/tmp/akam_dropbear/dropbear_ed25519_host_key"
    check "nothing persisted in CONFIG" test ! -e "$R/etc/config/dropbear"
    check "fallback logged" grep -q 'not writable' "$R/tmp/akam_ssh.log"
    check "SD key provisioned into the ephemeral dir" test -s "$R/tmp/akam_dropbear/authorized_keys"
    killdb
    chmod 755 "$R/etc/config"
fi

echo "== 9a. pidfile pointing at a non-dropbear process is stale"
new_root t9a
sleep 300 >/dev/null 2>&1 &
other=$!
echo "$other" >"$R/tmp/dropbear.pid"
check "status: not running" bash -c '! "$@"' _ "${SHCMD[@]}" "$R/usr/sbin/akam_ssh.sh" status
ssh_run start
check "dropbear launched despite stale pidfile" test "$(launches)" = 1
kill "$other" 2>/dev/null
killdb

echo "== 9b. /etc/shells without /bin/sh: bind mount of a private list"
new_root t9b
mkdir -p "$R/stubbin"
cat >"$R/stubbin/mount" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/mount_calls"
exit 0
EOF
chmod 755 "$R/stubbin/mount"
printf '/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "mount --bind called over /etc/shells" grep -qx -- "--bind $R/tmp/akam/shells $R/etc/shells" "$R/tmp/mount_calls"
check "private shells file lists /bin/sh" grep -qx /bin/sh "$R/tmp/akam/shells"
check "private shells file mode 644, dir 755" test "$(mode_of "$R/tmp/akam/shells")" = 644 -a "$(mode_of "$R/tmp/akam")" = 755
check "original /etc/shells untouched" test "$(cat "$R/etc/shells")" = /bin/bash
check "bind mount logged" grep -q 'bind-mounted' "$R/tmp/akam_ssh.log"
check "dropbear still launched" test -s "$R/tmp/dropbear.pid"
killdb
new_root t9c
mkdir -p "$R/stubbin"
# shellcheck disable=SC2016
printf '#!/bin/sh\necho x >>"$AKAM_ROOT/tmp/mount_calls"\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
printf '/bin/sh\n/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "/bin/sh already listed: no shells bind" bash -c "! grep -q -e --bind \"$R/tmp/mount_calls\""
killdb
new_root t9d
mkdir -p "$R/stubbin"
# shellcheck disable=SC2016
printf '#!/bin/sh\necho x >>"$AKAM_ROOT/tmp/mount_calls"\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
PATH="$R/stubbin:$PATH" ssh_run start
check "no /etc/shells: no shells bind" bash -c "! grep -q -e --bind \"$R/tmp/mount_calls\""
killdb
new_root t9e
mkdir -p "$R/stubbin"
printf '#!/bin/sh\nexit 1\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
printf '/bin/bash\n' >"$R/etc/shells"
PATH="$R/stubbin:$PATH" ssh_run start
check "mount failure is not fatal: dropbear launched" test -s "$R/tmp/dropbear.pid"
check "mount failure logged" grep -q 'bind mount failed' "$R/tmp/akam_ssh.log"
killdb

echo "== 9c. camera scripts only use applets present in the stock ROOTFS BusyBox"
# The stock BusyBox 1.30.1 has no tr, uniq, gzip, mktemp, flock, nohup, wget.
# shellcheck disable=SC2016
check "no tr/uniq/gzip/mktemp/flock/nohup/wget command in camera scripts" bash -c '
    ! grep -nE "(^|[|;&(]|\\$\\()[[:space:]]*(tr|uniq|gzip|mktemp|flock|nohup|wget)[[:space:]]" \
        "$1"/akam_ssh.sh "$1"/akam_lockdown.sh "$1"/ak_rtsp_wrapper.sh' _ "$TPL"

echo "== 9d. root HOME = key directory (stock / and /etc are 0775: dropbear would refuse the key)"
# Stub mount: "-o bind SRC DST" copies SRC over DST (a real bind needs root).
bind_stub() {
    mkdir -p "$R/stubbin"
    cat >"$R/stubbin/mount" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/mount_calls"
[ "$1" = -o ] && [ "$2" = bind ] && cp "$3" "$4"
EOF
    chmod 755 "$R/stubbin/mount"
}
new_root t9f
bind_stub
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
printf 'root:x:0:0:root:/:/bin/sh\ndaemon:x:1:1:daemon:/usr/sbin:/bin/sh\n' >"$R/etc/passwd"
PATH="$R/stubbin:$PATH" ssh_run start
H=$R/tmp/akam_home
check "root HOME rewritten in the private passwd" grep -qx "root:x:0:0:root:$H:/bin/sh" "$R/etc/passwd"
check "other passwd entries kept" grep -qx 'daemon:x:1:1:daemon:/usr/sbin:/bin/sh' "$R/etc/passwd"
check "HOME dir 700, key copy 600" test "$(mode_of "$H")" = 700 -a "$(mode_of "$H/authorized_keys")" = 600
check "HOME key copy == SD key" cmp -s "$H/authorized_keys" <(printf '%s\n' "$K1")
check "dropbear -D is the HOME dir" grep -q -- "-D $H " "$R/tmp/dropbear_launches"
check "host key still persistent" grep -q -- "-r $R/etc/config/dropbear/dropbear_ed25519_host_key" "$R/tmp/dropbear_launches"
check "HOME change logged" grep -q 'root HOME is now' "$R/tmp/akam_ssh.log"
killdb
: >"$R/tmp/mount_calls"
ssh_run_pathed() { PATH="$R/stubbin:$PATH" ssh_run "$@"; }
ssh_run_pathed start
check "respawn: passwd already points at HOME, no second bind" bash -c "! grep -q -e '-o bind' '$R/tmp/mount_calls'"
check "respawn: -D still the HOME dir" grep -q -- "-D $H " "$R/tmp/dropbear_launches"
killdb

new_root t9g
bind_stub
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
rm -f "$R/etc/passwd"
ln -s config/passwd "$R/etc/passwd"
PATH="$R/stubbin:$PATH" ssh_run start
check "dangling /etc/passwd symlink: root entry created through it" grep -qx "root:x:0:0:root:$R/tmp/akam_home:/bin/sh" "$R/etc/config/passwd"
check "dangling passwd: dropbear -D is the HOME dir" grep -q -- "-D $R/tmp/akam_home " "$R/tmp/dropbear_launches"
killdb

new_root t9h
mkdir -p "$R/stubbin"
printf '#!/bin/sh\nexit 1\n' >"$R/stubbin/mount"
chmod 755 "$R/stubbin/mount"
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
printf 'root:x:0:0:root:/:/bin/sh\n' >"$R/etc/passwd"
PATH="$R/stubbin:$PATH" ssh_run start
check "bind failure: real passwd untouched" test "$(cat "$R/etc/passwd")" = 'root:x:0:0:root:/:/bin/sh'
check "bind failure: falls back to the persistent -D dir" grep -q -- "-D $R/etc/config/dropbear " "$R/tmp/dropbear_launches"
check "bind failure logged, dropbear launched" bash -c "grep -q 'could not set root HOME' '$R/tmp/akam_ssh.log' && test -s '$R/tmp/dropbear.pid'"
killdb

echo "== 9z. deprecated camyn/ SD folder (one-release fallback)"
new_root t9z1
mv "$R/mnt/akam" "$R/mnt/camyn"
printf '%s\n' "$K1" >"$R/mnt/camyn/authorized_keys"
ssh_run start
check "legacy: key installed from camyn/" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1")
check "legacy: deprecation logged" grep -q 'DEPRECATED.*camyn/.*rename it to akam/' "$R/tmp/akam_ssh.log"
killdb
# akam/ wins over camyn/ when both exist, and nothing is logged as deprecated
new_root t9z2
mkdir -p "$R/mnt/camyn"
printf '%s\n' "$K1" >"$R/mnt/akam/authorized_keys"
printf '%s\n' "$K2" >"$R/mnt/camyn/authorized_keys"
ssh_run start
check "both: akam/ wins" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1")
check "both: no deprecation note" bash -c "! grep -q DEPRECATED '$R/tmp/akam_ssh.log'"
killdb
# card already mounted (toolkit /tmp/sd) with only the old folder
sd_root t9z3 /tmp/sd
mv "$R/tmp/sd/akam" "$R/tmp/sd/camyn"
PATH="$R/stubbin:$PATH" ssh_run start
check "mounted card: key installed from camyn/" cmp -s "$R/etc/config/dropbear/authorized_keys" <(printf '%s\n' "$K1")
check "mounted card: deprecation logged" grep -q 'DEPRECATED' "$R/tmp/akam_ssh.log"
killdb
unset AKAM_PROC_MOUNTS

echo "== 10. fixture sanity"
check "fixture has telnetd/ftpd lines" grep -q telnetd "$FIXTURE/sbin/service.sh"
check "fixture symlink present" test -L "$FIXTURE/lib/libfake.so"

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
