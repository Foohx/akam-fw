#!/usr/bin/env bash
# Host-side test of the LSC Rotatable 2K overlay and rescue scripts
# (firmware/lsc-rotatable-2k-ak3918av130/{overlay,rescue}), run in a fake root with stubs for mount, ip, the
# card's busybox and dropbearmulti, and /appfs/script/start.sh. Camera scripts run under $SH_UNDER_TEST
# (default: the pinned BusyBox ash, else "busybox sh", else "sh"). Nothing here touches hardware or a device.
# Group "rot2k": runs in its own CI job, separate from the 1080p jobs.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
FW=$REPO/firmware/lsc-rotatable-2k-ak3918av130
PL=$REPO/platforms/anyka-ak3918av130
# The sources of the 2K app. The shared files are DERIVED from the build, not listed by hand: every source of POC3_ORDER (make print-sources) and
# every header they include (image/source_hash.sh --list, the same list that guards stale binaries), so a module added to model.mk is scanned without
# touching this test. Two scopes:
#   A2K_APP     "app code": the platform and model directories (their root, not helpers/ or boot/), the LED tool, and the derived shared files of the
#               2K-only feature directories (src/features/{ptz,track,scene}, hal_ptz.h, log.h: what the old app/ directory held). The "only file X may
#               open / bind / connect / know the PTZ controller" checks use it.
#   A2K_INFRA   every OTHER derived shared file (net/, api/, core/, the features shared with the 1080p): they legitimately open files and sockets, so they
#               only get the anti-brick scan (no flash, mtd, nvram, upgrade, factory, device node, spawn, PTZ init request).
# Checks that grepped the whole old app/ directory (-r, any file type: the Makefile, the scripts) use A2K_APPALL.
A2K_DERIVED=()
while IFS= read -r f; do A2K_DERIVED+=("$f"); done < <(sh "$FW/image/source_hash.sh" --list | grep "^$REPO/src/" | grep -E '\.[ch]$')
A2K_DERIVED_APP=()
A2K_INFRA=()
for f in "${A2K_DERIVED[@]}"; do
    case $f in
        "$REPO"/src/features/ptz/* | "$REPO"/src/features/track/* | "$REPO"/src/features/scene/* | "$REPO"/src/hal/hal_ptz.h | "$REPO"/src/core/log.h) A2K_DERIVED_APP+=("$f") ;;
        *) A2K_INFRA+=("$f") ;;
    esac
done
A2K_APP=()
while IFS= read -r f; do A2K_APP+=("$f"); done < <(printf '%s\n' "$PL"/*.c "$PL"/*.h "$PL"/probes/*.c "$FW"/*.c "$FW"/*.h "$FW"/helpers/led/led.c "${A2K_DERIVED_APP[@]}" | sort -u)
# every file of the old app/ directory (any type): the platform tree and the model root, plus the derived shared files and the build rules
A2K_APPALL=()
while IFS= read -r f; do A2K_APPALL+=("$f"); done < <({ find "$PL" -type f; find "$FW" -maxdepth 1 -type f; printf '%s\n' "${A2K_DERIVED[@]}" "$REPO"/src/build/*.mk; } | sort -u)
A2K_APPC=()
a2k_src() { if [ -f "$PL/$1.c" ]; then echo "$PL/$1.c"; else echo "$FW/$1.c"; fi; }
for f in "${A2K_APP[@]}"; do case $f in *.c) A2K_APPC+=("$f") ;; esac; done
# the derivation must not shrink the coverage: the files the closed list used to name are all scanned, and every compiled source is
for f in src/features/ptz/ptz_patrol.c src/features/ptz/ptz_patrol.h src/features/track/track_logic.c src/features/track/track_logic.h src/features/track/track_relay.c \
    src/features/track/track_relay.h src/features/track/track_cfgkv.h src/features/scene/scene_check.c src/features/scene/scene_check.h src/hal/hal_ptz.h src/core/log.h; do
    case " ${A2K_APP[*]} " in *" $REPO/$f "*) ;; *) echo "scan list lost $f" >&2; exit 1 ;; esac
done
while IFS= read -r f; do
    case " ${A2K_APP[*]} ${A2K_INFRA[*]} " in *" $f "*) ;; *) echo "scan list misses the compiled source $f" >&2; exit 1 ;; esac
done < <(make -s -C "$PL" print-sources)

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
echo "shell under test: $SH_UNDER_TEST"
read -r -a SHCMD <<<"$SH_UNDER_TEST"

WORK=$(mktemp -d)
PASS=0
FAIL=0
cleanup() {
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
not_alive() { ! kill -0 "$1" 2>/dev/null; }
has() { grep -q -- "$2" "$1" 2>/dev/null; }
not_has() { ! grep -q -- "$2" "$1" 2>/dev/null; }
hasnt() { ! grep -q -- "$2" "$1" 2>/dev/null; }
# wait_for <file> [seconds]: a background step has written the file.
wait_for() {
    local i=0
    while [ ! -e "$1" ] && [ "$i" -lt "${2:-50}" ]; do sleep 0.1; i=$((i + 1)); done
    [ -e "$1" ]
}

# Test keys are generated now; no key material is stored in the repository.
ssh-keygen -q -t ed25519 -N '' -C test1 -f "$WORK/k1"
ssh-keygen -q -t ed25519 -N '' -C test2 -f "$WORK/k2"
ssh-keygen -q -t rsa -b 2048 -N '' -C rsa -f "$WORK/krsa"

STUBS=$WORK/stubs
mkdir -p "$STUBS"
cat >"$STUBS/mount" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/mount.log"
[ -z "${STUB_MOUNT_FAIL:-}" ]
EOF
chmod 755 "$STUBS/mount"
cat >"$STUBS/ip" <<'EOF'
#!/bin/sh
# stub /sbin/ip: routes in $AKAM_ROOT/tmp/routes
R=$AKAM_ROOT/tmp/routes
touch "$R"
v6=""
if [ "$1" = -6 ]; then v6="v6 "; shift; fi
case "$1 $2" in
    "addr show") echo "    inet 192.0.2.9/24 brd 192.0.2.255 scope global wlan0" ;;
    "route show") echo "192.0.2.0/24 dev wlan0 scope link"; grep -v '^v6 ' "$R" || true ;;
    "route add")
        if [ "$3" = blackhole ]; then e="${v6}blackhole $4"; else e="$3 dev $5"; fi
        if grep -qx "$e" "$R"; then echo "RTNETLINK answers: File exists" >&2; exit 2; fi
        echo "$e" >>"$R" ;;
    "route del")
        if [ "$3" = blackhole ]; then e="${v6}blackhole $4"; else e="$3 dev $5"; fi
        grep -vx "$e" "$R" >"$R.n" || true; cat "$R.n" >"$R"; rm -f "$R.n" ;;
esac
exit 0
EOF
chmod 755 "$STUBS/ip"

# new_root <name>: a fake camera root with a card, the overlay, stubs and a stock start.sh.
new_root() {
    R=$WORK/$1
    SDC=$R/mnt/sdcard
    mkdir -p "$R/tmp" "$R/etc" "$R/appfs/script" "$R/config" "$R/proc/net" "$R/sbin" "$SDC/akam"
    : >"$R/etc/shadow"
    printf 'root:x:0:0:root:/root:/bin/sh\n' >"$R/etc/passwd"
    printf '/bin/sh\n' >"$R/etc/shells"
    cp "$FW"/boot/card/akam_boot.sh "$FW"/boot/card/yy_debug.ini "$SDC/"
    cp "$FW"/boot/card/akam/*.sh "$SDC/akam/"
    cat >"$SDC/busybox" <<'EOF'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/busybox.log"
EOF
    cat >"$R/appfs/script/start.sh" <<'EOF'
#!/bin/sh
echo started >"$AKAM_ROOT/tmp/start_ran"
EOF
    chmod 755 "$SDC/busybox" "$R/appfs/script/start.sh"
    cat >"$SDC/akam/dropbearmulti" <<'EOF'
#!/bin/sh
# stub multi-call dropbear
prog=$1
shift
case "$prog" in
    dropbearkey) echo "FAKE-HOSTKEY $$ $(date +%s%N)" >"$4" ;;
    dropbear) echo "$*" >"$AKAM_ROOT/tmp/dropbear.args"; echo 12345 >"$AKAM_ROOT/tmp/dropbear.pid" ;;
esac
EOF
    chmod 755 "$SDC/akam/dropbearmulti"
    cat "$WORK/k1.pub" "$WORK/k2.pub" >"$SDC/akam/authorized_keys"
    export AKAM_ROOT=$R AKAM_IP=$STUBS/ip AKAM_WAIT_TRIES=1 AKAM_WAIT_SLEEP=0 AKAM_VERIFY_TRIES=1 AKAM_VERIFY_SLEEP=0
    unset STUB_MOUNT_FAIL AKAM_SD AKAM_LOG AKAM_PARAM_CONF AKAM_SH
    export PATH=$STUBS:$ORIG_PATH
}
ORIG_PATH=$PATH
run() { # run <script> [args]: a card script under the shell under test
    local s=$1
    shift
    "${SHCMD[@]}" "$s" "$@"
}
boot() { # boot: akam_boot.sh as init runs it, then give the background steps a moment
    AKAM_SH=$SH_UNDER_TEST run "$SDC/akam_boot.sh" >/dev/null 2>&1 || true
    wait_for "$R/tmp/start_ran" || true
}

# ------------------------------------------------------------------------------- akam_boot.sh, nominal
echo "== akam_boot.sh"
new_root boot1
boot
check "start.sh handed off to" test -e "$R/tmp/start_ran"
check "telnetd started with the card busybox" has "$R/tmp/busybox.log" "telnetd -l /bin/sh -p 23"
check "shadow bind-mounted over /etc/shadow" has "$R/tmp/mount.log" "--bind $R/tmp/etc/shadow $R/etc/shadow"
check "tmpfs shadow holds the root entry" has "$R/tmp/etc/shadow" '^root:'
wait_for "$R/tmp/dropbear.args" || true
check "dropbear started in the background" test -s "$R/tmp/dropbear.args"
check "log on the card" has "$SDC/akam/akam_boot.log" "handed off to"
check "decloud off without the flag" test ! -s "$R/tmp/routes"

new_root boot2
: >"$SDC/akam/no_telnet"
boot
check "flag no_telnet: no telnet, still handed off" test ! -e "$R/tmp/busybox.log" -a -e "$R/tmp/start_ran"

# ------------------------------------------------------------------------------- hand-off survives every failure
echo "== hand-off survives failing steps"
fail_case() { # fail_case <description> <setup command...>: break one step, boot, start.sh must still run
    local d=$1
    shift
    new_root "case$((++CASE))"
    "$@"
    boot
    check "start.sh still started: $d" test -e "$R/tmp/start_ran"
}
CASE=0
break_mount() { export STUB_MOUNT_FAIL=1; }
fail_case "mount fails" break_mount
rmf() { rm -rf "${SDC:?}/$1"; }
fail_case "no busybox on the card" rmf busybox
broken() { printf 'exit 7\n' >"$SDC/akam/$1"; }
fail_case "start_ssh.sh exits with an error" broken start_ssh.sh
fail_case "enable_local.sh exits with an error" broken enable_local.sh
fail_case "decloud.sh exits with an error" broken decloud.sh
syntax() { printf 'if then fi ((\n' >"$SDC/akam/$1"; }
fail_case "start_ssh.sh has a syntax error" syntax start_ssh.sh
fail_case "enable_local.sh has a syntax error" syntax enable_local.sh
fail_case "decloud.sh has a syntax error" syntax decloud.sh
fail_case "start_ssh.sh missing" rmf akam/start_ssh.sh
fail_case "common.sh missing" rmf akam/common.sh
fail_case "common.sh has a syntax error" syntax common.sh
fail_case "akam/ directory missing" rmf akam
unwritable_tmp() { rm -rf "$R/tmp/etc"; : >"$R/tmp/etc"; }
fail_case "/tmp/etc cannot be created" unwritable_tmp
check "no set -e in akam_boot.sh" hasnt "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam_boot.sh" '^[[:space:]]*set -[a-z]*e'

# ------------------------------------------------------------------------------- decloud.sh
echo "== card-less boot links every image file (akam_flash_boot.sh step 1)"
FB=$(mktemp -d)
mkdir -p "$FB/img/bin" "$FB/fake"
for f in busybox common.sh akam_api_key.sh akam_net.sh own_app_stub.sh dropbearmulti; do : >"$FB/img/$f"; done
sed -n '/^# 1) the card-shaped tree/,/^) || say/p' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/flash/akam_flash_boot.sh" >"$FB/step1.sh"
check "flash boot step 1 extracted" test -s "$FB/step1.sh"
IMG=$FB/img FAKE=$FB/fake sh -c 'say() { :; }; . "$0"' "$FB/step1.sh" >/dev/null 2>&1
check "flash boot: akam_api_key.sh, akam_net.sh, bin are linked; busybox sits one level up" bash -c "[ -L '$FB/fake/akam/akam_api_key.sh' ] && [ -L '$FB/fake/akam/akam_net.sh' ] && [ -L '$FB/fake/akam/bin' ] && [ ! -e '$FB/fake/akam/busybox' ] && [ -L '$FB/fake/busybox' ]"
rm -rf "$FB"

echo "== decloud.sh"
new_root dc
run "$SDC/akam/decloud.sh" on
check "no flag: no route" test ! -s "$R/tmp/routes"
check "no flag: logged" has "$SDC/akam/akam_boot.log" "not enabled"
: >"$SDC/akam/decloud"
run "$SDC/akam/decloud.sh" on
check "flag: 0.0.0.0/1 blackholed" has "$R/tmp/routes" "blackhole 0.0.0.0/1"
check "flag: 128.0.0.0/1 blackholed" has "$R/tmp/routes" "blackhole 128.0.0.0/1"
run "$SDC/akam/decloud.sh" on
check "second run: no duplicate route" test "$(grep -c 'blackhole 0.0.0.0/1' "$R/tmp/routes")" = 1
check "flag: IPv6 global unicast blackholed" has "$R/tmp/routes" "v6 blackhole 2000::/3"
check "flag: multicast route kept through the interface" has "$R/tmp/routes" "224.0.0.0/4 dev wlan0"
check "flag: the LAN route is not touched" test "$("$STUBS/ip" route show | grep -c '^192.0.2.0/24')" = 1
check "second run: no duplicate multicast route" test "$(grep -c '^224.0.0.0/4' "$R/tmp/routes")" = 1
check "status says blackholed" run "$SDC/akam/decloud.sh" status
run "$SDC/akam/decloud.sh" off
check "off removes every route it added" test ! -s "$R/tmp/routes"

# keeper: re-adds what yy_cam's Wi-Fi restarts drop; one instance; off stops it
new_root dk
: >"$SDC/akam/decloud"
export AKAM_KEEP_SECS=1
run "$SDC/akam/decloud.sh" on
run "$SDC/akam/decloud.sh" on
check "keeper: one start logged" test "$(grep -c 'keeper started' "$SDC/akam/akam_boot.log")" = 1
check "keeper: pidfile written" wait_for "$R/tmp/akam_decloud_keeper.pid"
KP=$(cat "$R/tmp/akam_decloud_keeper.pid")
check "keeper: running" kill -0 "$KP"
echo "239.255.255.250 dev wlan0" >>"$R/tmp/routes"
grep -v -e '^224.0.0.0/4 ' -e '^blackhole 128' "$R/tmp/routes" >"$R/tmp/r.n" || true
cat "$R/tmp/r.n" >"$R/tmp/routes"
i=0
while [ "$i" -lt 60 ] && ! has "$R/tmp/routes" '^224.0.0.0/4 dev wlan0'; do sleep 0.2; i=$((i + 1)); done
check "keeper: multicast route re-added" has "$R/tmp/routes" '^224.0.0.0/4 dev wlan0'
check "keeper: blackhole re-added" has "$R/tmp/routes" '^blackhole 128.0.0.0/1'
sleep 2.5
check "keeper: the change is logged once" test "$(grep -c 'keeper: routes were missing' "$SDC/akam/akam_boot.log")" = 1
check "keeper: yy_cam host route left alone" test "$(grep -c '^239.255.255.250 dev wlan0' "$R/tmp/routes")" = 1
check "keeper: no duplicate blackhole" test "$(grep -c 'blackhole 128.0.0.0/1' "$R/tmp/routes")" = 1
run "$SDC/akam/decloud.sh" off
sleep 0.3
check "off: keeper stopped" test ! -e "$R/tmp/akam_decloud_keeper.pid"
check "off: keeper process gone" not_alive "$KP"
check "off: only the stock host route remains" test "$(cat "$R/tmp/routes")" = "239.255.255.250 dev wlan0"
unset AKAM_KEEP_SECS

# keeper: two simultaneous starts leave one; a pidfile with a foreign live pid is stale; prefix /10 is not /1
new_root dk2
: >"$SDC/akam/decloud"
export AKAM_KEEP_SECS=1
sleep 60 &
FP=$!
echo "$FP" >"$R/tmp/akam_decloud_keeper.pid"
"$SDC/akam/decloud.sh" keeper </dev/null >/dev/null 2>&1 &
"$SDC/akam/decloud.sh" keeper </dev/null >/dev/null 2>&1 &
sleep 1
check "keeper race: the foreign pid in the pidfile was replaced" test "$(cat "$R/tmp/akam_decloud_keeper.pid")" != "$FP"
check "keeper race: exactly one keeper" test "$(pgrep -f "$SDC/akam/decloud.sh keeper" | wc -l)" = 1
run "$SDC/akam/decloud.sh" off
check "off: the foreign process is not killed" kill -0 "$FP"
kill "$FP" 2>/dev/null || true
new_root dk3
: >"$SDC/akam/decloud"
printf 'blackhole 0.0.0.0/16\nblackhole 128.0.0.0/10\n' >"$R/tmp/routes"
run "$SDC/akam/decloud.sh" on
check "keeper: /16 and /10 do not pass for /1 (both /1 added)" test "$(grep -c '^blackhole [0-9.]*/1$' "$R/tmp/routes")" = 2
run "$SDC/akam/decloud.sh" off

unset AKAM_KEEP_SECS

# ------------------------------------------------------------------------------- enable_local.sh
echo "== enable_local.sh"
PCONF='[system]
name=cam
[rtsp]
rtsp_on_off=0
rtsp_port=8554
[ptz]
ptz_pan_step_home=1275
'
new_root el
printf '%s' "$PCONF" >"$R/config/param.conf"
cp "$R/config/param.conf" "$WORK/param.orig"
run "$SDC/akam/enable_local.sh" apply
check "no flag: param.conf untouched" cmp -s "$R/config/param.conf" "$WORK/param.orig"
check "no flag: no backup written" test ! -e "$SDC/akam/backup"
: >"$SDC/akam/enable_param_conf"
run "$SDC/akam/enable_local.sh" apply
check "flag: [rtsp] rtsp_on_off=1" has "$R/config/param.conf" '^rtsp_on_off=1$'
check "flag: [onvif] section appended with onvif_on_off=1" has "$R/config/param.conf" '^onvif_on_off=1$'
check "flag: other keys kept" has "$R/config/param.conf" '^ptz_pan_step_home=1275$'
check "flag: no stray temp file" test ! -e "$R/config/param.conf.akam.new"
check "flag: original backed up on the card" cmp -s "$SDC/akam/backup/param.conf.orig" "$WORK/param.orig"
cp "$R/config/param.conf" "$WORK/param.after"
run "$SDC/akam/enable_local.sh" apply
check "second apply is idempotent" cmp -s "$R/config/param.conf" "$WORK/param.after"
check "backup still the original" cmp -s "$SDC/akam/backup/param.conf.orig" "$WORK/param.orig"
check "restore_param.sh puts the original back" run "$SDC/akam/restore_param.sh"
check "restored content is the original" cmp -s "$R/config/param.conf" "$WORK/param.orig"

new_root el2
printf '%s' "$PCONF" >"$R/config/param.conf"
: >"$SDC/akam/enable_param_conf"
: >"$SDC/akam/backup" # a FILE where the backup directory must go: the backup fails
run "$SDC/akam/enable_local.sh" apply
check "backup failure: param.conf NOT written" cmp -s "$R/config/param.conf" <(printf '%s' "$PCONF")
check "backup failure: logged" has "$SDC/akam/akam_boot.log" "NOT writing"

new_root el3
: >"$SDC/akam/enable_param_conf"
run "$SDC/akam/enable_local.sh" apply
check "flag but no param.conf: nothing created" test ! -e "$R/config/param.conf"

new_root el4
# 0x216A = 8554 listening (state 0A), 0x1388 = 5000 only connected (01)
cat >"$R/proc/net/tcp" <<'EOF'
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000:216A 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1 1 0
   1: 0100007F:1388 0100007F:9999 01 00000000:00000000 00:00000000 00000000     0        0 2 1 0
EOF
printf '%s' "$PCONF" >"$R/config/param.conf"
run "$SDC/akam/enable_local.sh" verify
check "verify: 8554 open" has "$SDC/akam/akam_boot.log" "port 8554 (rtsp): open"
check "verify: 5000 closed" has "$SDC/akam/akam_boot.log" "port 5000 (onvif): closed"
check "verify: param values logged" has "$SDC/akam/akam_boot.log" "rtsp_on_off='0'"
check "verify changes nothing" cmp -s "$R/config/param.conf" <(printf '%s' "$PCONF")
check "verify: tymotor not held logged" has "$SDC/akam/akam_boot.log" "motor node tymotor open by pid: none"

new_root el5
printf '%s\n' '[ptz]' 'ptz_pan_step_home=1275' 'ptz_tilt_step_home=816' '[rtsp]' 'rtsp_on_off=0' >"$R/config/param.conf"
mkdir -p "$R/proc/321/fd" "$R/proc/654/fd"
printf '%s\n' '250 tymotor' >"$R/proc/devices"
ln -s /dev/tymotor "$R/proc/321/fd/7"
ln -s /dev/null "$R/proc/654/fd/3"
cp "$R/config/param.conf" "$WORK/el5.before"
run "$SDC/akam/enable_local.sh" verify
check "verify: [ptz] values logged" has "$SDC/akam/akam_boot.log" "ptz\]: ptz_pan_step_home=1275 ptz_tilt_step_home=816"
check "verify: tymotor holder logged" has "$SDC/akam/akam_boot.log" "motor node tymotor open by pid: 321"
check "verify: ms41929 not held logged" has "$SDC/akam/akam_boot.log" "motor node ms41929 open by pid: none"
check "verify: proc/devices motor logged" has "$SDC/akam/akam_boot.log" "motor entries: 250 tymotor"
check "verify ptz state changes nothing" cmp -s "$R/config/param.conf" "$WORK/el5.before"

new_root el6
printf '%s\n' '[dsp]' 'ivs_mt_on_off = 0' 'ivs_hd_on_off=1' 'other_key=5' '[alg]' 'ak_av130_svp2_hd_enable=0' 'foo=1' '[rtsp]' 'rtsp_on_off=0' >"$R/config/param.conf"
mkdir -p "$R/proc/321/fd" "$R/proc/321/task/321" "$R/proc/321/task/340" "$R/proc/654/fd"
printf '%s\n' 'ak_npu 123456 1 - Live 0x00000000' 'other 1 0 - Live 0x0' >"$R/proc/modules"
ln -s /dev/npu "$R/proc/321/fd/9"
ln -s /dev/null "$R/proc/654/fd/3"
printf '%s\n' 'yy_cam' >"$R/proc/321/task/321/comm"
printf '%s\n' 'Y_ALG_AV130_HD' >"$R/proc/321/task/340/comm"
printf '%s\n' '340 (Y_ALG_AV130_HD) S 1 1 1 0 -1 4194368 10 0 0 0 1234 567 0 0 20 0 5 0 99 1000 100 4294967295' >"$R/proc/321/task/340/stat"
: >"$SDC/yy_sdcard_log"
cp "$R/config/param.conf" "$WORK/el6.before"
run "$SDC/akam/enable_local.sh" verify
check "verify: AI keys logged (dsp ivs_*, alg ak_av130_*)" has "$SDC/akam/akam_boot.log" "AI keys: dsp.ivs_mt_on_off = 0 dsp.ivs_hd_on_off=1 alg.ak_av130_svp2_hd_enable=0"
check "verify: other keys not logged" not_has "$SDC/akam/akam_boot.log" "other_key"
check "verify: log switches logged" has "$SDC/akam/akam_boot.log" "/mnt/sdcard/yy_sdcard_log:present /mnt/sdcard/yy_log_debug:absent /config/yy_log_debug:absent"
check "verify: npu module logged" has "$SDC/akam/akam_boot.log" "npu module: ak_npu 123456 1"
check "verify: npu node holder logged" has "$SDC/akam/akam_boot.log" "npu node open by pid: 321"
check "verify: detector thread ticks logged" has "$SDC/akam/akam_boot.log" "Y_ALG_AV130_HD(tid 340):utime=1234 stime=567"
check "verify AI state changes nothing" cmp -s "$R/config/param.conf" "$WORK/el6.before"
new_root el7
run "$SDC/akam/enable_local.sh" verify
check "verify: no AI data -> explicit none" has "$SDC/akam/akam_boot.log" "thread not found"
check "verify: no npu holder -> none" has "$SDC/akam/akam_boot.log" "npu node open by pid: none"

# ------------------------------------------------------------------------------- start_ssh.sh
echo "== start_ssh.sh"
new_root ssh1
run "$SDC/akam/start_ssh.sh"
check "dropbear started" test -s "$R/tmp/dropbear.args"
check "port 22" has "$R/tmp/dropbear.args" "-p 22"
check "host key persisted on the card" test -s "$SDC/akam/dropbear_ed25519_host_key"
check "dropbear uses the card host key" has "$R/tmp/dropbear.args" "-r $SDC/akam/dropbear_ed25519_host_key"
check "no password option" hasnt "$R/tmp/dropbear.args" " -s"
check "two keys installed in root's private home" test "$(wc -l <"$R/tmp/akam_home/authorized_keys")" = 2
check "authorized_keys is 0600" test "$(stat -c %a "$R/tmp/akam_home/authorized_keys")" = 600
check "private passwd bind-mounted" has "$R/tmp/mount.log" "--bind $R/tmp/akam_ssh/passwd $R/etc/passwd"
check "private passwd gives root the private HOME" has "$R/tmp/akam_ssh/passwd" "^root:x:0:0:root:$R/tmp/akam_home:"
cp "$SDC/akam/dropbear_ed25519_host_key" "$WORK/hk1"
rm -f "$R/tmp/dropbear.args" "$R/tmp/dropbear.pid"
run "$SDC/akam/start_ssh.sh"
check "host key unchanged on the next boot (stable fingerprint)" cmp -s "$SDC/akam/dropbear_ed25519_host_key" "$WORK/hk1"

new_root ssh2
{
    cat "$WORK/k1.pub"
    cat "$WORK/krsa.pub"
    echo "command=\"/bin/true\" $(cat "$WORK/k2.pub")"
    echo "garbage"
    echo "# comment"
} >"$SDC/akam/authorized_keys"
run "$SDC/akam/start_ssh.sh"
check "only the plain ed25519 key accepted" test "$(wc -l <"$R/tmp/akam_home/authorized_keys")" = 1
check "rsa, command= and junk rejected (logged)" has "$SDC/akam/akam_boot.log" "1 accepted, 3 rejected"

new_root ssh3
printf '%s\n' "garbage" >"$SDC/akam/authorized_keys"
run "$SDC/akam/start_ssh.sh"
check "no valid key: dropbear not started" test ! -e "$R/tmp/dropbear.args"
new_root ssh4
rm -f "$SDC/akam/authorized_keys"
run "$SDC/akam/start_ssh.sh"
check "no authorized_keys: dropbear not started" test ! -e "$R/tmp/dropbear.args"
new_root ssh5
rm -f "$SDC/akam/dropbearmulti"
check "no dropbearmulti: exits 0" run "$SDC/akam/start_ssh.sh"
new_root ssh6
printf '/usr/bin/other\n' >"$R/etc/shells"
run "$SDC/akam/start_ssh.sh"
check "/etc/shells without /bin/sh gets a private list" has "$R/tmp/mount.log" "$R/etc/shells"

# ------------------------------------------------------------------------------- rescue hack.sh
echo "== rescue/hack.sh"
new_root rs
cp "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/rescue/hack.sh" "$SDC/hack.sh"
run "$SDC/hack.sh" >/dev/null 2>&1
wait_for "$R/tmp/start_ran" || true
check "rescue: shadow patched" has "$R/tmp/mount.log" "--bind $R/tmp/etc/shadow $R/etc/shadow"
check "rescue: telnetd started" has "$R/tmp/busybox.log" "telnetd -l /bin/sh -p 23"
check "rescue: start.sh handed off to" test -e "$R/tmp/start_ran"
check "rescue: NO automatic flash dump on the card" test ! -e "$SDC/flash_backup"
check "rescue: no WAN block without the flag" test ! -s "$R/tmp/routes"
: >"$SDC/yy_local_only"
run "$SDC/hack.sh" >/dev/null 2>&1
sleep 0.5
check "rescue: flag yy_local_only blackholes the WAN" has "$R/tmp/routes" "blackhole 128.0.0.0/1"

# ------------------------------------------------------------------------------- own_app (PoC-1), akam_boot.sh side
echo "== own_app: flag, boot counter, bind mount"
new_root oa0
boot
check "no flag: no boot counter" test ! -e "$SDC/akam/own_app_boots"
check "no flag: yy_cam is not bind-mounted" not_has "$R/tmp/mount.log" "yy_cam"
check "no flag: only the shadow bind mount (as before)" test "$(wc -l <"$R/tmp/mount.log")" = 1
check "no flag: no stub copy in /tmp" test ! -e "$R/tmp/own_app_stub.sh"
check "no flag: stock hand-off" test -e "$R/tmp/start_ran"

new_root oa1
: >"$SDC/akam/own_app"
boot
check "flag: counter written (1)" test "$(cat "$SDC/akam/own_app_boots")" = 1
check "flag: stub bind-mounted over yy_cam" has "$R/tmp/mount.log" "--bind $R/tmp/own_app_stub.sh $R/appfs/bin/yy_cam"
check "flag: the stub copy is executable" test -x "$R/tmp/own_app_stub.sh"
check "flag: the stub copy equals the card's" cmp -s "$R/tmp/own_app_stub.sh" "$SDC/akam/own_app_stub.sh"
check "flag: still handed off to start.sh" test -e "$R/tmp/start_ran"
check "flag: bind mount logged before the hand-off" test "$(grep -n 'stub bind-mounted' "$SDC/akam/akam_boot.log" | cut -d: -f1)" -lt "$(grep -n 'handed off' "$SDC/akam/akam_boot.log" | cut -d: -f1)"
check "flag: the real yy_cam path is never written by the card scripts" test ! -e "$R/appfs/bin/yy_cam"
rm -f "$R/tmp/start_ran" "$R/tmp/mount.log"
boot
check "second unfinished boot: counter 2, still bind-mounted" test "$(cat "$SDC/akam/own_app_boots")" = 2 -a "$(grep -c yy_cam "$R/tmp/mount.log")" = 1
rm -f "$R/tmp/start_ran" "$R/tmp/mount.log"
boot
check "third unfinished boot: counter 3, still bind-mounted" test "$(cat "$SDC/akam/own_app_boots")" = 3 -a "$(grep -c yy_cam "$R/tmp/mount.log")" = 1
rm -f "$R/tmp/start_ran" "$R/tmp/mount.log"
boot
check "fourth boot: no bind mount, stock yy_cam boots" test "$(grep -c yy_cam "$R/tmp/mount.log")" = 0 -a -e "$R/tmp/start_ran"
check "fourth boot: counter untouched" test "$(cat "$SDC/akam/own_app_boots")" = 3
check "fourth boot: logged" has "$SDC/akam/akam_boot.log" "own_app disabled after 3 unfinished boots"
rm -f "$SDC/akam/own_app_boots" "$R/tmp/start_ran" "$R/tmp/mount.log"
boot
check "deleting the counter re-arms the app" test "$(cat "$SDC/akam/own_app_boots")" = 1 -a "$(grep -c yy_cam "$R/tmp/mount.log")" = 1

