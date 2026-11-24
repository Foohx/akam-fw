#!/usr/bin/env bash
# Host-side test of flash_appfs.sh --hot / --hot-preflight (LSC Rotatable 2K): the argument gates, the dry run with a
# fake ssh (it must never be called), and the camera-side helper (extracted from the script) against a FAKE /proc.
# Nothing here touches hardware, a device, or the network. Group "rot2k".
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/firmware/lsc-rotatable-2k-ak3918av130/tools/flash_appfs.sh

WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'chmod -R u+rwx "$WORK" 2>/dev/null || true; rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}
has() { grep -q -- "$2" "$1"; }
hasnt() { ! grep -q -- "$2" "$1"; }

# ---- the camera-side helper, extracted from the script, must parse and (when shellcheck exists) be clean ----------------
HELPER=$WORK/akam_hot.sh
sed -n "/^read -r -d '' HOT_REMOTE <<'AKAM_HOT_EOF'/,/^AKAM_HOT_EOF$/p" "$SCRIPT" | sed '1d;$d' >"$HELPER"
check "helper extracted" test -s "$HELPER"
check "helper parses under sh" sh -n "$HELPER"
if command -v shellcheck >/dev/null 2>&1; then check "helper is shellcheck clean (sh)" shellcheck -s sh "$HELPER"; fi
check "no flash/nvram write in the helper" bash -c "! grep -v '^ *#' '$HELPER' | grep -E '/dev/mtd|nvram|of='"

# ---- argument gates and the dry run -------------------------------------------------------------------------------------
mkdir -p "$WORK/bin"
cat >"$WORK/bin/ssh" <<EOF
#!/bin/sh
echo "ssh \$*" >>"$WORK/ssh.log"
exit 99
EOF
chmod +x "$WORK/bin/ssh"
: >"$WORK/ssh.log"
export PATH="$WORK/bin:$PATH"

out=$WORK/o.txt
if command -v mksquashfs >/dev/null 2>&1 && command -v unsquashfs >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    mkdir -p "$WORK/t1" "$WORK/t2" "$WORK/img" "$WORK/bk"
    echo one >"$WORK/t1/a"
    echo two >"$WORK/t2/a"
    mk() { # mk <srcdir> <out>: a tiny squashfs padded to the partition size
        mksquashfs "$1" "$2" -comp xz -noappend -quiet -all-root >/dev/null
        truncate -s $((0x490000)) "$2"
    }
    mk "$WORK/t1" "$WORK/img/appfs-akam.squashfs"
    mk "$WORK/t2" "$WORK/cur.squashfs"
    mk "$WORK/t1" "$WORK/bk/mtd3.bin"
    for n in 0 1 2 4 5; do head -c 4096 /dev/urandom >"$WORK/bk/mtd$n.bin"; done
    (cd "$WORK/img" && sha256sum appfs-akam.squashfs >appfs-akam.squashfs.sha256)
    python3 -I "$(dirname "$SCRIPT")/calc_partition_sign.py" appfs "$WORK/img/appfs-akam.squashfs" | sed -n 's/.*sign=//p' >"$WORK/img/appfs_sign.txt"
    (cd "$WORK/bk" && sha256sum mtd*.bin >SHA256SUMS)
    run() { CAM=root@192.0.2.10 BACKUP_DIR=$WORK/bk IMAGE_DIR=$WORK/img WORK_DIR=$WORK/w$RANDOM "$SCRIPT" "$@"; }

    rc=0
    run --hot >"$out" 2>&1 || rc=$?
    check "--hot without --update is refused" test "$rc" -ne 0
    check "  ...with the reason" has "$out" "need --update"
    rc=0
    run --hot-preflight >"$out" 2>&1 || rc=$?
    check "--hot-preflight without --update is refused" test "$rc" -ne 0
    rc=0
    run --update "$WORK/cur.squashfs" --hot --execute >"$out" 2>&1 || rc=$?
    check "--hot --execute without AKAM_I_HAVE_RECOVERY is refused" test "$rc" -ne 0
    check "  ...with the reason" has "$out" "AKAM_I_HAVE_RECOVERY=yes is not set"

    rc=0
    run --update "$WORK/cur.squashfs" --hot >"$out" 2>&1 || rc=$?
    check "--update --hot dry run succeeds" test "$rc" -eq 0
    check "dry run prints the relocation steps" has "$out" "H2 start a new feeder from /tmp"
    check "dry run prints the memory gate" has "$out" "MemFree > 12416 kB"
    check "dry run describes the single detached runner" has "$out" "ONE detached on-camera runner"
    check "dry run names the checkpoints and the dead-man" has "$out" "no answer within 120 s"
    check "dry run prints the 25 s feeder soak" has "$out" "soak it alone >= 25 s"
    check "dry run drops caches and logs MemAvailable" has "$out" "drop_caches, MemFree and MemAvailable logged at every gate"
    check "dry run stages in /tmp/akam_flash" has "$out" "cat > /tmp/akam_flash/appfs-new.bin"
    check "dry run never stages on the SD card" hasnt "$out" "cat > /mnt/sdcard/akam_flash"
    check "dry run does not demand the SD card" hasnt "$out" "needs the Akam SD card inserted"
    check "dry run ends with the manual reboot instruction" has "$out" "Reboot it yourself now"
    check "dry run wrote nothing" has "$out" "nothing was written"
    check "dry run never called ssh" test ! -s "$WORK/ssh.log"

    rc=0
    run --update "$WORK/cur.squashfs" --hot-preflight >"$out" 2>&1 || rc=$?
    check "--hot-preflight dry run succeeds" test "$rc" -eq 0
    check "  ...restores at the end" has "$out" "restart supervisor/ntp/keeper"
    check "  ...never reaches the flash step" hasnt "$out" "flashcp -v /tmp/akam_flash"
    check "  ...never called ssh" test ! -s "$WORK/ssh.log"

    rc=0
    run --update "$WORK/cur.squashfs" >"$out" 2>&1 || rc=$?
    check "plain --update dry run is unchanged (SD staging)" bash -c "[ $rc -eq 0 ] && grep -q 'cat > /mnt/sdcard/akam_flash/appfs-new.bin' '$out' && ! grep -q 'H2 ' '$out'"
    check "no ssh at all in any dry run" test ! -s "$WORK/ssh.log"
