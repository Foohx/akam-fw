#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# End-to-end scenarios for ./akam, one state directory per scenario, every step chained the way a
# user would run them (state transitions, gating, destructive confirmations, secrets):
#   (a) "no compile" path : doctor > get-kit (local kit) > prepare-backup-sd > find-camera > backup >
#                           image > prepare-flash-sd > flash (maintenance telnet) > verify
#   (b) "from source" path: the build step and the source-mode image step, rehearsed (--dry-run);
#                           compiling for real is covered by tests/build_firmware_test.sh
#   (c) update over SSH   : fake ssh and fake reflash tool
#   (d) restore           : the stock APP put back over the maintenance telnet
#   (e) cross-checks      : every destructive command refuses without a typed human answer
# Nothing real is touched: fake camera (tools/wizard/fake_camera.sh) on localhost, the real
# tools/sd/sd_card.sh in --dry-run --stage-dir mode over a fake lsblk, stub kit/assemble/ssh/reflash.
# The camera transcripts are reconstructed from the docs, not recorded on hardware.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/fixtures/fake_camera_env.sh
. "$ROOT/tests/fixtures/fake_camera_env.sh"
fc_require_busybox
# shellcheck source=tools/lib/host.sh
. "$ROOT/tools/lib/host.sh"

AKAM="$ROOT/akam"
T=$(mktemp -d)
trap 'fc_cleanup; kill $(jobs -p) 2>/dev/null; chmod -R u+rwX "$T" 2>/dev/null; if [ -n "${KEEP_TMP:-}" ]; then echo "kept: $T"; else rm -rf "$T"; fi' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { # name, command...
    local n=$1
    shift
    if "$@"; then ok "$n"; else ko "$n"; fi
}
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

SENTINEL="WifiP4ss_E2E_SENTINEL_77q"
BB=$AKAM_BUSYBOX
export HOME=$T/home
mkdir -p "$HOME"
export AKAM_BACKUP_ROOT=$T/bk
mkdir -p "$AKAM_BACKUP_ROOT"
export AKAM_XFER_CONNECT_TRIES=60 AKAM_CAMSH_TIMEOUT=20 NO_COLOR=1
unset AKAM_STATE_DIR AKAM_SSH AKAM_REFLASH AKAM_SD_PASS AKAM_SD_SSID AKAM_WIFI_PASS AKAM_WIFI_SSID AKAM_FORCE_TTY

# ------------------------------------------------------------------ stubs (host side)
STUBS=$T/stubs
mkdir -p "$STUBS/bin"
export STUB_LOG=$T/stub.log
: >"$STUB_LOG"
cat >"$STUBS/check_host.sh" <<'EOF'
#!/usr/bin/env bash
echo "check_host $*" >>"$STUB_LOG"
exit 0
EOF
cat >"$STUBS/verify_kit.sh" <<'EOF'
#!/usr/bin/env bash
ext=""
while [ $# -gt 1 ]; do
    case $1 in --extract-to) ext=$2; shift ;; esac
    shift
done
echo "verify_kit" >>"$STUB_LOG"
mkdir -p "$ext/bin"
printf '#!/bin/sh\necho flash tool\n' >"$ext/bin/flash_tool"
printf 'kit_format=1\nakam_version=1.0.0-e2e\n' >"$ext/MANIFEST.txt"
EOF
cat >"$STUBS/assemble.sh" <<'EOF'
#!/usr/bin/env bash
echo "assemble $*" >>"$STUB_LOG"
out=""
while [ $# -gt 0 ]; do [ "$1" = --out ] && out=$2; shift; done
mkdir -p "$out"
cp "$E2E_IMAGE" "$out/ak_rtsp_firmware_20260101_000000.squashfs"
printf 'compression: xz\nhardware verdict: stub\n' >"$out/build_report.txt"
EOF
FD=$T/lsblk
SYS=$T/sys
mkdir -p "$FD" "$SYS/class/block/sda" "$SYS/class/block/sdb"
cat >"$STUBS/bin/lsblk" <<'EOF'
#!/usr/bin/env bash
last=${*: -1}
case " $* " in
    *MOUNTPOINTS*) exit 1 ;;
    *" -nspro "*) cat "$FAKE_LSBLK_DIR/inv_$(basename "$last").txt" 2>/dev/null || exit 1 ;;
    *" -dpP "*)
        if [ "${last#/dev/}" != "$last" ]; then grep -F "NAME=\"$last\"" "$FAKE_LSBLK_DIR/disks.txt"; else cat "$FAKE_LSBLK_DIR/disks.txt"; fi ;;
    *) cat "$FAKE_LSBLK_DIR/tree_$(basename "$last").txt" 2>/dev/null | sed 's/ MOUNTPOINTS="[^"]*"//'; [ -f "$FAKE_LSBLK_DIR/tree_$(basename "$last").txt" ] || exit 1 ;;