new_root oa2
: >"$SDC/akam/own_app"
echo "garbage" >"$SDC/akam/own_app_boots"
boot
check "a corrupt counter disables the app (fail safe)" test "$(grep -c yy_cam "$R/tmp/mount.log")" = 0 -a -e "$R/tmp/start_ran"

new_root oa2b
: >"$SDC/akam/own_app"
echo "99999999999999999999" >"$SDC/akam/own_app_boots"
boot
check "an overlong numeric counter disables the app (fail safe)" test "$(grep -c yy_cam "$R/tmp/mount.log")" = 0 -a -e "$R/tmp/start_ran"

new_root oa2c
: >"$SDC/akam/own_app"
echo 0 >"$SDC/akam/own_app_boots"
chmod 444 "$SDC/akam/own_app_boots"
if [ "$(id -u)" != 0 ] && ! { : >>"$SDC/akam/own_app_boots"; } 2>/dev/null; then
    boot
    check "unwritable counter: stock boots, no bind mount" test "$(grep -c yy_cam "$R/tmp/mount.log")" = 0 -a -e "$R/tmp/start_ran"
    check "unwritable counter: logged" has "$SDC/akam/akam_boot.log" "cannot write the boot counter"
fi

new_root oa3
: >"$SDC/akam/own_app"
export STUB_MOUNT_FAIL=1
boot
check "bind mount failure: counter restored, stock boots" test "$(cat "$SDC/akam/own_app_boots")" = 0 -a -e "$R/tmp/start_ran"
check "bind mount failure: logged" has "$SDC/akam/akam_boot.log" "own_app stub could not be installed"
unset STUB_MOUNT_FAIL

new_root oa4
: >"$SDC/akam/own_app"
rm -f "$SDC/akam/own_app_stub.sh"
boot
check "missing stub on the card: stock boots, no yy_cam bind mount" test "$(grep -c yy_cam "$R/tmp/mount.log")" = 0 -a -e "$R/tmp/start_ran"

echo "== own_app: the stub"
# Stubs: a busybox whose watchdog applet stays alive (and records SIGTERM), insmod, wpa_supplicant, udhcpc, ping.
SECRET="S3CR3T-PSK-$$"
new_stub_root() { # new_stub_root <name>: a root ready to run the stub directly
    new_root "$1"
    : >"$SDC/akam/own_app"
    printf 'ctrl_interface=/tmp/wpa_supplicant\nnetwork={\n ssid="x"\n psk="%s"\n}\n' "$SECRET" >"$SDC/akam/wpa.conf"
    mkdir -p "$R/appfs/bin" "$R/sys/class/net/wlan0"
    cat >"$SDC/busybox" <<'EOS'
#!/bin/sh
case $1 in
    watchdog)
        echo "watchdog $*" >>"$AKAM_ROOT/tmp/order.log"
        echo $$ >>"$AKAM_ROOT/tmp/feeder.pids"
        trap 'echo TERM >"$AKAM_ROOT/tmp/feeder.term"; exit 0' TERM
        sleep 30 &
        wait $!
        ;;
    *) echo "$*" >>"$AKAM_ROOT/tmp/busybox.log" ;;
esac
EOS
    for c in insmod udhcpc ping; do
        cat >"$STUBS/$c" <<EOS
#!/bin/sh
echo "$c \$*" >>"\$AKAM_ROOT/tmp/order.log"
exit 0
EOS
        chmod 755 "$STUBS/$c"
    done
    cat >"$R/appfs/bin/wpa_supplicant" <<'EOS'
#!/bin/sh
echo "wpa_supplicant $*" >>"$AKAM_ROOT/tmp/order.log"
exit 0
EOS
    chmod 755 "$SDC/busybox" "$R/appfs/bin/wpa_supplicant"
    export AKAM_HEALTH_TRIES=3 AKAM_HEALTH_SLEEP=0 AKAM_IDLE_SLEEP=5 AKAM_IFACE_TRIES=3 AKAM_IFACE_SLEEP=0 AKAM_WPA_TRIES=3 AKAM_WPA_SLEEP=0
}
run_stub() { # run_stub: the stub in the background, output captured; sets SPID
    run "$SDC/akam/own_app_stub.sh" >"$R/tmp/stub.out" 2>&1 </dev/null &
    SPID=$!
}
end_stub() { kill -TERM "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; }
rmstubs() { rm -f "$STUBS/insmod" "$STUBS/udhcpc" "$STUBS/ping"; }

new_stub_root st1
echo 2 >"$SDC/akam/own_app_boots"
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
run_stub
wait_for "$R/tmp/feeder.pids" || true
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "stub: healthy logged" has "$SDC/akam/akam_boot.log" "own_app PoC-1 healthy"
check "stub: counter reset to 0" test "$(cat "$SDC/akam/own_app_boots")" = 0
check "stub: still alive after success (idle loop)" kill -0 "$SPID"
check "stub: exactly one feeder" test "$(wc -l <"$R/tmp/feeder.pids")" = 1
check "stub: feeder command line is watchdog -F -t 5 -T 20" has "$R/tmp/order.log" "watchdog watchdog -F -t 5 -T 20 /dev/watchdog"
check "stub: order is feeder, insmod, wpa_supplicant, udhcpc" test "$(cut -d' ' -f1 "$R/tmp/order.log" | tr '\n' ' ')" = "watchdog insmod wpa_supplicant udhcpc ping "
check "stub: insmod with the stock parameter" has "$R/tmp/order.log" "wifi_bt_comb=1"
check "stub: wpa_supplicant as stock" has "$R/tmp/order.log" "wpa_supplicant -Dnl80211 -i wlan0 -c $R/tmp/wpa.conf -B -P $R/tmp/wpa.pid"
check "stub: udhcpc with the Akam hostname" has "$R/tmp/order.log" "udhcpc -b -p .*udhcpc.pid -i wlan0 -x hostname:akam-rot2k"
check "stub: gateway ping" has "$R/tmp/order.log" "ping -c 1 -W 2 192.0.2.1"
check "stub: /tmp/wpa.conf is a 0600 copy" test "$(stat -c %a "$R/tmp/wpa.conf")" = 600 && cmp -s "$R/tmp/wpa.conf" "$SDC/akam/wpa.conf"
FP1=$(head -n1 "$R/tmp/feeder.pids")
end_stub
check "stub: SIGTERM to the stub stops the feeder with SIGTERM (never a kill)" wait_for "$R/tmp/feeder.term"
sleep 0.5
check "stub: and the feeder is gone" not_alive "$FP1"
check "stub: wpa.conf content never printed or logged" test -z "$(grep -rl -- "$SECRET" "$SDC/akam/akam_boot.log" "$R/tmp/stub.out" "$R/tmp/order.log" "$R/tmp/busybox.log" 2>/dev/null || true)"

echo "== own_app: opt-in time sync (akam/ntp, akam_ntp.sh, akam-sntp)"
mk_fake_sntp() { # mk_fake_sntp: a fake akam-sntp (a script) on the card with a matching BUILDINFO
    mkdir -p "$SDC/akam/bin"
    cat >"$SDC/akam/bin/akam-sntp" <<'EOS'
#!/bin/sh
echo "$*" >>"$AKAM_ROOT/tmp/sntp.args"
exit 0
EOS
    printf 'tool=akam-sntp\nsha256=%s\n' "$(sha256sum "$SDC/akam/bin/akam-sntp" | cut -d' ' -f1)" >"$SDC/akam/bin/akam-sntp.BUILDINFO"
}
wait_healthy() {
    local i=0
    while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
}
new_stub_root nt1
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
cp "$REPO/src/scripts/akam_ntp.sh" "$SDC/akam/akam_ntp.sh"
mk_fake_sntp
printf 'server=192.0.2.123\nserver=time.example.org\n' >"$SDC/akam/ntp"
run_stub
wait_healthy
wait_for "$R/tmp/sntp.args" 50 || true
check "ntp: the helper ran the card sntp once per pass with the 1080p command line (-n -q -p S -p S)" test "$(cat "$R/tmp/sntp.args" 2>/dev/null)" = "-n -q -p 192.0.2.123 -p time.example.org"
check "ntp: the helper logs to /tmp/akam_ntp.log (RAM), not the card" test -f "$R/tmp/akam_ntp.log" -a ! -e "$SDC/akam/akam_ntp.log"
check "ntp: started after the network was healthy, logged" has "$SDC/akam/akam_boot.log" "time sync helper started"
check "ntp: the binary runs from a verified copy in /tmp" test -x "$R/tmp/akam-sntp"
check "ntp: a resident helper holds the lock while waiting for the next pass" test -d "$R/tmp/akam_ntp.lock"
HP=$(cat "$R/tmp/akam_ntp.lock/pid" 2>/dev/null || echo 0)
end_stub
sleep 0.5
check "ntp: stopping the stub stops the helper" not_alive "$HP"

new_stub_root nt2
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
cp "$REPO/src/scripts/akam_ntp.sh" "$SDC/akam/akam_ntp.sh"
mk_fake_sntp
run_stub
wait_healthy
sleep 0.5
check "ntp: no akam/ntp file = no helper, no sntp copy, no log, no lock" test ! -e "$R/tmp/sntp.args" -a ! -e "$R/tmp/akam-sntp" -a ! -e "$R/tmp/akam_ntp.lock" -a ! -e "$R/tmp/akam_ntp.log"
check "ntp: nothing about time sync in the card log without akam/ntp" not_has "$SDC/akam/akam_boot.log" "time sync"
end_stub

new_stub_root nt3
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
cp "$REPO/src/scripts/akam_ntp.sh" "$SDC/akam/akam_ntp.sh"
mk_fake_sntp
printf 'server=192.0.2.123\n' >"$SDC/akam/ntp"
printf '# tampered\n' >>"$SDC/akam/bin/akam-sntp"
run_stub
wait_healthy
sleep 0.5
check "ntp: a binary that does not match its BUILDINFO is refused, the stub stays healthy" has "$SDC/akam/akam_boot.log" "akam-sntp refused" 
check "ntp: refused binary is never copied or run" test ! -e "$R/tmp/akam-sntp" -a ! -e "$R/tmp/sntp.args"
end_stub

new_stub_root nt4
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
printf 'server=192.0.2.123\n' >"$SDC/akam/ntp"
run_stub
wait_healthy
sleep 0.3
check "ntp: akam/ntp without akam_ntp.sh: logged, stub healthy" has "$SDC/akam/akam_boot.log" "akam_ntp.sh missing"
end_stub

new_stub_root nt5
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
cp "$REPO/src/scripts/akam_ntp.sh" "$SDC/akam/akam_ntp.sh"
printf 'server=192.0.2.123\n' >"$SDC/akam/ntp"
run_stub
wait_healthy
sleep 0.3
check "ntp: akam/ntp without the sntp binary: logged, no helper" has "$SDC/akam/akam_boot.log" "no akam/bin/akam-sntp" 
check "ntp: no helper lock without a binary" test ! -e "$R/tmp/akam_ntp.lock"
end_stub

check "decloud: blackholes the Internet only, no server of any kind is named in decloud.sh (a LAN NTP server stays reachable)" test -z "$(grep -v '^[[:space:]]*#' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/decloud.sh" | grep -i -E 'ntp|pool\.' || true)"

new_stub_root st2
echo 1 >"$SDC/akam/own_app_boots"
: >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'FAILED' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "failure: logged" has "$SDC/akam/akam_boot.log" "own_app PoC-1 FAILED: no IPv4 address and default route"
FP2=$(head -n1 "$R/tmp/feeder.pids")
sleep 0.3
check "failure: feeder SIGKILLed (dead, and its SIGTERM handler never ran)" not_alive "$FP2"
check "failure: no SIGTERM path used" test ! -e "$R/tmp/feeder.term"
check "failure: counter not reset (the boot stays counted)" test "$(cat "$SDC/akam/own_app_boots")" = 1
check "failure: no 'healthy' line" not_has "$SDC/akam/akam_boot.log" "PoC-1 healthy"
check "failure: the stub does not exit on its own" kill -0 "$SPID"
end_stub

new_stub_root st3
rm -f "$SDC/akam/wpa.conf"
run_stub
i=0
while ! grep -q 'FAILED' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "no wpa.conf on the card: failure, feeder killed, Wi-Fi never started" has "$SDC/akam/akam_boot.log" "akam/wpa.conf missing"
sleep 0.3
check "no wpa.conf: feeder dead" not_alive "$(head -n1 "$R/tmp/feeder.pids")"
check "no wpa.conf: no supplicant, no DHCP" not_has "$R/tmp/order.log" "wpa_supplicant\|udhcpc"
end_stub
rmstubs
# wlan0 wait loop: appears late, never appears
new_stub_root st4
rm -rf "$R/sys/class/net/wlan0"
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
AKAM_IFACE_TRIES=30 AKAM_IFACE_SLEEP=0.1
export AKAM_IFACE_TRIES AKAM_IFACE_SLEEP
( sleep 0.6; mkdir -p "$R/sys/class/net/wlan0" ) &
run_stub
i=0
while ! grep -q 'PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "wlan0 wait: appears late, healthy" has "$SDC/akam/akam_boot.log" "wlan0 present after"
check "wlan0 wait: ip link set up logged" has "$SDC/akam/akam_boot.log" "ip link set wlan0 up ok"
check "wlan0 wait: wpa_supplicant started only after wlan0 existed" has "$SDC/akam/akam_boot.log" "wpa_supplicant started (attempt 1)"
end_stub
new_stub_root st5
rm -rf "$R/sys/class/net/wlan0"
export AKAM_IFACE_TRIES=2 AKAM_IFACE_SLEEP=0
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "wlan0 wait: bounded, logged, the stub carries on" has "$SDC/akam/akam_boot.log" "wlan0 did not appear within"
end_stub

# Wi-Fi enable GPIO, as stock yy_cam: gpio10, polarity 0 (active low): disabled level 1, enabled level 0, then insmod.
# The fake sysfs gpio tree is plain files, so the order is proven by what the insmod stub sees in the value file.
mk_gpio_root() { # mk_gpio_root <name> [no-gpio10-dir]
    new_stub_root "$1"
    export AKAM_WIFI_PRE_SLEEP=0 AKAM_WIFI_POST_SLEEP=0
    mkdir -p "$R/sys/class/gpio/gpio55"
    : >"$R/sys/class/gpio/export"
    echo in >"$R/sys/class/gpio/gpio55/direction"
    echo 1 >"$R/sys/class/gpio/gpio55/value"
    [ -n "${2:-}" ] || mkdir -p "$R/sys/class/gpio/gpio10"
    cat >"$STUBS/insmod" <<'EOS'
#!/bin/sh
echo "insmod $* gpio10=$(cat "$AKAM_ROOT/sys/class/gpio/gpio10/value" 2>/dev/null)" >>"$AKAM_ROOT/tmp/order.log"
exit 0
EOS
    chmod 755 "$STUBS/insmod"
    echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
}
wait_healthy() {
    i=0
    while ! grep -q -E 'PoC-1 healthy|FAILED' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
}
mk_gpio_root st8
run_stub
wait_healthy
LOGF=$SDC/akam/akam_boot.log
check "wifi gpio: gpio10 driven out" test "$(cat "$R/sys/class/gpio/gpio10/direction")" = out
check "wifi gpio: gpio10 ends at the enabled level 0 (polarity 0 = active low)" test "$(cat "$R/sys/class/gpio/gpio10/value")" = 0
check "wifi gpio: disabled level 1 logged, then enabled level 0" test "$(grep -n -E 'gpio10: out, disabled level 1' "$LOGF" | cut -d: -f1)" -lt "$(grep -n -E 'gpio10: enabled level 0' "$LOGF" | cut -d: -f1)"
check "wifi gpio: enabled before insmod" test "$(grep -n 'gpio10: enabled level 0' "$LOGF" | cut -d: -f1)" -lt "$(grep -n 'insmod atbm6132c' "$LOGF" | cut -d: -f1)"
check "wifi gpio: the insmod stub saw gpio10 = 0 and the stock parameter" has "$R/tmp/order.log" "wifi_bt_comb=1 gpio10=0"
check "wifi gpio: gpio55 (the red LED) is not touched" test "$(cat "$R/sys/class/gpio/gpio55/direction")/$(cat "$R/sys/class/gpio/gpio55/value")" = in/1
check "wifi gpio: healthy case writes no diagnostics" test "$(grep -c 'diag:' "$LOGF")" = 0
end_stub
rmstubs
mk_gpio_root st8b nodir
run_stub
wait_healthy
check "wifi gpio: exports gpio10 when it is not exported yet" test "$(cat "$R/sys/class/gpio/export")" = 10
check "wifi gpio: a pin that cannot be set is logged and the boot carries on" has "$SDC/akam/akam_boot.log" "gpio10 could not be set (continuing)"
check "wifi gpio: insmod still runs" has "$R/tmp/order.log" "wifi_bt_comb=1"
end_stub
rmstubs
mk_gpio_root st8c
AKAM_WIFI_EN_POL=1
export AKAM_WIFI_EN_POL
run_stub
wait_healthy
check "wifi gpio: polarity 1 (active high) ends at 1" test "$(cat "$R/sys/class/gpio/gpio10/value")" = 1
check "wifi gpio: polarity 1 logs disabled 0 then enabled 1" has "$SDC/akam/akam_boot.log" "gpio10: enabled level 1"
end_stub
unset AKAM_WIFI_EN_POL
rmstubs
check "wifi gpio: no /dev/mem or devmem in the stub (sysfs only)" test -z "$(grep -E '/dev/mem|devmem|mmap' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_stub.sh" | grep -v -E '^[[:space:]]*#' || true)"

# Failure diagnostics: wlan0 never appears -> gpio state, USB ids, filtered dmesg tail in the card log, bounded.
mk_gpio_root st9
LOGF=$SDC/akam/akam_boot.log
rm -rf "$R/sys/class/net/wlan0"
export AKAM_IFACE_TRIES=1 AKAM_IFACE_SLEEP=0
mkdir -p "$R/sys/bus/usb/devices/usb1" "$R/sys/bus/usb/devices/1-1"
echo 1d6b >"$R/sys/bus/usb/devices/usb1/idVendor"
echo 0002 >"$R/sys/bus/usb/devices/usb1/idProduct"
echo 007a >"$R/sys/bus/usb/devices/1-1/idVendor"
echo 6012 >"$R/sys/bus/usb/devices/1-1/idProduct"
{
    for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18; do echo "[ $n.0] usb 1-1: event $n"; done
    echo "[ 19.0] atbm: ssid=MyHomeNet psk=$SECRET"
    echo "[ 20.0] unrelated line about the sensor"
    echo "[ 21.0] wlan0: passphrase=$SECRET"
    echo "[ 22.0] ak_hcd: port reset"
} >"$R/tmp/dmesg.raw"
# shellcheck disable=SC2016  # $AKAM_ROOT is expanded by the stub, not here
printf '#!/bin/sh\ncat "$AKAM_ROOT/tmp/dmesg.raw"\n' >"$STUBS/dmesg"
chmod 755 "$STUBS/dmesg"
run_stub
wait_healthy
end_stub
check "diag: the missing wlan0 is logged" has "$LOGF" "wlan0 did not appear within"
check "diag: gpio state logged" has "$LOGF" "diag: gpio10 direction=out value=0 (set: disabled 1, enabled 0)"
check "diag: usb device list logged" has "$LOGF" "diag: /sys/bus/usb/devices: 1-1 usb1"
check "diag: usb ids logged (device)" has "$LOGF" "diag: usb 1-1 idVendor=007a idProduct=6012"
check "diag: usb ids logged (root hub)" has "$LOGF" "diag: usb usb1 idVendor=1d6b idProduct=0002"
check "diag: dmesg tail is at most 15 lines" test "$(grep -c 'diag: dmesg: ' "$LOGF")" = 15
check "diag: dmesg tail keeps the newest line" has "$LOGF" "ak_hcd: port reset"
check "diag: dmesg filter drops lines that match no keyword" test "$(grep -c 'unrelated line' "$LOGF")" = 0
check "diag: no secret, psk, ssid or passphrase line in the card log" test -z "$(grep -i -E "$SECRET|psk|passphrase|MyHomeNet|ssid" "$LOGF" || true)"
check "diag positive control: the raw dmesg held the secret" has "$R/tmp/dmesg.raw" "$SECRET"
unset AKAM_IFACE_TRIES AKAM_IFACE_SLEEP AKAM_WIFI_PRE_SLEEP AKAM_WIFI_POST_SLEEP
rm -f "$STUBS/dmesg"
rmstubs

# wpa_supplicant retry and the output filter (positive control: a fake secret line must never reach the card log)
mk_flaky_wpa() { # mk_flaky_wpa <failures before success>
    cat >"$R/appfs/bin/wpa_supplicant" <<EOS
#!/bin/sh
n=\$(cat "\$AKAM_ROOT/tmp/wpa.count" 2>/dev/null || echo 0)
n=\$((n + 1))
echo \$n >"\$AKAM_ROOT/tmp/wpa.count"
echo "wpa_supplicant \$*" >>"\$AKAM_ROOT/tmp/order.log"
if [ \$n -le $1 ]; then
    echo 'Successfully initialized wpa_supplicant'
    echo 'network: psk="$SECRET"' >&2
    echo 'Config: ssid=MyHomeNet' >&2
    echo 'passphrase=$SECRET' >&2
    echo 'nl80211: Could not configure driver mode' >&2
    exit 255
fi
exit 0
EOS
    chmod 755 "$R/appfs/bin/wpa_supplicant"
}
new_stub_root st6
mk_flaky_wpa 2
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "wpa retry: attempts 1 and 2 logged as failed" test "$(grep -c 'wpa_supplicant attempt [12] of 3 failed' "$SDC/akam/akam_boot.log")" = 2
check "wpa retry: the third attempt succeeds, health reached" has "$SDC/akam/akam_boot.log" "wpa_supplicant started (attempt 3)"
check "wpa retry: a harmless output line reaches the card log" has "$SDC/akam/akam_boot.log" "wpa_supplicant: nl80211: Could not configure driver mode"
check "wpa filter: no secret, psk, passphrase or ssid line in the card log" test -z "$(grep -i -E "$SECRET|psk|passphrase|MyHomeNet|ssid=" "$SDC/akam/akam_boot.log" || true)"
end_stub
new_stub_root st7
mk_flaky_wpa 9
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'FAILED' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
sleep 0.3
check "wpa retry: three attempts then failure" test "$(cat "$R/tmp/wpa.count")" = 3
check "wpa retry: failure kills the feeder (SIGKILL path unchanged)" not_alive "$(head -n1 "$R/tmp/feeder.pids")"
check "wpa filter positive control: the raw output held the secret (it stays in /tmp)" has "$R/tmp/wpa.out" "$SECRET"
check "wpa retry: no secret on the card in the failure case" test -z "$(grep -i -E "$SECRET|psk=|passphrase|ssid=" "$SDC/akam/akam_boot.log" || true)"
end_stub
unset AKAM_IFACE_TRIES AKAM_IFACE_SLEEP AKAM_WPA_TRIES AKAM_WPA_SLEEP
check "the stub never reads the credentials (no cat/grep/sed/read/source of wpa.conf)" test -z "$(sed -e 's/^[[:space:]]*#.*$//' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_stub.sh" | grep -E '(cat|grep|sed|awk|head|tail|read|source|\.)[[:space:]].*wpa\.conf' || true)"
unset AKAM_HEALTH_TRIES AKAM_HEALTH_SLEEP AKAM_IDLE_SLEEP

# ------------------------------------------------------------------------------- own app: supervisor and stub auto-start
echo "== own app: supervisor (own_app_run.sh)"
# A fake akamd: a shell script whose behaviour is chosen by the file tmp/app.mode. It records its environment
# and arguments (app.runs) and its pid (app.pids).
mk_app() { # mk_app <mode>: fake app + a BUILDINFO with its real sha256 on the card, in the current $R
    mkdir -p "$SDC/akam/bin"
    cat >"$SDC/akam/bin/akamd" <<'EOS'
#!/bin/sh
if [ "$1" = "--ptz-stop" ]; then echo stop >>"$AKAM_ROOT/tmp/app.stops"; exit 0; fi
echo "$LD_LIBRARY_PATH|$*" >>"$AKAM_ROOT/tmp/app.runs"
echo $$ >>"$AKAM_ROOT/tmp/app.pids"
echo "akamd says hello"
case $(cat "$AKAM_ROOT/tmp/app.mode") in
    crash) exit 3 ;;
    spam)
        trap 'exit 0' TERM
        while :; do printf '%1000s\n' x; sleep 0.05; done
        ;;
    ignore)
        trap '' TERM
        while :; do sleep 0.1; done
        ;;
    *)
        trap 'echo term >"$AKAM_ROOT/tmp/app.term"; sleep 1; echo gone >"$AKAM_ROOT/tmp/app.exited"; exit 0' TERM
        while :; do sleep 0.1; done
        ;;
esac
EOS
    chmod 755 "$SDC/akam/bin/akamd"
    printf '%s\n' "$1" >"$R/tmp/app.mode"
    printf 'tool=akamd\nsha256=%s\nsymcheck=passed\n' "$(sha256sum "$SDC/akam/bin/akamd" | cut -d' ' -f1)" >"$SDC/akam/bin/akamd.BUILDINFO"
}
wait_grep() { # wait_grep <file> <pattern> [tenths of a second]
    local i=0
    while ! grep -q -- "$2" "$1" 2>/dev/null && [ "$i" -lt "${3:-100}" ]; do sleep 0.1; i=$((i + 1)); done
    grep -q -- "$2" "$1" 2>/dev/null
}
runs() { { wc -l <"$R/tmp/app.runs"; } 2>/dev/null || echo 0; }
count_grep() { local n; n=$(grep -c -- "$2" "$1" 2>/dev/null) || true; echo "${n:-0}"; } # count_grep <file> <pattern>: 0 when the file is missing
run_sup() { # run_sup: the supervisor in the background; sets SUPPID
    run "$SDC/akam/own_app_run.sh" >"$R/tmp/sup.out" 2>&1 </dev/null &
    SUPPID=$!
}
wait_sup() { # wait_sup [tenths]: the supervisor exited on its own
    local i=0
    while kill -0 "$SUPPID" 2>/dev/null && [ "$i" -lt "${1:-100}" ]; do sleep 0.1; i=$((i + 1)); done
    not_alive "$SUPPID"
}
end_sup() { # SIGTERM, bounded wait
    kill -TERM "$SUPPID" 2>/dev/null || true
    wait_sup 100 || kill -KILL "$SUPPID" 2>/dev/null || true
    wait "$SUPPID" 2>/dev/null || true
}
export AKAM_APP_POLL=0.1 AKAM_APP_STOP_WAIT=5

# --- start, options, restart after kill -9, stop
new_root sup1
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
# comment
main_kbps = 3000   # trailing comment
sub_kbps=10
gop=abc
bogus=1
port=0
main_kbps=3000; touch /tmp/pwned
evil=$(touch EVIL)
sub_kbps=0256

