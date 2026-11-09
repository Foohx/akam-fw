#!/usr/bin/env bash
# shellcheck disable=SC2016  # commands for other shells are single-quoted on purpose
# Tests for the camera steps of ./akam (find-camera, backup, flash, verify, update, restore)
# against tools/wizard/fake_camera.sh on localhost, with fake ssh and reflash tools. The real camera
# is never contacted. The fake shell's transcripts are reconstructed from docs/akam_v1_backup_sd.md
# and docs/akam_v1_flash_and_restore.md, not recorded on hardware.
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

BB=$AKAM_BUSYBOX
export HOME=$T/home
mkdir -p "$HOME"
export AKAM_BACKUP_ROOT=$T/bk
export AKAM_XFER_CONNECT_TRIES=60
export AKAM_CAMSH_TIMEOUT=20
export NO_COLOR=1
mkdir -p "$AKAM_BACKUP_ROOT"
unset AKAM_STATE_DIR AKAM_SSH AKAM_REFLASH

# a STATE ARGS...: run the wizard with a real-terminal answer on stdin (AKAM_FORCE_TTY=1).
a() {
    local st=$1
    shift
    AKAM_FORCE_TTY=1 bash "$AKAM" --no-color --state-dir "$st" "$@"
}
new_state() { rm -rf "$T/st_$1"; mkdir -p "$T/st_$1"; chmod 700 "$T/st_$1"; echo "$T/st_$1"; }
mark() { printf 'step_%s_done=2026-01-01T00:00:00Z\n' "$@"; }

# ------------------------------------------------------------------ fake camera A (stock + toolkit card)
fc_start || { echo "cannot start the fake camera"; exit 1; }
CAM_A_ROOT=$FC_ROOT
CAM_A_PORT=$FC_PORT
APP_SHA=$(host_sha256 "$CAM_A_ROOT/dev/mtd7")
{
    grep -v -e '^#' -e '^validated' "$ROOT/tools/release/supported_hardware.txt"
    echo "validated|Fake camera|AK3918AV130|4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025|8 partitions; mtd7=APP 00500000|squashfs xz block=131072 bcj=none|$APP_SHA|1.30.1"
} >"$T/hw_validated.txt"
export AKAM_HW_LIST=$T/hw_validated.txt

# ------------------------------------------------------------------ find-camera
S=$(new_state find)
mark doctor prepare_backup_sd >"$S/state"
a "$S" --ip 127.0.0.1 find-camera </dev/null >"$T/find.out" 2>&1
check "find-camera: succeeds against the fake camera" test $? -eq 0
check "find-camera: shows the open ports and the Linux version" bash -c 'grep -q "Open ports: '"$CAM_A_PORT"'" "$1" && grep -q "Linux version" "$1"' _ "$T/find.out"
check "find-camera: identifies stock firmware with the toolkit shell" has "$T/find.out" "stock firmware with the toolkit shell"
check "find-camera: records ip and kind in the state" bash -c 'grep -q "^camera_ip=127.0.0.1$" "$1" && grep -q "^camera_kind=stock$" "$1" && grep -q "^step_find_camera_done=" "$1"' _ "$S/state"

S=$(new_state find2)
mark doctor prepare_backup_sd >"$S/state"
CLOSED=$(fc_free_port)
AKAM_PORT_TELNET=$CLOSED a "$S" --ip 127.0.0.1 find-camera </dev/null >"$T/find2.out" 2>&1
check "find-camera: a camera that does not answer fails with advice" bash -c '[ "$1" -ne 0 ] && grep -q "does not answer on any expected port" "$2"' _ $? "$T/find2.out"
check "find-camera: a failed probe does not mark the step" hasnt "$S/state" step_find_camera_done

S=$(new_state find3)
mark doctor prepare_backup_sd >"$S/state"
a "$S" --ip -oProxyCommand=x find-camera </dev/null >"$T/find3.out" 2>&1
check "find-camera: an address that looks like an option is refused" bash -c '[ "$1" -ne 0 ] && grep -q "not a valid address" "$2"' _ $? "$T/find3.out"

S=$(new_state find4)
mark doctor prepare_backup_sd >"$S/state"
before=$(fc_cmds | wc -l)
a "$S" --ip 127.0.0.1 --manual-telnet find-camera </dev/null >"$T/find4.out" 2>&1
check "find-camera --manual-telnet: sends nothing to the shell" bash -c '[ "$(wc -l <"$1")" = "$2" ]' _ "$CAM_A_ROOT/ctl/commands.log" "$before"