else
    echo "  skip - mksquashfs/unsquashfs/python3 missing: dry-run tests"
fi

# ---- the helper against a fake /proc --------------------------------------------------------------------------------------
PROC=$WORK/proc
HD=$WORK/hot
mkdir -p "$HD"
echo 0 >"$WORK/panic"
henv() { echo "AKAM_PROC=$PROC AKAM_HOT_DIR=$HD AKAM_MOUNTS=$WORK/mounts AKAM_MEMINFO=$WORK/meminfo AKAM_SD=$WORK/sd AKAM_FLASHDIR=$WORK/fd AKAM_DROP=$WORK/drop AKAM_PANIC=$WORK/panic AKAM_OOMFILE=$WORK/oom AKAM_SOAK=1 AKAM_CONFIRM_S=${CS:-3} AKAM_WORKER=${WK:-$WORK/fakeworker.sh} AKAM_RBWORKER=$WORK/fakerb.sh AKAM_FLASH_RES=$WORK/flash.result AKAM_FLASH_LOG=$WORK/flash.log AKAM_FEEDER_PID=$WORK/akam_feeder.pid"; }
# shellcheck disable=SC2046  # henv is a word list of KEY=safe-path
hh() { env $(henv) sh "$HELPER" "$@"; }
# shellcheck disable=SC2046
hhx() { exec env $(henv) sh "$HELPER" "$@"; }
addp() { # addp <pid> <exe-target> <cmdline words...> ; env AP_FD=<file target> AP_MAPS=<text>
    local pid=$1 exe=$2
    shift 2
    mkdir -p "$PROC/$pid/fd"
    ln -sfn "$exe" "$PROC/$pid/exe"
    ln -sfn / "$PROC/$pid/cwd"
    printf '%s\0' "$@" >"$PROC/$pid/cmdline"
    printf '%s\n' "${AP_MAPS:-00400000-00500000 r-xp 0 0 0 $exe}" >"$PROC/$pid/maps"
    [ -z "${AP_FD:-}" ] || ln -sfn "$AP_FD" "$PROC/$pid/fd/3"
    : >"$PROC/$pid/environ"
    [ -z "${AP_ENV:-}" ] || printf '%s\0' "$AP_ENV" >"$PROC/$pid/environ"
}
mkdir -p "$PROC" "$WORK/sd"
# the real process list of the card-less camera (hardware read-only scan): scripts show as "sh /mnt/sdcard/...", their fd
# resolves into /appfs; dropbear and the feeder are invoked through /mnt/sdcard but their exe is in /appfs
AP_FD=/appfs/script/init.sh addp 338 /bin/busybox /bin/sh /appfs/script/init.sh
AP_FD=/appfs/akam/own_app_stub.sh addp 507 /bin/busybox /bin/sh /appfs/akam/own_app_stub.sh
addp 514 /appfs/akam/busybox /mnt/sdcard/busybox watchdog -F -t 5 -T 20 /dev/watchdog
addp 443 /appfs/akam/dropbearmulti /mnt/sdcard/akam/dropbearmulti dropbear -r /config/akam/key -D /tmp/akam_home -p 22 -P /tmp/dropbear.pid -T 3 -E
addp 16615 /appfs/akam/dropbearmulti /mnt/sdcard/akam/dropbearmulti dropbear -r /config/akam/key -D /tmp/akam_home -p 22 -P /tmp/dropbear.pid -T 3 -E
addp 602 /appfs/bin/wpa_supplicant /appfs/bin/wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf
AP_FD=/appfs/akam/akam_ntp.sh addp 706 /bin/busybox sh /mnt/sdcard/akam/akam_ntp.sh run
AP_FD=/appfs/akam/own_app_run.sh AP_ENV="AKAM_X=/tmp/y" addp 709 /bin/busybox sh /mnt/sdcard/akam/own_app_run.sh
AP_MAPS="00400000-00500000 r-xp 0 0 0 /appfs/lib/libakam.so" addp 783 /tmp/akamd /tmp/akamd --audio 1
AP_FD=/appfs/akam/decloud.sh addp 806 /bin/busybox sh /mnt/sdcard/akam/decloud.sh keeper
addp 19 /bin/busybox /bin/busybox sleep 3600   # touches nothing in /appfs: must not be listed