garbage line
EOS
AKAM_APP_BACKOFF_MIN=0 AKAM_APP_BACKOFF_MAX=0 run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: the app runs" test "$(runs)" = 1
check "supervisor: LD_LIBRARY_PATH is the camera's /appfs/lib" test "$(cut -d'|' -f1 "$R/tmp/app.runs")" = "$R/appfs/lib"
check "supervisor: only the valid conf lines became options (-m 3000 -s 256)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "-m 3000 -s 256"
check "supervisor: bad lines are logged and ignored" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 6
check "supervisor: a value is never evaluated (no file created)" test ! -e "$R/tmp/pwned" -a ! -e "$SDC/akam/EVIL" -a ! -e EVIL
check "supervisor: sha256 verified, copy in /tmp" has "$SDC/akam/akam_boot.log" "akamd verified"
check "supervisor: the /tmp copy is executable and equal to the card file" test -x "$R/tmp/akamd" -a "$(cat "$R/tmp/akamd")" = "$(cat "$SDC/akam/bin/akamd")"
check "supervisor: app output goes to akam/akam.log" has "$SDC/akam/akam.log" "akamd says hello"
P1=$(head -n1 "$R/tmp/app.pids")
kill -KILL "$P1"
i=0
while [ "$(runs)" -lt 2 ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "supervisor: kill -9 of the app: restarted" test "$(runs)" = 2
check "supervisor: the exit status is logged (137)" has "$SDC/akam/akam_boot.log" "exited with status 137"
sleep 0.3
P2=$(tail -n1 "$R/tmp/app.pids")
check "supervisor: the new pid differs" test "$P1" != "$P2"
end_sup
check "supervisor: SIGTERM reaches the app (it ran its TERM handler)" test -e "$R/tmp/app.term"
check "supervisor: it waited for the app to finish (handler took 1 s)" test -e "$R/tmp/app.exited"
check "supervisor: app and supervisor are gone" not_alive "$P2"
check "supervisor: stop logged" has "$SDC/akam/akam_boot.log" "supervisor stopped"

# --- defaults without a conf
new_root sup2
mk_app run
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: no conf: no option (the app's defaults: 2000/800 kbit/s)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = ""
check "supervisor: no conf: logged as defaults" has "$SDC/akam/akam_boot.log" "options: (defaults)"
end_sup

# --- detection keys: valid values become -D/-H/-T, out-of-range or non-numeric ones are ignored
new_root supdet
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
detect = 1
detect_hz=10
detect_threshold=950
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: detect keys in range become -D 1 -H 10 -T 950" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "-D 1 -H 10 -T 950"
end_sup
new_root supdet2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
detect=0
detect_hz=11
detect_threshold=499
detect_threshold=951
detect_hz=0
detect=2
detect_hz=fast
detect_threshold=500
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: detect=0 and 500 pass; 11, 0, 499, 951, 2 and non-numbers are ignored (-D 0 -T 500)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "-D 0 -T 500"
check "supervisor: the rejected detect values are logged" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 5
end_sup

# --- snapshot keys: snapshot 0..1 and snapshot_interval 5..300 become --snapshot / --snapshot-interval; the zones are not a key
new_root supsnap
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
snapshot = 0
snapshot_interval=300
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: snapshot keys in range become --snapshot 0 --snapshot-interval 300" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--snapshot 0 --snapshot-interval 300"
end_sup
new_root supsnap2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
snapshot=2
snapshot_interval=4
snapshot_interval=301
snapshot_interval=fast
snapshot=1
snapshot_interval=5
zones=porch
zone_mode=all
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: snapshot 2, interval 4 / 301 / non-number are ignored; 1 and 5 pass; zones are not a supervisor key" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--snapshot 1 --snapshot-interval 5"
check "supervisor: the rejected snapshot values and the zone lines are logged" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 6
end_sup

# --- sub_res: 640x360 is the only sub size (accepted, nothing passed to the app); every other value is dropped and logged
new_root supres
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
sub_res=1280x720
sub_res=960x540
sub_res=640x360
sub_res=640x360;ls
sub_kbps=900
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: sub_res 640x360 is accepted without an option, bigger sizes are ignored; sub_kbps passes" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "-s 900"
check "supervisor: the unsupported sub_res lines are logged with the SoC limit" test "$(grep -c 'unsupported on this SoC (sub max 640x480: 3DNR buffer / venc DMA pool)' "$SDC/akam/akam_boot.log")" -ge 3
end_sup

# --- video keys: video0_* / video1_* (kbps, max_kbps, fps, min_qp, max_qp, gop, rc), strict per-stream ranges, rc an exact word
new_root supvid
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
video0_kbps = 3000
video0_max_kbps=4000
video0_fps=10
video0_min_qp=28
video0_max_qp=45
video0_gop=60
video0_rc=cbr
video1_kbps=300
video1_max_kbps=0
video1_fps=15
video1_rc=avbr
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: video keys in range become --video0-kbps ... --video1-rc avbr" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--video0-kbps 3000 --video0-max-kbps 4000 --video0-fps 10 --video0-min-qp 28 --video0-max-qp 45 --video0-gop 60 --video0-rc cbr --video1-kbps 300 --video1-max-kbps 0 --video1-fps 15 --video1-rc avbr"
end_sup
new_root supvid2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
video0_kbps=63
video0_kbps=8001
video1_kbps=31
video1_kbps=1001
video0_max_kbps=63
video1_max_kbps=31
video1_max_kbps=1501
video0_fps=0
video0_fps=16
video0_min_qp=9
video0_max_qp=52
video1_gop=0
video1_gop=301
video0_rc=VBR
video0_rc=2
video1_rc=vbr;ls
video2_kbps=1000
video0_kbps=abc
video1_kbps=512
video1_rc=vbr
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: out-of-range video values, other rc words, a third stream and non-numbers are ignored; the valid ones pass" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--video1-kbps 512 --video1-rc vbr"
check "supervisor: the rejected video lines are logged" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 17
end_sup

# --- day/night and image keys: the only words are night auto|day|night and ir_led auto|off (exact), the image numbers 0..100,
# mains_hz 50 or 60
new_root supdn
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
night = day
ir_led=off
image_brightness=0
image_contrast = 100
image_sharpness=50
mains_hz=60
wdr=0
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: day/night and image keys become --night day --ir-led off --image-brightness 0 --image-contrast 100 --image-sharpness 50 --mains-hz 60 --wdr 0" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--night day --ir-led off --image-brightness 0 --image-contrast 100 --image-sharpness 50 --mains-hz 60 --wdr 0"
end_sup
new_root supdn2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
night=Night
night=nigh
night=1
night=night;touch EVIL
night=
ir_led=on
ir_led=1
ir_led=
ir_led=auto;touch EVIL
image_brightness=101
image_contrast=-1
image_sharpness=abc
mains_hz=55
mains_hz=49
mains_hz=61
night=night
mains_hz=50
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: only the exact words pass (Night, nigh, 1, injection, empty, on, 1, auto;... are ignored); out-of-range image values and mains 55/49/61 are ignored (--night night --mains-hz 50)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--night night --mains-hz 50"
check "supervisor: the rejected day/night and image values are logged" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 13
check "supervisor: a word value is never evaluated (no file created)" test ! -e "$SDC/akam/EVIL" -a ! -e EVIL
end_sup

# --- status LED keys: led auto|on|off and led_<rule> patterns (digits and colons only, never evaluated)
new_root supled
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
led = auto
led_boot=2:100:100:1000
led_person = 3:250:250:5000
led_idle=off
led_stream=on
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: led and led_<rule> patterns become --led auto --led-boot ... --led-person ... --led-idle off --led-stream on" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--led auto --led-boot 2:100:100:1000 --led-person 3:250:250:5000 --led-idle off --led-stream on"
end_sup
new_root supled2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
led=blink
led=1
led=
led_person=3:250:250
led_person=3:250:250:5000:1
led_person=1:2.5:250:0
led_person=-1:250:250:0
led_person=1:250:250:0;touch EVIL
led_night=ON
led_night=
led_nowifi=1:250:250:0
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: bad led values are ignored (only the valid led_nowifi pattern passes)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--led-nowifi 1:250:250:0"
check "supervisor: the rejected led values are logged and never evaluated" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 9 -a ! -e "$SDC/akam/EVIL" -a ! -e EVIL
end_sup

# --- audio key (opt-in microphone): audio=0|1 only, default 0 means no --audio at all (the app's own default, off)
new_root supau
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
audio = 1
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: audio=1 becomes --audio 1" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--audio 1"
end_sup
new_root supau2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
audio=2
audio=yes
audio=
audio=1;touch EVIL
audio=-1
audio=0
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: audio 2, yes, empty, injection and -1 are ignored; only audio=0 passes (--audio 0)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--audio 0"
check "supervisor: the rejected audio values are logged and never evaluated" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 5 -a ! -e "$SDC/akam/EVIL" -a ! -e EVIL
end_sup
new_root supau3
mk_app run
: >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: no audio key: no --audio flag (off by default, the app's argument line is unchanged)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = ""
end_sup

# --- tracking and button keys: track 0|1, button_long_ms 1500..10000, track_area_<name>=PMIN:PMAX:TMIN:TMAX (at most 4)
new_root suptr
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
track = 1
button_long_ms = 3000
track_area_door = 500:1500:500:1500
track_area_yard=1400:2400:400:1200
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track, button_long_ms and track_area_<name> become --track, --button-long-ms and --track-area name=..." test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--track 1 --button-long-ms 3000 --track-area door=500:1500:500:1500 --track-area yard=1400:2400:400:1200"
end_sup
new_root suptr2
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
track=2
track=yes
button_long_ms=1000
button_long_ms=10001
button_long_ms=2000;touch EVIL
track_area_Door=1:2:3:4
track_area_1door=1:2:3:4
track_area_door=1:2:3
track_area_door=1:2:3:4:5
track_area_door=a:2:3:4
track_area_door=1::3:4
track_area_door=1:2:3:
track_area_door=1:2:3:4;touch EVIL
track_area_abcdefghijklmnopq=1:2:3:4
track_area_a=1:2:3:4
track_area_b=1:2:3:4
track_area_c=1:2:3:4
track_area_d=1:2:3:4
track_area_e=1:2:3:4
track=0
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: bad track / button_long_ms / track_area values are ignored, at most 4 areas pass (only the valid ones reach the app)" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--track-area a=1:2:3:4 --track-area b=1:2:3:4 --track-area c=1:2:3:4 --track-area d=1:2:3:4 --track 0"
check "supervisor: the rejected values are logged and never evaluated" test "$(grep -c 'akam.conf line .* ignored' "$SDC/akam/akam_boot.log")" -ge 14 -a ! -e "$SDC/akam/EVIL" -a ! -e EVIL
end_sup

# --- tracking tuning keys: track_return home|stay, track_return_delay_s, track_grace_s, track_band_pct, track_tilt, track_lead_pct
new_root suptr3
mk_app run
cat >"$SDC/akam/akam.conf" <<'EOS'
track_return = home
track_return_delay_s = 0
track_grace_s=5
track_band_pct=10
track_tilt=0
track_lead_pct=0
ptz_travel_budget_k=100
ptz_travel_budget_k=5
track_return=maybe
track_return_delay_s=86401
track_band_pct=1
track_lead_pct=101
track_tilt=2
track_return=home;touch EVIL
EOS
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: the track_return / delay / grace / band / tilt / lead keys become their options, bad ones are ignored" test "$(cut -d'|' -f2 "$R/tmp/app.runs")" = "--track-return home --track-return-delay-s 0 --track-grace-s 5 --track-band-pct 10 --track-tilt 0 --track-lead-pct 0 --ptz-travel-budget-k 100"
check "supervisor: the bad tracking values were never evaluated" test ! -e "$SDC/akam/EVIL" -a ! -e EVIL
end_sup

# --- track_head_pct, and the conf is re-read at every app (re)start
new_root suptr4
mk_app crash
printf 'track_tilt=1\ntrack_head_pct=20\ntrack_debug=1\ntrack_tilt_return_s=15\ntrack_head_pct=51\n' >"$SDC/akam/akam.conf"
AKAM_APP_BACKOFF_MIN=1 AKAM_APP_BACKOFF_MAX=1 AKAM_APP_CRASH_MAX=50 run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_head_pct 0..50 becomes --track-head-pct, 51 is ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-tilt 1 --track-head-pct 20 --track-debug 1 --track-tilt-return-s 15"
printf 'track_tilt=1\ntrack_head_pct=0\n' >"$SDC/akam/akam.conf"
i=0
while [ "$(runs)" -lt 2 ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "supervisor: a conf change applies at the next app restart (no supervisor restart)" test "$(sed -n 2p "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-tilt 1 --track-head-pct 0"
check "supervisor: the options are logged at each start" test "$(count_grep "$SDC/akam/akam_boot.log" 'akamd options:')" -ge 2
end_sup

# --- track_gain_pan / track_gain_tilt / track_tilt_band_pct
new_root suptr5
mk_app run
printf 'track_gain_pan=1200\ntrack_gain_tilt=1000\ntrack_tilt_band_pct=8\ntrack_gain_pan=199\ntrack_gain_tilt=4001\ntrack_tilt_band_pct=1\ntrack_gain_tilt=abc\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_gain_pan / track_gain_tilt / track_tilt_band_pct become their options, out-of-range values are ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-gain-pan 1200 --track-gain-tilt 1000 --track-tilt-band-pct 8"
end_sup

# --- track_motion continuous|steps, track_zone_pct / track_zone_y_pct / track_zone_head_pct, track_lease_ms
new_root suptr6
mk_app run
printf 'track_motion=steps\ntrack_zone_pct=50\ntrack_zone_y_pct=25\ntrack_zone_head_pct=0\ntrack_lease_ms=600\ntrack_zone_pct=91\ntrack_lease_ms=100\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_motion / track_zone_pct / track_zone_y_pct / track_zone_head_pct / track_lease_ms become their options, out-of-range values are ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-motion steps --track-zone-pct 50 --track-zone-y-pct 25 --track-zone-head-pct 0 --track-lease-ms 600"
end_sup
new_root suptr7
mk_app run
printf 'track_motion=teleport\ntrack_motion=continuous\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_motion accepts only continuous or steps (an exact word)" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-motion continuous"
end_sup

# --- track_lost_policy: the whole chain or nothing
new_root suplost1
mk_app run
printf 'track_lost_policy=backsweep,exit,local:300:4:20,route:walk:45,home\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: a valid track_lost_policy becomes --track-lost-policy" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-lost-policy backsweep,exit,local:300:4:20,route:walk:45,home"
end_sup
for bad in 'hold:0' 'fly' 'backsweep,' 'stay,home' 'local:30:4:20' 'local:300:1:20' 'route:Bad' 'route:a:0' 'hold:5:5' 'exit;reboot' 'none,home' 'a,b,c,d,e,f,g,h,i'; do
    new_root suplost2
    mk_app run
    printf 'track_lost_policy=%s\n' "$bad" >"$SDC/akam/akam.conf"
    run_sup
    wait_grep "$R/tmp/app.runs" . || true
    check "supervisor: track_lost_policy '$bad' is ignored as a whole" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = ""
    end_sup
done
new_root suplost3
mk_app run
printf 'track_lost_policy=none\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_lost_policy=none is accepted" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-lost-policy none"
end_sup

# --- track_tilt_back_s
new_root suptb
mk_app run
printf 'track_tilt_back_s=8\ntrack_tilt_back_s=601\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_tilt_back_s 0..600 becomes --track-tilt-back-s, 601 is ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-tilt-back-s 8"
end_sup

# --- track_close_scan, track_close_scan_up, track_close_scan_down
new_root supcs
mk_app run
printf 'track_close_scan=0\ntrack_close_scan=2\ntrack_close_scan_up=120\ntrack_close_scan_down=601\ntrack_close_scan_down=400\ntrack_close_scan_cooldown_s=601\ntrack_close_scan_cooldown_s=45\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_close_scan 0|1 and track_close_scan_up/_down/_cooldown_s 0..600 become --track-close-scan*, out of range is ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-close-scan 0 --track-close-scan-up 120 --track-close-scan-down 400 --track-close-scan-cooldown-s 45"
end_sup

# --- detect_debug / detect_model_thr (experiment E1)
new_root supdd
mk_app run
printf 'detect_debug=1\ndetect_debug=2\ndetect_model_thr=450\ndetect_model_thr=299\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: detect_debug 0..1 and detect_model_thr 300..950 become --detect-log / --detect-model-thr, out of range ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--detect-log 1 --detect-model-thr 450"
end_sup

# --- detect_head_threshold (tracking rule 6: the score a head box needs)
new_root suphd
mk_app run
printf 'detect_head_threshold=650\ndetect_head_threshold=299\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: detect_head_threshold 300..950 becomes --detect-head-thr, out of range ignored" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--detect-head-thr 650"
end_sup

# --- track_lag_ms (signed) and detect_age_window
new_root suplag
mk_app run
printf 'track_lag_ms=-120\ntrack_lag_ms=301\ntrack_lag_ms=-151\ntrack_lag_ms=abc\ndetect_age_window=1\ntrack_lag_ms=150\n' >"$SDC/akam/akam.conf"
run_sup
wait_grep "$R/tmp/app.runs" . || true
check "supervisor: track_lag_ms -150..300 (negative allowed) becomes --track-lag-ms=N, out of range and words are ignored; detect_age_window 0|1" test "$(head -n1 "$R/tmp/app.runs" | cut -d'|' -f2)" = "--track-lag-ms=-120 --detect-age-window 1 --track-lag-ms=150"
end_sup

# --- motor stop after the app ended (a continuous move has no hardware dead-man): one `akamd --ptz-stop` per app exit, polled every second by default
new_root supstop
mk_app crash
run_sup
wait_grep "$R/tmp/app.stops" . || true
check "supervisor: when the app dies it asks the app binary for a motor stop (--ptz-stop)" test -s "$R/tmp/app.stops"
check "supervisor: the stop call is not counted as an app run (the first run is the real one)" test "$(grep -c -- '--ptz-stop' "$R/tmp/app.runs" 2>/dev/null || true)" = 0
end_sup
new_root supstop2
mk_app run
run_sup
wait_grep "$R/tmp/app.runs" . || true
kill -TERM "$SUPPID" 2>/dev/null || true
wait_grep "$R/tmp/app.stops" . 100 || true
check "supervisor: a stop signal stops the app and then the motor" test -s "$R/tmp/app.stops"
end_sup
# shellcheck disable=SC2016  # the pattern is literal text of the script
check "supervisor: the death poll is 1 s by default (was 5 s)" grep -q '^POLL=${AKAM_APP_POLL:-1}' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh"
check "app: the crash handler stops the motor (async-signal-safe) before anything else" grep -q 'a2k_ptz_emergency_stop' "$REPO/platforms/anyka-ak3918av130/main.c"
check "app: --ptz-stop is a tool mode that exits before any hardware init" grep -q '"--ptz-stop"' "$REPO/platforms/anyka-ak3918av130/main.c"

# --- refusals: sha mismatch, no BUILDINFO hash, missing binary
new_root sup3
mk_app run
printf '\n# tampered\n' >>"$SDC/akam/bin/akamd"
run_sup
wait_sup 50 || true
check "sha mismatch: the supervisor exits" not_alive "$SUPPID"
check "sha mismatch: the app never ran" test ! -e "$R/tmp/app.runs"
check "sha mismatch: no copy in /tmp" test ! -e "$R/tmp/akamd"
check "sha mismatch: refusal logged" has "$SDC/akam/akam_boot.log" "refused: sha256 of the card file does not match"
wait "$SUPPID" 2>/dev/null && rc=0 || rc=$?
check "sha mismatch: exit status 1" test "$rc" = 1
new_root sup4
mk_app run
printf 'tool=akamd\n' >"$SDC/akam/bin/akamd.BUILDINFO"
run_sup
wait_sup 50 || true
check "BUILDINFO without sha256: refused, app never ran" test ! -e "$R/tmp/app.runs" -a "$(grep -c 'no valid sha256' "$SDC/akam/akam_boot.log")" = 1
new_root sup5
run_sup
wait_sup 50 || true
check "no binary: the supervisor logs and exits" has "$SDC/akam/akam_boot.log" "akamd: .*missing"
wait "$SUPPID" 2>/dev/null || true

# --- backoff: 1 s, 2 s, then capped at 2 s
new_root sup6
mk_app crash
AKAM_APP_BACKOFF_MIN=1 AKAM_APP_BACKOFF_MAX=2 AKAM_APP_CRASH_MAX=50 run_sup
i=0
while [ "$(count_grep "$SDC/akam/akam_boot.log" 'restarting akamd in')" -lt 3 ] && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "backoff: 1 s, then 2 s, then the 2 s cap" test "$(grep -o 'restarting akamd in [0-9]* s' "$SDC/akam/akam_boot.log" | head -n3 | sed 's/.* in //;s/ s//' | tr '\n' ' ')" = "1 2 2 "
check "backoff: each restart is a new run" test "$(runs)" -ge 3
end_sup

# --- give up after more than N crashes in the window
new_root sup7
mk_app crash
AKAM_APP_BACKOFF_MIN=0 AKAM_APP_BACKOFF_MAX=0 AKAM_APP_CRASH_MAX=3 run_sup
wait_sup 100 || true
check "give-up: the supervisor ends by itself" not_alive "$SUPPID"
sleep 0.3
check "give-up: the app ran exactly 4 times (more than 3 crashes)" test "$(runs)" = 4
check "give-up: logged, with the reason" has "$SDC/akam/akam_boot.log" "giving up"
wait "$SUPPID" 2>/dev/null && rc=0 || rc=$?
check "give-up: clean exit status" test "$rc" = 0

# --- crashes spread over more than the window never trigger the give-up
new_root sup8
mk_app crash
AKAM_APP_BACKOFF_MIN=1 AKAM_APP_BACKOFF_MAX=1 AKAM_APP_CRASH_MAX=2 AKAM_APP_CRASH_WINDOW=2 run_sup
i=0
while [ "$(runs)" -lt 6 ] && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "window: exits older than the window no longer count (still restarting after 6 runs)" test "$(runs)" -ge 6 -a "$(grep -c 'giving up' "$SDC/akam/akam_boot.log")" = 0
end_sup

# --- the supervisor counts time on /proc/uptime, not on the wall clock (the SNTP client steps 1970 -> 2026 once):
# a frozen fake uptime keeps every exit inside the window although 1 s backoffs pass on the wall clock (sup8 above
# proves that the wall clock would age them out), so the give-up must trigger; and the log must not show a huge run.
new_root sup8m
mk_app crash
printf '777.25 1500.00\n' >"$R/tmp/uptime"
AKAM_APP_UPTIME="$R/tmp/uptime" AKAM_APP_BACKOFF_MIN=1 AKAM_APP_BACKOFF_MAX=1 AKAM_APP_CRASH_MAX=2 AKAM_APP_CRASH_WINDOW=1 run_sup
wait_sup 100 || true
check "monotonic: crash window runs on the uptime source (give-up with a frozen uptime)" has "$SDC/akam/akam_boot.log" "giving up"
check "monotonic: no run length derived from the wall clock" test "$(grep -c 'exited with status [0-9]* after 0 s' "$SDC/akam/akam_boot.log")" = 3
end_sup
new_root sup8n
mk_app crash
printf 'garbage\n' >"$R/tmp/uptime"
AKAM_APP_UPTIME="$R/tmp/uptime" AKAM_APP_BACKOFF_MIN=0 AKAM_APP_BACKOFF_MAX=0 AKAM_APP_CRASH_MAX=3 run_sup
wait_sup 100 || true
check "monotonic: an unreadable uptime falls back to the wall clock (still gives up)" has "$SDC/akam/akam_boot.log" "giving up"
end_sup

# --- the app log is capped: copy-truncate at the limit, one generation
new_root sup9
mk_app spam
AKAM_APP_LOG_MAX=4000 run_sup
sleep 3
end_sup
check "log cap: a rotated generation exists" test -s "$SDC/akam/akam.log.1"
check "log cap: the live log stays near the limit" test "$(wc -c <"$SDC/akam/akam.log")" -lt 12000
check "log cap: the rotated one too" test "$(wc -c <"$SDC/akam/akam.log.1")" -lt 12000
check "log cap: no second generation" test ! -e "$SDC/akam/akam.log.2"
check "log cap: the default limit is 256 KiB x 2" test "$(grep -c 'LOG_MAX:-262144' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh")" = 1

# --- an app that ignores SIGTERM is killed after the grace period
new_root sup10
mk_app ignore
AKAM_APP_STOP_WAIT=1 run_sup
wait_grep "$R/tmp/app.pids" . || true
PI=$(head -n1 "$R/tmp/app.pids")
check "SIGKILL: the stubborn app runs" kill -0 "$PI"
end_sup
check "SIGKILL: the app is dead after the grace period" not_alive "$PI"
check "SIGKILL: logged" has "$SDC/akam/akam_boot.log" "did not stop within 1 s: SIGKILL"

echo "== own app: auto-start from the stub, stop order"
stub_env() {
    export AKAM_HEALTH_TRIES=3 AKAM_HEALTH_SLEEP=0 AKAM_IDLE_SLEEP=5 AKAM_IFACE_TRIES=3 AKAM_IFACE_SLEEP=0 AKAM_WPA_TRIES=3 AKAM_WPA_SLEEP=0
    export AKAM_SH=$SH_UNDER_TEST
    echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
}
new_stub_root sa1
stub_env
mk_app run
run_stub
wait_grep "$R/tmp/app.runs" . || true
check "stub: after healthy the app is started by the supervisor" test "$(runs)" = 1
check "stub: the supervisor start is logged" has "$SDC/akam/akam_boot.log" "app supervisor started"
check "stub: healthy was logged before the supervisor started" test "$(grep -n 'PoC-1 healthy\|app supervisor started' "$SDC/akam/akam_boot.log" | cut -d: -f1 | head -n2 | tr '\n' ' ' | awk '{print ($1 < $2)}')" = 1
check "stub: still one feeder (the app has its own process, never a second feeder)" test "$(wc -l <"$R/tmp/feeder.pids")" = 1
check "the supervisor never names the watchdog (outside comments)" test "$(sed -e 's/^[[:space:]]*#.*$//' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh" | grep -c -i watchdog)" = 0
FPA=$(head -n1 "$R/tmp/feeder.pids")
PA=$(head -n1 "$R/tmp/app.pids")
kill -TERM "$SPID"
wait_grep "$R/tmp/feeder.term" . 200 || true
wait "$SPID" 2>/dev/null || true
check "stop order: the app got SIGTERM" test -e "$R/tmp/app.term"
check "stop order: the app had finished (its handler takes 1 s) before the feeder got SIGTERM" test "$R/tmp/feeder.term" -nt "$R/tmp/app.exited"
sleep 0.3
check "stop order: app, feeder and stub are all gone" bash -c "! kill -0 $PA 2>/dev/null && ! kill -0 $FPA 2>/dev/null && ! kill -0 $SPID 2>/dev/null"

# a crash-looping app never touches the feeder, the stub or the network
new_stub_root sa2
stub_env
mk_app crash
AKAM_APP_BACKOFF_MIN=0 AKAM_APP_BACKOFF_MAX=0 AKAM_APP_CRASH_MAX=2 run_stub
wait_grep "$SDC/akam/akam_boot.log" "giving up" 100 || true
check "crash loop: the supervisor gave up" has "$SDC/akam/akam_boot.log" "giving up"
check "crash loop: the app ran 3 times" test "$(runs)" = 3
sleep 0.3
check "crash loop: the stub is still alive" kill -0 "$SPID"
check "crash loop: the feeder is alive and was never restarted (one pid)" test "$(wc -l <"$R/tmp/feeder.pids")" = 1 -a ! -e "$R/tmp/feeder.term"
check "crash loop: Wi-Fi was brought up once (one wpa_supplicant, one insmod)" test "$(grep -c '^wpa_supplicant' "$R/tmp/order.log")" = 1 -a "$(grep -c '^insmod' "$R/tmp/order.log")" = 1
check "crash loop: the boot counter was reset (healthy)" test "$(cat "$SDC/akam/own_app_boots" 2>/dev/null || echo 0)" = 0
end_stub

# no binary / flag no_app: PoC-1 behaviour, no supervisor
new_stub_root sa3
stub_env
run_stub
wait_grep "$SDC/akam/akam_boot.log" "no akam/bin/akamd" || true
check "no binary on the card: nothing started, PoC-1 behaviour" has "$SDC/akam/akam_boot.log" "no app started"
check "no binary on the card: no app ran" test ! -e "$R/tmp/app.runs"
end_stub
new_stub_root sa4
stub_env
mk_app run
: >"$SDC/akam/no_app"
run_stub
wait_grep "$SDC/akam/akam_boot.log" "flag akam/no_app" || true
check "flag akam/no_app: the app is not started" has "$SDC/akam/akam_boot.log" "app not started"
check "flag akam/no_app: no app ran" test ! -e "$R/tmp/app.runs"
end_stub
unset AKAM_SH AKAM_HEALTH_TRIES AKAM_HEALTH_SLEEP AKAM_IDLE_SLEEP AKAM_IFACE_TRIES AKAM_IFACE_SLEEP AKAM_WPA_TRIES AKAM_WPA_SLEEP AKAM_APP_POLL AKAM_APP_STOP_WAIT
check "the supervisor script is on every card the builder writes (overlay/akam/*.sh)" test -x "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh"

echo "== akamd frame ring (C unit test)"
if command -v cc >/dev/null 2>&1; then
 if cc -Wall -Wextra -Werror -std=gnu99 "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/stream_unit" "$REPO/tests/rot2k_stream_unit.c" "$REPO/platforms/anyka-ak3918av130/stream.c" "$REPO/src/net/rtsp/h264_params.c" "$REPO/src/net/rtsp/rtp_clock.c" -lpthread; then
        "$WORK/stream_unit" >"$WORK/stream_unit.out" 2>&1 || true
        check "stream ring unit test passes" grep -q 'rot2k stream unit: 0 failed' "$WORK/stream_unit.out"
        grep 'FAIL' "$WORK/stream_unit.out" || true
    else
        ko "stream ring unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: stream ring unit test"
fi

echo "== akamd detection state (C unit test)"
if command -v cc >/dev/null 2>&1; then
 if cc -Wall -Wextra -Werror -std=gnu99 "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/detect_unit" "$REPO/tests/rot2k_detect_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.c"; then
        "$WORK/detect_unit" >"$WORK/detect_unit.out" 2>&1 || true
        check "detection state unit test passes" grep -q 'rot2k detect unit: 0 failed' "$WORK/detect_unit.out"
        grep 'FAIL' "$WORK/detect_unit.out" || true
    else
        ko "detection state unit test does not compile"
    fi
    if cc -O2 -Wall -Wextra -Werror -std=gnu99 "${INC_2K[@]}" -o "$WORK/track_relay_unit" "$REPO/tests/rot2k_track_relay_unit.c" "$REPO/src/features/track/track_relay.c"; then
        "$WORK/track_relay_unit" >"$WORK/track_relay_unit.out" 2>&1 || true
        check "track relay unit test passes (ZNCC follow on synthetic frames, gated, capped, detector box wins)" grep -q 'rot2k track relay unit: 0 failed' "$WORK/track_relay_unit.out"
        grep 'FAIL' "$WORK/track_relay_unit.out" || true
    else
        ko "track relay unit test does not compile"
    fi
    echo "== detection zones (C unit test: strict parser, fuzz, membership, per-zone hysteresis)"
    ZN_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/zn_probe" - 2>/dev/null; then ZN_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/zones_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_zones_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.c"; then
        "$WORK/zones_unit" >"$WORK/zones_unit.out" 2>&1 || true
        check "zones unit test passes (parser, fuzz, feet point, zone_mode any/all, hysteresis, state file)" grep -q 'rot2k zones unit: 0 failed' "$WORK/zones_unit.out"
        grep 'FAIL' "$WORK/zones_unit.out" || true
    else
        ko "zones unit test does not compile"
    fi
    echo "== JPEG snapshot module (C unit test, stub encoder: ring bounds, rate limit, failures)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/snap_unit" "$REPO/tests/rot2k_snap_unit.c" "$REPO/platforms/anyka-ak3918av130/snap.c"; then
        "$WORK/snap_unit" >"$WORK/snap_unit.out" 2>&1 || true
        check "snapshot unit test passes (ring bounds, rate limit, 3 failures disable, slow publisher)" grep -q 'rot2k snap unit: 0 failed' "$WORK/snap_unit.out"
        grep 'FAIL' "$WORK/snap_unit.out" || true
    else
        ko "snapshot unit test does not compile"
    fi
    echo "== akam-mic (PoC microphone tool: C unit test of the WAV header, dBFS maths and output path rule; host compile of the tool)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
    if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -I"$FW/helpers/mic" -o "$WORK/mic_unit" "$REPO/tests/rot2k_mic_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic_core.c" -lm; then
        "$WORK/mic_unit" >"$WORK/mic_unit.out" 2>&1 || true
        check "mic unit test passes (WAV header, dBFS/RMS maths, /tmp-only path rule)" grep -q 'rot2k mic unit: 0 failed' "$WORK/mic_unit.out"
        grep 'FAIL' "$WORK/mic_unit.out" || true
    else
        ko "mic unit test does not compile"
    fi
    if cc -Wall -Wextra -Werror -std=gnu99 -I"$FW/helpers/mic" -I"$REPO/src/hal" -o "$WORK/mic_host" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic_core.c"; then
        ok "akam-mic compiles clean on the host"
        check "akam-mic: a path outside /tmp is refused before anything is opened (exit 2)" test "$("$WORK/mic_host" /etc/x.wav >/dev/null 2>&1; echo $?)" = 2
        check "akam-mic: usage errors exit 2 (seconds 0 and 31, rate 44100, no file)" test "$("$WORK/mic_host" -s 0 /tmp/x.wav >/dev/null 2>&1; echo $?)$("$WORK/mic_host" -s 31 /tmp/x.wav >/dev/null 2>&1; echo $?)$("$WORK/mic_host" -r 44100 /tmp/x.wav >/dev/null 2>&1; echo $?)$("$WORK/mic_host" >/dev/null 2>&1; echo $?)" = 2222
    else
        ko "akam-mic does not compile on the host"
    fi
    echo "== akamd microphone track (C unit tests: multi-reader ring, wake pipes, L16 payload, RTP timestamps, start gate; capture thread without a device)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/audio_unit" "$REPO/tests/rot2k_audio_unit.c" "$REPO/platforms/anyka-ak3918av130/aring.c" "$REPO/src/net/rtsp/rtp_clock.c"; then
        "$WORK/audio_unit" >"$WORK/audio_unit.out" 2>&1 || true
        check "audio unit test passes (ring bounds, drop oldest, fan-out threads, wake pipes, gap, L16 byte order, RTP timestamps, start gate)" grep -q 'rot2k audio unit: 0 failed' "$WORK/audio_unit.out"
        grep 'FAIL' "$WORK/audio_unit.out" || true
    else
        ko "audio unit test does not compile"
    fi
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/audio_thread" "$REPO/tests/rot2k_audio_thread_test.c" "$REPO/platforms/anyka-ak3918av130/audio.c" "$REPO/platforms/anyka-ak3918av130/aring.c" "$REPO/src/net/rtsp/rtp_clock.c"; then
        "$WORK/audio_thread" >"$WORK/audio_thread.out" 2>&1 || true
        check "audio capture thread without a device passes (stays down, logs, stops promptly, never crashes)" grep -q 'rot2k audio thread: 0 failed' "$WORK/audio_thread.out"
        grep 'FAIL' "$WORK/audio_thread.out" || true
    else
        ko "audio capture thread test does not compile"
    fi
    echo "== akamd speaker and two-way audio (C unit test of the speaker glue on the real shared engine and talk session, no device)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
    if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/speaker_unit" "$REPO/tests/rot2k_speaker_unit.c" "$FW_2K/speaker.c" "$REPO/src/features/audio/audio_out.c" "$REPO/src/features/audio/talk.c" "$REPO/src/features/audio/audio_jitter.c" "$REPO/src/features/audio/audio_vad.c" "$REPO/src/features/audio/g711.c" "$REPO/src/net/rtsp/rtp_in.c" "$REPO/src/net/rtsp/rtp_clock.c" -lm; then
        "$WORK/speaker_unit" >"$WORK/speaker_unit.out" 2>&1 || true
        check "speaker unit test passes (same keys, ranges and defaults as the 1080p, opt-in enable, live volume/gain/amp/duplex, clean failure without a device, crash guard rules, engine file never written)" grep -q 'rot2k speaker unit: 0 failed' "$WORK/speaker_unit.out"
        grep 'FAIL' "$WORK/speaker_unit.out" || true
    else
        ko "speaker unit test does not compile"
    fi
    echo "== akamd picture settings (C unit test of saturation, denoise and white balance on a fake ISP)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
    if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/picture_unit" "$REPO/tests/rot2k_picture_unit.c" "$REPO/platforms/anyka-ak3918av130/picture.c"; then
        "$WORK/picture_unit" >"$WORK/picture_unit.out" 2>&1 || true
        check "picture unit test passes (saturation range, the 3D NR record shift and its refusals, white balance type, replay after a day/night switch, unbound ISP)" grep -q 'rot2k picture unit: 0 failed' "$WORK/picture_unit.out"
        grep 'FAIL' "$WORK/picture_unit.out" || true
    else
        ko "picture unit test does not compile"
    fi
    echo "== akam-sntp (C unit test of the packet/era/offset core, and a loopback run of the client in a private network namespace)"
    # shellcheck disable=SC2086  # ZN_SAN is empty or one flag
    if cc -Wall -Wextra -Werror -std=gnu99 -g $ZN_SAN -I"$FW/helpers/sntp" -o "$WORK/sntp_unit" "$REPO/tests/rot2k_sntp_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/sntp/sntp_core.c"; then
        "$WORK/sntp_unit" >"$WORK/sntp_unit.out" 2>&1 || true
        check "sntp unit test passes (request, reply checks, NTP eras 2036/2038, offset, 32-bit target)" grep -q 'rot2k sntp unit: 0 failed' "$WORK/sntp_unit.out"
        grep 'FAIL' "$WORK/sntp_unit.out" || true
    else
        ko "sntp unit test does not compile"
    fi
    if cc -Wall -Wextra -Werror -std=gnu99 -o "$WORK/sntp_host" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/sntp/sntp.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/sntp/sntp_core.c"; then
        ok "akam-sntp compiles clean on the host"
        if command -v python3 >/dev/null 2>&1 && timeout 10 unshare -rn sh -c 'PATH=/usr/sbin:/usr/bin:/sbin:/bin; ip link set lo up 2>/dev/null' 2>/dev/null; then
            cat >"$WORK/fakentp.py" <<'EOPY'
import socket, struct, sys, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 123))
s.settimeout(10)
open(sys.argv[1], "w").write("up")
mode = sys.argv[2]
def ts(t):
    return struct.pack(">II", int(t) + 2208988800, int((t % 1) * 2**32))