S=$(new_state find_ask)
mark doctor prepare_backup_sd >"$S/state"
printf '127.0.0.1\n' | a "$S" find-camera >"$T/find5.out" 2>&1
check "find-camera: asks for the address when none is given" bash -c 'grep -q "^camera_ip=127.0.0.1$" "$1"' _ "$S/state"

# ------------------------------------------------------------------ backup: layout refusal
S=$(new_state bk_layout)
{ mark doctor prepare_backup_sd find_camera; echo camera_ip=127.0.0.1; echo camera_kind=stock; echo camera_telnet=1; } >"$S/state"
cp "$CAM_A_ROOT/proc/mtd" "$T/mtd.orig"
sed -i 's/00500000 00001000 "APP"/00600000 00001000 "APP"/' "$CAM_A_ROOT/proc/mtd"
: >"$CAM_A_ROOT/ctl/commands.log"
printf 'y\n' | a "$S" backup >"$T/bk_layout.out" 2>&1
check "backup: a different MTD layout is refused" bash -c '[ "$1" -ne 0 ] && grep -q "not the supported 8-partition layout" "$2"' _ $? "$T/bk_layout.out"
check "backup: nothing is read from the flash when the layout differs" bash -c '! grep -q "dd if=" "$1"' _ "$CAM_A_ROOT/ctl/commands.log"
check "backup: layout refusal leaves the step undone" hasnt "$S/state" step_backup_done
cp "$T/mtd.orig" "$CAM_A_ROOT/proc/mtd"

# ------------------------------------------------------------------ backup: untested firmware needs the typed phrase
S=$(new_state bk_untested)
{ mark doctor prepare_backup_sd find_camera; echo camera_ip=127.0.0.1; echo camera_kind=stock; echo camera_telnet=1; } >"$S/state"
printf 'y\n' | AKAM_HW_LIST="$ROOT/tools/release/supported_hardware.txt" a "$S" backup >"$T/bk_unt.out" 2>&1
check "backup: untested firmware without the typed phrase fails" bash -c '[ "$1" -ne 0 ] && grep -q "untested firmware not confirmed" "$2"' _ $? "$T/bk_unt.out"
check "backup: that failure leaves the step undone" hasnt "$S/state" step_backup_done
check "backup: the verdict is shown" has "$T/bk_unt.out" "Hardware verdict for this stock APP: untested"

# ------------------------------------------------------------------ backup: the real thing (validated list)
S=$(new_state bk)
{ mark doctor prepare_backup_sd find_camera; echo camera_ip=127.0.0.1; echo camera_kind=stock; echo camera_telnet=1; } >"$S/state"
: >"$CAM_A_ROOT/ctl/commands.log"
t0=$SECONDS
printf 'y\n' | a "$S" backup >"$T/bk.out" 2>&1
rc=$?
echo "(backup took $((SECONDS - t0)) s)"
check "backup: succeeds on a validated firmware" test "$rc" -eq 0
BKDIR=$(sed -n 's/^backup_dir=//p' "$S/state")
check "backup: the folder is ~/akam-backup-<date> under the chosen root" bash -c '[[ "$1" == "$2"/akam-backup-* ]]' _ "$BKDIR" "$AKAM_BACKUP_ROOT"
check "backup: the 8 files equal the camera partitions" bash -c '
    for i in 0 1 2 3 4 5 6 7; do cmp -s "$1/mtdblock$i.bin" "$2/dev/mtdblock$i" || exit 1; done' _ "$BKDIR" "$CAM_A_ROOT"
check "backup: SHA256SUMS verifies" bash -c 'cd "$1" && sha256sum -c SHA256SUMS >/dev/null 2>&1 && [ "$(grep -c . SHA256SUMS)" = 8 ]' _ "$BKDIR"
check "backup: the two reads on the card are identical (READS_IDENTICAL)" has "$T/bk.out" "READS_IDENTICAL"
check "backup: both dumps exist on the camera's card" bash -c '
    for r in 1 2; do for i in 0 1 2 3 4 5 6 7; do cmp -s "$1/tmp/sd/dump$r/mtdblock$i.bin" "$1/dev/mtdblock$i" || exit 1; done; done' _ "$CAM_A_ROOT"