esac
EOF
printf '#!/usr/bin/env bash\nexit 1\n' >"$STUBS/bin/findmnt"
cat >"$FD/disks.txt" <<'EOF'
NAME="/dev/sda" TYPE="disk" RM="0" TRAN="sata" MODEL="Samsung\x20SSD"
NAME="/dev/sdb" TYPE="disk" RM="1" TRAN="usb" MODEL="Card\x20Reader"
EOF
printf 'NAME="/dev/sda" TYPE="disk" MOUNTPOINT=""\nNAME="/dev/sda1" TYPE="part" MOUNTPOINT="/"\n' >"$FD/tree_sda.txt"
printf 'NAME="/dev/sdb" TYPE="disk" MOUNTPOINT=""\nNAME="/dev/sdb1" TYPE="part" MOUNTPOINT="/run/media/u/AKAMSD"\n' >"$FD/tree_sdb.txt"
echo 976773168 >"$SYS/class/block/sda/size"
echo 7744512 >"$SYS/class/block/sdb/size"
export FAKE_LSBLK_DIR=$FD AKAM_SYSFS=$SYS
cat >"$STUBS/bin/sudo" <<'EOF'
#!/usr/bin/env bash
echo "SUDO RAN" >>"$STUB_LOG"
EOF
cat >"$STUBS/bin/unsquashfs" <<'EOF'
#!/usr/bin/env bash
case $1 in
    -s) printf 'Found a valid SQUASHFS 4:0 superblock.\nCompression xz\nBlock size 131072\n' ;;
    -l) printf 'squashfs-root\nsquashfs-root/bin\nsquashfs-root/sbin\nsquashfs-root/local\n' ;;
esac
EOF
# Synthetic toolkit, as in tests/wizard_core_test.sh.
TK=$T/toolkit
mkdir -p "$TK/sd_card/custom/scripts" "$TK/sd_card/custom/configs" "$TK/sd_card/custom/state"
printf '#!/bin/sh\necho hack' >"$TK/sd_card/hack.sh"
printf 'TELNET=0\nTELNET_PORT=23\nOFFLINE=0\nONVIF=1\n' >"$TK/sd_card/custom/configs/hack.conf"
printf '#!/bin/sh\necho entry\n' >"$TK/sd_card/custom/scripts/entrypoint.sh"
: >"$TK/sd_card/custom/state/shadow.done"
TK_HASH=$( (cd "$TK/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1)
export AKAM_TOOLKIT_DIR=$TK
CARD=$T/card
export CARD TK_HASH REAL_SD="$ROOT/tools/sd/sd_card.sh"
cat >"$STUBS/sd.sh" <<'EOF'
#!/usr/bin/env bash
cmd=${1:-}
[ $# -eq 0 ] || shift
echo "sd $cmd $*" >>"$STUB_LOG"
mode=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do [ "${args[$i]}" = --mode ] && mode=${args[$((i + 1))]}; done
extra=()
case $cmd in
    prepare) rm -rf "${CARD:?}/$mode"; extra=(--dry-run --stage-dir "$CARD/$mode") ;;
    wipe) extra=(--dry-run) ;;
    verify)
        out=()
        for ((i = 0; i < ${#args[@]}; i++)); do
            if [ "${args[$i]}" = --device ]; then i=$((i + 1)); else out+=("${args[$i]}"); fi
        done
        args=("${out[@]}")
        extra=(--stage-dir "$CARD/$mode")
        ;;
esac
AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=$TK_HASH exec bash "$REAL_SD" "$cmd" ${args[@]+"${args[@]}"} ${extra[@]+"${extra[@]}"}
EOF
cat >"$STUBS/ssh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/ssh.log"
cmd=""; seen=0
for x in "$@"; do
    if [ "$seen" = 1 ]; then cmd="$cmd $x"; fi
    case $x in root@*) seen=1 ;; esac
done
cmd=${cmd# }
case $cmd in
    true) exit 0 ;;
    "cat > /tmp/akam_hwcheck.sh") cat >"$STUB_DIR/hwcheck.copy"; exit 0 ;;
    "sh /tmp/akam_hwcheck.sh") echo "PASS port 22 listening"; echo "PASS port 554 listening"; exit 0 ;;