rc=0
hh scan >"$out" 2>&1 || rc=$?
check "scan: all known roles -> exit 0" test "$rc" -eq 0
for r in init feeder stub supervisor app dropbear wpa ntp keeper; do check "scan lists role $r" has "$out" " $r \["; done
check "scan ignores a process that does not touch /appfs" hasnt "$out" "sleep"
addp 20 /appfs/bin/mystery /appfs/bin/mystery
rc=0
hh scan >"$out" 2>&1 || rc=$?
check "scan: an unknown /appfs process aborts (exit 2)" test "$rc" -eq 2
check "  ...and is named UNEXPECTED" has "$out" "UNEXPECTED"
rm -rf "$PROC/20"
rc=0
hh scan free >"$out" 2>&1 || rc=$?
check "scan free: processes still touching /appfs -> exit 4" test "$rc" -eq 4

hh capture >"$out" 2>&1
check "capture: dropbear argv saved" test -f "$HD/cap.dropbear.443.argv"
check "capture: AKAM_ env of the supervisor saved" has "$HD/cap.supervisor.709.env" "AKAM_X=/tmp/y"
check "capture: the stub, init and the app are not replayed" bash -c "! ls '$HD'/cap.stub.* '$HD'/cap.init.* '$HD'/cap.app.* >/dev/null 2>&1"
addp 21 /appfs/akam/dropbearmulti /mnt/sdcard/akam/dropbearmulti dropbear -r "/config/a b/key" -p 22
rc=0
hh capture >"$out" 2>&1 || rc=$?
check "capture: an argument with a space is refused (exit 3)" test "$rc" -eq 3
rm -rf "$PROC/21"

# free: everything that touches /appfs gone
for n in 338 507 514 443 16615 602 706 709 783 806; do rm -rf "${PROC:?}/$n"; done
rc=0
hh scan free >"$out" 2>&1 || rc=$?
check "scan free: nothing touches /appfs -> exit 0" test "$rc" -eq 0
check "  ...says so" has "$out" "no process touches /appfs"
addp 22 /appfs/akam/busybox /tmp/akam_hot/busybox watchdog -F   # a cwd/exe still in /appfs after the relocation
rc=0
hh scan free >"$out" 2>&1 || rc=$?
check "scan free: a leftover mapping of /appfs is caught (exit 4)" test "$rc" -eq 4
rm -rf "$PROC/22"
ln -sfn /appfs/akam "$PROC/19/cwd"
rc=0
hh scan free >"$out" 2>&1 || rc=$?
check "scan free: a cwd in /appfs is caught (exit 4)" test "$rc" -eq 4
ln -sfn / "$PROC/19/cwd"

# card-less proof
printf 'rootfs / squashfs ro 0 0\n/dev/mtdblock3 /appfs squashfs ro 0 0\ntmpfs /mnt/sdcard tmpfs rw 0 0\n' >"$WORK/mounts"
ln -sfn /appfs/akam/busybox "$WORK/sd/busybox"
rc=0
hh cardless >"$out" 2>&1 || rc=$?
check "cardless: tmpfs bind + Akam tree -> ok" test "$rc" -eq 0
printf '/dev/mmcblk0p1 /mnt/sdcard vfat rw 0 0\n' >"$WORK/mounts"
rc=0
hh cardless >"$out" 2>&1 || rc=$?
check "cardless: a real SD card is refused" test "$rc" -ne 0
printf 'tmpfs /mnt/sdcard tmpfs rw 0 0\n/dev/mmcblk0p1 /media vfat rw 0 0\n' >"$WORK/mounts"
rc=0
hh cardless >"$out" 2>&1 || rc=$?
check "cardless: any mounted mmcblk is refused" test "$rc" -ne 0