check "backup: layout, free space and kernel were checked" bash -c 'grep -q LAYOUT_OK "$1" && grep -q "Free space on the card" "$1" && grep -q "matches a validated entry" "$1"' _ "$T/bk.out"
check "backup: reminds of the second copy and asks for it" bash -c 'grep -q "SECOND MEDIUM" "$1"' _ "$T/bk.out"
check "backup: the step is done and records the folder" bash -c 'grep -q "^step_backup_done=" "$1" && grep -q "^backup_sha=" "$1"' _ "$S/state"
check "backup: the camera was never rebooted" test ! -s "$CAM_A_ROOT/ctl/events"
check "backup: no password-looking data in the log" hasnt "$S/akam.log" "PASSWORD"

# backup folder usable by the image step's own checks (same layout rules)
check "backup: no stray partition file in the folder" bash -c 'ls "$1"/mtdblock*.bin | wc -l | grep -q "^8$"' _ "$BKDIR"

# ------------------------------------------------------------------ backup: untested with the phrase
S=$(new_state bk_unt2)
{ mark doctor prepare_backup_sd find_camera; echo camera_ip=127.0.0.1; echo camera_kind=stock; echo camera_telnet=1; } >"$S/state"
printf 'I UNDERSTAND THIS IS UNTESTED\ny\n' | AKAM_HW_LIST="$ROOT/tools/release/supported_hardware.txt" a "$S" backup >"$T/bk_unt2.out" 2>&1
check "backup: untested firmware with the typed phrase goes on" bash -c 'grep -q "^step_backup_done=" "$1"' _ "$S/state"

# ------------------------------------------------------------------ backup of an Akam camera is refused
S=$(new_state bk_akam)
{ mark doctor prepare_backup_sd find_camera; echo camera_ip=127.0.0.1; echo camera_kind=akam; echo camera_telnet=1; } >"$S/state"
: >"$CAM_A_ROOT/ctl/commands.log"
a "$S" backup </dev/null >"$T/bk_akam.out" 2>&1
check "backup: refuses to 'back up' a camera that already runs Akam" bash -c '[ "$1" -ne 0 ] && grep -q "already runs Akam" "$2"' _ $? "$T/bk_akam.out"
check "backup: ...and sends nothing to it" bash -c '! test -s "$1"' _ "$CAM_A_ROOT/ctl/commands.log"

# ------------------------------------------------------------------ backup: --backup-copy
S=$(new_state bk_copy)
{ mark doctor prepare_backup_sd; } >"$S/state"
printf 'y\n' | a "$S" backup --backup-dir "$BKDIR" --backup-copy "$T/second" >"$T/bk_copy.out" 2>&1
check "backup --backup-copy: copies the folder and checks the copy" bash -c '[ -f "$1/mtdblock7.bin" ] && grep -q "Copy checked" "$2"' _ "$T/second" "$T/bk_copy.out"

# ------------------------------------------------------------------ fake camera B (stock + flash card)
head -c 5242880 /dev/urandom >"$T/ak_rtsp_firmware_20260101_000000.squashfs"
IMG=$T/ak_rtsp_firmware_20260101_000000.squashfs
IMG_MD5=$(host_md5 "$IMG")
IMG_SHA=$(host_sha256 "$IMG")
printf '#!/bin/sh\necho flash tool\n' >"$T/flash_tool"
fc_stop
fc_start --flash-card "$IMG" "$T/flash_tool" || { echo "cannot start fake camera B"; exit 1; }
CAM_B_ROOT=$FC_ROOT
# Scripts that "the card" carries: the fake ones, so that pushing is not needed in the main flow.
mkdir -p "$T/scripts"
cp "$CAM_B_ROOT/tmp/sd/install_with_flash_tool.sh" "$CAM_B_ROOT/tmp/sd/flash_guard.sh" "$T/scripts/"
export AKAM_FLASH_SCRIPTS_DIR=$T/scripts
export AKAM_FLASH_TOOL=$T/flash_tool
STOCK_MD5=$(host_md5 "$CAM_B_ROOT/dev/mtd7")

seed_flash_state() { # STATE_DIR KIND TELNET
    {
        mark doctor get_kit prepare_backup_sd find_camera backup image prepare_flash_sd
        echo mode=source
        echo camera_ip=127.0.0.1
        echo "camera_kind=$2"
        echo "camera_telnet=$3"
        echo "backup_dir=$BKDIR"
        echo "backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
        echo "image_path=$IMG"
        echo "image_sha=$IMG_SHA"
        echo image_kit_sha=source
        echo "image_backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
        echo "flash_sd_image_sha=$IMG_SHA"
    } >"$1/state"
}