d, a = s.recvfrom(100)
t = time.time()
org = d[40:48]
li_vn_mode, stratum = 0x24, 2
if mode == "badorigin": org = bytes(8)
if mode == "wrongmode": li_vn_mode = 0x23
if mode == "kod": stratum = 0
s.sendto(bytes([li_vn_mode, stratum, 0, 0]) + bytes(20) + org + ts(t) + ts(t), a)
EOPY
            for m in good badorigin wrongmode kod; do
                rm -f "$WORK/fakentp.up"
                rc=$(timeout 30 unshare -rn sh -c "PATH=/usr/sbin:/usr/bin:/sbin:/bin; ip link set lo up; python3 '$WORK/fakentp.py' '$WORK/fakentp.up' $m & sleep 1; '$WORK/sntp_host' -n -q -p 127.0.0.1 2>/dev/null; echo \$?" 2>/dev/null | tail -n1)
                if [ "$m" = good ]; then check "sntp loopback: a valid reply from a fake server ends with exit 0 (no step needed, clock already right)" test "$rc" = 0
                else check "sntp loopback: reply '$m' is refused, exit 1 after the 3 s timeout" test "$rc" = 1; fi
            done
        else
            echo "  skip - no python3 or no unprivileged network namespace: sntp loopback run"
        fi
        check "sntp: usage errors exit 2 (no server; option as server; unknown option)" test "$("$WORK/sntp_host" >/dev/null 2>&1; echo $?)$("$WORK/sntp_host" -p -x >/dev/null 2>&1; echo $?)$("$WORK/sntp_host" -z >/dev/null 2>&1; echo $?)" = 222
    else
        ko "akam-sntp does not compile on the host"
    fi
    echo "== akamd MQTT module (C unit test, stub socket, fuzz; and the host broker sink)"
if command -v cc >/dev/null 2>&1; then
    MQ_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe" - 2>/dev/null; then MQ_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # MQ_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $MQ_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/mqtt_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_mqtt_unit.c" "$REPO/platforms/anyka-ak3918av130/mqtt_bind.c" "$REPO/src/features/ptz/ptz_vel.c" "$REPO/platforms/anyka-ak3918av130/mqtt_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/src/api/api_events.c" "$REPO/src/net/mqtt/mqtt.c" "$REPO/src/net/mqtt/mqtt_ha.c"; then
        "$WORK/mqtt_unit" >"$WORK/mqtt_unit.out" 2>&1 || true
        check "MQTT module unit test passes (config, golden payloads, fuzz, reconnect, no secret in the log)" grep -q 'rot2k mqtt unit: 0 failed' "$WORK/mqtt_unit.out"
        grep 'FAIL' "$WORK/mqtt_unit.out" || true
    else
        ko "MQTT module unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: MQTT unit test"
fi
if command -v python3 >/dev/null 2>&1; then
    python3 -I "$REPO/tests/rot2k_mqtt_sink_test.py" >"$WORK/mqtt_sink.out" 2>&1 || true
    check "mqtt_sink.py test passes" grep -q 'rot2k mqtt sink test: 0 failed' "$WORK/mqtt_sink.out"
    grep 'FAIL' "$WORK/mqtt_sink.out" || true
else
    echo "  skip - no python3: mqtt_sink test"
fi

echo "== day/night and image (C unit test: pins on a fake sysfs, IR-cut sequence, engine on the shared policy, image)"
if command -v cc >/dev/null 2>&1; then
    NT_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe2" - 2>/dev/null; then NT_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # NT_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g -pthread $NT_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/night_unit" "$REPO/tests/rot2k_night_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.c" "$REPO/platforms/anyka-ak3918av130/ircut.c" "$REPO/platforms/anyka-ak3918av130/image.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/night_ctl.c" "$REPO/src/features/night/night_logic.c" "$REPO/src/core/cfg_file.c"; then
        "$WORK/night_unit" >"$WORK/night_unit.out" 2>&1 || true
        check "day/night and image unit test passes" grep -q 'rot2k night unit: 0 failed' "$WORK/night_unit.out"
        grep 'FAIL' "$WORK/night_unit.out" || true
    else
        ko "day/night and image unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: day/night unit test"
fi
echo "== hal_sys (C unit test of the /proc parsers, the sampler on fixtures and the sys MQTT sensors)"
if command -v cc >/dev/null 2>&1; then
    SY_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe4" - 2>/dev/null; then SY_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # SY_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $SY_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/sys_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_sys_unit.c" "$REPO/src/hal/hal_sys.c" "$REPO/platforms/anyka-ak3918av130/mqtt_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/src/net/mqtt/mqtt_ha.c"; then
        "$WORK/sys_unit" >"$WORK/sys_unit.out" 2>&1 || true
        check "sys unit test passes (cpu/load/mem/uptime/wifi parsers, fixtures, absent values, MQTT diagnostic sensors)" grep -q 'rot2k sys unit: 0 failed' "$WORK/sys_unit.out"
        grep 'FAIL' "$WORK/sys_unit.out" || true
    else
        ko "sys unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: sys unit test"
fi
echo "== status LED (C unit test: pattern parser, 3:250:250:5000 timing on a fake clock, rule priority, engine on a fake driver, route parser, Home Assistant controls)"
if command -v cc >/dev/null 2>&1; then
    LD_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe5" - 2>/dev/null; then LD_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # LD_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $LD_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/led_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_led_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/leds.c" "$REPO/src/features/light/led_pat.c" "$REPO/src/features/light/light_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal.c" "$REPO/src/hal/hal_sys.c" "$REPO/platforms/anyka-ak3918av130/mqtt_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/src/net/mqtt/mqtt_ha.c"; then
        "$WORK/led_unit" >"$WORK/led_unit.out" 2>&1 || true
        check "status LED unit test passes (patterns, timing, rule priority, engine, route parser, select / text discovery and command parsing)" grep -q 'rot2k led unit: 0 failed' "$WORK/led_unit.out"
        grep 'FAIL' "$WORK/led_unit.out" || true
    else
        ko "status LED unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: status LED unit test"
fi
echo "== video settings (C unit test: defaults unchanged, ranges, strict parsing, runtime file of the differing keys, set / pending / applied / refused re-open)"
if command -v cc >/dev/null 2>&1; then
    VD_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_vd" - 2>/dev/null; then VD_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # VD_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $VD_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/video_unit" "$REPO/tests/rot2k_video_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c"; then
        "$WORK/video_unit" >"$WORK/video_unit.out" 2>&1 || true
        check "video unit test passes (defaults unchanged, per-stream ranges, strict parsing, runtime file, pending / applied / refused)" grep -q 'rot2k video unit: 0 failed' "$WORK/video_unit.out"
        grep 'FAIL' "$WORK/video_unit.out" || true
    else
        ko "video unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: video unit test"
fi
echo "== live video apply (C unit test, fake encoder: live setters, refusal falls back, disabled flag, video_live=0, debug hook parsing)"
if command -v cc >/dev/null 2>&1; then
    VL_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_vl" - 2>/dev/null; then VL_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # VL_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $VL_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/vlive_unit" "$REPO/tests/rot2k_vlive_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/vlive.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c"; then
        "$WORK/vlive_unit" >"$WORK/vlive_unit.out" 2>&1 || true
        check "live video unit test passes (set_fps / set_qp / get+set_attr / set_rc_type, refusal = re-open + setter off, video_live=0, hook commands)" grep -q 'rot2k vlive unit: 0 failed' "$WORK/vlive_unit.out"
        grep 'FAIL' "$WORK/vlive_unit.out" || true
    else
        ko "live video unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: live video unit test"
fi
echo "== ISP test hook (C unit test, fake ISP: line parser, get never writes, fx / wb / ccm / nr changes touch only the intended fields, matrix blend keeps the row sums, restore, round trip, command file)"
if command -v cc >/dev/null 2>&1; then
    IT_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_it" - 2>/dev/null; then IT_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # IT_SAN is empty or one flag
    if cc -Wall -Wextra -Werror -std=gnu99 -g $IT_SAN "${INC_2K[@]}" -o "$WORK/isptest_unit" "$REPO/tests/rot2k_isptest_unit.c" "$REPO/platforms/anyka-ak3918av130/isptest.c"; then
        (cd "$WORK" && ./isptest_unit >isptest_unit.out 2>&1) || true
        check "ISP test hook unit test passes (parser, get / fx / wb / ccm / nr, restore, round trip, command file)" grep -q 'rot2k isptest unit: 0 failed' "$WORK/isptest_unit.out"
        grep 'FAIL' "$WORK/isptest_unit.out" || true
    else
        ko "ISP test hook unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: ISP test hook unit test"
fi
echo "== front button, person tracking, runtime zones (C unit tests: debouncer + press policy + input pin on a fake gpio dir + LED override; track_logic; tracking wiring + areas on a fake motor; live zones + atomic store)"
if command -v cc >/dev/null 2>&1; then
    BT_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_bt" - 2>/dev/null; then BT_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # BT_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/button_unit" "$REPO/tests/rot2k_button_unit.c" "$REPO/src/hal/hal_input.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal_input_sysfs.c" "$REPO/src/features/button/button_gesture.c" "$REPO/src/features/button/button_cfg.c" "$REPO/src/features/button/button_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/button_rt.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" "$REPO/src/api/json_min.c" "$REPO/src/api/api_events.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/leds.c" "$REPO/src/features/light/led_pat.c" "$REPO/src/features/light/light_logic.c"; then
        "$WORK/button_unit" >"$WORK/button_unit.out" 2>&1 || true
        check "button unit test passes (debounce, gesture engine timing tables incl. noise and overlapping sequences, action whitelist, modes, dispatch with fake ops, defaults identical, persistence only on change, HTTP handlers, dead zone, hold cue, calibration lock, held at start, input pin read-only on a fake gpio dir, LED override)" grep -q 'rot2k button unit: 0 failed' "$WORK/button_unit.out"
        grep 'FAIL' "$WORK/button_unit.out" || true
    else
        ko "button unit test does not compile"
    fi
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_unit" "$REPO/tests/rot2k_track_unit.c" "$REPO/src/features/track/track_logic.c"; then
        if "$WORK/track_unit" >"$WORK/track_unit.out" 2>&1; then
            ok "track_logic unit test passes (pure policy: dead band, sticky target, rate limit, soft range, home after loss)"
        else
            ko "track_logic unit test fails"
            cat "$WORK/track_unit.out"
        fi
    else
        ko "track_logic unit test does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_wire_unit" "$REPO/tests/rot2k_track_wire_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c"; then
        "$WORK/track_wire_unit" >"$WORK/track_wire_unit.out" 2>&1 || true
        check "tracking wiring unit test passes (sign mapping, off by default, never parked / calibrating / unknown / privacy, home after loss, named areas: parse, overlap, enable, clip, corners, persistence)" grep -q 'rot2k track wire unit: 0 failed' "$WORK/track_wire_unit.out"
        grep 'FAIL' "$WORK/track_wire_unit.out" || true
    else
        ko "tracking wiring unit test does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g -O1 $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_sim" "$REPO/tests/rot2k_track_sim.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" -lm; then
        "$WORK/track_sim" >"$WORK/track_sim.out" 2>&1 || true
        check "tracking simulation passes (walking person, constant / stop-and-go / reversal / dropouts, fake PTZ with realistic motion time: in frame up to 300 steps/s, no oscillation, return stay / home, soft range and rate limit kept)" grep -q 'rot2k track sim: 0 failed' "$WORK/track_sim.out"
        grep 'FAIL' "$WORK/track_sim.out" || true
    else
        ko "tracking simulation does not compile"
    fi
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_cont_unit" "$REPO/tests/rot2k_track_cont_unit.c" "$REPO/src/features/track/track_logic.c" -lm; then
        "$WORK/track_cont_unit" >"$WORK/track_cont_unit.out" 2>&1 || true
        check "tracking policy unit test passes (comfort zone and hysteresis, body / head reference, outlier boxes, robust velocity, 3D state near / far consistency, continuous command, sweep back after an overshoot, replays of two hardware logs)" grep -q 'rot2k track cont unit: 0 failed' "$WORK/track_cont_unit.out"
        grep 'FAIL' "$WORK/track_cont_unit.out" || true
    else
        ko "tracking policy unit test does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g -O1 $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_cont_sim" "$REPO/tests/rot2k_track_cont_sim.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" -lm; then
        "$WORK/track_cont_sim" >"$WORK/track_cont_sim.out" 2>&1 || true
        check "tracking motion simulation passes (DEV vs STEPS vs CONT: walkers 0.5-1.5 m/s at 1-4 m, crouch / stand, approach, occlusion, two people crossing, feints and sprints never leave the frame, overshoot search)" grep -q 'rot2k track cont sim: 0 failed' "$WORK/track_cont_sim.out"
        grep 'FAIL' "$WORK/track_cont_sim.out" || true
    else
        ko "tracking motion simulation does not compile"
    fi
    python3 -I "$REPO/tests/rot2k_track_lagfit_test.py" >"$WORK/track_lagfit.out" 2>&1 || true
    check "tools/track_lagfit test passes (known lag / gains of synthetic sweeps are recovered, a static recording is refused)" grep -q 'rot2k track lagfit test: 0 failed' "$WORK/track_lagfit.out"
    grep 'FAIL' "$WORK/track_lagfit.out" || true
    # the raw recorder (track_debug=2) and the replay of recordings (tests/fixtures/track: the hardware logs of the continuous replays)
    # shellcheck disable=SC2086
    if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN "${INC_2K[@]}" -o "$WORK/track_replay" "$REPO/tests/rot2k_track_replay.c" "$REPO/src/features/track/track_logic.c" -lm; then
        rp_ok=1
        for fx in "$REPO"/tests/fixtures/track/*.raw; do
            "$WORK/track_replay" --twice --heads -q "$fx" >"$WORK/track_replay.out" 2>&1 || { rp_ok=0; echo "replay of $fx failed:"; cat "$WORK/track_replay.out"; }
        done
        check "track replay: every fixture (hardware logs) replays deterministically, with its expectations, and head boxes added change no decision" test "$rp_ok" = 1
    else
        ko "track replay tool does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/track_rec_unit" "$REPO/tests/rot2k_track_rec_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" -lm; then
        "$WORK/track_rec_unit" >"$WORK/track_rec_unit.out" 2>&1 || true
        check "track raw recorder unit test passes (I / R / C / D lines, rolling bound, the replay decides like the live tracker)" grep -q 'rot2k track rec unit: 0 failed' "$WORK/track_rec_unit.out"
        grep 'FAIL' "$WORK/track_rec_unit.out" || true
    else
        ko "track raw recorder unit test does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g $BT_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/zlive_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_zlive_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/platforms/anyka-ak3918av130/zlive.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c"; then
        "$WORK/zlive_unit" >"$WORK/zlive_unit.out" 2>&1 || true
        check "live zones unit test passes (ignore rectangles, text forms, persistence, runtime file wins, applies at home only, atomic store)" grep -q 'rot2k zlive unit: 0 failed' "$WORK/zlive_unit.out"
        grep 'FAIL' "$WORK/zlive_unit.out" || true
    else
        ko "live zones unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: button / tracking / live zones unit tests"
fi
echo "== hal_light (C unit test of the portable light logic with a fake driver, the 2K descriptor, the sysfs driver on fake directories; host compile of akam-led)"
if command -v cc >/dev/null 2>&1; then
    LT_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe3" - 2>/dev/null; then LT_SAN="-fsanitize=address,undefined"; fi
    LT_SRC=("$REPO/src/features/light/light_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal_light_sysfs.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.c")
    # shellcheck disable=SC2086  # LT_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $LT_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/light_unit" "$REPO/tests/rot2k_light_unit.c" "${LT_SRC[@]}"; then
        "$WORK/light_unit" >"$WORK/light_unit.out" 2>&1 || true
        check "light unit test passes (on/off, blink 1 Hz / 0.5 Hz, 2K descriptor ir + blue, pin whitelist refuses 22 / 49 / 55, sysfs drivers)" grep -q 'rot2k light unit: 0 failed' "$WORK/light_unit.out"
        grep 'FAIL' "$WORK/light_unit.out" || true
    else
        ko "light unit test does not compile"
    fi
 if cc -Wall -Wextra -Werror -std=gnu99 "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/led_host" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/led/led.c" "${LT_SRC[@]}"; then
        ok "akam-led compiles clean on the host"
        check "akam-led: 'ir' is refused without --force (exit 2) before any file is touched" test "$("$WORK/led_host" ir on >/dev/null 2>&1; echo $?)" = 2
        check "akam-led: usage errors exit 2 (no args, unknown light, unknown mode, ir-cut pin names, --secs 0 and 61)" test "$("$WORK/led_host" >/dev/null 2>&1; echo $?)$("$WORK/led_host" nope on >/dev/null 2>&1; echo $?)$("$WORK/led_host" white dim >/dev/null 2>&1; echo $?)$("$WORK/led_host" ircut on >/dev/null 2>&1; echo $?)$("$WORK/led_host" --secs 0 white blink-fast >/dev/null 2>&1; echo $?)$("$WORK/led_host" --secs 61 white blink-fast >/dev/null 2>&1; echo $?)" = 222222
        check "akam-led list prints the two emitters" test "$("$WORK/led_host" list | tr '\n' ' ')" = "ir blue "
    else
        ko "akam-led does not compile on the host"
    fi
else
    echo "  skip - no host C compiler: light unit test"
fi
check "the shared day/night policy is compiled from platforms/anyka-ak3918, not copied: no night_logic.c in the app, listed in model.mk and in the source hash (the hash list is generated from the build)" bash -c "test ! -e '$PL/night_logic.c' && test ! -e '$FW/night_logic.c' && grep -q '^SHARED_AK := .* night_logic cfg_file' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk' && make -s -C '$REPO/platforms/anyka-ak3918av130' print-sources | grep -q '/src/features/night/night_logic.c' && sh '$REPO/firmware/lsc-rotatable-2k-ak3918av130/image/source_hash.sh' --list | grep -q '/src/features/night/night_logic.c'"

echo "== akamd detection thread ending (stubbed vendor calls: disable chn 16, else drain it)"
    # The ABI size asserts describe the 32-bit camera ABI: neutered for this host build, the ARM build keeps them.
 if cc -Wall -Wextra -Werror -std=gnu99 -pthread '-D_Static_assert(a,b)=' "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/detect_thread" "$REPO/platforms/anyka-ak3918av130/orient.c" \
        "$REPO/tests/rot2k_detect_thread_test.c" "$REPO/platforms/anyka-ak3918av130/detect.c" "$REPO/src/features/track/track_relay.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.c" "$REPO/platforms/anyka-ak3918av130/detpub.c" "$REPO/src/api/api_events.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/src/net/rtsp/rtp_clock.c"; then
        for m in ok refused stopping; do
            "$WORK/detect_thread" $m >"$WORK/detect_thread.$m.out" 2>&1 || true
            check "detection thread end ($m) passes" grep -q "rot2k detect thread ($m): 0 failed" "$WORK/detect_thread.$m.out"
            grep 'FAIL' "$WORK/detect_thread.$m.out" || true
        done
    else
        ko "detection thread test does not compile"
    fi
    echo "== akamd detection frame choice (stubbed vendor calls: newest frame of the chn 16 queue)"
 if cc -Wall -Wextra -Werror -std=gnu99 -pthread '-D_Static_assert(a,b)=' "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/detect_fresh" "$REPO/platforms/anyka-ak3918av130/orient.c" \
        "$REPO/tests/rot2k_detect_fresh_test.c" "$REPO/platforms/anyka-ak3918av130/detect.c" "$REPO/src/features/track/track_relay.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.c" "$REPO/platforms/anyka-ak3918av130/detpub.c" "$REPO/src/api/api_events.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" "$REPO/src/net/rtsp/rtp_clock.c"; then
        "$WORK/detect_fresh" >"$WORK/detect_fresh.out" 2>&1 || true
        check "detection newest-frame test passes" grep -q 'rot2k detect fresh: 0 failed' "$WORK/detect_fresh.out"
        grep 'FAIL' "$WORK/detect_fresh.out" || true
    else
        ko "detection newest-frame test does not compile"
    fi
else
    echo "  skip - no host C compiler: detection state unit test"
fi

# ------------------------------------------------------------------------------- static anti-brick scan
echo "== static scan: no flash, no upgrade or factory trigger in any on-camera script"
# scan <file>...: prints every non-comment line with a forbidden pattern. Only FULL-LINE comments are ignored
# (they may document what the scripts must never do); a trailing "# ..." is scanned, so it cannot hide code.
# A device path can be built from variables ("/dev/m""td"), so ANY /dev/ reference other than /dev/null is
# forbidden, as are eval, dd and remount (a script has no business with any of them).
FORBID='mtd|mtdblock|flash_erase|flashcp|nvram|fw_setenv|yy_upgrade|YYOS_UPGRADE|rmem_blk|fac_av130|mode=factory|of=/dev|/dev/|\beval\b|\bdd\b|remount'
scan() {
    local f
    for f in "$@"; do
        allow='s|/dev/null||g'
        # The ONE exception, like /dev/tymotor in the C source: the literal /dev/watchdog, in the stub alone.
        [ "$(basename "$f")" != own_app_stub.sh ] || allow='s|/dev/null||g;s|/dev/watchdog||g'
        sed -e 's/^[[:space:]]*#.*$//' -e "$allow" "$f" | grep -n -E "$FORBID" | sed "s|^|$f:|" || true
    done
}
SCANNED=()
while IFS= read -r f; do SCANNED+=("$f"); done < <(find "$FW/boot/card" "$FW/boot/rescue" -type f | sort)
[ "${#SCANNED[@]}" -ge 8 ] || ko "static scan: expected at least 8 on-camera files, found ${#SCANNED[@]}"
hits=$(scan "${SCANNED[@]}")
check "no forbidden pattern outside comments" test -z "$hits"
[ -z "$hits" ] || echo "$hits"
printf '#!/bin/sh\n# mtd in a comment is fine\ndd if=x of=/dev/mtdblock3\nnvram set A=1\n' >"$WORK/bad.sh"
check "/dev/watchdog appears once, in own_app_stub.sh only" test "$(grep -rl '/dev/watchdog' "${SCANNED[@]}" | sed 's|.*/||' | tr '\n' ' ')" = "own_app_stub.sh "
check "/dev/watchdog literal is used exactly once (outside comments) in the stub" test "$(sed -e 's/^[[:space:]]*#.*$//' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_stub.sh" | grep -c '/dev/watchdog')" = 1
printf '#!/bin/sh\ncat /dev/watchdog\n' >"$WORK/wd.sh"
check "another script using /dev/watchdog is flagged" test "$(scan "$WORK/wd.sh" | wc -l)" = 1
check "the scanner does flag a bad script (positive control)" test "$(scan "$WORK/bad.sh" | wc -l)" = 2
# shellcheck disable=SC2016  # the literal "$X" belongs to the bad script
printf '#!/bin/sh\nD=/dev/m"t"d3\necho x # flash_erase\neval "$X"\nmount -o remount,rw /\ncat x >/dev/null\n' >"$WORK/bad2.sh"
check "the scanner flags a built device path, a trailing-comment trick, eval, remount (and not /dev/null)" test "$(scan "$WORK/bad2.sh" | wc -l)" = 4
# The C sources of the model dir (akam_ptz) get the same scan: the ONLY device path allowed is the literal
# "/dev/tymotor", in ptz/akam_ptz.c alone. Full-line C comments are ignored; the init requests may be spelled
# ONLY as the exact tokens 0x40100c03u and 0x40280c04u, and only in ptz/akam_ptz.c; any other 0x0c03/0x0c04
# spelling is a hit (the sweep and absolute-move commands are command words, covered by the unit test).
# strip_c_comments: removes /* */ and // comments (string- and char-literal aware) so that a comment can neither
# hide code (a line starting with * or after /* x */) nor be flagged.
strip_c_comments() {
    awk '
    BEGIN { inb = 0 }
    {
        out = ""; n = length($0); i = 1; q = ""
        while (i <= n) {
            c = substr($0, i, 1); d = substr($0, i, 2)
            if (inb) { if (d == "*/") { inb = 0; i += 2 } else i++; continue }
            if (q != "") {
                out = out c
                if (c == "\\") { out = out substr($0, i + 1, 1); i += 2; continue }
                if (c == q) q = ""
                i++; continue
            }
            if (d == "/*") { inb = 1; out = out " "; i += 2; continue }
            if (d == "//") break
            if (c == "\"" || c == "\047") q = c
            out = out c; i++
        }
        print out
    }' "$1"
}
# Also forbidden in C: the calibration request numbers in any spelling, process spawning, and adjacent string
# literals (which could build a device path piecewise).
# An EMPTY string literal after = ( , ? : (an initializer or a default argument, e.g. `const char *e = "";` in the shared ptz_vel.c, now scanned because the scan list
# is derived from the build) is not a piecewise device path: it is removed before the adjacent-literal rule runs; "a""b" and "a" "b" are still caught.
# allow_init_sed: sed script removing the two exact init request tokens, for ptz/akam_ptz.c only
allow_init_sed() {
    case "$1" in
        */ptz/akam_ptz.c | */hal_ptz.c) printf '%s' 's/\b0x40100c03u\b//g; s/\b0x40280c04u\b//g' ;;
        */mic/mic.c | */audio.c) printf '%s' 's/^#define PCM_CAPTURE "\/dev\/pcmC0D0c"$//' ;;
        */speaker.c) printf '%s' 's/^#define PCM_PLAYBACK "\/dev\/pcmC0D0p"$//' ;; # the playback node, handed to the shared speaker engine (checked separately: once, never opened here)
        *) printf '%s' 's/^$//' ;;
    esac
}
scan_c() {
    local f adj='|\"[[:space:]]*\"'
    for f in "$@"; do
        adj='|\"[[:space:]]*\"'
        case "$f" in */onvif_services.c | */onvif_ptz.c) adj='' ;; esac # the ONVIF services build XML by concatenating string literals (\"" is an attribute quote), never a path
        strip_c_comments "$f" | sed -e 's|"/dev/tymotor"||g' -e 's|\([=(,?:][[:space:]]*\)""|\1 |g' -e 's|/onvif/device_service||g' -e 's|/ver10/device/wsdl||g' -e "$(allow_init_sed "$f")" |
            grep -n -E "$FORBID|0x0*c0[34]|0x[0-9a-fA-F]*c0[34][uU]?\b|\bsystem\(|\bpopen\(|\bexec[lv]$adj" | sed "s|^|$f:|" || true
    done
}
CSRC=()
while IFS= read -r f; do CSRC+=("$f"); done < <(printf '%s\n' "${A2K_APP[@]}" "$FW"/helpers/*/*.c "$FW"/helpers/*/*.h | sort)
check "C sources: found akam_ptz.c" test "${#CSRC[@]}" -ge 1
chits=$(scan_c "${CSRC[@]}")
check "C sources: no forbidden pattern" test -z "$chits"
[ -z "$chits" ] || echo "$chits"
# The shared modules the 2K links but that are not 2K-only (A2K_INFRA, derived from the build): anti-brick scan. They open files and sockets by design
# (api_httpd, http_srv, cfg_file ...), so only the dangerous things are forbidden: flash / mtd / nvram / upgrade / factory words, device nodes other than
# /dev/urandom, process spawning, and the two PTZ motor init request numbers.
INFRA_FORBID='mtdblock|\bmtd[0-9]|flash_erase|flashcp|nvram|fw_setenv|yy_upgrade|YYOS_UPGRADE|rmem_blk|fac_av130|mode=factory|/dev/|\bsystem[[:space:]]*\(|\bpopen[[:space:]]*\(|\bexec[lv][pe]?[[:space:]]*\(|\bfork[[:space:]]*\(|0x0*c0[34][uU]?\b|\bioctl[[:space:]]*\([^,]*,[[:space:]]*(0x|_IO)'
infra_hits() {
    local f
    for f in "$@"; do
        strip_c_comments "$f" | sed -e 's|"/dev/urandom"||g' -e 's|/onvif/device_service||g' -e 's|/ver10/device/wsdl||g' | grep -n -E "$INFRA_FORBID" | sed "s|^|$f:|" || true
    done
}
check "shared modules of the build (derived, ${#A2K_INFRA[@]} files): the derived list is large" test "${#A2K_INFRA[@]}" -ge 40
ihits=$(infra_hits "${A2K_INFRA[@]}")
check "shared modules of the build: no flash/mtd/nvram/upgrade/factory word, no device node but /dev/urandom, no spawn, no PTZ init request" test -z "$ihits"
[ -z "$ihits" ] || echo "$ihits"
check "C sources: akam_ptz.c holds the tymotor literal once" test "$(grep -c '"/dev/tymotor"' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz.c")" = 1
# The PoC microphone tool opens the capture node and nothing else: the one allowed line is the exact
# '#define PCM_CAPTURE "/dev/pcmC0D0c"' in audio/mic.c (read-only open, see the file header). Any other file,
# the playback (p) or loopback (l) node, or a second use of the literal is a hit.
check "mic: the capture node literal is defined once, in helpers/mic/mic.c and the platform audio.c only, and opened O_RDONLY (speaker.c names only the playback node, checked below)" bash -c "test \"\$(grep -rl 'pcmC0D0' '$FW' '$PL' --include=*.c --include=*.h | sed 's|.*/||' | sort | tr '\\n' ' ')\" = 'audio.c mic.c speaker.c ' && test \"\$(grep -c '^#define PCM_CAPTURE \"/dev/pcmC0D0c\"\$' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c')\" = 1 && grep -q 'open(PCM_CAPTURE, O_RDONLY)' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c'"
check "mic: no playback/loopback node, no GPIO, no speaker request in the code (comments ignored)" test -z "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c" | grep -E 'pcmC0D0[pl]|[Gg][Pp][Ii][Oo]|SPEAKER' || true)"
# The app's capture thread (app/audio.c) is the second and last file that may name the node and call ioctl. Narrow
# on purpose: one literal, one open() (read only, on that literal), ioctl only on its own fd with the seven capture request
# macros (the same numbers as the PoC tool, compared below), no playback/loopback node, no GPIO, no speaker request.
AU=$REPO/platforms/anyka-ak3918av130/audio.c
check "audio: audio.c holds the capture literal once, opens it O_RDONLY once, and has no other open()" test "$(grep -c '^#define PCM_CAPTURE "/dev/pcmC0D0c"$' "$AU")" = 1 -a "$(grep -c 'open(PCM_CAPTURE, O_RDONLY)' "$AU")" = 1 -a "$(strip_c_comments "$AU" | grep -c -E '(^|[^_a-zA-Z0-9])open\(')" = 1
check "audio: audio.c has no playback/loopback node, no GPIO, no speaker request (comments ignored)" test -z "$(strip_c_comments "$AU" | grep -E 'pcmC0D0[pl]|[Gg][Pp][Ii][Oo]|SPEAKER' || true)"
check "audio: every ioctl in audio.c is on the capture fd with one of the seven AKPCM_IOC_ macros (8 calls)" test "$(strip_c_comments "$AU" | grep -c 'ioctl(')" = 8 -a "$(strip_c_comments "$AU" | grep -c -E 'ioctl\(fd, AKPCM_IOC_(GET_PARS|SET_RATE|SET_STATE|GET_PERIODS|SET_PERIODS|SET_MODE|RESET_BUF), ')" = 8
# The speaker glue (firmware/<2k>/speaker.c) is the only 2K file that names the PLAYBACK node, once, and it never opens or ioctls it: the shared engine does
# (src/features/audio/audio_out.c, which names no device). No capture node, no loopback node, no GPIO and no sysfs in it (the ak_pcm driver owns the amplifier).
SPK=$REPO/firmware/lsc-rotatable-2k-ak3918av130/speaker.c
check "speaker: speaker.c holds the playback literal once, no capture/loopback node, no GPIO, no open() or ioctl(), no sysfs (comments ignored)" test "$(strip_c_comments "$SPK" | grep -c '^#define PCM_PLAYBACK "/dev/pcmC0D0p"')" = 1 -a "$(strip_c_comments "$SPK" | grep -c 'pcmC0D0')" = 1 -a -z "$(strip_c_comments "$SPK" | grep -E 'pcmC0D0[cl]|[Gg][Pp][Ii][Oo]|/sys/|(^|[^_a-zA-Z0-9])open\(|ioctl\(' || true)"
check "speaker: the shared engine names no device node and the model hands it over (audio_out_setup called by the 2K speaker glue)" test -z "$(strip_c_comments "$REPO/src/features/audio/audio_out.c" | grep '"/dev/' || true)" -a "$(grep -c 'audio_out_setup(PCM_PLAYBACK' "$SPK")" = 1
AKP=$REPO/src/hal/akpcm.h
check "audio: the seven request numbers and struct akpcm_pars (src/hal/akpcm.h, shared with the PoC tool and the 1080p) are the verified ones, defined nowhere else" test "$(grep -E '^#define AKPCM_IOC_(GET_PARS|SET_RATE|SET_STATE|GET_PERIODS|SET_PERIODS|SET_MODE|RESET_BUF) ' "$AKP" | awk '{print $2, $3}' | tr '\n' ';')" = "AKPCM_IOC_GET_PARS 0x401c50e0;AKPCM_IOC_SET_RATE 0x800450ea;AKPCM_IOC_SET_STATE 0x80045010;AKPCM_IOC_SET_PERIODS 0x80045030;AKPCM_IOC_GET_PERIODS 0x40045030;AKPCM_IOC_SET_MODE 0x000050e1;AKPCM_IOC_RESET_BUF 0x000050e3;" -a "$(grep -c 'uint32_t format;' "$AKP")" = 1 -a -z "$(grep -E '^#define (AKPCM_)?IOC_|^struct akpcm_pars' "$AU" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c" || true)" -a "$(grep -c '#include "akpcm.h"' "$AU" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/mic/mic.c" | grep -c ':1')" = 2
check "audio: the capture is opt-in: a2k_audio_start is called once, under 'if (audio_on)', and audio_on defaults to 0" test "$(grep -c 'a2k_audio_start()' "$REPO/platforms/anyka-ak3918av130/main.c")" = 1 -a "$(grep -c 'audio_on = 0;' "$REPO/platforms/anyka-ak3918av130/main.c")" = 1 -a "$(grep -c -E '^    if \(audio_on\) \{' "$REPO/platforms/anyka-ak3918av130/main.c")" = 1
printf '#define PCM_CAPTURE "/dev/pcmC0D0c"\n' >"$WORK/mic_other.c"
check "the C scanner flags the capture node in any file but audio/mic.c (positive control)" test "$(scan_c "$WORK/mic_other.c" | wc -l)" = 1
printf 'int f(void){return open("/dev/mtd0",2);}\nint g(void){return system("nvram set a=1");}\n#define P "/dev/tymotor"\n' >"$WORK/bad.c"
check "the C scanner flags mtd and nvram (positive control)" test "$(scan_c "$WORK/bad.c" | wc -l)" = 2
printf 'int h(void){return open("/dev/ms41929",2);}\n' >"$WORK/bad2.c"
check "the C scanner flags any other /dev path" test "$(scan_c "$WORK/bad2.c" | wc -l)" = 1
printf '*p = open("/dev/mtd0",2);\n/* x */ int a = open("/dev/mtd1",2);\nint b = open("/de" "v/mtd2",2);\n' >"$WORK/bad3.c"
check "the C scanner is not fooled by a leading * or a leading comment, nor by split literals" test "$(scan_c "$WORK/bad3.c" | wc -l)" = 3
mkdir -p "$WORK/ptz"
printf 'int a = 0x40100c03u;\nint b = 0x40280c04u;\n' >"$WORK/ptz/akam_ptz.c"
check "the C scanner allows the exact init request tokens in ptz/akam_ptz.c (positive control)" test "$(scan_c "$WORK/ptz/akam_ptz.c" | wc -l)" = 0
cp "$WORK/ptz/akam_ptz.c" "$WORK/other.c"
check "the C scanner flags the init tokens in any other file" test "$(scan_c "$WORK/other.c" | wc -l)" = 2
printf 'int a = 0x0c03;\nint b = 0x40100c04u;\nint c = 0x401c0c03u;\nint d = 0x40100c03;\n' >"$WORK/ptz/akam_ptz.c"
check "the C scanner still flags every other spelling of 0x0c03/0x0c04, even in akam_ptz.c" test "$(scan_c "$WORK/ptz/akam_ptz.c" | wc -l)" = 4
# The safe motion header is a ptz source but gets NO init request tokens: only akam_ptz.c may spell them. The move
# request is sent from one place only, with a command taken from the 0..7 direction table (cmd 9/10/12 never).
cp "$WORK/ptz/akam_ptz.c" "$WORK/ptz/akam_ptz_safe.h"
printf 'int a = 0x40100c03u;\n' >"$WORK/ptz/akam_ptz_safe.h"
check "the C scanner flags the init tokens in ptz/akam_ptz_safe.h (negative control)" test "$(scan_c "$WORK/ptz/akam_ptz_safe.h" | wc -l)" = 1
printf 'int a = 0x401c0c02u;\nint b = 0x801c0c01u;\nint c = 0x00000c00u;\n' >"$WORK/ptz/akam_ptz_safe.h"
check "the C scanner allows the stop/pos/move request numbers in ptz/akam_ptz_safe.h (positive control)" test "$(scan_c "$WORK/ptz/akam_ptz_safe.h" | wc -l)" = 0
check "ptz sources: akam_ptz_safe.h exists and holds no /dev path" test -f "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" -a "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" | grep -c '/dev/')" = 0
check "ptz sources: the move request is sent from ptzs_send_move only" test "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" | grep -c 'PTZ_REQ_MOVE')" = 2
check "ptz sources: ptzs_send_move refuses any command word above 7 itself" test "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" | grep -c 'if (dir > 7)')" -ge 1
check "ptz sources: no driver command word 9, 10, 12 literal in the move path" test "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" | grep -c -E 'buf\[0\] = (9|10|12)\b')" = 0
printf '/* open("/dev/mtd0")\n * 0x0c03 */\nint x; // system("x")\n' >"$WORK/ok.c"
check "the C scanner ignores comments (block, multi-line, line)" test "$(scan_c "$WORK/ok.c" | wc -l)" = 0