esac
exit 0
EOF
cat >"$STUBS/reflash.sh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/reflash.log"
echo "reflash_over_ssh: pre-checks passed"
case " $* " in *" --i-understand-no-uart "*) echo "reflash_over_ssh: read-back md5 verified" ;; esac
exit 0
EOF
chmod +x "$STUBS"/*.sh "$STUBS"/ssh "$STUBS"/bin/*
export STUB_DIR=$STUBS
: >"$STUBS/ssh.log"
: >"$STUBS/reflash.log"
export PATH="$STUBS/bin:$PATH"
export AKAM_CHECK_HOST=$STUBS/check_host.sh AKAM_VERIFY_KIT=$STUBS/verify_kit.sh
export AKAM_ASSEMBLE=$STUBS/assemble.sh AKAM_SD_CLI=$STUBS/sd.sh
export AKAM_SSH=$STUBS/ssh AKAM_REFLASH=$STUBS/reflash.sh

# ------------------------------------------------------------------ fixtures
KIT=$T/akam-kit-1.0.0-e2e.tar.xz
echo "kit bytes" >"$KIT"
PUB=$T/akam.pub
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq akam-e2e' >"$PUB"
printf 'fake key\n' >"$T/key"
IMG_NAME=ak_rtsp_firmware_20260101_000000.squashfs
IMG=$T/$IMG_NAME
head -c 5242880 /dev/urandom >"$IMG"
export E2E_IMAGE=$IMG
IMG_SHA=$(host_sha256 "$IMG")
IMG_MD5=$(host_md5 "$IMG")
printf '#!/bin/sh\necho flash tool\n' >"$T/flash_tool"

fc_start || { echo "cannot start the fake camera"; exit 1; }
CAM_A=$FC_ROOT
APP_SHA=$(host_sha256 "$CAM_A/dev/mtd7")
{
    grep -v -e '^#' -e '^validated' "$ROOT/tools/release/supported_hardware.txt"
    echo "validated|Fake camera|AK3918AV130|4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025|8 partitions; mtd7=APP 00500000|squashfs xz block=131072 bcj=none|$APP_SHA|1.30.1"
} >"$T/hw_validated.txt"
export AKAM_HW_LIST=$T/hw_validated.txt

# a STATE ARGS...: run the wizard with a pretend terminal (answers on stdin). Used as
# `printf 'x\n' | a STATE ...`; with `</dev/null` it plays an absent human.
a() {
    local st=$1
    shift
    AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" "$@"
}
new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
sget() { grep -m1 "^$2=" "$1/state" | cut -d= -f2-; }
done_step() { grep -q "^step_$2_done=" "$1/state"; }
status_of() { # STATE STEP-LABEL-REGEX
    AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$1" status 2>&1 </dev/null
}

# ================================================================== (a) no-compile path
SA=$(new_state a)
a "$SA" find-camera --ip 127.0.0.1 </dev/null >"$T/a0.out" 2>&1
check "a: find-camera is blocked before doctor and get-kit" bash -c '[ "$1" -ne 0 ] && grep -q "needs these steps first" "$2"' _ $? "$T/a0.out"
a "$SA" status >"$T/a_s0.out" 2>&1 </dev/null
check "a: at the start only doctor is todo, the rest blocked" bash -c 'grep -c "\[blocked" "$1" | grep -q "^[5-9]$" && grep -q "\[todo" "$1"' _ "$T/a_s0.out"

a "$SA" doctor >"$T/a1.out" 2>&1 </dev/null
check "a1 doctor: succeeds and is recorded" bash -c '[ "$1" -eq 0 ]' _ $?
check "a1 doctor: state has the step" done_step "$SA" doctor

a "$SA" get-kit --kit "$KIT" >"$T/a2.out" 2>&1 </dev/null
check "a2 get-kit (local kit): succeeds" bash -c '[ "$1" -eq 0 ]' _ $?
check "a2 get-kit: mode=kit and the kit hash recorded" bash -c '[ "$(grep -m1 ^mode= "$1/state" | cut -d= -f2)" = kit ] && grep -q "^kit_sha=" "$1/state"' _ "$SA"

AKAM_WIFI_PASS=$SENTINEL a "$SA" prepare-backup-sd --device /dev/sdb --ssid E2ENet </dev/null >"$T/a3x.out" 2>&1
check "a3 prepare-backup-sd: without a human typing ERASE nothing is erased" bash -c '[ "$1" -ne 0 ] && ! grep -q "^sd prepare" "$2"' _ $? "$STUB_LOG"
printf 'ERASE /dev/sdb\n' | AKAM_WIFI_PASS=$SENTINEL a "$SA" prepare-backup-sd --device /dev/sdb --ssid E2ENet >"$T/a3.out" 2>&1
check "a3 prepare-backup-sd: succeeds after the typed ERASE phrase" bash -c '[ "$1" -eq 0 ]' _ $?
check "a3 prepare-backup-sd: step recorded" done_step "$SA" prepare_backup_sd
check "a3 prepare-backup-sd: the card tool got the Wi-Fi password (delivery proven)" has "$CARD/backup/custom/configs/wifi.conf" "$SENTINEL"

printf '\n' | a "$SA" find-camera --ip 127.0.0.1 >"$T/a4.out" 2>&1
check "a4 find-camera: finds the stock camera with the toolkit shell" bash -c '[ "$1" -eq 0 ] && grep -q "stock firmware with the toolkit shell" "$2"' _ $? "$T/a4.out"
check "a4 find-camera: ip and kind recorded" bash -c '[ "$(grep -m1 ^camera_ip= "$1/state" | cut -d= -f2)" = 127.0.0.1 ] && [ "$(grep -m1 ^camera_kind= "$1/state" | cut -d= -f2)" = stock ]' _ "$SA"

: >"$CAM_A/ctl/commands.log"
printf 'y\n' | a "$SA" backup >"$T/a5.out" 2>&1
check "a5 backup: succeeds on the validated firmware" bash -c '[ "$1" -eq 0 ]' _ $?
BKDIR=$(sget "$SA" backup_dir)
check "a5 backup: eight files equal to the camera partitions, SHA256SUMS verifies" bash -c '
    for i in 0 1 2 3 4 5 6 7; do cmp -s "$1/mtdblock$i.bin" "$2/dev/mtdblock$i" || exit 1; done
    cd "$1" && sha256sum -c SHA256SUMS >/dev/null 2>&1' _ "$BKDIR" "$CAM_A"
check "a5 backup: step recorded, camera never rebooted" bash -c 'grep -q "^step_backup_done=" "$1/state" && [ ! -s "$2/ctl/events" ]' _ "$SA" "$CAM_A"

a "$SA" image --out "$T/img_a" >"$T/a6x.out" 2>&1 </dev/null
check "a6 image: validated hardware needs no typed phrase" bash -c '[ "$1" -eq 0 ]' _ $?
check "a6 image: assembled from the kit and the backup, image recorded" bash -c '[ "$(grep -m1 ^image_sha= "$1/state" | cut -d= -f2)" = "$2" ] && grep -q "^assemble .*--backup-dir $3 " "$4"' _ "$SA" "$IMG_SHA" "$BKDIR" "$STUB_LOG"
check "a6 image: step recorded" done_step "$SA" image

a "$SA" prepare-flash-sd --device /dev/sdb --pubkey "$PUB" --ssid E2ENet </dev/null >"$T/a7x.out" 2>&1
check "a7 prepare-flash-sd: refused without a typed ERASE" bash -c '[ "$1" -ne 0 ]' _ $?
printf 'ERASE /dev/sdb\n' | AKAM_WIFI_PASS=$SENTINEL a "$SA" prepare-flash-sd --device /dev/sdb --pubkey "$PUB" --ssid E2ENet >"$T/a7.out" 2>&1
check "a7 prepare-flash-sd: succeeds, card made for this image" bash -c '[ "$1" -eq 0 ] && [ "$(grep -m1 ^flash_sd_image_sha= "$2/state" | cut -d= -f2)" = "$3" ]' _ $? "$SA" "$IMG_SHA"
check "a7 prepare-flash-sd: image, flash_tool and backup are on the staged card" bash -c '[ -f "$1/flash-files/custom_firmware/$2" ] && [ -f "$1/flash-files/flash_tool" ] && [ -f "$1/flash-files/original_firmware/mtdblock7.bin" ]' _ "$CARD" "$IMG_NAME"

# the same camera, now with the flash card in it (second fake camera, same address)
fc_stop
fc_start --flash-card "$IMG" "$T/flash_tool" || { echo "cannot start fake camera B"; exit 1; }
CAM_B=$FC_ROOT
mkdir -p "$T/scripts"
cp "$CAM_B/tmp/sd/install_with_flash_tool.sh" "$CAM_B/tmp/sd/flash_guard.sh" "$T/scripts/"
export AKAM_FLASH_SCRIPTS_DIR=$T/scripts AKAM_FLASH_TOOL=$T/flash_tool
STOCK_B_MD5=$(host_md5 "$CAM_B/dev/mtd7")
CLOSED_TEL=$(fc_free_port)

: >"$CAM_B/ctl/confirm.log"
printf 'y\n' | a "$SA" flash >"$T/a8x.out" 2>&1
check "a8 flash: a human who types anything but YES flashes nothing" bash -c '[ "$1" -ne 0 ] && ! grep -q "CONFIRM=YES" "$2" && [ "$(md5sum <"$3" | cut -d" " -f1)" = "$4" ]' _ $? "$CAM_B/ctl/confirm.log" "$CAM_B/dev/mtd7" "$STOCK_B_MD5"
check "a8 flash: ... and the step stays undone" bash -c '! grep -q "^step_flash_done=" "$1/state"' _ "$SA"
printf 'YES\n' | a "$SA" flash >"$T/a8.out" 2>&1
check "a8 flash: succeeds over the maintenance telnet after a typed YES" bash -c '[ "$1" -eq 0 ]' _ $?
check "a8 flash: YES reached the camera exactly once, typed by the human" bash -c '[ "$(grep -c "CONFIRM=YES" "$1")" = 1 ]' _ "$CAM_B/ctl/confirm.log"
check "a8 flash: /dev/mtd7 equals the image, md5 recorded" bash -c 'cmp -s "$1" "$2" && grep -q "^flash_md5=$3$" "$4/state"' _ "$CAM_B/dev/mtd7" "$IMG" "$IMG_MD5" "$SA"
check "a8 flash: the camera was never rebooted by the wizard" test ! -s "$CAM_B/ctl/events"

# verify: fake ssh + listeners for the ports the real camera would show
P_SSH=$(fc_free_port)
P_RTSP=$(fc_free_port)
P_CTL=$(fc_free_port)
for p in "$P_SSH" "$P_RTSP" "$P_CTL"; do "$BB" nc -lk -p "$p" -e true >/dev/null 2>&1 & done
sleep 0.5
export AKAM_PORT_SSH=$P_SSH AKAM_PORT_RTSP=$P_RTSP AKAM_PORT_CTL=$P_CTL
AKAM_PORT_FTP=$(fc_free_port)
AKAM_PORT_TELNET23=$(fc_free_port)
export AKAM_PORT_FTP AKAM_PORT_TELNET23 AKAM_WAIT_SECS=10
printf '# fake hwcheck\n' >"$T/hwcheck.sh"
export AKAM_HWCHECK=$T/hwcheck.sh
printf '\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$SA" --identity "$T/key" verify >"$T/a9.out" 2>&1
check "a9 verify: succeeds (SSH, hwcheck, ports; telnet closed without the card)" bash -c '[ "$1" -eq 0 ]' _ $?
check "a9 verify: step recorded, the whole path is done" bash -c 'grep -q "^step_verify_done=" "$1/state"' _ "$SA"
a "$SA" status >"$T/a_s1.out" 2>&1 </dev/null
check "a: final status has nine done steps and nothing blocked or todo" bash -c '[ "$(grep -c "\[done" "$1")" = 9 ] && ! grep -q -e "\[todo" -e "\[blocked" -e "\[stale" "$1"' _ "$T/a_s1.out"

# ================================================================== (b) from source (rehearsal)
SB=$(new_state b)
# a rehearsal keeps its own state (state.dry): seed it with the steps before the build
printf 'step_doctor_done=2026-01-01T00:00:00Z\nstep_get_kit_done=2026-01-01T00:00:00Z\n' >"$SB/state.dry"
: >"$STUB_LOG"
a "$SB" --dry-run build </dev/null >"$T/b1.out" 2>&1
check "b build (rehearsal): shows bootstrap, make and the dropbear build as WOULD lines" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD run: bash .*bootstrap.sh" "$2" && grep -q "WOULD run: bash -c .*make -C src/ak_rtsp" "$2" && grep -q "WOULD run: bash .*build_dropbear.sh" "$2"' _ $? "$T/b1.out"
check "b build (rehearsal): nothing compiled, real state untouched" bash -c '! grep -q "^mode=source" "$1/state" 2>/dev/null' _ "$SB"
check "b build (rehearsal): the build tools profile was asked of the doctor script" has "$STUB_LOG" "check_host --profile build"
# the source path is also reachable by the state, with the same rules for the image step
SBI=$(new_state b2)
{
    for s in doctor get_kit prepare_backup_sd find_camera backup; do printf 'step_%s_done=2026-01-01T00:00:00Z\n' "$s"; done
    echo mode=source
    echo "backup_dir=$BKDIR"
    echo "backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
} >"$SBI/state.dry"
chmod 600 "$SBI/state.dry"
a "$SBI" --dry-run image </dev/null >"$T/b2.out" 2>&1
check "b image (rehearsal, source mode): uses build_firmware.sh on the unsquashed backup" bash -c '[ "$1" -eq 0 ] && grep -q "WOULD run: bash -c .*unsquashfs.*build_firmware.sh. --backup-dir" "$2"' _ $? "$T/b2.out"

# ================================================================== (c) update over SSH
SC=$(new_state c)
{
    for s in doctor get_kit prepare_backup_sd find_camera backup image prepare_flash_sd flash verify; do printf 'step_%s_done=2026-01-01T00:00:00Z\n' "$s"; done
    echo mode=kit
    echo camera_ip=127.0.0.1
    echo camera_kind=akam
} >"$SC/state"
NEWIMG=$T/new_image.squashfs
head -c 5242880 /dev/urandom >"$NEWIMG"
NEW_MD5=$(host_md5 "$NEWIMG")
: >"$STUBS/reflash.log"
printf 'y\n' | a "$SC" --identity "$T/key" --image "$NEWIMG" update >"$T/c1.out" 2>&1
check "c update: a typed 'y' is not a YES, the flash is never started" bash -c '[ "$1" -ne 0 ] && ! grep -q -e "--i-understand-no-uart" "$2"' _ $? "$STUBS/reflash.log"
: >"$STUBS/reflash.log"
printf 'YES\n' | a "$SC" --identity "$T/key" --image "$NEWIMG" update >"$T/c2.out" 2>&1
check "c update: reflash over SSH after a typed YES (precheck first)" bash -c '[ "$1" -eq 0 ] && sed -n 1p "$2" | grep -q -e "--precheck-only" && sed -n 2p "$2" | grep -q -e "--i-understand-no-uart"' _ $? "$STUBS/reflash.log"
check "c update: host, key and the md5 of the new image were passed" bash -c 'grep -q -e "--host 127.0.0.1" "$1" && grep -q -e "--md5 $2" "$1"' _ "$STUBS/reflash.log" "$NEW_MD5"
check "c update: the camera's read-back verdict is relayed" has "$T/c2.out" "read-back md5 verified"
check "c update: never rebooted by the wizard, no telnet used (no flash script on the card)" bash -c '[ ! -s "$1/ctl/events" ]' _ "$CAM_B"

# ================================================================== (d) restore
SD=$(new_state d)
{
    for s in doctor get_kit prepare_backup_sd find_camera backup image prepare_flash_sd; do printf 'step_%s_done=2026-01-01T00:00:00Z\n' "$s"; done
    echo mode=kit
    echo camera_ip=127.0.0.1
    echo camera_kind=stock
    echo camera_telnet=1
    echo "backup_dir=$BKDIR"
    echo "backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
    echo "image_path=$IMG"
    echo "image_sha=$IMG_SHA"
    echo "flash_sd_image_sha=$IMG_SHA"
} >"$SD/state"
: >"$CAM_B/ctl/confirm.log"
printf 'y\ny\ny\n' | AKAM_PORT_TELNET=$FC_PORT a "$SD" restore --backup-dir "$BKDIR" >"$T/d1.out" 2>&1
check "d restore: without the typed YES the APP is not touched" bash -c '[ "$1" -ne 0 ] && cmp -s "$2" "$3" && ! grep -q "CONFIRM=YES" "$4"' _ $? "$CAM_B/dev/mtd7" "$IMG" "$CAM_B/ctl/confirm.log"
# (the stock APP is already on the card after the refused try above: no replace question this time)
printf 'YES\n' | AKAM_PORT_TELNET=$FC_PORT a "$SD" restore --backup-dir "$BKDIR" >"$T/d2.out" 2>&1
check "d restore: succeeds after a typed YES" bash -c '[ "$1" -eq 0 ]' _ $?
check "d restore: /dev/mtd7 equals the backed up stock APP" bash -c 'cmp -s "$1" "$2"' _ "$CAM_B/dev/mtd7" "$BKDIR/mtdblock7.bin"
check "d restore: CONFIG untouched (said so), camera not rebooted, YES typed once" bash -c 'grep -q "CONFIG was not touched" "$1" && [ ! -s "$2/ctl/events" ] && [ "$(grep -c "CONFIRM=YES" "$2/ctl/confirm.log")" = 1 ]' _ "$T/d2.out" "$CAM_B"

# ================================================================== (e) cross-checks
# Every destructive step refuses without a human: empty stdin and no terminal.
: >"$STUB_LOG"
: >"$CAM_B/ctl/confirm.log"
SE=$(new_state e)
cp "$SD/state" "$SE/state"
bash "$AKAM" --no-color --state-dir "$SE" flash </dev/null >"$T/e1.out" 2>&1
check "e flash without a terminal: refused" test $? -ne 0
bash "$AKAM" --no-color --state-dir "$SE" restore --backup-dir "$BKDIR" </dev/null >"$T/e2.out" 2>&1
check "e restore without a terminal: refused" test $? -ne 0
bash "$AKAM" --no-color --state-dir "$SE" --identity "$T/key" --image "$NEWIMG" update </dev/null >"$T/e3.out" 2>&1
check "e update without a terminal: refused" test $? -ne 0
AKAM_WIFI_PASS=$SENTINEL bash "$AKAM" --no-color --state-dir "$SE" prepare-backup-sd --device /dev/sdb --ssid X </dev/null >"$T/e4.out" 2>&1
check "e prepare-backup-sd without a terminal: refused" test $? -ne 0
bash "$AKAM" --no-color --state-dir "$SE" wipe-sd --device /dev/sdb </dev/null >"$T/e5.out" 2>&1
check "e wipe-sd without a terminal: refused" test $? -ne 0
check "e none of them wrote anything: no YES at the camera, no SD erase/wipe, no new reflash" bash -c '
    ! grep -q "CONFIRM=YES" "$1" && ! grep -q -e "^sd prepare" -e "^sd wipe" "$2" && [ "$(grep -c -e "--i-understand-no-uart" "$3")" = 1 ]' _ "$CAM_B/ctl/confirm.log" "$STUB_LOG" "$STUBS/reflash.log"
# the wizard source never sends a YES by itself: it only ever forwards what the human typed
check "e the wizard code has no literal YES sent to a camera or a reflash tool" bash -c '
    ! grep -rn -E "(printf|echo)[^|]*YES[^|]*\|" "$1"/tools/wizard/lib/*.sh "$1"/akam' _ "$ROOT"
# nothing ever ran sudo (the SD tool is stubbed to its dry-run form, but the wizard must not call it)
check "e sudo was never run" bash -c '! grep -q "SUDO RAN" "$1"' _ "$STUB_LOG"

# ================================================================== secrets and files
leaks=0
for f in "$T"/st_*/state "$T"/st_*/akam.log "$T"/*.out "$STUB_LOG" "$STUBS/ssh.log" "$STUBS/reflash.log" "$CAM_A/ctl/commands.log" "$CAM_B/ctl/commands.log" "$CAM_B/ctl/confirm.log"; do
    [ -f "$f" ] || continue
    if grep -q "$SENTINEL" "$f"; then leaks=$((leaks + 1)); echo "  leak in $f"; fi
done
check "no Wi-Fi password in any state file, log, output or tool log (only the staged card has it)" test "$leaks" -eq 0
check "state directories are private (700), state and log files (600)" bash -c '
    for d in "$1"/st_*; do
        [ "$(stat -c %a "$d")" = 700 ] || exit 1
        for f in state akam.log; do [ ! -e "$d/$f" ] || [ "$(stat -c %a "$d/$f")" = 600 ] || exit 1; done
    done' _ "$T"
check "the wizard log recorded commands of the whole path" has "$SA/akam.log" "CMD"

echo
echo "wizard_e2e_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