# memory gate
printf 'MemTotal: 60000 kB\nMemFree:        20000 kB\n' >"$WORK/meminfo"
rc=0
hh memgate 99999999 1 >"$out" 2>&1 || rc=$?
check "memgate: too little MemFree is refused" test "$rc" -ne 0
check "memgate: logs MemFree and MemAvailable" has "$out" "mem\[memory gate\]: MemFree 20000 kB, MemAvailable n/a"
printf 'MemTotal: 60000 kB\nMemFree:        20000 kB\nMemAvailable:   26000 kB\n' >"$WORK/meminfo"
rm -f "$WORK/drop"
rc=0
hh memgate 1 1 >"$out" 2>&1 || rc=$?
check "memgate: passes and creates the staging dir" bash -c "[ $rc -eq 0 ] && [ -d '$WORK/fd' ]"
check "  ...logs MemAvailable" has "$out" "MemAvailable 26000 kB"
check "  ...dropped the caches first" bash -c "[ \"\$(cat '$WORK/drop')\" = 3 ]"
rmdir "$WORK/fd"
rc=0
hh memgate 1 99999999999 >"$out" 2>&1 || rc=$?
check "memgate: /tmp space gate refuses" test "$rc" -ne 0

# the tools the flash path runs must not be in /appfs (cut sync mkdir rmdir sleep included)
TB=$WORK/tb
mkdir -p "$TB"
for t in md5sum nohup dd sh cat awk sed ip cut sync mkdir rmdir sleep; do ln -sf "$(command -v "$t" 2>/dev/null || echo /bin/true)" "$TB/$t"; done
printf '#!/bin/sh\n' >"$TB/flashcp"
chmod +x "$TB/flashcp"
rc=0
PATH="$TB:$PATH" hh tools >"$out" 2>&1 || rc=$?
check "tools: all present, none in /appfs" test "$rc" -eq 0
for t in cut sync mkdir rmdir sleep; do check "tools: checks $t" has "$out" "^$t "; done
mkdir -p "$WORK/appfs"
printf '#!/bin/sh\n' >"$WORK/appfs/sleep"
chmod +x "$WORK/appfs/sleep"
ln -sfn "$WORK/appfs/sleep" "$TB/sleep"
rc=0
PATH="$TB:$PATH" hh tools >"$out" 2>&1 || rc=$?
check "tools: sleep linked into /appfs is refused" test "$rc" -ne 0