# ------------------------------------------------------------------ flash over the maintenance telnet: YES gate
S=$(new_state fl_empty)
seed_flash_state "$S" stock 1
a "$S" flash </dev/null >"$T/fl_empty.out" 2>&1
check "flash: empty stdin (no human) fails" test $? -ne 0
check "flash: no YES was sent to the camera (empty stdin)" bash -c '! grep -q "CONFIRM=YES" "$1" 2>/dev/null' _ "$CAM_B_ROOT/ctl/confirm.log"
check "flash: the flash script was told to abort" bash -c 'grep -q "CONFIRM=NO" "$1"' _ "$CAM_B_ROOT/ctl/confirm.log"
check "flash: the partition is untouched after the refused flash" bash -c '[ "$(md5sum <"$1" | cut -d" " -f1)" = "$2" ]' _ "$CAM_B_ROOT/dev/mtd7" "$STOCK_MD5"
check "flash: the step stays undone" hasnt "$S/state" step_flash_done
check "flash: the camera md5 line was compared with the PC" has "$T/fl_empty.out" "same md5 as this PC"

: >"$CAM_B_ROOT/ctl/confirm.log"
S=$(new_state fl_notty)
seed_flash_state "$S" stock 1
bash "$AKAM" --no-color --state-dir "$S" flash </dev/null >"$T/fl_notty.out" 2>&1
check "flash: without a terminal it refuses" test $? -ne 0
check "flash: no YES sent without a terminal" bash -c '! grep -q "CONFIRM=YES" "$1" 2>/dev/null' _ "$CAM_B_ROOT/ctl/confirm.log"

S=$(new_state fl_wrong)
seed_flash_state "$S" stock 1
printf 'yes\n' | a "$S" flash >"$T/fl_wrong.out" 2>&1
check "flash: a typed 'yes' (not YES) is not accepted" test $? -ne 0
check "flash: nothing written after a wrong confirmation" bash -c '! grep -q "CONFIRM=YES" "$1" 2>/dev/null && [ "$(md5sum <"$2" | cut -d" " -f1)" = "$3" ]' _ "$CAM_B_ROOT/ctl/confirm.log" "$CAM_B_ROOT/dev/mtd7" "$STOCK_MD5"

# ------------------------------------------------------------------ flash over the maintenance telnet: success
S=$(new_state fl_ok)
seed_flash_state "$S" stock 1
printf 'YES\n' | a "$S" flash >"$T/fl_ok.out" 2>&1
check "flash: succeeds when the human types YES" test $? -eq 0
check "flash: the camera received YES exactly once, typed by the human" bash -c '[ "$(grep -c "CONFIRM=YES" "$1")" = 1 ]' _ "$CAM_B_ROOT/ctl/confirm.log"
check "flash: /dev/mtd7 now equals the image" bash -c 'cmp -s "$1" "$2"' _ "$CAM_B_ROOT/dev/mtd7" "$IMG"
check "flash: the read-back md5 is verified" has "$T/fl_ok.out" "Read-back md5 of /dev/mtd7 = image md5 ($IMG_MD5)"
check "flash: the card was bound on /mnt" test -L "$CAM_B_ROOT/mnt"
check "flash: tells the user to power-cycle by hand" has "$T/fl_ok.out" "power-cycle it BY HAND"
check "flash: nothing rebooted the camera" test ! -s "$CAM_B_ROOT/ctl/events"
check "flash: the step is done" bash -c 'grep -q "^step_flash_done=" "$1" && grep -q "^flash_md5=$2$" "$1"' _ "$S/state" "$IMG_MD5"
check "flash: the log contains no YES sent by the wizard" bash -c '! grep -q "CAM> YES" "$1"' _ "$S/akam.log"

check "flash: the script runs with HUP ignored (a dropped link must not kill the write)" bash -c 'grep -q "; trap .. HUP; sh /mnt/install_with_flash_tool.sh" "$1"' _ "$CAM_B_ROOT/ctl/commands.log"

# ------------------------------------------------------------------ flash: layout mismatch refused before any write
S=$(new_state flash_layout)
seed_flash_state "$S" stock 1
cp "$CAM_B_ROOT/proc/mtd" "$T/mtdB.orig"
sed -i 's/00500000 00001000 "APP"/00600000 00001000 "APP"/' "$CAM_B_ROOT/proc/mtd"
: >"$CAM_B_ROOT/ctl/confirm.log"
: >"$CAM_B_ROOT/ctl/commands.log"
printf 'YES\n' | a "$S" flash >"$T/fl_layout.out" 2>&1
check "flash: a different MTD layout is refused" bash -c '[ "$1" -ne 0 ] && grep -q "not the supported 8-partition layout" "$2"' _ $? "$T/fl_layout.out"
check "flash: layout refusal never starts the flash script" bash -c '! grep -q "install_with_flash_tool" "$1" && [ ! -s "$2" ]' _ "$CAM_B_ROOT/ctl/commands.log" "$CAM_B_ROOT/ctl/confirm.log"
cp "$T/mtdB.orig" "$CAM_B_ROOT/proc/mtd"