# The own-app PoC sources (app/*.c) are stricter than the rest: our code opens NO device node, flash, nvram, upgrade
# or watchdog path (the vendor libs open their own nodes), and the only literal paths allowed in an fopen are
# /proc/ text files (RSS, free RAM) plus the output file taken from argv. No bare open(), no /dev, no spawning.
APPC=()
while IFS= read -r f; do APPC+=("$f"); done < <(printf '%s\n' "${A2K_APPC[@]}" | sort)
check "app C sources: poc0, poc2, poc4 and the akamd sources present" test "${#APPC[@]}" -ge 8
# Sockets: rtsp_session.c (the RTSP server of PoC-3) is the ONLY app file that may bind, listen and accept; mqtt_bind.c
# (the outbound MQTT client) is the ONLY app file that may connect() (and it may not listen); both may create a socket; no
# app file may use datagram sockets (sendto/recvfrom).
APP_NET=rtsp_session.c
# The local control API server (src/api/api_httpd.c since the 1080p shares it, specs/control-api.md 4.1; not scanned here any more, it is no app file) is the second and last file that may create a socket, bind, listen and accept (one
# listener, only while an API key exists); it may not connect(), and it never uses datagram sockets.
APP_NET2=akam2k_httpd.c
APP_CONNECT=mqtt_bind.c
# The detection state writer (detstate.c) is the ONLY app file that may rename() and unlink(): it publishes the
# /tmp state file through "<path>.tmp" + rename. Its fopen takes the path as an argument (no literal path in the file).
APP_STATE=detstate.c
# The runtime settings store (store.c) is the fourth: it replaces /config/akam/<file> through "<path>.tmp" + rename (the paths come from its callers, no
# literal path in the file), and may unlink its own temp file when a write fails.
APP_STORE=store.c
# The snapshot ring (snap.c) is the second and last: it writes /tmp/akam2k_snap_<slot>.jpg through "<name>.tmp" + rename
# and deletes the evicted slots. Its directory is the fixed /tmp (a test hook can change it, never the card).
APP_RING=snap.c
# The day/night pins (pin.c) are the third and last exception: the ONLY app file that may name the GPIO subsystem and open
# a file by a built path (open() on sysfs value files, kept open for the async-signal-safe emergency release). It is checked
# separately below (exact pin numbers, one literal path, no create, no mmap, no /dev).
APP_PIN=pin.c
APP_FORBID='flash|upgrade|watchdog|gpio|ioctl|nvram|mtd|/dev|reboot|\bsystem|\bpopen|\bexec[lv]|\bfork|\bsendto|\brecvfrom|\bopen[[:space:]]*\(|\bcreat[[:space:]]*\(|\bmmap[[:space:]]*\(|fopen[[:space:]]*\([[:space:]]*"[^"]*"'
APP_AUDIO=audio.c
# The microphone capture thread is the fourth exception: it may call ioctl and open(), and name the capture node (only through
# the exact '#define PCM_CAPTURE "/dev/pcmC0D0c"' line, removed before the scan, and checked separately above).
APP_FORBID_AUDIO='flash|upgrade|watchdog|gpio|nvram|mtd|/dev|reboot|\bsystem|\bpopen|\bexec[lv]|\bfork|\bsendto|\brecvfrom|\bcreat[[:space:]]*\(|\bmmap[[:space:]]*\(|fopen[[:space:]]*\('
APP_FORBID_PIN='flash|upgrade|watchdog|ioctl|nvram|mtd|/dev|reboot|\bsystem|\bpopen|\bexec[lv]|\bfork|\bsendto|\brecvfrom|\bcreat[[:space:]]*\(|\bmmap[[:space:]]*\(|fopen[[:space:]]*\(|O_CREAT|O_TRUNC'
# The hal_light module (docs/porting/lsc-rotatable-2k/hal-light.md). Its files are the portable light logic, the 2K binding,
# the control tool and the generic sysfs driver. They may SAY "gpio" (a descriptor field and enum value of hal_light.h, no
# subsystem access: the pin number never leaves the macros of pin.h and the whitelist of pin.c); everything else of
# APP_FORBID still applies to them. Only the driver (hal_light_sysfs.c) may open() a file, because the LED-class brightness and
# trigger files are named by the descriptor: it gets the pin file's pattern (no fopen, no create, no truncate, no /dev, no ioctl).
APP_FORBID_LIGHT=${APP_FORBID/gpio|/}
APP_LIGHT_DRV=hal_light_sysfs.c
# The hal_ptz module (docs/porting/lsc-rotatable-2k/ptz-findings.md, "hal_ptz"): the ENGINE (hal_ptz.c) reaches the driver only through the
# ops table (the call is spelled ioctl_fn) and spells the two init request tokens (checked by scan_c above); the DRIVER (hal_ptz_drv.c) is the
# only app file that opens the motor node (the path comes from the descriptor, no literal), calls ioctl and replaces its files by temp + rename.
# The node literal "/dev/tymotor" lives in the 2K descriptor hal.c alone.
APP_FORBID_PTZ_ENGINE=${APP_FORBID/ioctl|/}
APP_FORBID_PTZ_DRV='flash|upgrade|watchdog|gpio|nvram|mtd|/dev|reboot|\bsystem|\bpopen|\bexec[lv]|\bfork|\bsendto|\brecvfrom|\bcreat[[:space:]]*\(|\bmmap[[:space:]]*\(|fopen[[:space:]]*\([[:space:]]*"[^"]*"|O_CREAT|O_TRUNC'
app_hits() {
    local f pat
    for f in "$@"; do
        pat=$APP_FORBID
        case "$(basename "$f")" in
            light_logic.c | hal.c | led.c | hal_input.c | hal_input_sysfs.c) pat=$APP_FORBID_LIGHT ;;
            "$APP_LIGHT_DRV") pat=$APP_FORBID_PIN ;;
            hal_ptz.c) pat=$APP_FORBID_PTZ_ENGINE ;;
            hal_ptz_drv.c) pat=$APP_FORBID_PTZ_DRV ;;
        esac
        [ "$(basename "$f")" != "$APP_PIN" ] || pat=$APP_FORBID_PIN
        [ "$(basename "$f")" != "$APP_AUDIO" ] || pat=$APP_FORBID_AUDIO
        strip_c_comments "$f" | { case "$(basename "$f")" in
            "$APP_AUDIO") sed -e 's/^#define PCM_CAPTURE "\/dev\/pcmC0D0c"$//' ;;
            speaker.c) sed -e 's/^#define PCM_PLAYBACK "\/dev\/pcmC0D0p"$//' ;; # the exact playback node line of the speaker glue (see the speaker checks)
            ptz_desc.c) sed -e 's|"/dev/tymotor"||' ;;
            onvif_services.c) sed -e 's|/onvif/device_service||g' -e 's|/ver10/device/wsdl||g' ;; # the fixed ONVIF service paths (the word "dev" of device_service)
            settings.c) sed -e 's/sys\.flash_writes_[a-z]*//g' -e 's/"sys\.flash_writes_"//g' ;; # the /state counter names (read-only rows)
            *) cat ;;
        esac; } | grep -n -E "$pat" |
            grep -v -E 'fopen[[:space:]]*\(file, "r"\)' | sed "s|^|$f:|" || true
        [ "$(basename "$f")" = "$APP_STATE" ] || [ "$(basename "$f")" = vlive.c ] || [ "$(basename "$f")" = isptest.c ] || [ "$(basename "$f")" = "$APP_RING" ] || [ "$(basename "$f")" = "$APP_STORE" ] || [ "$(basename "$f")" = hal_ptz_drv.c ] ||
            strip_c_comments "$f" | grep -n -E '\bunlink|\brename|\bremove[[:space:]]*\(' | sed "s|^|$f:|" || true
        [ "$(basename "$f")" = "$APP_NET" ] || [ "$(basename "$f")" = "$APP_NET2" ] || [ "$(basename "$f")" = "$APP_CONNECT" ] ||
            strip_c_comments "$f" | grep -n -E '\bsocket' | sed "s|^|$f:|" || true
        [ "$(basename "$f")" = "$APP_NET" ] || [ "$(basename "$f")" = "$APP_NET2" ] ||
            strip_c_comments "$f" | grep -n -E '\bbind[[:space:]]*\(|\blisten[[:space:]]*\(|\baccept[[:space:]]*\(' | sed "s|^|$f:|" || true
        [ "$(basename "$f")" = "$APP_CONNECT" ] ||
            strip_c_comments "$f" | grep -n -E '\bconnect[[:space:]]*\(' | sed "s|^|$f:|" || true
    done
}
# The detection sources: one fopen (the temp state file, path from the caller), the model path is the fixed constant under
# /appfs/resource/model/ and is checked at start, the state file is the fixed /tmp one.
DS=$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.c
DT=$REPO/platforms/anyka-ak3918av130/detect.c
check "detstate.c: exactly one fopen, with a variable path (never a literal)" test "$(strip_c_comments "$DS" | grep -c 'fopen(')" = 1 -a "$(strip_c_comments "$DS" | grep -c 'fopen(tmp, "w")')" = 1
check "detect.c: no fopen, no file of its own" test "$(strip_c_comments "$DT" | grep -c 'fopen(')" = 0
check "detection model is a fixed constant under /appfs/resource/model/ and checked before the thread starts" bash -c "grep -q '#define A2K_DET_MODEL \"/appfs/resource/model/' '$REPO/platforms/anyka-ak3918av130/detect.h' && grep -q 'a2k_model_path_ok(A2K_DET_MODEL)' '$DT'"
check "detection state file is the fixed /tmp/akam2k_detect.state" grep -q '#define A2K_DET_STATE_FILE "/tmp/akam2k_detect.state"' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/detstate.h"
check "no socket in the detection sources" test -z "$(strip_c_comments "$DS" | grep -E '\bsocket|\bbind[[:space:]]*\(|\blisten|\baccept|\bconnect[[:space:]]*\(' || true; strip_c_comments "$DT" | grep -E '\bsocket|\bbind[[:space:]]*\(|\blisten|\baccept|\bconnect[[:space:]]*\(' || true)"
check "app C sources: no flash/upgrade/watchdog/gpio/ioctl/open/mmap/unlink/literal-path fopen" test -z "$(app_hits "${APPC[@]}")"
[ -z "$(app_hits "${APPC[@]}")" ] || app_hits "${APPC[@]}"
# hal_ptz: the motor node and the driver requests stay in their few files
HP=$REPO/platforms/anyka-ak3918av130/hal_ptz.c
HD=$REPO/platforms/anyka-ak3918av130/hal_ptz_drv.c
check "ptz: the node literal \"/dev/tymotor\" is in the 2K descriptor (ptz_desc.c) once and nowhere else in app/" test "$(grep -l '"/dev/tymotor"' "${A2K_APPALL[@]}" | sed 's|.*/||' | tr '\n' ' ')" = "ptz_desc.c " -a "$(grep -c '"/dev/tymotor"' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz_desc.c")" = 1
check "ptz: hal_ptz.c reuses the validated safe-motion layer (one include of akam_ptz_safe.h from helpers/ptz) and sends no move request of its own" test "$(grep -c '#include "akam_ptz_safe.h"' "$HP")" = 1 -a "$(strip_c_comments "$HP" | grep -c -E 'PTZ_REQ_MOVE')" = 0
check "ptz: the calibration sends only the directional commands (ptzs_send_move, command words from the descriptor, never a 9 / 10 / 12 literal) and the descriptor refuses a word above 7" test "$(strip_c_comments "$HP" | grep -c -E 'buf\[0\] = (8|9|10|11|12)\b|ptzs_send_move\(x, (8|9|1[0-2])\b')" = 0 -a "$(strip_c_comments "$HP" | grep -c -E 'd->dir\[i\] > 7')" = 1
check "ptz: the two init request tokens are in hal_ptz.c (the engine) and the tool, and in no other app file" test "$(grep -lE '0x40100c03u|0x40280c04u' "${A2K_APPALL[@]}" | sed 's|.*/||' | tr '\n' ' ')" = "hal_ptz.c "
check "ptz: the driver opens exactly one file with open() (the descriptor's node, O_RDWR), renames one temp file onto the target, unlinks only that temp file" test "$(strip_c_comments "$HD" | grep -c -E '(^|[^_a-zA-Z0-9])open\(')" = 1 -a "$(strip_c_comments "$HD" | grep -c 'open(g_dev, O_RDWR)')" = 1 -a "$(strip_c_comments "$HD" | grep -c -E '\brename\(')" = 1 -a "$(strip_c_comments "$HD" | grep -c -E '\bunlink\(tmp\)')" = 2 -a "$(strip_c_comments "$HD" | grep -c -E '\bunlink\(')" = 2
check "ptz: only ptz.c, its switched-off stand-ins (ptz_off.c, scene_off.c for the scene hook type), main.c, rtsp_session.c, mqtt_bind.c, the 2K binding and the button / tracking / zones / motion wiring (button, detect, track, zlive, motion) and the API settings table (settings.c: ptz.* state rows and actions) know the PTZ controller or its entities" test "$(grep -lE 'hal_ptz_|a2k_ptz_|[^_a-z]ptz\.h' "${A2K_APP[@]}" | sed 's|.*/||' | LC_ALL=C sort | tr '\n' ' ')" = "button.c button.h detect.c hal.h hal_ptz.c hal_ptz.h hal_ptz_drv.c main.c motion.c motion.h mqtt_bind.h mqtt_cfg.h onvif_model.c onvif_ptz.c onvif_services.h patrol.c patrol.h patrol_run.c ptz.c ptz.h ptz_desc.c ptz_off.c rtsp_session.c scene.c scene.h scene_off.c settings.c track.c track.h track_run.c track_run.h zlive.h "
check "ptz: the privacy hook is one read in the video path (send_video_frame) and the video path calls nothing else of the controller" test "$(grep -c 'a2k_ptz_' "$REPO/platforms/anyka-ak3918av130/rtsp_session.c")" = 1 -a "$(grep -c 'a2k_ptz_privacy_active()' "$REPO/platforms/anyka-ak3918av130/rtsp_session.c")" = 1
check "ptz: main.c stops the controller after MQTT (no command reaches the motor while it stops) and before the capture" bash -c "m=\$(grep -n '^    a2k_mqtt_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1); p=\$(grep -n '^    a2k_ptz_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1); c=\$(grep -n '^    a2k_cap_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | tail -n1 | cut -d: -f1); test -n \"\$m\" && test \"\$m\" -lt \"\$p\" && test \"\$p\" -lt \"\$c\""
check "ptz: the defaults are calibration auto and privacy off (opt-in), the park is the descriptor's" test "$(grep -c -E 'c->cal = HAL_PTZ_CAL_AUTO;|c->park_pan = c->park_tilt = -1;|c->privacy = 0;' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz.c")" = 3
check "button / tracking: tracking is off by default (track=0), the button thread stops right after MQTT and before the LED and day/night stops (the pin files close there)" bash -c "grep -q 'int ptz_on = 0, track_on = 0' '$REPO/platforms/anyka-ak3918av130/main.c' && m=\$(grep -n '^    a2k_mqtt_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1) && b=\$(grep -n '^    a2k_button_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1) && n=\$(grep -n '^    a2k_night_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1) && test -n \"\$m\" && test -n \"\$b\" && test \"\$m\" -lt \"\$b\" && test \"\$b\" -lt \"\$n\""
check "button / tracking: only pin.c reads the button line, through hal_input; the button file names neither a pin number nor a path" test -z "$(grep -n -E '\\b71\\b|/sys/|/dev/|a2k_pin_' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/button.c" "$REPO/src/features/button/button_logic.c" "$REPO/src/hal/hal_input.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track_run.c" "$REPO/platforms/anyka-ak3918av130/zlive.c" || true)"
check "zones / tracking: the runtime files are the fixed /config/akam/zones and /config/akam/track_areas, and only the detection state writer, the snapshot ring, the settings store and the motor driver rename()" test "$(grep -c -E '#define A2K_STORE_(ZONES|TRACK) A2K_STORE_DIR \"/(zones|track_areas)\"' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.h")" = 2 -a "$(grep -l 'rename(' "${A2K_APPC[@]}" | sed 's|.*/||' | LC_ALL=C sort | tr '\n' ' ')" = "detstate.c hal_ptz_drv.c snap.c store.c "