# ---- feeder: oom protect, md5 reuse, soak, fallback check (a REAL sleep is the old feeder: fake pids are never killed) --
realsleep() { sh -c 'sleep 300 >/dev/null 2>&1 & echo $!'; }
cleanup_fake() {
    local f p
    for f in "$HD"/fb.pids "$HD"/db22.pid "$HD"/dbfake.pids; do
        [ -f "$f" ] || continue
        while read -r p; do kill "$p" 2>/dev/null || true; done <"$f"
    done
}
HD=$WORK/hot2
PROC=$WORK/proc2
mkdir -p "$HD" "$PROC"
printf '#!/bin/sh\necho $$ >>"%s/fb.pids"\nexec sleep 60\n' "$HD" >"$HD/busybox"
chmod +x "$HD/busybox"
OLD=$(realsleep)
addp "$OLD" /appfs/akam/busybox /mnt/sdcard/busybox watchdog -F -t 5 -T 20 /dev/watchdog
addp 443 /appfs/akam/dropbearmulti /mnt/sdcard/akam/dropbearmulti dropbear -r /config/akam/key -p 22 -P /tmp/dropbear.pid -T 3
hh capture >"$out" 2>&1
rm -f "$WORK/oom"
rc=0
hh feeder >"$out" 2>&1 || rc=$?
check "feeder: relocation succeeds" test "$rc" -eq 0
check "feeder: soaks alone after the old one is killed" has "$out" "soaked 1 s alone"
check "feeder: the old feeder is dead" bash -c "! kill -0 $OLD 2>/dev/null"
check "feeder: oom_score_adj -1000 (panic_on_oom 0)" bash -c "[ \"\$(cat '$WORK/oom')\" = -1000 ]"
newf=$(cat "$HD/feeder.pid")
check "feeder: the new one lives" kill -0 "$newf"
check "feeder: the new pid is also in the akam_net.sh feeder pid file" test "$(cat "$WORK/akam_feeder.pid")" = "$newf"
# fallback: the new feeder cannot start (exits at once), the old command (not present on the host) is not alive either
rm -rf "$PROC" "$HD"; mkdir -p "$PROC" "$HD"
printf '#!/bin/sh\nexit 1\n' >"$HD/busybox"
chmod +x "$HD/busybox"
OLD=$(realsleep)
addp "$OLD" /appfs/akam/busybox /mnt/sdcard/busybox watchdog -F -t 5 -T 20 /dev/watchdog
hh capture >"$out" 2>&1
echo 1 >"$WORK/panic"
rm -f "$WORK/oom"
rc=0
hh feeder >"$out" 2>&1 || rc=$?
check "feeder: no feeder can start -> exit 1" test "$rc" -eq 1
check "feeder: the dead fallback is reported CRITICAL" has "$out" "CRITICAL: FAILED to start a feeder from /tmp AND the fallback feeder"
check "feeder: panic_on_oom != 0 leaves oom_score_adj alone" test ! -e "$WORK/oom"
echo 0 >"$WORK/panic"
kill "$OLD" 2>/dev/null || true
# stage: a reused /tmp copy is md5-checked
rm -rf "$PROC" "$HD"; mkdir -p "$PROC" "$HD" "$WORK/fakeapp"
printf 'real\n' >"$WORK/fakeapp/busybox"
chmod +x "$WORK/fakeapp/busybox"
addp 31 "$WORK/fakeapp/busybox" /mnt/sdcard/busybox watchdog -F
printf '%s\n' "$WORK/fakeapp/busybox" >"$HD/cap.feeder.31.exe"
printf 'x\0watchdog\0' >"$HD/cap.feeder.31.argv"
printf 'stale\n' >"$HD/busybox"
chmod +x "$HD/busybox"
rc=0
hh stage >"$out" 2>&1 || rc=$?
check "stage: a stale /tmp copy is detected and replaced" bash -c "[ $rc -eq 0 ] && grep -q 'differs' '$out' && cmp -s '$WORK/fakeapp/busybox' '$HD/busybox'"
hh stage >"$out" 2>&1
check "stage: an md5-equal copy is reused" has "$out" "reused .* (md5 equal)"

# ---- restore: port-22 listener by pid file, frees /tmp/akam_flash ---------------------------------------------------------
rm -rf "$PROC" "$HD"; mkdir -p "$PROC" "$HD"
printf '#!/bin/sh\necho $$ >>"%s/fb.pids"\nexec sleep 60\n' "$HD" >"$HD/busybox"
# shellcheck disable=SC2016  # the fake dropbear script is literal
printf '#!/bin/sh\npf=""\nwhile [ $# -gt 0 ]; do [ "$1" != -P ] || pf=$2; shift; done\nsleep 60 >/dev/null 2>&1 &\necho $! >"$pf"\necho $! >>"%s/dbfake.pids"\n' "$HD" >"$HD/dropbearmulti"
chmod +x "$HD/busybox" "$HD/dropbearmulti"
OLD=$(realsleep)
addp "$OLD" /appfs/akam/busybox /mnt/sdcard/busybox watchdog -F -t 5 -T 20 /dev/watchdog
addp 443 /appfs/akam/dropbearmulti /mnt/sdcard/akam/dropbearmulti dropbear -r /config/akam/key -p 22 -P /tmp/dropbear.pid -T 3
hh capture >"$out" 2>&1
addp 32 /tmp/akam_hot/wpa_supplicant /tmp/akam_hot/wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf
ALT=$(realsleep)
echo "$ALT" >"$HD/db2222.pid"
rm -rf "${PROC:?}/$OLD" "$PROC/443"
kill "$OLD" 2>/dev/null || true
LIVEP=$(realsleep)
echo "$LIVEP" >"$HD/db22.pid"
mkdir -p "$WORK/fd"
: >"$WORK/fd/appfs-new.bin"
rc=0
hh restore >"$out" 2>&1 || rc=$?
check "restore: succeeds" test "$rc" -eq 0
sleep 0.3
check "restore: stops the temporary port-2222 listener" bash -c "! kill -0 $ALT 2>/dev/null"
check "restore: a live port-22 pid file means no second dropbear" hasnt "$out" "dropbear from /tmp on port 22"
check "restore: deletes the staged images (frees the RAM)" test ! -e "$WORK/fd"
kill "$LIVEP" 2>/dev/null || true
sleep 0.3
rc=0
hh restore >"$out" 2>&1 || rc=$?
check "restore: a dead port-22 pid file starts the dropbear again" has "$out" "dropbear from /tmp on port 22"
rm -rf "$PROC/32"
rc=0
hh restore >"$out" 2>&1 || rc=$?
check "restore: no wpa_supplicant alive is reported (exit 5)" bash -c "[ $rc -eq 5 ] && grep -q 'NO wpa_supplicant is running' '$out'"
cleanup_fake
kill "$(cat "$HD/db22.pid" 2>/dev/null || echo 0)" 2>/dev/null || true