# ------------------------------------------------------------------ restore over telnet (pushes the stock APP)
S=$(new_state restore)
seed_flash_state "$S" stock 1
printf 'y\nYES\n' | a "$S" restore --backup-dir "$BKDIR" >"$T/restore.out" 2>&1
check "restore: succeeds" test $? -eq 0
check "restore: the stock APP was pushed to the card and verified" has "$T/restore.out" "Verified on the camera: /mnt/custom_firmware/"
check "restore: /dev/mtd7 equals the backup APP" bash -c 'cmp -s "$1" "$2"' _ "$CAM_B_ROOT/dev/mtd7" "$BKDIR/mtdblock7.bin"
check "restore: exactly one image stays on the card" bash -c '[ "$(ls "$1"/tmp/sd/custom_firmware | wc -l)" = 1 ]' _ "$CAM_B_ROOT"
check "restore: tells that CONFIG was not touched" has "$T/restore.out" "CONFIG was not touched"
check "restore: nothing rebooted the camera" test ! -s "$CAM_B_ROOT/ctl/events"

# ------------------------------------------------------------------ flash: image on the card differs -> pushed after a yes
S=$(new_state fl_push)
seed_flash_state "$S" stock 1
printf 'y\nYES\n' | a "$S" flash >"$T/fl_push.out" 2>&1
check "flash: a different image on the card is replaced only after a yes, then flashed" bash -c 'grep -q "Replace the image on the card" "$1"' _ "$T/fl_push.out"
check "flash (after push): /dev/mtd7 equals the image" bash -c 'cmp -s "$1" "$2"' _ "$CAM_B_ROOT/dev/mtd7" "$IMG"

S=$(new_state fl_push_no)
seed_flash_state "$S" stock 1
printf 'n\n' | a "$S" restore --backup-dir "$BKDIR" >"$T/fl_push_no.out" 2>&1
check "restore: refusing to replace the image on the card stops before any flash" bash -c '[ "$1" -ne 0 ] && cmp -s "$2" "$3"' _ $? "$CAM_B_ROOT/dev/mtd7" "$IMG"

# ------------------------------------------------------------------ SSH routes (fake ssh and fake reflash tool)
mkdir -p "$T/stubs"
cat >"$T/stubs/ssh" <<'EOF'
#!/usr/bin/env bash
# fake ssh: logs, answers `true`, stores the stdin of `cat > FILE`, fakes akam_hwcheck.sh
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
    "cat > /tmp/akam_hwcheck.sh") cat >"$STUB_DIR/hwcheck.copy"; exit 0 ;;
    "/usr/sbin/akam_rtsp_auth.sh status") echo "${STUB_RTSP_STATUS:-disabled}"; exit 0 ;;
    "/usr/sbin/akam_rtsp_auth.sh set-hash") cat >"$STUB_DIR/rtsp_auth.stdin"; exit 0 ;;
    "sh /tmp/akam_hwcheck.sh")
        echo "PASS port 22 listening"
        echo "PASS port 554 listening"
        [ "${STUB_HWCHECK_RC:-0}" = 0 ] || echo "FAIL port 24 listening: must be closed"
        exit "${STUB_HWCHECK_RC:-0}"
        ;;
esac
exit 0
EOF
cat >"$T/stubs/reflash.sh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$STUB_DIR/reflash.log"
echo "reflash_over_ssh: pre-checks passed"
i=0
while [ $i -lt 300 ]; do printf '\r[#######] %d%% PROGRESS_JUNK' "$i"; i=$((i + 1)); done
echo
case " $* " in
    *" --i-understand-no-uart "*) echo "reflash_over_ssh: read-back md5 verified"; exit "${STUB_REFLASH_RC:-0}" ;;