# akam-sntp (../sntp, not under app/: it is the one tool that may open a datagram socket): no file at all, no
# spawn, no listening socket, no device, settimeofday and the datagram socket calls only in the client file, no ctype.
SNA=$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/sntp/sntp.c
SNALL=("$FW"/helpers/sntp/*.c "$FW"/helpers/sntp/*.h)
check "sntp sources: no spawn, exec, system, ioctl, listening socket, device or flash path" test -z "$(for f in "${SNALL[@]}"; do strip_c_comments "$f" | grep -E '\bsystem|\bpopen|\bexec[lv]|\bfork|\bioctl|\bbind[[:space:]]*\(|\blisten|\baccept|/mtd|nvram|watchdog|gpio|/dev/[a-qs-z]' || true; done)"
check "sntp sources: no file is opened at all (no fopen, open, creat)" test -z "$(for f in "${SNALL[@]}"; do strip_c_comments "$f" | grep -E '\bfopen|\bopen[[:space:]]*\(|\bcreat[[:space:]]*\(' || true; done)"
check "sntp sources: no ctype (the camera libc lacks __ctype_b_loc)" test -z "$(for f in "${SNALL[@]}"; do strip_c_comments "$f" | grep -E 'ctype|\bis(alpha|digit|space|alnum|upper|lower|xdigit|print|punct)\b|\bto(upper|lower)\b' || true; done)"
check "sntp: settimeofday and sockets are used by the client file only, never by the pure core" test -z "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/sntp/sntp_core.c" | grep -E 'settimeofday|socket|gettimeofday|#include <(sys|netdb|arpa|unistd)' || true)"
check "sntp: the clock is stepped through settimeofday only (no stime, no adjtime, no clock_settime)" test -z "$(strip_c_comments "$SNA" | grep -E '\bstime|\badjtime|clock_settime' || true)"
check "sntp: the poc3 Makefile builds it from helpers/sntp and the symcheck covers it" bash -c "grep -q 'SNTP_SRC := \$(MODEL_DIR)/helpers/sntp/sntp.c' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk' && grep -q '^HELPERS := .*sntp:akam-sntp:sntp' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk' && grep -q 'for b in \$(ALL_BINS)' '$REPO/platforms/anyka-ak3918av130/Makefile' && grep -q '^ALL_BINS := \$(APP) \$(HELPER_BINS)' '$REPO/platforms/anyka-ak3918av130/Makefile'"
PN=$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.c
PH=$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.h
check "pins: exactly five pin numbers are defined, 24, 25 (IR-cut), 48 (IR LED), 56 (blue status LED) and 71 (the button, INPUT only); 22 / 49 / 55 are not" test "$(grep -E '^#define A2K_PIN_[A-Z_]+ [0-9]+' "$PH" | sed 's/.* //' | sort -n | tr '\n' ' ')" = "24 25 48 56 71 "
check "pins: 71 is the input pin only (A2K_PIN_BUTTON), the output whitelist does not name it and the input whitelist is that one macro" bash -c "grep -q 'return pin == A2K_PIN_BUTTON;' '$PN' && ! grep -E 'return pin == A2K_PIN_IRCUT_P.*A2K_PIN_BUTTON' '$PN'"
check "pins: the whitelist is those four macros and nothing else" bash -c "strip() { sed -e 's://.*$::' '$PN'; }; test \"\$(strip | grep -c -E 'return pin == A2K_PIN_IRCUT_P \|\| pin == A2K_PIN_IRCUT_M \|\| pin == A2K_PIN_IRLED \|\| pin == A2K_PIN_BLUE;')\" = 1"
check "day/night pins: no other pin number of the stock map (9 10 22 49 50 55) appears as a number in the pin sources" test -z "$({ strip_c_comments "$PN"; strip_c_comments "$PH"; } | grep -E '(^|[^0-9A-Za-z_.])(9|10|22|49|50|55)([^0-9A-Za-z_.]|$)' || true)"
check "day/night pins: the only absolute path in the pin file is /sys/class/gpio (the root, no /dev, no /proc)" test "$(strip_c_comments "$PN" | grep -o -E '"/[^"]*"' | sort -u | tr '\n' ' ')" = '"/sys/class/gpio" '
check "day/night pins: every path is built from the fixed root and a whitelisted pin number (all opens follow the whitelist test)" bash -c "test \"\$(grep -c 'a2k_pin_allowed(pin) ? slot(pin) : -1' '$PN')\" = 2"
check "day/night pins: no pin is ever unexported, the outputs are written ('out' once) and the ONE input is read-only ('in' once, its value file opened O_RDONLY, never written)" test -z "$(strip_c_comments "$PN" | grep -E 'unexport|"high"|"low"' || true)" -a "$(strip_c_comments "$PN" | grep -c '"out"')" = 1 -a "$(strip_c_comments "$PN" | grep -c '"in"')" = 1 -a "$(strip_c_comments "$PN" | grep -c 'O_RDONLY')" = 1
check "day/night: only ircut.c drives the pins (a2k_pin_open / a2k_pin_set appear in the pin file, the ircut file and the generic light driver only)" test "$(for f in "${APPC[@]}"; do [ "$(strip_c_comments "$f" | grep -c -E 'a2k_pin_(open|set)\b')" = 0 ] || basename "$f"; done | LC_ALL=C sort | tr '\n' ' ')" = "hal_light_sysfs.c ircut.c pin.c "
check "day/night: the IR-cut pulse is the stock's 300 ms and both coil pins are released (constants)" bash -c "grep -q '#define A2K_IRCUT_PULSE_MS 300' '$REPO/platforms/anyka-ak3918av130/ircut.h' && grep -q 'A2K_IRCUT_DAY_PIN A2K_PIN_IRCUT_P' '$REPO/platforms/anyka-ak3918av130/ircut.h' && grep -q 'A2K_IRCUT_NIGHT_PIN A2K_PIN_IRCUT_M' '$REPO/platforms/anyka-ak3918av130/ircut.h'"
check "day/night: the crash and timeout handlers release the coil (async-signal-safe), main stops the engine before the capture" bash -c "test \"\$(grep -c 'a2k_pin_emergency_low();' '$REPO/platforms/anyka-ak3918av130/main.c')\" = 2 && a=\$(grep -n '^    a2k_night_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | cut -d: -f1) && c=\$(grep -n '^    a2k_cap_stop();' '$REPO/platforms/anyka-ak3918av130/main.c' | tail -n1 | cut -d: -f1) && test -n \"\$a\" && test \"\$a\" -lt \"\$c\""
check "vendor calls of the day/night and image modules live in cap.c only (no ak_ call in the pin, ircut, image, night files)" test -z "$(grep -l -E '\bak_[a-z_0-9]+[[:space:]]*\(' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/pin.c" "$REPO/platforms/anyka-ak3918av130/ircut.c" "$REPO/platforms/anyka-ak3918av130/image.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/night_ctl.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/night_run.c" || true)"
LL=$REPO/src/features/light/light_logic.c
LD=$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal_light_sysfs.c
LT=$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/led/led.c
LB=$REPO/firmware/lsc-rotatable-2k-ak3918av130/hal.c
check "hal_light: the portable files carry no model prefix and no vendor call (hal_light.h, light_logic.c/.h)" test -f "$REPO/src/hal/hal_light.h" -a -f "$LL" -a -f "$REPO/src/features/light/light_logic.h" -a -z "$(grep -l -E '\bak_[a-z_0-9]+[[:space:]]*\(' "$LL" "$LD" "$LT" "$LB" || true)"
check "hal_light: the pure logic includes no system header but string.h and does no I/O, no clock, no pin call" test "$(strip_c_comments "$LL" | grep -E '^#include' | sort -u | tr '\n' ' ')" = '#include "light_logic.h" #include <string.h> ' -a -z "$(strip_c_comments "$LL" | grep -E '\bopen|\bwrite|\bread[[:space:]]*\(|printf|a2k_pin|clock_gettime|time[[:space:]]*\(' || true)"
check "hal_light: no pin number or sysfs path outside the descriptor binding (logic, driver, tool, interface hold none)" test -z "$({ strip_c_comments "$LL"; strip_c_comments "$LD"; strip_c_comments "$LT"; strip_c_comments "$REPO/src/hal/hal_light.h"; strip_c_comments "$REPO/src/features/light/light_logic.h"; } | grep -E '/sys/|/dev/|(^|[^0-9A-Za-z_.])(9|10|22|24|25|48|49|50|55|56|71)([^0-9A-Za-z_.]|$)' || true)"
check "hal_light: the 2K binding takes its pin numbers from the pin.h macros and holds no IR-cut pin and no literal number" test -z "$(strip_c_comments "$LB" | grep -E '[^A-Za-z_0-9][0-9]{2,}[^A-Za-z_0-9]|IRCUT' || true)"
check "hal_light: the 2K descriptor is ir=48, blue=56, both active high, no exclusion, no white / alarm" bash -c "grep -q '\"ir\", LIGHT_GPIO, A2K_PIN_IRLED, 1' '$LB' && grep -q '\"blue\", LIGHT_GPIO, A2K_PIN_BLUE, 1' '$LB' && ! grep -q -w -E 'WHITE|ALARM|white|alarm' '$LB' && grep -q 'A2K_PIN_BLUE 56' '$PH' && ! grep -q -E 'A2K_PIN_(WHITE|ALARM)' '$PH'"
check "hal_light: the sysfs driver opens write-only, never creates, builds only <led_dir>/trigger and <led_dir>/brightness" test "$(strip_c_comments "$LD" | grep -c 'open(path, O_WRONLY | O_CLOEXEC)')" = 1 -a "$(strip_c_comments "$LD" | grep -o -E '"[^"]*"' | LC_ALL=C sort -u | tr '\n' ' ')" = '"%s/brightness" "%s/trigger" "0" "1" "hal_light.h" "none" "pin.h" ' -a -z "$(strip_c_comments "$LD" | grep -E 'O_CREAT|O_TRUNC|O_RDWR|mmap|ioctl' || true)"
check "akam-led: refuses ir without --force and never names the IR-cut pins or the pin helper" bash -c "grep -q 'strcmp(name, \"ir\") == 0 && !force' '$LT' && ! grep -q -E 'IRCUT|a2k_pin|akam2k_pin' '$LT'"
check "akam-led: blink is bounded (1..60 s, alarm guard) and ends with the light off" bash -c "grep -q 'secs < 1 || secs > 60' '$LT' && grep -q 'alarm((unsigned)secs + 2u)' '$LT' && grep -q 'light_set(&l, name, LIGHT_OFF' '$LT'"
check "akam-led: no spawn, no file of its own, no socket" test -z "$(strip_c_comments "$LT" | grep -E '\bsystem|\bpopen|\bexec[lv]|\bfork|fopen|\bopen[[:space:]]*\(|\bsocket' || true)"
check "akam-led: the poc3 Makefile builds it, the symcheck covers it and BUILDINFO lists its own sha" bash -c "grep -q 'LED_SRC := \$(MODEL_DIR)/helpers/led/led.c' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk' && grep -q '^HELPERS := .*led:akam-led:led' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk' && grep -q 'for b in \$(ALL_BINS)' '$REPO/platforms/anyka-ak3918av130/Makefile' && grep -q '_sha256=' '$REPO/platforms/anyka-ak3918av130/Makefile' && grep -q '_source_sha256=' '$REPO/platforms/anyka-ak3918av130/Makefile' && grep -q 'for h in \$(HELPERS)' '$REPO/platforms/anyka-ak3918av130/Makefile'"
mkdir -p "$WORK/lt"
printf 'int f(void){return open("/sys/class/leds/x/brightness",1);}\n' >"$WORK/lt/light_logic.c"
check "the app C scanner flags open() in the pure light logic (positive control)" test "$(app_hits "$WORK/lt/light_logic.c" | wc -l)" = 1
printf 'int g = 1; int f(void){return open("x",1);}\n' >"$WORK/lt/hal_light_sysfs.c"
check "the app C scanner allows open() in the generic light driver only (positive control)" test "$(app_hits "$WORK/lt/hal_light_sysfs.c" | wc -l)" = 0
printf 'struct e { int gpio; };\n' >"$WORK/lt/hal.c"
check "the app C scanner lets the light files say gpio, but not any other file (positive control)" test "$(app_hits "$WORK/lt/hal.c" | wc -l)" = 0 -a "$(cp "$WORK/lt/hal.c" "$WORK/lt/other.c"; app_hits "$WORK/lt/other.c" | wc -l)" = 1
printf 'int f(void){return ioctl(3,1);}\n' >"$WORK/lt/hal_light_sysfs.c"
check "the app C scanner still flags ioctl in the generic light driver (positive control)" test "$(app_hits "$WORK/lt/hal_light_sysfs.c" | wc -l)" = 1
P2=$REPO/platforms/anyka-ak3918av130/probes/poc2_frame.c
check "poc2_frame: exactly two fopen (the /proc reader and the output file)" test "$(strip_c_comments "$P2" | grep -c 'fopen(')" = 2
check "poc2_frame: output restricted to /mnt/ or /tmp/, rejects .., capped at 8 MiB" bash -c "grep -q 'strncmp(o.out, \"/mnt/\", 5)' '$P2' && grep -q 'strncmp(o.out, \"/tmp/\", 5)' '$P2' && grep -q 'strstr(o.out, \"..\")' '$P2' && grep -q 'MAX_OUT_BYTES (8u' '$P2'"
check "poc2_frame: the alarm guard is armed before run() is called" bash -c "a=\$(grep -n '^    alarm((unsigned)o.secs)' '$P2' | cut -d: -f1); r=\$(grep -n 'rc = run(&o, &s)' '$P2' | cut -d: -f1); test -n \"\$a\" && test -n \"\$r\" && test \"\$a\" -lt \"\$r\""
P4=$REPO/platforms/anyka-ak3918av130/probes/poc4_detect.c
check "poc4_detect: exactly one fopen (the /proc reader), no other file of its own" test "$(strip_c_comments "$P4" | grep -c 'fopen(')" = 1
check "poc4_detect: model path restricted to /appfs/resource/model/ and rejects .." bash -c "grep -q 'strncmp(o.model, MODEL_DIR' '$P4' && grep -q 'strstr(o.model, \"..\")' '$P4' && grep -q '#define MODEL_DIR \"/appfs/resource/model/\"' '$P4'"
check "poc4_detect: the alarm guard is armed before run() is called" bash -c "a=\$(grep -n '^    alarm((unsigned)o.secs)' '$P4' | cut -d: -f1); r=\$(grep -n 'rc = run(&o, &s)' '$P4' | cut -d: -f1); test -n \"\$a\" && test -n \"\$r\" && test \"\$a\" -lt \"\$r\""
ZC=$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c
SN=$REPO/platforms/anyka-ak3918av130/snap.c
check "zones.c: one fopen, read-only, with a variable path; lstat before it (a FIFO is never opened)" test "$(strip_c_comments "$ZC" | grep -c 'fopen(')" = 1 -a "$(strip_c_comments "$ZC" | grep -c 'fopen(path, "r")')" = 1 -a "$(strip_c_comments "$ZC" | grep -c 'lstat(')" = 1
check "zones.c: the zones file is the fixed card path /mnt/sdcard/akam/zones" grep -q '#define A2K_ZONES_DEFAULT "/mnt/sdcard/akam/zones"' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.h"
check "zones.c: no socket, no write, no spawn" test -z "$(strip_c_comments "$ZC" | grep -E '\bsocket|\bconnect[[:space:]]*\(|fopen\([^)]*"[wa]|\bfwrite|\bsystem|\bfork' || true)"
check "snap.c: one fopen, write only, with a variable path (the ring slot), under the fixed /tmp" test "$(strip_c_comments "$SN" | grep -c 'fopen(')" = 1 -a "$(strip_c_comments "$SN" | grep -c 'fopen(tmp, "wb")')" = 1 -a "$(grep -c '#define A2K_SNAP_DIR "/tmp"' "$REPO/platforms/anyka-ak3918av130/snap.h")" = 1
check "snap.c: no socket, no vendor call, no card path" test -z "$(strip_c_comments "$SN" | grep -E '\bsocket|\bconnect[[:space:]]*\(|\bak_[a-z_0-9]+[[:space:]]*\(|/mnt|sdcard' || true)"
check "the JPEG encoder is opened in cap.c only: a third ak_venc_open_ex call, profile Jpeg (7), enc_type 1" test "$(grep -c 'rc = ak_venc_open_ex(' "$REPO/platforms/anyka-ak3918av130/cap.c")" = 2 -a "$(grep -c 'vp->profile = 7;' "$REPO/platforms/anyka-ak3918av130/cap.c")" = 1 -a "$(grep -c 'vp->enc_type = 1;' "$REPO/platforms/anyka-ak3918av130/cap.c")" = 1 -a -z "$(grep -l 'ak_venc_open_ex(' "$REPO/platforms/anyka-ak3918av130/snap.c" "$REPO/platforms/anyka-ak3918av130/detect.c" "$REPO/platforms/anyka-ak3918av130/mqtt_bind.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/zones.c" || true)"
check "JPEG encoder init QP is inside the lib range 10..100 (hardware: init_qp 0 was refused) and min/max QP 1..100" bash -c "grep -q 'vp->init_qp = 50;' '$REPO/platforms/anyka-ak3918av130/cap.c' && grep -q 'vp->min_qp = 1;' '$REPO/platforms/anyka-ak3918av130/cap.c' && grep -q 'vp->max_qp = 100;' '$REPO/platforms/anyka-ak3918av130/cap.c'"
check "snapshot caps: 200 KB per JPEG, 3 files, 600 KB in /tmp; MQTT publishes at most 200 KB" bash -c "grep -q 'A2K_SNAP_MAX_JPEG (200u << 10)' '$REPO/platforms/anyka-ak3918av130/snap.h' && grep -q 'A2K_SNAP_RING 3' '$REPO/platforms/anyka-ak3918av130/snap.h' && grep -q 'A2K_SNAP_TOTAL_MAX (600u << 10)' '$REPO/platforms/anyka-ak3918av130/snap.h' && grep -q 'A2K_MQTT_SNAP_MAX (200u << 10)' '$REPO/platforms/anyka-ak3918av130/mqtt_bind.h'"
printf 'int f(void){rename("a","b");return unlink("a");}\n' >"$WORK/ring_ok.c"
mkdir -p "$WORK/ring" && cp "$WORK/ring_ok.c" "$WORK/ring/$APP_RING"
check "the app C scanner allows rename and unlink in the snapshot ring file (positive control)" test "$(app_hits "$WORK/ring/$APP_RING" | wc -l)" = 0
printf 'int s(void){return socket(2,1,0);}\n' >"$WORK/badapp3.c"
check "the app C scanner flags a socket outside the RTSP file (positive control)" test "$(app_hits "$WORK/badapp3.c" | wc -l)" = 1
mkdir -p "$WORK/net" && cp "$WORK/badapp3.c" "$WORK/net/$APP_NET"
check "the app C scanner allows a socket in the RTSP file" test -z "$(app_hits "$WORK/net/$APP_NET")"
printf 'int c(void){return connect(3,0,0);}\n' >"$WORK/net/rtsp_session.c"
check "the app C scanner flags connect() even in the RTSP file (positive control)" test "$(app_hits "$WORK/net/$APP_NET" | wc -l)" = 1
mkdir -p "$WORK/mq" && printf 'int c(void){int s=socket(2,1,0);return connect(s,0,0);}\n' >"$WORK/mq/$APP_CONNECT"
check "the app C scanner allows socket and connect in the MQTT client file only" test "$(app_hits "$WORK/mq/$APP_CONNECT" | wc -l)" = 0
cp "$WORK/mq/$APP_CONNECT" "$WORK/mq/other.c"
check "the app C scanner flags socket and connect in any other file (positive control)" test "$(app_hits "$WORK/mq/other.c" | wc -l)" = 2
printf 'int l(void){\nbind(3,0,0);\nlisten(3,1);\nreturn accept(3,0,0);}\n' >"$WORK/mq/$APP_CONNECT"
check "the app C scanner flags bind/listen/accept in the MQTT client file (positive control)" test "$(app_hits "$WORK/mq/$APP_CONNECT" | wc -l)" = 3
printf 'int r(void){return recvfrom(3,0,0,0,0,0);}\n' >"$WORK/mq/$APP_CONNECT"
check "the app C scanner flags datagram calls in the MQTT client file (positive control)" test "$(app_hits "$WORK/mq/$APP_CONNECT" | wc -l)" = 1
MQ=$REPO/platforms/anyka-ak3918av130/mqtt_bind.c
MC=$REPO/platforms/anyka-ak3918av130/mqtt_cfg.c
check "connect() appears in mqtt_bind.c and nowhere else in app/" test "$(for f in "${APPC[@]}"; do [ "$(strip_c_comments "$f" | grep -c -E '\bconnect[[:space:]]*\(')" = 0 ] || basename "$f"; done | tr '\n' ' ')" = "mqtt_bind.c "
check "mqtt_bind.c: no file of its own; mqtt_cfg.c: two fopen, read-only, both with a variable path (the shared config / akam/mqtt_pass reader, and the MAC)" test "$(strip_c_comments "$MQ" | grep -c 'fopen(')" = 0 -a "$(strip_c_comments "$MC" | grep -c 'fopen(path, "r")')" = 2 -a "$(strip_c_comments "$MC" | grep -c 'fopen(')" = 2
check "MQTT: no log line or printf mentions the password or the user" test -z "$({ strip_c_comments "$MQ"; strip_c_comments "$MC"; } | grep -E 'a2k_log|printf' | grep -E 'pass|user' | grep -v -E 'password set|login %s|c->pass\[0\]|c->user\[0\]' || true)"
# Changed with the user-file settings of the API (cfg_request): the supervisor now WRITES the files of the settings the API queued (mqtt, mqtt_pass, rtsp.conf, ntp,
# akam.conf), because the app never spawns a process and cannot replace the links of the card-less tree. Its knowledge of MQTT and passwords stays confined to
# that one marked block (tests/rot2k_cfg_test.sh runs it): outside it the supervisor still knows nothing about MQTT, no key, no password, no option passed to the app;
# inside it a password is only read from the request file and written to its file through builtins, never echoed, logged or put in a command line.
OAR=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh
check "the supervisor knows nothing about MQTT outside cfg_request: no key, no password, no option for it" test -z "$(sed '/^# --- cfg_request begin/,/^# --- cfg_request end/d' "$OAR" | grep -i -E 'mqtt|password|pass=' || true)"
# shellcheck disable=SC2016  # the grep patterns are literal
check "cfg_request: a password is never echoed, logged or passed on a command line" test -z "$(sed -n '/^# --- cfg_request begin/,/^# --- cfg_request end/p' "$OAR" | grep -E '(echo|log|printf [^>|]*>&2)[^|>]*\$(c_mw|c_rw|_v|_pw)|\$\(.*\$c_(mw|rw)' | grep -v -E "md5sum|cfg_mqtt_get" || true)"
check "cfg_request: the block is marked once and the call is in the supervisor loop" bash -c "[ \$(grep -c '^# --- cfg_request begin' '$OAR') = 1 ] && grep -q '^        cfg_request\$' '$OAR'"
check "the MQTT password key is read by the app only: main passes the file path, never a secret" bash -c "grep -q 'mqtt-conf' '$REPO/platforms/anyka-ak3918av130/main.c' && ! grep -E 'getopt_long|optarg' '$REPO/platforms/anyka-ak3918av130/mqtt_bind.c' '$REPO/platforms/anyka-ak3918av130/mqtt_cfg.c'"
mkdir -p "$WORK/st" && printf 'int f(void){rename("a","b");return unlink("a");}\n' >"$WORK/st/$APP_STATE"
check "the app C scanner allows rename and unlink in the state writer only" test -z "$(app_hits "$WORK/st/$APP_STATE")" -a "$(app_hits "$WORK/st/$APP_STATE" | wc -l)" = 0
cp "$WORK/st/$APP_STATE" "$WORK/st/other.c"
check "the app C scanner flags rename and unlink in any other app file (positive control)" test "$(app_hits "$WORK/st/other.c" | wc -l)" = 1
printf 'int f(void){return system("x");}\n' >"$WORK/badapp2.c"
check "the app C scanner flags system() (positive control)" test "$(app_hits "$WORK/badapp2.c" | wc -l)" = 1
# the day/night pin file: gpio and open() are its business, nothing else is
mkdir -p "$WORK/pin"
printf 'int f(void){int fd=open("/sys/class/gpio/gpio24/value",1);return fd;}\n' >"$WORK/pin/$APP_PIN"
check "the app C scanner allows the GPIO subsystem and open() in the pin file only (positive control)" test "$(app_hits "$WORK/pin/$APP_PIN" | wc -l)" = 0
cp "$WORK/pin/$APP_PIN" "$WORK/pin/other.c"
check "the app C scanner flags the same text in any other app file (negative control: gpio and open)" test "$(app_hits "$WORK/pin/other.c" | wc -l)" = 1
printf 'int f(void){return open("/dev/mem",2);}\n' >"$WORK/pin/$APP_PIN"
check "the app C scanner flags /dev/mem even in the pin file (negative control)" test "$(app_hits "$WORK/pin/$APP_PIN" | wc -l)" = 1
printf 'int f(void){return open("/x",65);}\nint g(void){return system("x");}\nvoid *h(void){return mmap(0,1,1,1,3,0);}\nint k(void){return creat("/x",1);}\n' >"$WORK/pin/$APP_PIN"
check "the app C scanner flags system, mmap and creat in the pin file (negative controls)" test "$(app_hits "$WORK/pin/$APP_PIN" | wc -l)" = 3
printf 'int f(void){return open("/x",2);}\nint g(void){FILE*h=fopen("/etc/x","r");return h!=0;}\n' >"$WORK/badapp.c"
check "the app C scanner flags a bare open and a literal-path fopen (positive control)" test "$(app_hits "$WORK/badapp.c" | wc -l)" = 2
# Struct sizes and offsets of the transcribed vendor structs are _Static_asserts: compile-check them with the pinned
# cross compiler when it is present (no vendor lib needed for -fsyntax-only).
XCC=$REPO/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1/bin/arm-linux-gcc
if [ -x "$XCC" ]; then
    check "poc2_frame.c compiles clean for ARM (struct size asserts, -Wall -Wextra -Werror)" "$XCC" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -march=armv5te -mfloat-abi=soft "$REPO/platforms/anyka-ak3918av130/probes/poc2_frame.c"
    check "poc4_detect.c compiles clean for ARM (struct size asserts, -Wall -Wextra -Werror)" "$XCC" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -march=armv5te -mfloat-abi=soft "$REPO/platforms/anyka-ak3918av130/probes/poc4_detect.c"
    check "poc0_versions.c compiles clean for ARM" "$XCC" -fsyntax-only -Wall -Wextra -Werror -march=armv5te -mfloat-abi=soft "$REPO/platforms/anyka-ak3918av130/probes/poc0_versions.c"
    # PoC-3 (akamd): the shared headers come from platforms/anyka-ak3918, the RTSP file also needs SmolRTSP (a fetched,
    # untracked dependency: skipped when it is not there).
    for f in main cap stream detect detstate mqtt_bind mqtt_cfg zones snap pin ircut image night_ctl night_run; do
 check "$f.c compiles clean for ARM" "$XCC" -std=gnu99 -fsyntax-only -Wall -Wextra -Werror -march=armv5te -mfloat-abi=soft "${INC_2K[@]}" "${SHARED_INC[@]}" "$(a2k_src "$f")"
    done
    SMOL=${SMOLRTSP:-$REPO/third_party/smolrtsp}
    if [ -d "$SMOL/build/_deps" ]; then
 check "rtsp_session.c compiles clean for ARM" "$XCC" -std=gnu99 -fsyntax-only -Wall -Wextra -Werror -march=armv5te -mfloat-abi=soft "${INC_2K[@]}" "${SHARED_INC[@]}" -I"$SMOL/include" -I"$SMOL/build/_deps/slice99-src" -I"$SMOL/build/_deps/datatype99-src" -I"$SMOL/build/_deps/interface99-src" -I"$SMOL/build/_deps/metalang99-src/include" -DSMOLRTSP_WITH_H264 "$REPO/platforms/anyka-ak3918av130/rtsp_session.c"
    else
        echo "  skip - no SmolRTSP checkout: ARM syntax check of rtsp_session.c"
    fi
else
    echo "  skip - no pinned toolchain: ARM syntax check of the app sources"
fi

echo "== akam_ptz: unit test and smoke run"
if command -v cc >/dev/null 2>&1; then
    check "akam_ptz unit test builds" cc -Wall -Wextra -Werror -o "$WORK/ptz_unit" "$REPO/tests/rot2k_ptz_unit.c"
    "$WORK/ptz_unit" >"$WORK/ptz_unit.out" 2>&1 || true
    check "akam_ptz unit test passes" grep -q 'rot2k ptz unit: 0 failed' "$WORK/ptz_unit.out"
    if cc -Wall -Wextra -Werror -o "$WORK/akam_ptz_host" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz.c"; then
        check "akam_ptz: usage error exits 2" bash -c "\"$WORK/akam_ptz_host\" move 9 100 2>/dev/null; test \$? = 2"
        check "akam_ptz: raw is refused (exit 2)" bash -c "\"$WORK/akam_ptz_host\" raw 2>/dev/null; test \$? = 2"
        # No motor node on the host: the open fails cleanly (exit 1), nothing is sent.
        if [ ! -e /dev/tymotor ]; then
            check "akam_ptz: no device on the host -> clean open failure (exit 1)" bash -c "\"$WORK/akam_ptz_host\" pos 2>/dev/null; test \$? = 1"
        fi
    else
        ko "akam_ptz does not compile on the host"
    fi
else
    echo "  skip - no host C compiler: akam_ptz unit test"
fi

echo "== hal_ptz + PTZ controller: unit test (fake driver: calibration, saved position, park/home, soft limits, stall, cancel, thread)"
if command -v cc >/dev/null 2>&1; then
    HP_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe2" - 2>/dev/null; then HP_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # HP_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $HP_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/hal_ptz_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_hal_ptz_unit.c" "$REPO/platforms/anyka-ak3918av130/hal_ptz.c" "$REPO/platforms/anyka-ak3918av130/hal_ptz_drv.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz.c" "$REPO/src/features/ptz/ptz_lease.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz_desc.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol.c" "$REPO/src/features/ptz/ptz_patrol.c"; then
        "$WORK/hal_ptz_unit" >"$WORK/hal_ptz_unit.out" 2>&1 && hal_ptz_unit_rc=0 || hal_ptz_unit_rc=$?
        check "hal_ptz unit test passes" grep -q 'rot2k hal_ptz unit: 0 failed' "$WORK/hal_ptz_unit.out"
        grep 'FAIL' "$WORK/hal_ptz_unit.out" || true
        if ! grep -q 'rot2k hal_ptz unit: 0 failed' "$WORK/hal_ptz_unit.out"; then
            echo "  hal_ptz_unit: no success summary, exit code ${hal_ptz_unit_rc}; last 40 lines of its output:"
            tail -n 40 "$WORK/hal_ptz_unit.out" | sed 's/^/    | /'
        fi
 if cc -Wall -Wextra -Werror -std=gnu99 -g $HP_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/ptz_cont_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_ptz_cont_unit.c" "$REPO/platforms/anyka-ak3918av130/hal_ptz.c" "$REPO/platforms/anyka-ak3918av130/hal_ptz_drv.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz.c" "$REPO/src/features/ptz/ptz_lease.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz_desc.c"; then
            "$WORK/ptz_cont_unit" >"$WORK/ptz_cont_unit.out" 2>&1 && ptz_cont_unit_rc=0 || ptz_cont_unit_rc=$?
            check "continuous motion unit test passes (safe layer + HAL session + controller thread on a fake driver: lease, retarget without a stop, soft range per axis, stall / wrong way / uncommanded axis, cancel, manual preemption, position history)" grep -q 'rot2k ptz cont unit: 0 failed' "$WORK/ptz_cont_unit.out"
            grep 'FAIL' "$WORK/ptz_cont_unit.out" || true
            if ! grep -q 'rot2k ptz cont unit: 0 failed' "$WORK/ptz_cont_unit.out"; then
                echo "  ptz_cont_unit: no success summary, exit code ${ptz_cont_unit_rc}; last 40 lines of its output:"
                tail -n 40 "$WORK/ptz_cont_unit.out" | sed 's/^/    | /'
            fi
        else
            ko "continuous motion unit test does not compile"
        fi
    else
        ko "hal_ptz unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: hal_ptz unit test"
fi

echo "== ONVIF (device, media, PTZ, WS-Discovery): the real stack on loopback with a fake motor, python stdlib client (Digest, golden XML, mapping, lease, refusals)"
if command -v cc >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    OV_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_ov" - 2>/dev/null; then OV_SAN="-fsanitize=address,undefined"; fi
    # shellcheck disable=SC2086  # OV_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $OV_SAN -pthread -DONVIF_SCRATCH=2048 '-DWSD_INSTANCE_FILE="/tmp/akam2k_wsd_instance_test"' "${INC_2K[@]}" "${SHARED_INC[@]}" \
        -o "$WORK/onvif_server" "$REPO/tests/rot2k_onvif_server.c" "$REPO/platforms/anyka-ak3918av130/onvif_services.c" "$REPO/platforms/anyka-ak3918av130/onvif_ptz.c" "$REPO/src/net/onvif/onvif_common.c" "$REPO/src/net/onvif/http_srv.c" "$REPO/src/net/onvif/soap.c" "$REPO/src/net/onvif/onvif.c" "$REPO/src/net/onvif/wsd.c" "$REPO/src/net/crypto/cred.c" \
        "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/crypto/md5.c" "$REPO/src/net/crypto/sha1.c" "$REPO/src/net/crypto/b64.c" "$REPO/src/features/ptz/ptz_vel.c" "$REPO/src/features/ptz/ptz_lease.c"; then
        python3 -I "$REPO/tests/rot2k_onvif_test.py" "$WORK/onvif_server" >"$WORK/onvif_test.out" 2>&1 || true
        check "ONVIF conformance smoke passes (pre-auth list, Digest, golden XML of every operation, ContinuousMove mapping table, Timeout clamp, Stop, home, presets, refusals, WS-Discovery)" grep -q 'rot2k onvif test: 0 failed' "$WORK/onvif_test.out"
        grep 'FAIL' "$WORK/onvif_test.out" || true
    else
        ko "ONVIF test harness does not compile"
    fi
else
    echo "  skip - no host C compiler or python3: ONVIF conformance smoke"
fi
check "ONVIF: off by default (onvif=0), opt-in key onvif / onvif_port in akam.conf only, the service never starts without the stream password" bash -c "grep -q 'onvif_on = 0' '$REPO/platforms/anyka-ak3918av130/main.c' && grep -q 'no_password' '$REPO/platforms/anyka-ak3918av130/onvif_services.c' && grep -q 'onvif) lo=0 hi=1 opt=--onvif' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh'"
check "ONVIF: the 2K ONVIF files write no file (zero flash writes) and open no device" test "$(strip_c_comments "$REPO/platforms/anyka-ak3918av130/onvif_services.c" | grep -c -E 'fopen|open\(|rename\(|fwrite|write\(')" = 0 -a "$(strip_c_comments "$REPO/platforms/anyka-ak3918av130/onvif_ptz.c" | grep -c -E 'fopen|open\(|rename\(|fwrite|write\(')" = 0

echo "== PTZ patrol (specs/ptz-patrol.md): pure engine unit test + fuzz, binding core on a fake motor, the real tracker core, flash policy"
if command -v cc >/dev/null 2>&1; then
    PP_SAN=""
    if echo 'int main(void){return 0;}' | cc -x c -fsanitize=address,undefined -o "$WORK/san_probe_pp" - 2>/dev/null; then PP_SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"; fi
    # shellcheck disable=SC2086  # PP_SAN is empty or flags
    if cc -Wall -Wextra -Werror -std=gnu99 -g -O1 $PP_SAN -o "$WORK/ptz_patrol_unit" "$REPO/tests/rot2k_ptz_patrol_unit.c" "$REPO/src/features/ptz/ptz_patrol.c"; then
        "$WORK/ptz_patrol_unit" >"$WORK/ptz_patrol_unit.out" 2>&1 || true
        check "ptz_patrol unit test passes (model and limits, sweep / raster expansion, text and JSON forms, fuzz, run sequences with a simulated motor: dwell timing, loop / pingpong / once, preemption and resume, stop conditions, failed moves, clamping)" grep -q 'rot2k ptz_patrol unit: 0 failed' "$WORK/ptz_patrol_unit.out"
        grep 'FAIL' "$WORK/ptz_patrol_unit.out" || true
    else
        ko "ptz_patrol unit test does not compile"
    fi
    # shellcheck disable=SC2086
 if cc -Wall -Wextra -Werror -std=gnu99 -g -O1 $PP_SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/patrol_wire_unit" "$REPO/tests/rot2k_patrol_wire_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol.c" "$REPO/src/features/ptz/ptz_patrol.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/track.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c"; then
        "$WORK/patrol_wire_unit" >"$WORK/patrol_wire_unit.out" 2>&1 || true
        check "patrol wiring unit test passes (off by default, one flash write per config change and none while patrolling, write limiter, priorities, tracker pause / resume with the real tracker core, idle trigger, thread)" grep -q 'rot2k patrol wire unit: 0 failed' "$WORK/patrol_wire_unit.out"
        grep 'FAIL' "$WORK/patrol_wire_unit.out" || true
    else
        ko "patrol wiring unit test does not compile"
    fi
    check "ptz_patrol.c is pure: no hardware, driver, file, thread or clock call" bash -c "! grep -En 'hal_ptz|a2k_ptz|akam2k_|pthread|fopen|open\\(|ioctl|clock_gettime|time\\(' '$REPO/src/features/ptz/ptz_patrol.c' '$REPO/src/features/ptz/ptz_patrol.h' | grep -v '^[^:]*:[0-9]*:.*\\*'"
    check "the patrol never calls a driver: its core talks to the motor only through the ops table (no hal_ptz_ or ioctl in patrol.c)" bash -c "! grep -En 'hal_ptz_|ioctl|a2k_ptz_(command|nudge|goto|patrol_goto)' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol.c'"
    check "the patrol writes its config only through the store (no fopen / rename in the patrol files)" bash -c "! grep -En 'fopen|rename\\(|fwrite|open\\(' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol.c' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol_run.c' '$REPO/src/features/ptz/ptz_patrol.c'"
else
    echo "  skip - no host C compiler: patrol unit tests"
fi

echo "== RAM log ring: unit test (rotation, size cap, segment headers, order across an app restart)"
if cc -Wall -Wextra -Werror -std=gnu99 -g -o "$WORK/ramlog_unit" "$REPO/tests/rot2k_ramlog_unit.c"; then
    TMPDIR="$WORK" "$WORK/ramlog_unit" >"$WORK/ramlog_unit.out" 2>&1 || true
    check "RAM log ring unit test passes (rotation, cap, the order of the two segments survives an app restart)" grep -q 'rot2k ramlog unit: 0 failed' "$WORK/ramlog_unit.out"
    grep 'FAIL' "$WORK/ramlog_unit.out" || true
else
    ko "RAM log ring unit test does not compile"
fi

echo "== scene check: unit test (synthetic scenes: identical, shifted, rotated, other scene, uniform, lighting; reference file; boot decision)"
if command -v cc >/dev/null 2>&1; then
    # shellcheck disable=SC2086  # HP_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $HP_SAN "${INC_2K[@]}" "${SHARED_INC[@]}" -DFIXTURE_DIR="\"$REPO/tests/fixtures/scene\"" -o "$WORK/scene_unit" "$REPO/tests/rot2k_scene_unit.c" "$REPO/src/features/scene/scene_check.c" -lm; then
        "$WORK/scene_unit" >"$WORK/scene_unit.out" 2>&1 || true
        check "scene check unit test passes" grep -q 'rot2k scene unit: 0 failed' "$WORK/scene_unit.out"
        grep 'FAIL' "$WORK/scene_unit.out" || true
    else
        ko "scene check unit test does not compile"
    fi
else
    echo "  skip - no host C compiler: scene check unit test"
fi
if command -v cc >/dev/null 2>&1; then
    # shellcheck disable=SC2086  # HP_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $HP_SAN -pthread -DA2K_SCENE_TEST "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/scene_glue_unit" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/tests/rot2k_scene_glue_unit.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/scene.c" "$REPO/src/features/scene/scene_check.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" "$REPO/src/features/track/drift_match.c"; then
        "$WORK/scene_glue_unit" >"$WORK/scene_glue_unit.out" 2>&1 || true
        check "scene runtime unit test passes (two-step write, view changed during the window, boots counter, invalidation)" grep -q 'rot2k scene glue unit: 0 failed' "$WORK/scene_glue_unit.out"
        grep 'FAIL' "$WORK/scene_glue_unit.out" || true
    else
        ko "scene runtime unit test does not compile"
    fi
fi
if command -v cc >/dev/null 2>&1; then
    # shellcheck disable=SC2086  # HP_SAN is empty or one flag
 if cc -Wall -Wextra -Werror -std=gnu99 -g $HP_SAN "${SHARED_INC[@]}" -DDRIFT_CORPUS_DIR="\"$REPO/tests/fixtures/drift\"" -o "$WORK/drift_unit" "$REPO/tests/rot2k_drift_unit.c" "$REPO/src/features/track/drift_match.c" -lm; then
        "$WORK/drift_unit" >"$WORK/drift_unit.out" 2>&1 || true
        check "drift matcher unit test passes (synthetic shifts, day against night, a person, refusals; the real 2K frames when the corpus is present)" grep -q ' checks, 0 failed' "$WORK/drift_unit.out"
        grep 'FAIL' "$WORK/drift_unit.out" || true
    else
        ko "drift matcher unit test does not compile"
    fi
fi
check "drift matcher: pure (no I/O, no clock, no libm)" test -z "$(strip_c_comments "$REPO/src/features/track/drift_match.c" | grep -E 'fopen|open\(|ioctl|socket|clock|time\(|#include <math.h>' || true)"
check "drift: ptz_drift_ref, ptz_drift_correct and ptz_boot_calibrate are akam.conf keys passed to the app, validated" bash -c "grep -q 'ptz_drift_ref) lo=0 hi=1 opt=--ptz-drift-ref' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'ptz_drift_correct:off | ptz_drift_correct:adjust | ptz_drift_correct:full' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'ptz_boot_calibrate:auto | ptz_boot_calibrate:always' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'ptz-drift-correct' '$REPO/platforms/anyka-ak3918av130/main.c' && grep -q 'ptz-boot-calibrate' '$REPO/platforms/anyka-ak3918av130/main.c'"
check "tilt re-home: ptz_tilt_rehome (off|idle) and ptz_tilt_rehome_gap_min (1..1440) are akam.conf keys passed to the app, validated" bash -c "grep -q 'ptz_tilt_rehome:off | ptz_tilt_rehome:idle' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'ptz_tilt_rehome_gap_min) lo=1 hi=1440 opt=--ptz-tilt-rehome-gap-min' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'ptz-tilt-rehome' '$REPO/platforms/anyka-ak3918av130/main.c'"
check "scene: the dump dir is fixed (/tmp/akam_scene_dbg) in a production build: the test-only override is under #ifdef A2K_SCENE_TEST" bash -c "grep -B1 'g_dbg_dir, sizeof g_dbg_dir, \"%s.dbg\"' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/scene.c' | grep -q '#ifdef A2K_SCENE_TEST'"
check "scene: the comparison module is pure (no hardware, no file or network call) and needs no libm" test -z "$(strip_c_comments "$REPO/src/features/scene/scene_check.c" | grep -E 'fopen|open\(|ioctl|socket|rename|unlink|#include <math.h>' || true)"
check "scene: the 2K enables the person-tracking pan inversion by default (measured on the camera) and keys exist for both axes" bash -c "grep -q 'track_inv_pan = 1' '$REPO/platforms/anyka-ak3918av130/main.c' && grep -q 'track_inv_tilt = 0' '$REPO/platforms/anyka-ak3918av130/main.c' && grep -q 'track_invert_pan) lo=0 hi=1 opt=--track-invert-pan' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q 'track_invert_tilt) lo=0 hi=1 opt=--track-invert-tilt' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh'"
check "scene: ptz_scene_check and ptz_scene_min are akam.conf keys passed to the app" bash -c "grep -q 'ptz_scene_check) lo=0 hi=1 opt=--ptz-scene-check' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh' && grep -q -- '--ptz-scene-min' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh'"
check "park: the code default is 1275:80 (the closed pose, below the soft range)" bash -c "grep -q '1275, 1300, 1275, 80, 4000' '$REPO/firmware/lsc-rotatable-2k-ak3918av130/ptz_desc.c'"

echo "== net settings (akam_net.sh: hostname, Wi-Fi country/band, dead-man; stub integration; registry rows)"
NS=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/akam_net.sh
HS=$REPO/src/scripts/akam_hostname.sh
check "net: akam_hostname.sh is the shared template, reused unchanged (not copied into the firmware tree)" test ! -e "$FW/boot/card/akam/akam_hostname.sh"
check "net: akam_net.sh and the stub parse" bash -c "sh -n '$NS' && sh -n '$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_stub.sh'"
# new_net <name>: a fake camera: fake wpa_supplicant/udhcpc/hostname/ip driven by files in $N/tmp.
#   wpa_supplicant: starts a sleeping daemon (pidfile from -P), "associates" unless $N/ap24only exists and the conf has the 5 GHz
#                   global freq_list, or $N/failstart_5 exists and the conf has it (refuses to start)
#   udhcpc: records its arguments, a daemon too; the lease ($N/up) exists only when associated; "ip addr flush" drops it
new_net() {
    N=$WORK/$1
    mkdir -p "$N/tmp" "$N/config/akam" "$N/bin" "$N/dm" "$N/sys/class/net/wlan0"
    # daemons named like the real ones (the script checks /proc/PID/cmdline before it signals a pid)
    for dn in wpa_supplicant udhcpc other; do cp -L "$(command -v sleep)" "$N/dm/$dn"; done
    # the watchdog feeder is `busybox watchdog -F -t 5 -T 20 /dev/watchdog`: "watchdog" is argv[1], not the program name
    printf '#!/bin/sh\ntrap "exit 0" TERM\nsleep 300 &\nwait\n' >"$N/dm/busybox"
    chmod 755 "$N/dm/busybox"
    echo 1 >"$N/sys/class/net/wlan0/carrier"
    printf 'ctrl_interface=/tmp/wpa_supplicant\nnetwork={\n ssid="x"\n psk="%s"\n}\n' "$SECRET" >"$N/config/akam/wpa.conf"
    cat >"$N/bin/wpa_supplicant" <<'EOS'
#!/bin/sh
# models wpa_supplicant 2.10 (strings + key tables of the camera binary: global country and freq_list, network scan_freq/freq_list)
conf= pf=
while [ $# -gt 0 ]; do
    case $1 in -c) conf=$2; shift ;; -P) pf=$2; shift ;; esac
    shift
done
g5=$(grep -c '^freq_list=.*5180' "$conf")
fl=$(grep -c '^[ 	]*scan_freq=' "$conf") # keys this model refuses: none are ever rendered (global country + freq_list only)
echo "wpa_supplicant conf=$(grep -c . "$conf") 5g=$g5 fl=$fl" >>"$AKAM_ROOT/tmp/calls.log"
if [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/failstart_5" ]; then exit 1; fi
rm -f "$AKAM_ROOT/assoc"; [ -e "$AKAM_ROOT/flush_broken" ] || rm -f "$AKAM_ROOT/up"
echo 0 >"$AKAM_ROOT/sys/class/net/wlan0/carrier"
if [ "$fl" != 0 ] || [ -e "$AKAM_ROOT/dies_all" ] || { [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/dies_5" ]; }; then
    # daemonizes, then dies while parsing: the parent still returns 0
    ( : ) &
    echo $! >"$pf"
    wait
    exit 0
fi
if [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/ap24only" ]; then :; elif [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/dies_late_5" ]; then :; else touch "$AKAM_ROOT/assoc"; echo 1 >"$AKAM_ROOT/sys/class/net/wlan0/carrier"; fi
"$AKAM_ROOT/dm/wpa_supplicant" 300 >/dev/null 2>&1 &
sp=$!
echo $sp >"$pf"
if [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/dies_late_5" ]; then
    ( sleep 2; kill $sp 2>/dev/null ) >/dev/null 2>&1 &
fi
EOS
    cat >"$N/bin/udhcpc" <<'EOS'
#!/bin/sh
pf=
for a in "$@"; do [ "${prev:-}" = -p ] && pf=$a; prev=$a; done
echo "udhcpc $*" >>"$AKAM_ROOT/tmp/calls.log"
[ ! -e "$AKAM_ROOT/udhcpc_fail" ] || exit 1
if [ -e "$AKAM_ROOT/udhcpc_fail_name" ] && echo "$*" | grep -q "hostname:$(cat "$AKAM_ROOT/udhcpc_fail_name")\$"; then exit 1; fi
[ ! -e "$AKAM_ROOT/assoc" ] || touch "$AKAM_ROOT/up"
"$AKAM_ROOT/dm/udhcpc" 300 >/dev/null 2>&1 &
echo $! >"$pf"
EOS
    cat >"$N/bin/hostname" <<'EOS'
#!/bin/sh
if [ $# -eq 0 ]; then cat "$AKAM_ROOT/tmp/khost" 2>/dev/null; else echo "$1" >"$AKAM_ROOT/tmp/khost"; fi
EOS
    cat >"$N/bin/ip" <<'EOS'
#!/bin/sh
echo "ip $*" >>"$AKAM_ROOT/tmp/ip.log"
case "$1 $2" in
    "addr show") [ ! -e "$AKAM_ROOT/up" ] || echo "    inet 192.0.2.9/24 scope global wlan0" ;;
    "route show") [ ! -e "$AKAM_ROOT/up" ] || echo "192.0.2.0/24 dev wlan0 scope link" ;;
    "addr flush") [ -e "$AKAM_ROOT/flush_broken" ] || rm -f "$AKAM_ROOT/up" ;;
    "addr del") rm -f "$AKAM_ROOT/up" ;;
esac
exit 0
EOS
    cat >"$N/bin/pidof" <<'EOS'
#!/bin/sh
# only the fake daemons of this camera (never a real process of the host)
pgrep -f "^$AKAM_ROOT/dm/$1 " | tr '\n' ' '
echo
EOS
    cat >"$N/bin/reboot" <<'EOS'
#!/bin/sh
echo reboot >>"$AKAM_ROOT/reboot.log"
EOS
    chmod 755 "$N/bin/wpa_supplicant" "$N/bin/udhcpc" "$N/bin/hostname" "$N/bin/ip" "$N/bin/pidof" "$N/bin/reboot"
    # first run: associated and leased, as after boot
    touch "$N/assoc" "$N/up"
    "$N/dm/wpa_supplicant" 300 >/dev/null 2>&1 &
    echo $! >"$N/tmp/wpa.pid"
    "$N/dm/udhcpc" 300 >/dev/null 2>&1 &
    echo $! >"$N/tmp/udhcpc.pid"
    OLDW=$(cat "$N/tmp/wpa.pid")
    OLDD=$(cat "$N/tmp/udhcpc.pid")
    export AKAM_ROOT=$N AKAM_NET_CFG=$N/config/akam AKAM_TMP_DIR=$N/tmp AKAM_WPAS=$N/bin/wpa_supplicant AKAM_IP=$N/bin/ip \
        AKAM_HOSTNAME_CMD=$N/bin/hostname AKAM_HOSTNAME_SH=$HS AKAM_NET_DEADMAN=2 AKAM_NET_POLL=1 AKAM_NET_STOP_WAIT=2 AKAM_NET_ALIVE_WAIT=1 AKAM_NET_FG=1 AKAM_SH="${SHCMD[*]}" \
        AKAM_PIDOF=$N/bin/pidof AKAM_NET_REBOOT_CMD=$N/bin/reboot AKAM_NET_FEEDER_PID=$N/tmp/akam_feeder.pid
    export PATH=$N/bin:$ORIG_PATH
}
net() { "${SHCMD[@]}" "$NS" "$@"; }
net_rc() { # net_rc <args>: exit status of akam_net.sh, output in $WORK/net.out
    "${SHCMD[@]}" "$NS" "$@" >"$WORK/net.out" 2>&1
    echo $?
}

new_net net1
USERCONF=$(cat "$N/config/akam/wpa.conf")
check "net hostname: default is akam-rot2k" test "$(net hostname show)" = akam-rot2k
check "net hostname: set validates, lowercases, stores, sets the kernel name" bash -c "[ \"\$($(printf '%q ' "${SHCMD[@]}") '$NS' hostname set My-Cam)\" = my-cam ] && [ \"\$(cat '$N/config/akam/hostname')\" = my-cam ] && [ \"\$(cat '$N/tmp/khost')\" = my-cam ]"
check "net hostname: DHCP restarted with the new name (the old udhcpc is stopped)" bash -c "grep -q 'udhcpc -b -p $N/tmp/udhcpc.pid -i wlan0 -x hostname:my-cam' '$N/tmp/calls.log' && ! kill -0 $OLDD 2>/dev/null"
check "net hostname: show returns the stored name" test "$(net hostname show)" = my-cam
for bad in -bad bad- 12345 a_b a.b "a b" abcdefghijklmnopqrstuvwxyz0123456 ""; do
    check "net hostname: '$bad' refused (status 2), nothing changed" test "$(net_rc hostname set "$bad")" = 2 -a "$(cat "$N/config/akam/hostname")" = my-cam -a "$(cat "$N/tmp/khost")" = my-cam
done
check "net hostname: a bad stored name is ignored (default used)" bash -c "echo 'bad_name' >'$N/config/akam/hostname'; [ \"\$($(printf '%q ' "${SHCMD[@]}") '$NS' hostname show)\" = akam-rot2k ]"
echo my-cam >"$N/config/akam/hostname"
echo wrong >"$N/tmp/khost"
check "net hostname: apply (boot) sets the kernel name and prints it" test "$(net hostname apply)" = my-cam -a "$(cat "$N/tmp/khost")" = my-cam
check "net hostname: off forgets the name, back to akam-rot2k at once, DHCP renewed" bash -c "[ \"\$($(printf '%q ' "${SHCMD[@]}") '$NS' hostname off)\" = akam-rot2k ] && [ ! -e '$N/config/akam/hostname' ] && [ \"\$(cat '$N/tmp/khost')\" = akam-rot2k ] && tail -n1 '$N/tmp/calls.log' | grep -q 'hostname:akam-rot2k'"
check "net hostname: usage error is status 2" test "$(net_rc hostname)" = 2 -a "$(net_rc wifi nope)" = 2 -a "$(net_rc)" = 2

net_alive() { kill -0 "$(cat "$N/tmp/wpa.pid")" 2>/dev/null && echo y; }
same_stock() { cmp -s "$N/tmp/wpa.conf" "$N/config/akam/wpa.conf" && echo y; }
# --- hostname edge cases
check "net hostname: an empty stored file means the default" bash -c ": >'$N/config/akam/hostname'; [ \"\$($(printf '%q ' "${SHCMD[@]}") '$NS' hostname show)\" = akam-rot2k ]"
rm -f "$N/config/akam/hostname"
: >"$N/tmp/calls.log"
net hostname set same-cam >/dev/null
: >"$N/tmp/calls.log"
check "net hostname: setting the same name again does not restart DHCP" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' hostname set Same-Cam >/dev/null && [ ! -s '$N/tmp/calls.log' ]"
touch "$N/udhcpc_fail"
check "net hostname: no udhcpc starts at all (new or old name): status 1, the name is stored, LAST RESORT reset requested (no feeder: reboot)" test "$(net_rc hostname set other-cam)" = 1 -a "$(cat "$N/config/akam/hostname")" = other-cam -a "$(grep -c reboot "$N/reboot.log")" = 1
rm -f "$N/udhcpc_fail" "$N/reboot.log"
echo other2 >"$N/udhcpc_fail_name"
: >"$N/tmp/calls.log"
check "net hostname: udhcpc refuses the new name: restarted with the PREVIOUS name (status 1, no reset), a DHCP client keeps running" test "$(net_rc hostname set other2)" = 1 -a ! -e "$N/reboot.log" -a "$(tail -n1 "$N/tmp/calls.log" | grep -c 'hostname:other-cam$')" = 1 -a "$(grep -c 'hostname:other2$' "$N/tmp/calls.log")" = 3
rm -f "$N/udhcpc_fail_name"
check "net hostname: set and off ignore SIGHUP (the trap is set before udhcpc is restarted)" test "$(grep -c "trap '' HUP" "$NS")" -ge 3

mkdir -p "$N/tmp/akam_net.lock"
sleep 30 &
LP=$!
echo "$LP" >"$N/tmp/akam_net.lock/pid"
check "net hostname: refused (status 4, nothing stored) while a Wi-Fi change holds the lock" test "$(net_rc hostname set busy-cam)" = 4 -a "$(net_rc hostname off)" = 4 -a "$(cat "$N/config/akam/hostname")" = other2
kill "$LP" 2>/dev/null || true
rm -rf "$N/tmp/akam_net.lock"
check "net hostname: no usable /config (cannot create the directory): status 1, kernel name untouched, no DHCP restart" bash -c "echo keep >'$N/tmp/khost'; : >'$N/tmp/calls.log'; AKAM_NET_CFG=/proc/akam-nope/akam $(printf '%q ' "${SHCMD[@]}") '$NS' hostname set cam-x >/dev/null 2>&1; rc=\$?; [ \$rc = 1 ] && [ \"\$(cat '$N/tmp/khost')\" = keep ] && [ ! -s '$N/tmp/calls.log' ]"
check "net wifi: no usable /config: show gives the defaults" bash -c "[ \"\$(AKAM_NET_CFG=/proc/akam-nope/akam $(printf '%q ' "${SHCMD[@]}") '$NS' wifi show | tr '\n' ' ')\" = 'country=none band=2.4 running=2.4 ' ]"
echo my-cam >"$N/config/akam/hostname"

# --- Wi-Fi rendering. band 2.4 (default) = the stock bytes
check "net wifi: defaults country=none band=2.4" test "$(net wifi show | tr '\n' ' ')" = "country=none band=2.4 running=2.4 "
rm -f "$N/tmp/wpa.conf"
net wifi render
check "net wifi render: default band 2.4 is BYTE-IDENTICAL to wpa.conf (cmp), 0600" bash -c "[ \$(stat -c %a '$N/tmp/wpa.conf') = 600 ] && cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf'"
for legacy in 'wifi_band=auto' 'wifi_band=6' 'wifi_band=' 'wifi_band=2.4 5' 'wifi_band=junk
wifi_band=5' 'wifi_band=5'; do
    printf '%s\nwifi_country=DE\n' "$legacy" >"$N/config/akam/net"
    case $legacy in 'wifi_band=5') continue ;; esac
    net wifi render
    check "net wifi: stored '$(echo "$legacy" | head -n1)' reads as 2.4, rendered byte-identical, country ignored on 2.4" bash -c "[ \"\$($(printf '%q ' "${SHCMD[@]}") '$NS' wifi show | tr '\n' ' ')\" = 'country=DE band=2.4 running=2.4 ' ] && cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf'"
done
printf 'wifi_country=DE\r\nwifi_band=auto\r\n' >"$N/config/akam/net"
check "net wifi: a CRLF net file with the legacy auto reads as 2.4 (country DE)" test "$(net wifi show | tr '\n' ' ')" = "country=DE band=2.4 running=2.4 "
printf 'wifi_country=DE\r\nwifi_band=5\r\n' >"$N/config/akam/net"
check "net wifi: CRLF residue is stripped (DE, 5)" test "$(net wifi show | tr '\n' ' ')" = "country=DE band=5 running=2.4 "
printf 'wifi_country=de\nwifi_band=5\n' >"$N/config/akam/net"
check "net wifi: a lowercase stored country is invalid = none, and band 5 without a country reads as 2.4" test "$(net wifi show | tr '\n' ' ')" = "country=none band=2.4 running=2.4 "
net wifi render
check "net wifi render: stored 5 without a country renders the stock bytes" cmp -s "$N/tmp/wpa.conf" "$N/config/akam/wpa.conf"

# band 5 rendering on a rich conf: two networks, comments, a commented-out block, the user's own freq_list/country, no final newline
printf 'ctrl_interface=/tmp/wpa_supplicant\ncountry=ZZ\n# network={\nnetwork={\n ssid="a b"\n scan_freq=2412\n psk="%s"\n}\n\nnetwork={\n\tssid="c"\n\tkey_mgmt=NONE\n  }  ' "$SECRET" >"$N/rich.conf"
printf 'wifi_country=DE\nwifi_band=5\n' >"$N/config/akam/net"
export AKAM_NET_WPA_SRC=$N/rich.conf
net wifi render >"$WORK/net.out" 2>&1
sz=$(stat -c %s "$N/rich.conf")
check "net wifi render band 5: the user's conf is an untouched prefix (byte for byte, ssid/psk included), only country=DE and the 5 GHz freq_list are appended after it" bash -c "head -c $sz '$N/tmp/wpa.conf' | cmp -s - '$N/rich.conf' && [ \"\$(tail -c +$((sz + 1)) '$N/tmp/wpa.conf' | grep -v '^\$' | cut -c1-20 | tr '\n' '|')\" = 'country=DE|freq_list=5180 5200 |' ]"
check "net wifi render band 5: the appended freq_list lists 5 GHz channels only (no 24xx), the render is quiet, 0600, the file ends with a newline" bash -c "grep '^freq_list=' '$N/tmp/wpa.conf' | tail -n1 | grep -q '^freq_list=5180 5200 5220 5240\$' && ! grep -q '^freq_list=.*24[0-9][0-9]' '$N/tmp/wpa.conf' && [ ! -s '$WORK/net.out' ] && [ \$(stat -c %a '$N/tmp/wpa.conf') = 600 ] && [ \"\$(tail -c1 '$N/tmp/wpa.conf' | od -An -c | tr -d ' ')\" = '\\n' ]"
sed 's/$/\r/' "$N/rich.conf" >"$N/crlf.conf"
export AKAM_NET_WPA_SRC=$N/crlf.conf
net wifi render
check "net wifi render band 5: a CRLF conf renders too, the appended keys are plain LF lines" bash -c "grep -qx 'country=DE' '$N/tmp/wpa.conf' && grep -q '^freq_list=5180' '$N/tmp/wpa.conf'"
printf 'network={\n ssid="x"\n psk="%s"\n' "$SECRET" >"$N/open.conf"
export AKAM_NET_WPA_SRC=$N/open.conf
cp "$N/tmp/wpa.conf" "$N/before.conf"
check "net wifi render band 5: an unterminated block is refused (status 1), the current /tmp/wpa.conf is untouched" test "$(net_rc wifi render)" = 1 -a "$(cmp -s "$N/tmp/wpa.conf" "$N/before.conf" && echo same)" = same
check "net wifi set band 5 with an unterminated block in wpa.conf: refused up front (status 1), no restart, nothing saved" bash -c ": >'$N/tmp/calls.log'; rm -f '$N/config/akam/net'; printf 'wifi_country=DE\\nwifi_band=2.4\\n' >'$N/config/akam/net'; AKAM_NET_FG=1 $(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 5 >/dev/null 2>&1; rc=\$?; [ \$rc = 1 ] && [ ! -s '$N/tmp/calls.log' ] && grep -qx 'wifi_band=2.4' '$N/config/akam/net'"
rm -f "$N/config/akam/net"
unset AKAM_NET_WPA_SRC
net wifi render
check "net wifi render band 5 (plain conf): user's file is never modified, the secret never printed" test "$(cat "$N/config/akam/wpa.conf")" = "$USERCONF" -a -z "$(net wifi render 2>&1 | grep -F -- "$SECRET" || true)"
rm -f "$N/config/akam/net"
net wifi render
check "net wifi render: a missing wpa.conf fails (status 1), no conf written" bash -c "mv '$N/config/akam/wpa.conf' '$N/w'; rm -f '$N/tmp/wpa.conf'; $(printf '%q ' "${SHCMD[@]}") '$NS' wifi render 2>/dev/null; rc=\$?; mv '$N/w' '$N/config/akam/wpa.conf'; [ \$rc = 1 ] && [ ! -e '$N/tmp/wpa.conf' ]"
net wifi render

# --- runtime change
: >"$N/tmp/calls.log"
check "net wifi set: invalid values refused (status 2), no restart, nothing saved: bad country, auto, 6, missing, band 5 without a country" test "$(net_rc wifi set country F1)" = 2 -a "$(net_rc wifi set country FRA)" = 2 -a "$(net_rc wifi set band auto)" = 2 -a "$(net_rc wifi set band 6)" = 2 -a "$(net_rc wifi set band)" = 2 -a "$(net_rc wifi set band 5)" = 2 -a ! -s "$N/tmp/calls.log" -a ! -e "$N/config/akam/net"
cp "$N/tmp/wpa.conf" "$N/stock.conf"
OLDW=$(cat "$N/tmp/wpa.pid")
check "net wifi set country de on 2.4: saved at once (uppercased), NO restart, conf and processes untouched, the output says so" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' wifi set country de >'$WORK/net.out' 2>&1; rc=\$?; [ \$rc = 0 ] && grep -qx 'wifi_country=DE' '$N/config/akam/net' && grep -qx 'wifi_band=2.4' '$N/config/akam/net' && [ ! -s '$N/tmp/calls.log' ] && kill -0 $OLDW && cmp -s '$N/tmp/wpa.conf' '$N/stock.conf' && grep -q 'no effect on 2.4' '$WORK/net.out'"
check "net wifi set country none on 2.4: saved, no restart" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' wifi set country none >/dev/null 2>&1; grep -qx 'wifi_country=none' '$N/config/akam/net' && [ ! -s '$N/tmp/calls.log' ]"
check "net wifi set band 2.4 on 2.4: unchanged, nothing written or restarted" bash -c "m=\$(stat -c %Y '$N/config/akam/net'); $(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 2.4 >'$WORK/net.out' 2>&1; grep -q unchanged '$WORK/net.out' && [ ! -s '$N/tmp/calls.log' ]"
net wifi set country DE >/dev/null 2>&1
OLDW=$(cat "$N/tmp/wpa.pid")
check "net wifi set band 5 (healthy): status 0, saved after success, wpa_supplicant restarted with the 5 GHz conf (global freq_list + country), DHCP restarted, old processes gone" bash -c "[ \$($(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 5 >/dev/null 2>&1; echo \$?) = 0 ] && grep -qx 'wifi_band=5' '$N/config/akam/net' && grep -qx 'wifi_country=DE' '$N/config/akam/net' && grep -q 'wpa_supplicant.*5g=1 fl=0' '$N/tmp/calls.log' && grep -q 'udhcpc -b -p' '$N/tmp/calls.log' && ! kill -0 $OLDW 2>/dev/null && kill -0 \$(cat '$N/tmp/wpa.pid') && grep -qx 'country=DE' '$N/tmp/wpa.conf'"
check "net wifi set country none while on band 5 is refused (status 2), saved config unchanged" test "$(net_rc wifi set country none)" = 2 -a "$(grep -c 'wifi_country=DE' "$N/config/akam/net")" = 1
check "net wifi set country fr while on band 5: restarted with country=FR, saved" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' wifi set country fr >/dev/null 2>&1; grep -qx 'wifi_country=FR' '$N/config/akam/net' && grep -qx 'country=FR' '$N/tmp/wpa.conf' && grep -qx 'wifi_band=5' '$N/config/akam/net'"
check "net wifi set band 2.4 from 5 (healthy): the rendered conf is byte-identical to wpa.conf again (cmp), saved" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 2.4 >/dev/null 2>&1; cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf' && grep -qx 'wifi_band=2.4' '$N/config/akam/net' && grep -qx 'wifi_country=FR' '$N/config/akam/net' && kill -0 \$(cat '$N/tmp/wpa.pid')"

# --- dead-man: the AP is 2.4 GHz only, band 5 loses the network
touch "$N/ap24only"
OLDW=$(cat "$N/tmp/wpa.pid")
: >"$N/tmp/calls.log"
rc=$(net_rc wifi set band 5)
check "net dead-man: band 5 on a 2.4-only AP is reverted (status 3)" test "$rc" = 3
check "net dead-man: the saved config is unchanged (2.4, FR)" bash -c "grep -qx 'wifi_band=2.4' '$N/config/akam/net' && grep -qx 'wifi_country=FR' '$N/config/akam/net'"
check "net dead-man: the previous conf (stock bytes) is back, wpa_supplicant runs again with a lease" bash -c "cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf' && kill -0 \$(cat '$N/tmp/wpa.pid') && [ -e '$N/up' ]"
check "net dead-man: wpa_supplicant was restarted twice (try, revert), DHCP too" test "$(grep -c '^wpa_supplicant' "$N/tmp/calls.log")" = 2 -a "$(grep -c '^udhcpc' "$N/tmp/calls.log")" = 2
check "net dead-man: logged" bash -c "grep -q 'NOT healthy' '$N/tmp/akam_net.log' && grep -q 'REVERTING' '$N/tmp/akam_net.log' && grep -q 'revert: wifi is back' '$N/tmp/akam_net.log'"
rm -f "$N/ap24only"
# wpa_supplicant that will not start with the 5 GHz conf
touch "$N/failstart_5"
: >"$N/tmp/calls.log"
rc=$(net_rc wifi set band 5)
check "net dead-man: a wpa_supplicant that will not start with the new conf is reverted at once, Wi-Fi not left down, saved unchanged" test "$rc" = 3 -a -e "$N/up" -a "$(grep -c 'wifi_band=2.4' "$N/config/akam/net")" = 1 -a "$(net_alive)" = y -a "$(same_stock)" = y
rm -f "$N/failstart_5"
# wpa_supplicant that daemonizes, then exits (what a refused conf does): immediate revert, not after the 60 s
touch "$N/dies_5"
: >"$N/tmp/calls.log"
: >"$N/tmp/akam_net.log"
s=$(date +%s)
rc=$(AKAM_NET_DEADMAN=30 net_rc wifi set band 5)
el=$(($(date +%s) - s))
check "net dead-man: a wpa_supplicant that exits right after its start is reverted at once (status 3, well under the dead-man time)" test "$rc" = 3 -a "$el" -lt 15
check "net dead-man: ... logged as a failure, wpa_supplicant runs with the stock bytes and a lease" bash -c "grep -q 'exited right after its start' '$N/tmp/akam_net.log' && grep -q 'FAILED' '$N/tmp/akam_net.log' && kill -0 \$(cat '$N/tmp/wpa.pid') && cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf' && [ -e '$N/up' ] && grep -qx 'wifi_band=2.4' '$N/config/akam/net'"
rm -f "$N/dies_5"
# a wpa_supplicant that dies LATER, while we wait for the lease
touch "$N/dies_late_5"
s=$(date +%s)
rc=$(AKAM_NET_DEADMAN=30 net_rc wifi set band 5)
el=$(($(date +%s) - s))
check "net dead-man: a wpa_supplicant that dies while waiting for the lease is reverted at once (no 30 s wait)" test "$rc" = 3 -a "$el" -lt 15 -a "$(net_alive)" = y
rm -f "$N/dies_late_5"
check "net: no restart ever got a conf with a per-network key added by us (fl=0)" test "$(grep -c 'fl=[1-9]' "$N/tmp/calls.log")" = 0
# revert chain: the saved band 5 itself stops working (starts refused): revert to 5 fails, falls to the stock 2.4 bytes and saves 2.4
printf 'wifi_country=FR\nwifi_band=5\n' >"$N/config/akam/net"
net wifi render
touch "$N/failstart_5"
: >"$N/tmp/akam_net.log"
rc=$(net_rc wifi set country DE)
check "net dead-man: previous band 5 fails too: falls back to the STOCK 2.4 bytes (status 3), running, saved as 2.4 (country kept)" test "$rc" = 3 -a -e "$N/up" -a "$(net_alive)" = y -a "$(same_stock)" = y -a "$(grep -c 'STOCK band 2.4' "$N/tmp/akam_net.log")" = 1 -a "$(grep -cx 'wifi_band=2.4' "$N/config/akam/net")" = 1 -a "$(grep -cx 'wifi_country=FR' "$N/config/akam/net")" = 1
rm -f "$N/failstart_5"
check "net: the user's wpa.conf is unchanged after all of it" test "$(cat "$N/config/akam/wpa.conf")" = "$USERCONF"
# --- review round: the running conf comes back byte for byte, the last resort, link checks, stale pids, locks
printf 'wifi_country=DE\nwifi_band=2.4\n' >"$N/config/akam/net"
net wifi render
printf '# running-marker\n' >>"$N/tmp/wpa.conf"
cp "$N/tmp/wpa.conf" "$N/running.conf"
printf '# source changed since boot\n' >>"$N/config/akam/wpa.conf"
touch "$N/ap24only"
rm -f "$N/reboot.log"
rc=$(net_rc wifi set band 5)
check "net revert: the conf that was RUNNING is restored byte for byte (not a re-render of a changed source), the copy is removed, no reset needed" test "$rc" = 3 -a "$(cmp -s "$N/tmp/wpa.conf" "$N/running.conf" && echo y)" = y -a ! -e "$N/tmp/wpa.conf.prev" -a "$(net_alive)" = y -a ! -e "$N/reboot.log"
rm -f "$N/ap24only"
printf '%s\n' "$USERCONF" >"$N/config/akam/wpa.conf"
net wifi render
USERCONF=$(cat "$N/config/akam/wpa.conf")
# the card's wpa.conf next to the script wins over a stale /config/akam/wpa.conf
mkdir -p "$N/card"
cp "$NS" "$N/card/akam_net.sh"
cp "$HS" "$N/card/akam_hostname.sh"
printf 'ctrl_interface=/tmp/wpa_supplicant\n# card conf\nnetwork={\n ssid="card"\n psk="cardpsk"\n}\n' >"$N/card/wpa.conf"
rm -f "$N/tmp/wpa.conf"
"${SHCMD[@]}" "$N/card/akam_net.sh" wifi render
check "net: wpa.conf next to the script (card mode) is the source, not a stale /config/akam/wpa.conf" cmp -s "$N/tmp/wpa.conf" "$N/card/wpa.conf"
net wifi render
# last resort: wpa_supplicant can not run at all after a change
printf 'wifi_country=DE\nwifi_band=2.4\n' >"$N/config/akam/net"
touch "$N/dies_all"
rm -f "$N/reboot.log"
: >"$N/tmp/akam_net.log"
rc=$(net_rc wifi set band 5)
check "net last resort: no wpa_supplicant can be kept running: status 3, the camera reset is requested (no feeder: reboot), saved file unchanged" test "$rc" = 3 -a "$(grep -c reboot "$N/reboot.log")" = 1 -a "$(grep -c 'LAST RESORT' "$N/tmp/akam_net.log")" = 1 -a "$(grep -cx 'wifi_band=2.4' "$N/config/akam/net")" = 1
rm -f "$N/dies_all" "$N/reboot.log"
"$N/dm/busybox" watchdog -F -t 5 -T 20 /dev/watchdog >/dev/null 2>&1 &
WD=$!
echo "$WD" >"$N/tmp/akam_feeder.pid"
touch "$N/dies_all"
rc=$(net_rc wifi set band 5)
wait "$WD" 2>/dev/null || true
check "net last resort: with a feeder, it is SIGKILLed like the stub's fail does (no reboot command)" test "$rc" = 3 -a ! -e "$N/reboot.log" -a "$(kill -0 "$WD" 2>/dev/null && echo alive)" = ""
rm -f "$N/dies_all" "$N/tmp/akam_feeder.pid"
net wifi fallback >/dev/null 2>&1
touch "$N/udhcpc_fail"
rm -f "$N/reboot.log"
rc=$(net_rc wifi set band 5)
check "net last resort: udhcpc that will not start counts as not healthy: reverted, still no client, reset requested" test "$rc" = 3 -a "$(grep -c reboot "$N/reboot.log")" = 1
rm -f "$N/udhcpc_fail" "$N/reboot.log"
net wifi render
net_restart_ok() { net wifi fallback >/dev/null 2>&1; }
net_restart_ok
# link checks: a stale lease with no carrier is not "healthy"
touch "$N/ap24only" "$N/flush_broken"
: >"$N/tmp/ip.log"
rc=$(net_rc wifi set band 5)
check "net healthy: a stale lease without carrier is NOT healthy (reverted, status 3); a flush that leaves the address is followed by an explicit delete" test "$rc" = 3 -a "$(grep -c 'addr del' "$N/tmp/ip.log")" -ge 1
rm -f "$N/ap24only" "$N/flush_broken"
net_restart_ok
# stale pidfile: wpa_supplicant is found by name, an unrelated live pid in the pidfile is never signalled
"$N/dm/other" 300 >/dev/null 2>&1 &
UNREL=$!
echo "$UNREL" >"$N/tmp/wpa.pid"
OLDW=$(pgrep -f "^$N/dm/wpa_supplicant " | head -n1)
printf 'wifi_country=DE\nwifi_band=2.4\n' >"$N/config/akam/net"
rc=$(net_rc wifi set band 5)
check "net stop: a pidfile naming an unrelated process is ignored (it survives), the real wpa_supplicant is found by name and replaced" test "$rc" = 0 -a "$(kill -0 "$UNREL" 2>/dev/null && echo y)" = y -a "$(kill -0 "$OLDW" 2>/dev/null && echo y)" = ""
kill "$UNREL" 2>/dev/null || true
echo 999999 >"$N/tmp/wpa.pid"
OLDW=$(pgrep -f "^$N/dm/wpa_supplicant " | head -n1)
rc=$(net_rc wifi set band 2.4)
check "net stop: a dead pid in the pidfile falls through to pidof (the running wpa_supplicant is replaced, none left over)" test "$rc" = 0 -a "$(kill -0 "$OLDW" 2>/dev/null && echo y)" = "" -a "$(pgrep -fc "^$N/dm/wpa_supplicant ")" = 1
# lock directory without a pid
mkdir -p "$N/tmp/akam_net.lock"
rm -f "$N/tmp/akam_net.lock/pid"
sleep 30 &
LP=$!
( sleep 1; echo "$LP" >"$N/tmp/akam_net.lock/pid" ) &
check "net lock: a lock directory without a pid is a change starting up (busy), not a stale lock" test "$(net_rc hostname set busy-cam)" = 4
kill "$LP" 2>/dev/null || true
wait 2>/dev/null || true
rm -rf "$N/tmp/akam_net.lock"
mkdir -p "$N/tmp/akam_net.lock"
check "net lock: ... one that stays without a pid for a few seconds is stale and taken over" test "$(net_rc hostname set takeover-cam)" = 0 -a ! -e "$N/tmp/akam_net.lock"
# stale net.new.* of a power cut
: >"$N/config/akam/net.new.4242"
printf 'wifi_country=none\nwifi_band=2.4\n' >"$N/config/akam/net"
net_rc wifi set country FR >/dev/null
check "net save: stale net.new.* files are removed" test ! -e "$N/config/akam/net.new.4242" -a "$(grep -c wifi_country=FR "$N/config/akam/net")" = 1

# fallback (boot)
printf 'wifi_country=FR\nwifi_band=5\n' >"$N/config/akam/net"
net wifi render
net wifi fallback
check "net wifi fallback (no save: this boot only): stock bytes, wpa_supplicant running, the saved band 5 is kept" bash -c "cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf' && grep -qx 'wifi_band=5' '$N/config/akam/net' && kill -0 \$(cat '$N/tmp/wpa.pid')"
net wifi render
net wifi fallback save
check "net wifi fallback save: stock bytes rendered, wpa_supplicant running, band 2.4 saved (country kept), no lock left" bash -c "cmp -s '$N/tmp/wpa.conf' '$N/config/akam/wpa.conf' && grep -qx 'wifi_band=2.4' '$N/config/akam/net' && grep -qx 'wifi_country=FR' '$N/config/akam/net' && kill -0 \$(cat '$N/tmp/wpa.pid') && [ ! -e '$N/tmp/akam_net.lock' ]"
# saved band != running band (boot fallback without save): show says so and `set band 5` retries instead of answering "unchanged"
printf 'wifi_country=FR\nwifi_band=5\n' >"$N/config/akam/net"
net wifi render
net wifi fallback
check "net wifi show: saved band 5 but running=2.4 after a no-save fallback" test "$(net wifi show | tr '\n' ' ')" = "country=FR band=5 running=2.4 "
: >"$N/tmp/calls.log"
rc=$(net_rc wifi set band 5)
check "net wifi set band 5 while running 2.4 (saved 5): retried (restart, status 0), not answered unchanged" test "$rc" = 0 -a "$(grep -c unchanged "$WORK/net.out")" = 0 -a "$(grep -c '5g=1' "$N/tmp/calls.log")" = 1 -a "$(net wifi show | tr '\n' ' ')" = "country=FR band=5 running=5 "
rc=$(net_rc wifi set band 5)
check "net wifi set band 5 again (now running 5): unchanged, no restart" test "$rc" = 0 -a "$(grep -c unchanged "$WORK/net.out")" = 1 -a "$(grep -c '5g=1' "$N/tmp/calls.log")" = 1
# result of the last request (state net.apply of the control API)
rc=$(net_rc wifi set country none)
check "net result: a refused request leaves 'refused' in akam_net.res" test "$rc" = 2 -a "$(sed 's/ .*//' "$N/tmp/akam_net.res")" = refused
# stop_pid stops EVERY wpa_supplicant (a twin that is not in the pidfile too)
"$N/dm/wpa_supplicant" 300 >/dev/null 2>&1 &
TWIN=$!
rc=$(net_rc wifi set band 2.4)
check "net stop: every wpa_supplicant is stopped (the twin outside the pidfile too), one new one runs, result ok" test "$rc" = 0 -a "$(kill -0 "$TWIN" 2>/dev/null && echo y)" = "" -a "$(pgrep -fc "^$N/dm/wpa_supplicant ")" = 1 -a "$(cat "$N/tmp/akam_net.res")" = ok
# a script run from the TMP dir never takes its own rendered $TMP/wpa.conf as the source
cp "$NS" "$N/tmp/akam_net.sh"
printf '# rendered leftovers\n' >"$N/tmp/wpa.conf"
( unset AKAM_NET_WPA_SRC; "${SHCMD[@]}" "$N/tmp/akam_net.sh" wifi render ) >/dev/null 2>&1
check "net: HERE/wpa.conf equal to the rendered \$TMP/wpa.conf is skipped (the source stays /config/akam/wpa.conf)" cmp -s "$N/tmp/wpa.conf" "$N/config/akam/wpa.conf"
rm -f "$N/tmp/akam_net.sh"
# a pid-less lock gets the 3 s grace in the detached path too (busy when its owner shows up, stale after the grace)
mkdir -p "$N/tmp/akam_net.lock"
rm -f "$N/tmp/akam_net.lock/pid"
printf 'wifi_country=FR\nwifi_band=2.4\n' >"$N/config/akam/net"
sleep 30 &
LB=$!
( sleep 1; echo "$LB" >"$N/tmp/akam_net.lock/pid" ) &
check "net lock_busy: a lock whose pid appears within the 3 s grace is busy (status 4, detached path)" test "$(unset AKAM_NET_FG; net_rc wifi set band 5)" = 4
kill "$LB" 2>/dev/null || true
wait 2>/dev/null || true
rm -rf "$N/tmp/akam_net.lock"
net wifi render
# detached run (the SSH session may die with the link)
unset AKAM_NET_FG
: >"$N/tmp/calls.log"
: >"$N/tmp/akam_net.log"
# no wall-clock bound (a loaded host): the foreground path of a change never prints "pending", only the detached one does
check "net wifi set (detached): returns with 'pending' (the worker does the change)" bash -c "$(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 5 | grep -q pending"
i=0
while ! grep -q 'healthy: saved' "$N/tmp/akam_net.log" 2>/dev/null && [ "$i" -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
check "net wifi set (detached): the worker finished, saved band 5" grep -qx 'wifi_band=5' "$N/config/akam/net"
# detached worker immune to SIGHUP of its parent session
: >"$N/tmp/akam_net.log"
( trap '' PIPE; "${SHCMD[@]}" "$NS" wifi set band 2.4 >/dev/null 2>&1 ) &
HP=$!
wait "$HP" 2>/dev/null || true
i=0
while ! grep -q 'healthy: saved' "$N/tmp/akam_net.log" 2>/dev/null && [ "$i" -lt 600 ]; do sleep 0.1; i=$((i + 1)); done
check "net wifi set (detached): survives its launcher, band 2.4 saved" grep -qx 'wifi_band=2.4' "$N/config/akam/net"
# lock
mkdir -p "$N/tmp/akam_net.lock"
sleep 30 &
LP=$!
echo "$LP" >"$N/tmp/akam_net.lock/pid"
check "net wifi set: a running change holds the lock (status 4, detached too), so does fallback" test "$(net_rc wifi set band 5)" = 4 -a "$(AKAM_NET_FG=1 net_rc wifi set band 5)" = 4 -a "$(net_rc wifi fallback)" = 4
kill "$LP" 2>/dev/null || true
check "net wifi set: a stale lock is taken over" test "$(AKAM_NET_FG=1 net_rc wifi set band 5)" = 0 -a ! -e "$N/tmp/akam_net.lock"
check "net wifi set: no wpa.conf = nothing done (status 1)" bash -c "mv '$N/config/akam/wpa.conf' '$N/w'; AKAM_NET_FG=1 $(printf '%q ' "${SHCMD[@]}") '$NS' wifi set band 2.4 >/dev/null 2>&1; rc=\$?; mv '$N/w' '$N/config/akam/wpa.conf'; [ \$rc = 1 ] && grep -qx 'wifi_band=5' '$N/config/akam/net'"
check "net wifi set country on 2.4 with no usable /config: status 1" bash -c "printf 'wifi_country=none\nwifi_band=2.4\n' >'$N/config/akam/net'; AKAM_NET_CFG=/proc/akam-nope/akam AKAM_NET_WPA_SRC='$N/config/akam/wpa.conf' AKAM_NET_FG=1 $(printf '%q ' "${SHCMD[@]}") '$NS' wifi set country DE >/dev/null 2>&1; [ \$? = 1 ]"
pkill -f "$N" 2>/dev/null || true
unset AKAM_NET_CFG AKAM_TMP_DIR AKAM_WPAS AKAM_HOSTNAME_CMD AKAM_HOSTNAME_SH AKAM_NET_DEADMAN AKAM_NET_POLL AKAM_NET_STOP_WAIT AKAM_NET_FG AKAM_SH AKAM_NET_ALIVE_WAIT AKAM_PIDOF AKAM_NET_REBOOT_CMD AKAM_NET_FEEDER_PID

# --- the stub: hostname + rendering at boot, band fallback
new_stub_root st_net
cp "$NS" "$SDC/akam/akam_net.sh"
cp "$HS" "$SDC/akam/akam_hostname.sh"
export AKAM_HOSTNAME_CMD=$STUBS/hostname
cat >"$STUBS/hostname" <<'EOS'
#!/bin/sh
if [ $# -eq 0 ]; then cat "$AKAM_ROOT/tmp/khost" 2>/dev/null; else echo "$1" >"$AKAM_ROOT/tmp/khost"; fi
EOS
chmod 755 "$STUBS/hostname"
mkdir -p "$R/config/akam"
echo Cam-Salon >"$R/config/akam/hostname"
printf 'wifi_country=DE\nwifi_band=5\n' >"$R/config/akam/net"
# a supplicant that only gets a route when the conf has no 5 GHz restriction (the AP is not on the band); it logs the
# restriction count, the country line and whether the conf is the stock bytes. $R/die5 = daemonizes then dies on a 5 GHz conf,
# $R/fail5 = exits 1 on it.
cat >"$R/appfs/bin/wpa_supplicant" <<'EOS'
#!/bin/sh
conf= pf=
while [ $# -gt 0 ]; do
    case $1 in -c) conf=$2; shift ;; -P) pf=$2; shift ;; esac
    shift
done
g5=$(grep -c '^freq_list=' "$conf")
same=0
cmp -s "$conf" "$AKAM_ROOT/../sd_wpa.conf" && same=1
echo "wpa_supplicant $g5 same=$same $(grep '^country=' "$conf")" >>"$AKAM_ROOT/tmp/order.log"
if [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/fail5" ]; then exit 1; fi
if [ "$g5" != 0 ] && [ -e "$AKAM_ROOT/die5" ]; then
    ( : ) &
    echo $! >"$pf"
    wait
    exit 0
fi
[ "$g5" != 0 ] || echo "default via 192.0.2.1 dev wlan0" >>"$AKAM_ROOT/tmp/routes"
"$AKAM_ROOT/dm/wpa_supplicant" 60 >/dev/null 2>&1 &
echo $! >"$pf"
EOS
chmod 755 "$R/appfs/bin/wpa_supplicant"
mkdir -p "$R/dm"
for dn in wpa_supplicant udhcpc; do cp -L "$(command -v sleep)" "$R/dm/$dn"; done
printf '#!/bin/sh\necho\n' >"$STUBS/pidof"
# shellcheck disable=SC2016  # the $ must reach the stub unexpanded
printf '#!/bin/sh\necho reboot >>"$AKAM_ROOT/tmp/reboot.log"\n' >"$STUBS/reboot"
chmod 755 "$STUBS/pidof" "$STUBS/reboot"
export AKAM_PIDOF=$STUBS/pidof AKAM_NET_REBOOT_CMD=$STUBS/reboot AKAM_FALLBACK_SLEEP=1
cp "$SDC/akam/wpa.conf" "$R/../sd_wpa.conf"
export AKAM_WIFI_PRE_SLEEP=0 AKAM_WIFI_POST_SLEEP=0 AKAM_HEALTH_TRIES=20 AKAM_HEALTH_SLEEP=1 AKAM_HEALTH_BUDGET=8 AKAM_NET_STOP_WAIT=1 AKAM_NET_ALIVE_WAIT=1
: >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "net stub: kernel hostname from /config/akam/hostname (lowercased)" test "$(cat "$R/tmp/khost")" = cam-salon
check "net stub: udhcpc with the configured hostname" has "$R/tmp/order.log" "udhcpc -b -p $R/tmp/udhcpc.pid -i wlan0 -x hostname:cam-salon"
check "net stub: first wpa_supplicant start has the rendered 5 GHz conf (freq_list, country=DE)" has "$R/tmp/order.log" "wpa_supplicant 1 same=0 country=DE"
check "net stub: no network on the saved band 5: falls back to the stock bytes (second start, same=1, no country) and becomes healthy" bash -c "grep -q 'wpa_supplicant 0 same=1 *\$' '$R/tmp/order.log' && grep -q 'falling back to band 2.4' '$SDC/akam/akam_boot.log' && grep -q 'own_app PoC-1 healthy' '$SDC/akam/akam_boot.log'"
check "net stub: the timeout fallback is for THIS boot only: the saved band 5 stays in /config (a slow router after a power cut must not lose it)" bash -c "grep -qx 'wifi_band=5' '$R/config/akam/net' && grep -qx 'wifi_country=DE' '$R/config/akam/net'"
check "net stub: wpa.conf in /tmp is 0600 and the secret is not in the card log" bash -c "[ \$(stat -c %a '$R/tmp/wpa.conf') = 600 ] && ! grep -qF -- '$SECRET' '$SDC/akam/akam_boot.log'"
end_stub
pkill -f "$R/tmp" 2>/dev/null || true
# band 5 and a supplicant that daemonizes then dies (refused conf): immediate fallback, no health wait, udhcpc started once
printf 'wifi_country=DE\nwifi_band=5\n' >"$R/config/akam/net"
touch "$R/die5"
: >"$R/tmp/order.log"
: >"$R/tmp/routes"
: >"$SDC/akam/akam_boot.log"
rm -f "$R/tmp/reboot.log"
# a runtime change still holds the lock when the stub's fallback starts: it must wait, not fail (a failure resets the camera)
mkdir -p "$R/tmp/akam_net.lock"
"$R/dm/udhcpc" 4 >/dev/null 2>&1 &
echo $! >"$R/tmp/akam_net.lock/pid"
run_stub
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "net stub: the fallback waited for the busy lock instead of failing (healthy, no reset)" bash -c "grep -q 'PoC-1 healthy' '$SDC/akam/akam_boot.log' && ! grep -q 'FAILED' '$SDC/akam/akam_boot.log' && [ ! -e '$R/tmp/reboot.log' ]"
check "net stub: a 5 GHz supplicant that exits right after its start: immediate fallback to the stock bytes, healthy, DHCP started once" bash -c "grep -q 'falling back to band 2.4 at once' '$SDC/akam/akam_boot.log' && ! grep -q 'no network with the saved Wi-Fi band' '$SDC/akam/akam_boot.log' && grep -q 'wpa_supplicant 0 same=1' '$R/tmp/order.log' && [ \$(grep -c '^udhcpc' '$R/tmp/order.log') = 1 ] && grep -q 'PoC-1 healthy' '$SDC/akam/akam_boot.log' && grep -qx 'wifi_band=2.4' '$R/config/akam/net'"
end_stub
pkill -f "$R/tmp" 2>/dev/null || true
# band 5 and a supplicant that will not start at all: stock bytes instead of the reset path
rm -f "$R/die5"
touch "$R/fail5"
printf 'wifi_country=DE\nwifi_band=5\n' >"$R/config/akam/net"
: >"$R/tmp/order.log"
: >"$R/tmp/routes"
: >"$SDC/akam/akam_boot.log"
run_stub
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "net stub: a 5 GHz supplicant that will not start: stock bytes started instead (healthy, not the reset path)" bash -c "grep -q 'falling back to band 2.4 at once' '$SDC/akam/akam_boot.log' && grep -q 'wpa_supplicant 0 same=1' '$R/tmp/order.log' && grep -q 'PoC-1 healthy' '$SDC/akam/akam_boot.log'"
end_stub
pkill -f "$R/tmp" 2>/dev/null || true
rm -f "$R/fail5"
# default band 2.4: the stub's wpa.conf is the stock copy and nothing is ever restarted
printf 'wifi_country=DE\nwifi_band=2.4\n' >"$R/config/akam/net"
: >"$R/tmp/order.log"
: >"$R/tmp/routes"
: >"$SDC/akam/akam_boot.log"
run_stub
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 150 ]; do sleep 0.1; i=$((i + 1)); done
check "net stub: band 2.4 (country DE saved): wpa.conf in /tmp is byte-identical to the card's, one wpa_supplicant start, no fallback" bash -c "cmp -s '$R/tmp/wpa.conf' '$SDC/akam/wpa.conf' && [ \$(grep -c '^wpa_supplicant' '$R/tmp/order.log') = 1 ] && ! grep -q 'falling back' '$SDC/akam/akam_boot.log'"
end_stub
pkill -f "$R/tmp" 2>/dev/null || true
unset AKAM_HOSTNAME_CMD AKAM_WIFI_PRE_SLEEP AKAM_WIFI_POST_SLEEP AKAM_HEALTH_BUDGET AKAM_NET_STOP_WAIT AKAM_NET_ALIVE_WAIT AKAM_PIDOF AKAM_NET_REBOOT_CMD AKAM_FALLBACK_SLEEP
rm -f "$STUBS/hostname"
# no band file (default 2.4): full budget, never a fallback
new_stub_root st_net2
cp "$NS" "$SDC/akam/akam_net.sh"
cp "$HS" "$SDC/akam/akam_hostname.sh"
export AKAM_HOSTNAME_CMD=$STUBS/hostname_none
printf '#!/bin/sh\nexit 0\n' >"$STUBS/hostname_none"
chmod 755 "$STUBS/hostname_none"
echo "default via 192.0.2.1 dev wlan0" >"$R/tmp/routes"
run_stub
i=0
while ! grep -q 'own_app PoC-1 healthy' "$SDC/akam/akam_boot.log" 2>/dev/null && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
check "net stub: default (no files in /config): hostname akam-rot2k, healthy, no fallback" bash -c "grep -q 'hostname:akam-rot2k' '$R/tmp/order.log' && grep -q 'PoC-1 healthy' '$SDC/akam/akam_boot.log' && ! grep -q 'falling back' '$SDC/akam/akam_boot.log' && ! grep -q '^country=' '$R/tmp/wpa.conf'"
end_stub
unset AKAM_HOSTNAME_CMD
rm -f "$STUBS/hostname_none"

echo "== net registry rows (C unit test: net.hostname, net.wifi_country, net.wifi_band)"
if command -v cc >/dev/null 2>&1; then
 if cc -Wall -Wextra -Werror -std=gnu99 -g "${INC_2K[@]}" -o "$WORK/netreg_unit" "$REPO/tests/rot2k_netreg_unit.c" "$FW/netreg.c" "$REPO/src/core/settings_reg.c" "$REPO/src/api/json_min.c" "$REPO/src/api/api_keys.c" "$REPO/src/net/crypto/sha256.c"; then
        "$WORK/netreg_unit" >"$WORK/netreg_unit.out" 2>&1 || true
        check "netreg unit test passes" grep -q 'rot2k netreg unit: 0 failed' "$WORK/netreg_unit.out"
        grep 'FAIL' "$WORK/netreg_unit.out" || true
    else
        ko "netreg unit test does not compile"
    fi
fi
check "net registry: the rows spawn nothing (the backend is installed by the caller)" test -z "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/netreg.c" | grep -E '\bsystem|\bpopen|\bexec[lv]|\bfork|fopen|\bopen[[:space:]]*\(|\bsocket' || true)"

echo "== user-file registry rows (C unit test: mqtt.*, onvif.*, rtsp.*, ntp.servers, audio.mic_enable)"
if command -v cc >/dev/null 2>&1; then
 if cc -Wall -Wextra -Werror -std=gnu99 -g "${INC_2K[@]}" -o "$WORK/cfgreg_unit" "$REPO/tests/rot2k_cfgreg_unit.c" "$FW/cfgreg.c" "$REPO/src/core/settings_reg.c" "$REPO/src/api/json_min.c" "$REPO/src/api/api_keys.c" "$REPO/src/net/crypto/sha256.c"; then
        "$WORK/cfgreg_unit" >"$WORK/cfgreg_unit.out" 2>&1 || true
        check "cfgreg unit test passes" grep -q 'rot2k cfgreg unit: 0 failed' "$WORK/cfgreg_unit.out"
        grep 'FAIL' "$WORK/cfgreg_unit.out" || true
    else
        ko "cfgreg unit test does not compile"
    fi
fi
check "user-file registry: the rows spawn nothing and open nothing (the backend is installed by the caller)" test -z "$(strip_c_comments "$REPO/firmware/lsc-rotatable-2k-ak3918av130/cfgreg.c" | grep -E '\bsystem|\bpopen|\bexec[lv]|\bfork|fopen|\bopen[[:space:]]*\(|\bsocket' || true)"

echo "== onvif_events_probe (host tool, loopback fake camera)"
if command -v python3 >/dev/null 2>&1; then
    python3 -I "$REPO/tests/rot2k_onvif_probe_test.py" >"$WORK/probe.out" 2>&1 || true
    check "onvif_events_probe test passes" grep -q 'rot2k onvif probe test: 0 failed' "$WORK/probe.out"
    check "onvif_events_probe holds no IPv4 literal" test "$(grep -cE '[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}' "$FW/tools/onvif_events_probe.py")" = 0
else
    echo "  skip - no python3: onvif_events_probe test"
fi

check "overlay yy_debug.ini is mode=debug only" test "$(cat "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/yy_debug.ini")" = "$(printf 'mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh')"
check "rescue yy_debug.ini is mode=debug only" test "$(cat "$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/rescue/yy_debug.ini")" = "$(printf 'mode=debug\nstart_sh=/mnt/sdcard/hack.sh')"


echo "== control API: on-camera key tool and the net.* request hand-over (docs/porting/lsc-rotatable-2k/control-api.md)"
AK_KEY=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/akam_api_key.sh
RUNSH=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh
check "api key tool: parses, executable, calls only the app's key mode (no secret in argv, no /dev, no eval)" bash -c "sh -n '$AK_KEY' && test -x '$AK_KEY' && grep -q -- '--api-key' '$AK_KEY' && ! grep -E 'urandom|/dev/|eval|openssl' '$AK_KEY'"
AKT=$WORK/apikey
mkdir -p "$AKT/tmp"
cat >"$AKT/tmp/akamd" <<'EOS'
#!/bin/sh
echo "app args: $*"
EOS
chmod 755 "$AKT/tmp/akamd"
check "api key tool: new passes --api-key and the arguments through" bash -c "[ \"\$(AKAM_TMP_DIR='$AKT/tmp' sh '$AK_KEY' new --scope admin --name a)\" = 'app args: --api-key new --scope admin --name a' ]"
check "api key tool: list and revoke are accepted" bash -c "AKAM_TMP_DIR='$AKT/tmp' sh '$AK_KEY' list >/dev/null && AKAM_TMP_DIR='$AKT/tmp' sh '$AK_KEY' revoke abc --force >/dev/null"
check "api key tool: another word is a usage error (2)" bash -c "AKAM_TMP_DIR='$AKT/tmp' sh '$AK_KEY' root; [ \$? = 2 ]" 2>/dev/null
check "api key tool: no app in /tmp is exit 1" bash -c "AKAM_TMP_DIR='$AKT/none' sh '$AK_KEY' list 2>/dev/null; [ \$? = 1 ]"
check "appfs image carries the key tool" grep -q 'akam_net.sh akam_api_key.sh' "$REPO/firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh"
# net_request: extracted from the supervisor and run against a fake akam_net.sh
NRT=$WORK/netreq
mkdir -p "$NRT/tmp" "$NRT/akam"
cat >"$NRT/akam/akam_net.sh" <<'EOS'
#!/bin/sh
echo "$*" >>"$NRT_LOG"
EOS
sed -n '/^net_request() {/,/^}/p' "$RUNSH" >"$NRT/fn.sh"
check "supervisor: net_request is in the supervisor and called from the polling loop" bash -c "test -s '$NRT/fn.sh' && grep -q '^        net_request\$' '$RUNSH'"
nr() { # nr <request line> -> the akam_net.sh arguments it was handed (empty when refused), file removed
    rm -f "$NRT/log" "$NRT/tmp/akam_net.req"
    printf '%s\n' "$1" >"$NRT/tmp/akam_net.req"
    NRT_LOG=$NRT/log TMP=$NRT/tmp AK=$NRT/akam sh -c ". '$NRT/fn.sh'; log() { :; }; net_request" >/dev/null 2>&1
    [ ! -e "$NRT/tmp/akam_net.req" ] || echo "REQUEST FILE LEFT"
    cat "$NRT/log" 2>/dev/null || true
}
check "net request: hostname set" test "$(nr 'hostname set cam-salon')" = "hostname set cam-salon"
check "net request: wifi country" test "$(nr 'wifi set country DE')" = "wifi set country DE"
check "net request: wifi band" test "$(nr 'wifi set band 2.4')" = "wifi set band 2.4"
check "net request: anything outside the grammar is dropped (and the request file removed)" test -z "$(nr 'wifi set ssid x')"
check "net request: a shell metacharacter is refused" test -z "$(nr 'hostname set a;touch /tmp/x')"
check "net request: a glob is refused" test -z "$(nr 'hostname set *')"
check "net request: too many words are refused" test -z "$(nr 'hostname set a b')"
check "net request: no file, nothing happens" bash -c "rm -f '$NRT/tmp/akam_net.req' '$NRT/log'; TMP='$NRT/tmp' AK='$NRT/akam' sh -c \". '$NRT/fn.sh'; log() { :; }; net_request\" && [ ! -e '$NRT/log' ]"

echo "rot2k overlay test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