# ---- the detached runner against a fake /proc (a fake PC answers the checkpoints) -------------------------------------------
printf '#!/bin/sh\n' >"$WORK/bin/flashcp"
chmod +x "$WORK/bin/flashcp"
printf '#!/bin/sh\necho OK >"%s"\necho worker ran\n' "$WORK/flash.result" >"$WORK/fakeworker.sh"
printf 'rootfs / squashfs ro 0 0\ntmpfs /mnt/sdcard tmpfs rw 0 0\n' >"$WORK/mounts"
printf 'MemTotal: 60000 kB\nMemFree:        20000 kB\nMemAvailable:   26000 kB\n' >"$WORK/meminfo"
runner_world() { # a card-less camera whose feeder already runs from /tmp (everything already relocated)
    rm -rf "$PROC" "$HD" "$WORK/fd" "$WORK/oom" "$WORK/flash.result"
    mkdir -p "$PROC" "$HD"
    addp 900 /tmp/akam_hot/busybox /tmp/akam_hot/busybox watchdog -F -t 5 -T 20 /dev/watchdog
    addp 901 /tmp/akam_hot/wpa_supplicant /tmp/akam_hot/wpa_supplicant -Dnl80211 -i wlan0 -c /tmp/wpa.conf
    echo '#!/bin/sh' >"$WORK/fakerb.sh"
}
fakepc() { # answer every WAIT_<name> by touching confirm.<name>; upload also stages the two images
    local i stl ph n
    for i in $(seq 1 600); do
        stl=$(cat "$HD/status" 2>/dev/null || true)
        ph=${stl%%|*}
        case $ph in
            WAIT_*)
                n=${ph#WAIT_}
                if [ "$n" = upload ]; then
                    mkdir -p "$WORK/fd"
                    echo a >"$WORK/fd/appfs-new.bin"
                    echo b >"$WORK/fd/appfs-backup.bin"
                    [ -z "${KILLRB:-}" ] || rm -f "$WORK/fakerb.sh"
                fi
                [ "$n" = "${SKIP:-}" ] || touch "$HD/confirm.$n" ;;
            WRITTEN* | ABORTED* | DONE*) return 0 ;;
        esac
        sleep 0.2
    done
}
# The fake PC is a polling background process: under load (a parallel full CI run) it can stall for seconds, so every
# checkpoint a test EXPECTS it to answer gets a wide window (PC_WAIT). Only the tests whose point is that a checkpoint
# expires use a short one: the dead-man (nobody answers at all) and the missing final go (GO_WAIT: the upload answer
# must still arrive in time, only the go is withheld, so it is not tiny).
PC_WAIT=60
GO_WAIT=10
PROC=$WORK/proc4
HD=$WORK/hot4
runner_world
rc=0
hh run rehearsal 1 1 1 >"$out" 2>&1 || rc=$?
check "runner rehearsal: exit 0" test "$rc" -eq 0
check "runner rehearsal: final status DONE_RESTORED" has "$HD/status" "^DONE_RESTORED|"
check "runner rehearsal: protects itself from the OOM killer" bash -c "[ \"\$(cat '$WORK/oom')\" = -1000 ]"
check "runner rehearsal: logs MemFree/MemAvailable at the gates" bash -c "grep -q 'mem\[H1\]: MemFree 20000 kB, MemAvailable 26000 kB' '$out' && grep -q 'mem\[memory gate\]' '$out'"
check "runner rehearsal: wpa already on /tmp is skipped cleanly (no checkpoint)" has "$out" "wpa: none runs from /appfs (already on /tmp): skipped"
check "runner rehearsal: leaves no staged images" test ! -e "$WORK/fd"

# dead-man: hot mode, nobody answers the upload checkpoint
runner_world
rc=0
CS=2 hh run hot 1 1 1 >"$out" 2>&1 || rc=$?
check "runner dead-man: no upload confirmation -> exit 1" test "$rc" -eq 1
check "runner dead-man: restored by itself, nothing written" has "$HD/status" "^ABORTED_RESTORED|the PC did not upload"
check "runner dead-man: staging dir freed" test ! -e "$WORK/fd"
check "runner dead-man: the worker never ran" test ! -e "$WORK/flash.result"