esac
exit "${STUB_PRECHECK_RC:-0}"
EOF
chmod +x "$T/stubs/ssh" "$T/stubs/reflash.sh"
export STUB_DIR=$T/stubs
export AKAM_SSH=$T/stubs/ssh AKAM_REFLASH=$T/stubs/reflash.sh
printf 'fake key\n' >"$T/key"
CLOSED_TEL=$(fc_free_port)

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_ok)
seed_flash_state "$S" akam 0
printf 'y\nYES\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash >"$T/ssh_ok.out" 2>&1
check "flash over SSH: succeeds with a typed YES" test $? -eq 0
check "flash over SSH: precheck first, then the flash with the no-UART flag" bash -c '
    sed -n 1p "$1" | grep -q -e "--precheck-only" && sed -n 2p "$1" | grep -q -e "--i-understand-no-uart"' _ "$STUB_DIR/reflash.log"
check "flash over SSH: passes host, key, md5 and flash_tool" bash -c 'grep -q -e "--host 127.0.0.1" -e "--identity '"$T"'/key" "$1" && grep -q -e "--md5 '"$IMG_MD5"'" "$1"' _ "$STUB_DIR/reflash.log"
check "flash over SSH: the progress bars are filtered from the output" hasnt "$T/ssh_ok.out" PROGRESS_JUNK
check "flash over SSH: the full output is kept in a log" has "$S/reflash.log" PROGRESS_JUNK
check "flash over SSH: the camera's verdict is relayed" has "$T/ssh_ok.out" "read-back md5 verified"
check "flash over SSH: the step is done and the route recorded" bash -c 'grep -q "^step_flash_done=" "$1" && grep -q "^flash_route=ssh$" "$1"' _ "$S/state"

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_noyes)
seed_flash_state "$S" akam 0
printf 'y\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash >"$T/ssh_noyes.out" 2>&1
check "flash over SSH: without a typed YES the flash is never started" bash -c '[ "$1" -ne 0 ] && ! grep -q -e "--i-understand-no-uart" "$2"' _ $? "$STUB_DIR/reflash.log"

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_empty)
seed_flash_state "$S" akam 0
AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash </dev/null >"$T/ssh_empty.out" 2>&1
check "flash over SSH: empty stdin fails and starts no flash" bash -c '[ "$1" -ne 0 ] && ! grep -q -e "--i-understand-no-uart" "$2"' _ $? "$STUB_DIR/reflash.log"

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_bad)
seed_flash_state "$S" akam 0
printf 'y\nYES\n' | STUB_REFLASH_RC=1 AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash >"$T/ssh_bad.out" 2>&1
check "flash over SSH: a failing reflash is reported and the step stays undone" bash -c '[ "$1" -ne 0 ] && grep -q "DO NOT power-cycle" "$2"' _ $? "$T/ssh_bad.out"
check "flash over SSH: ...and the step is not marked" hasnt "$S/state" step_flash_done

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_nokey)
seed_flash_state "$S" akam 0
printf 'y\nYES\n' | STUB_SSH_RC=255 AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash >"$T/ssh_nokey.out" 2>&1
check "flash: Akam running but SSH refused and telnet closed gives clear advice" bash -c '[ "$1" -ne 0 ] && grep -q "no way in" "$2"' _ $? "$T/ssh_nokey.out"

: >"$STUB_DIR/reflash.log"
S=$(new_state ssh_pre)
seed_flash_state "$S" akam 0
printf 'y\nYES\n' | STUB_PRECHECK_RC=1 AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" flash >"$T/ssh_pre.out" 2>&1
check "flash over SSH: a failing precheck stops before the typed YES is even asked" bash -c '[ "$1" -ne 0 ] && grep -q "the precheck failed" "$2" && ! grep -q -e "--i-understand-no-uart" "$3"' _ $? "$T/ssh_pre.out" "$STUB_DIR/reflash.log"

# update
: >"$STUB_DIR/reflash.log"
S=$(new_state upd)
printf 'camera_ip=127.0.0.1\n' >"$S/state"
printf 'YES\n' | a "$S" --identity "$T/key" --image "$IMG" update >"$T/upd.out" 2>&1
check "update: reflashes over SSH with a typed YES" bash -c '[ "$1" -eq 0 ] && grep -q -e "--i-understand-no-uart" "$2"' _ $? "$STUB_DIR/reflash.log"
: >"$STUB_DIR/reflash.log"
a "$S" --identity "$T/key" --image "$IMG" update </dev/null >"$T/upd2.out" 2>&1
check "update: empty stdin fails and starts no flash" bash -c '[ "$1" -ne 0 ] && ! grep -q -e "--i-understand-no-uart" "$2"' _ $? "$STUB_DIR/reflash.log"
head -c 1000 /dev/urandom >"$T/short.img"
printf 'YES\n' | a "$S" --identity "$T/key" --image "$T/short.img" update >"$T/upd3.out" 2>&1
check "update: an image of the wrong size is refused" bash -c '[ "$1" -ne 0 ] && grep -q "expected exactly 5242880" "$2"' _ $? "$T/upd3.out"