# early refusal: an SD card is mounted -> nothing changed, no restore
runner_world
printf '/dev/mmcblk0p1 /mnt/sdcard vfat rw 0 0\n' >"$WORK/mounts"
rc=0
hh run hot 1 1 1 >"$out" 2>&1 || rc=$?
check "runner: a mounted SD card is refused before anything changes" bash -c "[ $rc -eq 1 ] && grep -q '^ABORTED_UNCHANGED|not a card-less boot' '$HD/status'"
printf 'rootfs / squashfs ro 0 0\ntmpfs /mnt/sdcard tmpfs rw 0 0\n' >"$WORK/mounts"

# memory gate failure after the relocation -> restore
runner_world
rc=0
hh run hot 99999999 1 1 >"$out" 2>&1 || rc=$?
check "runner: a failed memory gate restores" has "$HD/status" "^ABORTED_RESTORED|memory gate failed"

# the happy hot path: every checkpoint answered, the worker writes, no restore afterwards
runner_world
fakepc &
pcpid=$!
rc=0
CS=$PC_WAIT hh run hot 1 1 1 >"$out" 2>&1 || rc=$?
wait "$pcpid" || true
check "runner hot: exit 0" test "$rc" -eq 0
check "runner hot: WRITTEN|OK" has "$HD/status" "^WRITTEN|OK"
check "runner hot: went through upload and go checkpoints" bash -c "grep -q 'confirmed by the PC: upload' '$out' && grep -q 'confirmed by the PC: go' '$out'"
check "runner hot: no wait for the PC once the write started" bash -c "awk '/mtd3 write started/{f=1} f&&/confirmed by the PC|no confirmation/{bad=1} END{exit bad}' '$out'"
check "runner hot: the images stay (no restore after the write)" test -s "$WORK/fd/appfs-new.bin"
check "runner hot: the worker ran" test -s "$WORK/flash.result"

# no final go: the worker never runs
runner_world
SKIP=go fakepc &
pcpid=$!
rc=0
CS=$GO_WAIT hh run hot 1 1 1 >"$out" 2>&1 || rc=$?
wait "$pcpid" || true
check "runner: no final go -> restored, the worker never ran" bash -c "grep -q '^ABORTED_RESTORED|the PC gave no final go' '$HD/status' && [ ! -e '$WORK/flash.result' ]"

# final gate failure (the rollback worker script is gone): the worker never runs
runner_world
KILLRB=1 fakepc &
pcpid=$!
rc=0
CS=$PC_WAIT hh run hot 1 1 1 >"$out" 2>&1 || rc=$?
wait "$pcpid" || true
check "runner: a failed final gate restores, the worker never ran" bash -c "grep -q '^ABORTED_RESTORED|the final gate failed' '$HD/status' && [ ! -e '$WORK/flash.result' ]"

# an unknown mode is refused
rc=0
hh run bogus 1 1 1 >"$out" 2>&1 || rc=$?
check "runner: an unknown mode is refused (exit 64)" test "$rc" -eq 64

# no wpa_supplicant after the restore of a rehearsal is reported
runner_world
rm -rf "$PROC/901"
rc=0
hh run rehearsal 1 1 1 >"$out" 2>&1 || rc=$?
check "runner rehearsal: no wpa_supplicant after the restore -> DONE_RESTORED_NO_WIFI" bash -c "[ $rc -eq 1 ] && grep -q '^DONE_RESTORED_NO_WIFI|' '$HD/status'"

# a signal during the write is ignored (the shield is inherited by the worker)
runner_world
printf '#!/bin/sh\nsleep 2\necho OK >"%s"\n' "$WORK/flash.result" >"$WORK/slowworker.sh"
fakepc &
pcpid=$!
(CS=$PC_WAIT WK=$WORK/slowworker.sh hhx run hot 1 1 1 >"$out" 2>&1) &
rpid=$!
i=0
while [ "$i" -lt 600 ] && ! grep -q '^WRITING|' "$HD/status" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
kill -TERM "$rpid" 2>/dev/null || true
kill -INT "$rpid" 2>/dev/null || true
kill -HUP "$rpid" 2>/dev/null || true
wait "$rpid" 2>/dev/null || true
wait "$pcpid" || true
check "runner: TERM/INT/HUP during the write are ignored, the worker finishes" bash -c "grep -q '^WRITTEN|OK' '$HD/status' && [ -s '$WORK/fd/appfs-new.bin' ]"