# ------------------------------------------------------------------ verify (fake ssh + listeners)
P_SSH=$(fc_free_port)
P_RTSP=$(fc_free_port)
P_CTL=$(fc_free_port)
for p in "$P_SSH" "$P_RTSP" "$P_CTL"; do "$BB" nc -lk -p "$p" -e true >/dev/null 2>&1 & done
sleep 0.5
export AKAM_PORT_SSH=$P_SSH AKAM_PORT_RTSP=$P_RTSP AKAM_PORT_CTL=$P_CTL
AKAM_PORT_FTP=$(fc_free_port)
AKAM_PORT_TELNET23=$(fc_free_port)
export AKAM_PORT_FTP AKAM_PORT_TELNET23
export AKAM_WAIT_SECS=10
printf '# fake hwcheck\n' >"$T/hwcheck.sh"
export AKAM_HWCHECK=$T/hwcheck.sh

seed_verify_state() {
    { mark doctor get_kit prepare_backup_sd find_camera backup image prepare_flash_sd flash; echo mode=source; echo camera_ip=127.0.0.1; echo camera_kind=akam
      echo "backup_dir=$BKDIR"; echo "backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
      echo "image_path=$IMG"; echo "image_sha=$IMG_SHA"; echo image_kit_sha=source; echo "image_backup_sha=$(host_sha256 "$BKDIR/mtdblock7.bin")"
      echo "flash_sd_image_sha=$IMG_SHA"; } >"$1/state"
}
S=$(new_state ver_ok)
seed_verify_state "$S"
printf '\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_ok.out" 2>&1
check "verify: succeeds when SSH works, hwcheck passes and the ports are right" test $? -eq 0
check "verify: hwcheck ran on the camera (copied to RAM then executed)" bash -c 'grep -q "fake hwcheck" "$1/hwcheck.copy" && grep -q "sh /tmp/akam_hwcheck.sh" "$1/ssh.log"' _ "$STUB_DIR"
check "verify: guides the card removal and the power cycle by hand" bash -c 'grep -q "take the microSD card out" "$1" && grep -q "plug the power back in" "$1"' _ "$T/ver_ok.out"
check "verify: checks the ports twice" bash -c '[ "$(grep -c "Open ports:" "$1")" = 2 ]' _ "$T/ver_ok.out"
check "verify: step done" bash -c 'grep -q "^step_verify_done=" "$1"' _ "$S/state"

S=$(new_state ver_telnet)
seed_verify_state "$S"
printf '\n' | AKAM_PORT_TELNET=$FC_PORT a "$S" --identity "$T/key" verify >"$T/ver_tel.out" 2>&1
check "verify: telnet still open without the card fails the step" bash -c '[ "$1" -ne 0 ] && grep -q "should be closed" "$2"' _ $? "$T/ver_tel.out"
check "verify: ...which stays undone" hasnt "$S/state" step_verify_done

S=$(new_state ver_nossh)
seed_verify_state "$S"
printf '\n' | STUB_SSH_RC=255 AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_nossh.out" 2>&1
check "verify: SSH refused fails the step and does not invite to remove the card" bash -c '[ "$1" -ne 0 ] && grep -q "SSH with" "$2" && ! grep -q "take the microSD card out" "$2"' _ $? "$T/ver_nossh.out"

S=$(new_state ver_hw)
seed_verify_state "$S"
printf '\n' | STUB_HWCHECK_RC=1 AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_hw.out" 2>&1
check "verify: a hwcheck failure fails the step" bash -c '[ "$1" -ne 0 ] && grep -q "reported a failure" "$2"' _ $? "$T/ver_hw.out"

S=$(new_state ver_empty)
seed_verify_state "$S"
AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify </dev/null >"$T/ver_empty.out" 2>&1
check "verify: without anyone to unplug the card the step is not done" bash -c '[ "$1" -ne 0 ]' _ $?
check "verify: ...and stays undone" hasnt "$S/state" step_verify_done