# a second runner is refused, and a stale lock of a dead pid is taken over
runner_world
(CS=60 hhx run hot 1 1 1 >"$out" 2>&1) &
rpid=$!
i=0
while [ "$i" -lt 100 ] && ! grep -q '^WAIT_upload|' "$HD/status" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
rc=0
hh run hot 1 1 1 >"$WORK/o2.txt" 2>&1 || rc=$?
check "runner: a second instance is refused" bash -c "[ $rc -eq 1 ] && grep -q 'refusing to start a second one' '$WORK/o2.txt'"
check "runner: the refused one leaves the status alone" has "$HD/status" "^WAIT_upload|"
kill -TERM "$rpid" 2>/dev/null || true
wait "$rpid" 2>/dev/null || true
check "runner: the lock is released when the runner ends" test ! -e "$HD/run.lock"
runner_world
mkdir "$HD/run.lock"
echo 999999 >"$HD/run.lock/pid"
rc=0
hh run rehearsal 1 1 1 >"$out" 2>&1 || rc=$?
check "runner: a stale lock (dead pid) is taken over" bash -c "[ $rc -eq 0 ] && grep -q 'taking it over' '$out'"

# SIGTERM while waiting for a checkpoint: the EXIT trap restores
runner_world
(CS=60 hhx run hot 1 1 1 >"$out" 2>&1) &
rpid=$!
i=0
while [ "$i" -lt 100 ] && ! grep -q '^WAIT_upload|' "$HD/status" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
kill -TERM "$rpid" 2>/dev/null || true
wait "$rpid" 2>/dev/null || true
check "runner SIGTERM: restores before it exits" bash -c "grep -q '^ABORTED_RESTORED|' '$HD/status' && [ ! -e '$WORK/fd' ]"

# ---- the rollback worker lock survives SIGKILL (extracted from the script, paths redirected into $WORK) -------------------
RW=$WORK/rw
mkdir -p "$RW/stage"
sed -n "/^ROLLBACK_WORKER='/,/^'\$/p" "$SCRIPT" | sed "1s/^ROLLBACK_WORKER='//;\$d" |
    sed "s#/tmp/#$RW/#g;s#@STAGE@#$RW/stage#g;s#/dev/mtdblock3#$RW/mtdblock3#g;s#/dev/mtd3#$RW/mtd3#g" >"$RW/akam_rollback_worker.sh"
check "rollback worker extracted" test -s "$RW/akam_rollback_worker.sh"
check "rollback worker parses under sh" sh -n "$RW/akam_rollback_worker.sh"
check "rollback worker sets oom_score_adj -1000 gated on panic_on_oom" bash -c "grep -q 'panic_on_oom' '$RW/akam_rollback_worker.sh' && grep -q -- '-1000' '$RW/akam_rollback_worker.sh'"
check "flash worker sets oom_score_adj -1000 too" bash -c "sed -n \"/^WORKER='/,/^'\\$/p\" '$SCRIPT' | grep -q -- '-1000'"
# shellcheck disable=SC2016
printf '#!/bin/sh\ncp "$2" "$3"\n' >"$RW/flashcp"
chmod +x "$RW/flashcp"
printf 'old image\n' >"$RW/stage/appfs-backup.bin"
: >"$RW/mtd3"
export_path=$RW:$PATH
mkdir "$RW/akam_rollback.lock"
echo 999999 >"$RW/akam_rollback.lock/pid"   # a SIGKILLed worker: the pid is dead
rc=0
PATH=$export_path sh "$RW/akam_rollback_worker.sh" >"$out" 2>&1 || rc=$?
check "rollback lock: a dead pid's lock is taken over" bash -c "[ $rc -eq 0 ] && grep -q 'taking it over' '$out' && [ \"\$(cat '$RW/akam_rollback.result')\" = ROLLED_BACK ]"
check "rollback lock: released at exit" test ! -e "$RW/akam_rollback.lock"
printf '#!/bin/sh\nsleep 20\n' >"$RW/akam_rollback_sleeper.sh"
sh "$RW/akam_rollback_sleeper.sh" &
sp=$!
mkdir "$RW/akam_rollback.lock"
echo "$sp" >"$RW/akam_rollback.lock/pid"
rc=0
PATH=$export_path sh "$RW/akam_rollback_worker.sh" >"$out" 2>&1 || rc=$?
kill "$sp" 2>/dev/null || true
check "rollback lock: a live worker's lock is respected" bash -c "[ $rc -ne 0 ] && grep -q 'holds' '$out'"
check "no-result message prints the rollback command and the poll command" bash -c "grep -q 'NORES=.*ROLLBACK_CMD.*ROLLBACK_POLL' '$SCRIPT'"

echo "rot2k flash hot test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