# verify ends by offering a password on the video stream: default No, never mandatory
rm -f "$STUB_DIR/rtsp_auth.stdin"
S=$(new_state ver_rtsp_no)
seed_verify_state "$S"
printf '\n\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_rtsp_no.out" 2>&1
check "verify: asks whether to protect the video stream" has "$T/ver_rtsp_no.out" "Protect the video stream with a username/password? [y/N]"
check "verify: an empty answer means No (nothing sent to the camera)" test ! -e "$STUB_DIR/rtsp_auth.stdin"
check "verify: the step is done whatever the answer" bash -c 'grep -q "^step_verify_done=" "$1"' _ "$S/state"

S=$(new_state ver_rtsp_yes)
seed_verify_state "$S"
printf '\ny\nviewer\nS3NTINEL-pw-1\nS3NTINEL-pw-1\n' | AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_rtsp_yes.out" 2>&1
check "verify: answering y sets the password" test $? -eq 0
EXPECT_HA1=$(printf '%s' 'viewer:akam:S3NTINEL-pw-1' | md5sum | cut -d' ' -f1)
check "verify: only user:akam:HA1 is sent, on stdin" test "$(cat "$STUB_DIR/rtsp_auth.stdin" 2>/dev/null)" = "viewer:akam:$EXPECT_HA1"
check "verify: the password never reaches ssh's command line" bash -c '! grep -q -e S3NTINEL -e "$2" "$1"' _ "$STUB_DIR/ssh.log" "$EXPECT_HA1"
check "verify: the password is not in the output" hasnt "$T/ver_rtsp_yes.out" S3NTINEL
check "verify: the HA1 is not in the output" hasnt "$T/ver_rtsp_yes.out" "$EXPECT_HA1"
check "verify: the password and the HA1 are not in the wizard log" bash -c '! grep -q -e S3NTINEL -e "$2" "$1"' _ "$S/akam.log" "$EXPECT_HA1"
check "verify: the URL hint never contains the password" has "$T/ver_rtsp_yes.out" "rtsp://viewer:PASSWORD@127.0.0.1:"

rm -f "$STUB_DIR/rtsp_auth.stdin"
S=$(new_state ver_rtsp_on)
seed_verify_state "$S"
printf '\ny\n' | STUB_RTSP_STATUS="enabled (user bob)" AKAM_PORT_TELNET=$CLOSED_TEL a "$S" --identity "$T/key" verify >"$T/ver_rtsp_on.out" 2>&1
check "verify: when a password is already set, it is not asked again" bash -c '! grep -q "Protect the video stream" "$1" && grep -q "Already on" "$1"' _ "$T/ver_rtsp_on.out"

# ------------------------------------------------------------------ dry-run: nothing is contacted
S=$(new_state dry)
mark doctor get_kit prepare_backup_sd image prepare_flash_sd >"$S/state.dry"
before=$(wc -l <"$CAM_B_ROOT/ctl/commands.log")
rf_before=$(wc -l <"$STUB_DIR/reflash.log")
for step in find-camera backup flash verify; do
    a "$S" --dry-run step "$step" </dev/null >"$T/dry_$step.out" 2>&1
    check "dry-run $step: succeeds" test $? -eq 0
done
a "$S" --dry-run --image "$IMG" update </dev/null >"$T/dry_update.out" 2>&1
check "dry-run update: succeeds" test $? -eq 0
a "$S" --dry-run restore </dev/null >"$T/dry_restore.out" 2>&1
check "dry-run restore: succeeds" test $? -eq 0
check "dry-run: shows WOULD lines for the camera steps" bash -c 'for s in find-camera backup flash verify update restore; do grep -q "WOULD" "$1/dry_$s.out" || exit 1; done' _ "$T"
check "dry-run: the camera received nothing" bash -c '[ "$(wc -l <"$1")" = "$2" ]' _ "$CAM_B_ROOT/ctl/commands.log" "$before"
check "dry-run: the reflash tool was not called" bash -c '[ "$(wc -l <"$1/reflash.log")" = "$2" ]' _ "$STUB_DIR" "$rf_before"
check "dry-run verify: shows the password question and sends nothing" bash -c 'grep -q "WOULD ask: Protect the video stream with a username/password? \[y/N\] (default: No)" "$1" && grep -q "WOULD run: ssh root@[^ ]* /usr/sbin/akam_rtsp_auth.sh set-hash" "$1"' _ "$T/dry_verify.out"
check "dry-run flash: asks for the typed YES (shown, never answered)" has "$T/dry_flash.out" "WOULD ask: Write the camera flash now? (type: YES)"

echo "wizard_camera_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
