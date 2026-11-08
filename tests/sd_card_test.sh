#!/usr/bin/env bash
# Host tests for tools/sd/sd_card.sh: fake lsblk + fake sysfs on PATH/AKAM_SYSFS, dry-run only.
# No real device is ever opened, wiped or mounted.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../tools/sd/sd_card.sh
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "  ok   - $d"; else FAIL=$((FAIL + 1)); echo "  FAIL - $d"; fi; }
refused() { ! "$@"; }

SENT="SeNtInEl-Pw"
# ------------------------------------------------------------ fake lsblk / sysfs
FB=$WORK/bin
FD=$WORK/lsblk
SYS=$WORK/sys
mkdir -p "$FB" "$FD" "$SYS/class/block"
cat >"$FB/lsblk" <<'EOF'
#!/usr/bin/env bash
# Fake lsblk: -d lists whole disks (optionally one), otherwise the tree of the last argument.
last=${*: -1}
case " $* " in
    *" -dpP "*)
        if [ "${last#/dev/}" != "$last" ]; then grep -F "NAME=\"$last\"" "$FAKE_LSBLK_DIR/disks.txt"; else cat "$FAKE_LSBLK_DIR/disks.txt"; fi ;;
    *) cat "$FAKE_LSBLK_DIR/tree_$(basename "$last").txt" 2>/dev/null || exit 1 ;;
esac
EOF
chmod +x "$FB/lsblk"
cat >"$FD/disks.txt" <<'EOF'
NAME="/dev/sda" TYPE="disk" RM="0" TRAN="sata" MODEL="Samsung\x20SSD"
NAME="/dev/sdb" TYPE="disk" RM="1" TRAN="usb" MODEL="Card\x20Reader"
NAME="/dev/sdc" TYPE="disk" RM="1" TRAN="usb" MODEL="Home\x20Disk"
NAME="/dev/sdd" TYPE="disk" RM="1" TRAN="usb" MODEL="Huge\x20Disk"
NAME="/dev/sde" TYPE="disk" RM="1" TRAN="usb" MODEL="Empty\x20Reader"
NAME="/dev/sdf" TYPE="disk" RM="1" TRAN="usb" MODEL="Swap\x20Stick"
NAME="/dev/sdg" TYPE="disk" RM="1" TRAN="" MODEL="Odd"
NAME="/dev/mmcblk0" TYPE="disk" RM="0" TRAN="" MODEL=""
NAME="/dev/mmcblk1" TYPE="disk" RM="0" TRAN="" MODEL=""
EOF
tree() { printf '%s\n' "$2" >"$FD/tree_$1.txt"; }
tree sda 'NAME="/dev/sda" TYPE="disk" MOUNTPOINT=""
NAME="/dev/sda1" TYPE="part" MOUNTPOINT="/"'
tree sdb 'NAME="/dev/sdb" TYPE="disk" MOUNTPOINT=""
NAME="/dev/sdb1" TYPE="part" MOUNTPOINT="/run/media/u/AKAMSD"'
tree sdc 'NAME="/dev/sdc" TYPE="disk" MOUNTPOINT=""
NAME="/dev/sdc1" TYPE="part" MOUNTPOINT="/home"'
tree sdd 'NAME="/dev/sdd" TYPE="disk" MOUNTPOINT=""'
tree sde 'NAME="/dev/sde" TYPE="disk" MOUNTPOINT=""'
tree sdf 'NAME="/dev/sdf" TYPE="disk" MOUNTPOINT=""
NAME="/dev/sdf1" TYPE="part" MOUNTPOINT="[SWAP]"'
tree sdg 'NAME="/dev/sdg" TYPE="disk" MOUNTPOINT=""'
tree mmcblk0 'NAME="/dev/mmcblk0" TYPE="disk" MOUNTPOINT=""
NAME="/dev/mmcblk0p1" TYPE="part" MOUNTPOINT=""'
tree mmcblk1 'NAME="/dev/mmcblk1" TYPE="disk" MOUNTPOINT=""
NAME="/dev/mmcblk1p2" TYPE="part" MOUNTPOINT="/boot"'
sysd() { mkdir -p "$SYS/class/block/$1"; echo "$2" >"$SYS/class/block/$1/size"; }
sysd sda 976773168
sysd sdb 7744512      # ~3.7 GB
sysd sdc 7744512
sysd sdd 536870912    # 256 GiB
sysd sde 0
sysd sdf 7744512
sysd sdg 7744512
sysd mmcblk0 15523840 # ~7.4 GiB
sysd mmcblk1 15523840
mkdir -p "$SYS/class/block/mmcblk0/device" "$SYS/class/block/mmcblk1/device"
echo SD >"$SYS/class/block/mmcblk0/device/type"
echo MMC >"$SYS/class/block/mmcblk1/device/type"

export FAKE_LSBLK_DIR=$FD AKAM_SYSFS=$SYS
export PATH=$FB:$PATH
unset AKAM_FORCE_TTY AKAM_WIFI_SSID AKAM_WIFI_PASS AKAM_SD_SSID AKAM_SD_PASS

# ------------------------------------------------------------ synthetic toolkit / key
TK=$WORK/toolkit
mkdir -p "$TK/sd_card/custom/scripts" "$TK/sd_card/custom/configs" "$TK/sd_card/custom/state"
printf '#!/bin/sh\necho hack' >"$TK/sd_card/hack.sh"
printf 'TELNET=0\nTELNET_PORT=23\nOFFLINE=0\nONVIF=1\n' >"$TK/sd_card/custom/configs/hack.conf"
printf '#!/bin/sh\necho entry\n' >"$TK/sd_card/custom/scripts/entrypoint.sh"
printf '#!/bin/sh\necho other\n' >"$TK/sd_card/custom/scripts/other.sh"
: >"$TK/sd_card/custom/state/shadow.done"
HASH=$( (cd "$TK/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1)
KEY=$WORK/k.pub
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq akam-test' >"$KEY"

OUT=$WORK/all.log # every output of every run, grepped for the secret at the end
: >"$OUT"
sd() { # sd <args...> ; synthetic pins are only honoured with --dry-run (script rule)
    AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=$HASH "$SCRIPT" "$@" >"$WORK/o" 2>&1 </dev/null
    local rc=$?
    cat "$WORK/o" >>"$OUT"
    return $rc
}
plain() { "$SCRIPT" "$@" >"$WORK/o" 2>&1 </dev/null; local rc=$?; cat "$WORK/o" >>"$OUT"; return $rc; }
has() { grep -q -- "$1" "$WORK/o"; }

# ------------------------------------------------------------ list-candidates
plain list-candidates; LIST=$(cat "$WORK/o")
check "list: USB card reader listed" grep -q '^/dev/sdb	' <<<"$LIST"
check "list: SD reader on mmcblk0 listed" grep -q '^/dev/mmcblk0	' <<<"$LIST"
for d in sda sdc sdd sde sdf sdg mmcblk1; do
    check "list: /dev/$d excluded" bash -c "! grep -q '^/dev/$d	' <<<'$LIST'"
done
check "list: 5 TSV fields, size from sysfs, model unescaped, mounts" \
    grep -qxF "/dev/sdb	3965190144	Card Reader	usb	/dev/sdb1:/run/media/u/AKAMSD" <<<"$LIST"
check "list: --human works" plain list-candidates --human

# ------------------------------------------------------------ refusals
for d in sda sdc sdd sde sdf sdg mmcblk1; do
    check "dry-run refuses /dev/$d" refused sd prepare --mode backup --device "/dev/$d" --dry-run --stage-dir "$WORK/r_$d" --toolkit "$TK"
done
check "refusal message names the reason (system path)" bash -c "AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=$HASH '$SCRIPT' prepare --mode backup --device /dev/sdc --dry-run --stage-dir '$WORK/r_x' --toolkit '$TK' 2>&1 | grep -q 'carries the system path /home'"
check "refuses a partition path" refused sd prepare --mode backup --device /dev/sdb1 --dry-run --stage-dir "$WORK/r_p" --toolkit "$TK"
check "refuses mmcblk0p1" refused sd prepare --mode backup --device /dev/mmcblk0p1 --dry-run --stage-dir "$WORK/r_p2" --toolkit "$TK"
check "refuses nvme" refused sd prepare --mode backup --device /dev/nvme0n1 --dry-run --stage-dir "$WORK/r_n" --toolkit "$TK"
check "refuses an unknown mode" refused sd prepare --mode weird --device /dev/sdb --dry-run --stage-dir "$WORK/r_m"
check "wipe refuses the internal disk" refused sd wipe --device /dev/sda --dry-run
check "wipe refuses the swap stick" refused sd wipe --device /dev/sdf --dry-run
check "real prepare refuses without a TTY" refused plain prepare --mode backup --device /dev/sdb --toolkit "$TK"
check "  ... and says why" has "without a terminal"
check "real wipe refuses without a TTY" refused plain wipe --device /dev/sdb
check "  ... and says why" has "without a terminal"
check "wipe needs a device" refused plain wipe
check "AKAM_TEST_DRYRUN_ONLY refused on a real run" refused sd prepare --mode backup --device /dev/sdb --toolkit "$TK"

# typed confirmation, exercised on the function itself (a real run needs a real device)
confirm() { # <typed answer>
    AKAM_FORCE_TTY=1 bash -c ". '$SCRIPT'; confirm_erase /dev/sdb" <<<"$1" >/dev/null 2>&1
}
check "ERASE <device> accepted" confirm "ERASE /dev/sdb"
check "YES is not enough" refused confirm "YES"
check "ERASE without device is not enough" refused confirm "ERASE"
check "wrong device refused" refused confirm "ERASE /dev/sdc"
check "no TTY refused by confirm" refused bash -c ". '$SCRIPT'; confirm_erase /dev/sdb" </dev/null >/dev/null 2>&1
check "partition naming sdX / mmcblkN" bash -c ". '$SCRIPT'; [ \"\$(be_part_path /dev/sdb)\" = /dev/sdb1 ] && [ \"\$(be_part_path /dev/mmcblk0)\" = /dev/mmcblk0p1 ]"

# ------------------------------------------------------------ backup card (dry-run)
export AKAM_WIFI_SSID="net'\$HOME\`id\`\"x" AKAM_WIFI_PASS="${SENT}'\$(touch $WORK/pwned)\`\"\\"
check "backup dry-run on /dev/sdb" sd prepare --mode backup --device /dev/sdb --dry-run --stage-dir "$WORK/b" --toolkit "$TK"
B=$WORK/b
check "backup: hack.conf telnet 24 / offline" bash -c "grep -qx TELNET=1 '$B/custom/configs/hack.conf' && grep -qx TELNET_PORT=24 '$B/custom/configs/hack.conf' && grep -qx OFFLINE=1 '$B/custom/configs/hack.conf'"
check "backup: dump.done, no shadow.done" bash -c "test -f '$B/custom/state/dump.done' && test ! -e '$B/custom/state/shadow.done'"
check "backup: lock marker + hook on its own line (hack.sh had no newline)" bash -c "grep -qx '# DECLOUD_ENTRYPOINT_LOCK' '$B/custom/scripts/entrypoint.sh' && grep -qx '# DECLOUD_WIZARD_HOOK' '$B/hack.sh' && head -n1 '$B/hack.sh' | grep -qx 'echo hack\\|#!/bin/sh'"
check "backup: no AKAM_MAINTENANCE" test ! -e "$B/AKAM_MAINTENANCE"
check "backup: no *.bin at the root" bash -c "! find '$B' -maxdepth 1 -iname '*.bin' | grep -q ."
# shellcheck disable=SC1091
wifi_ok() { ( . "$1/custom/configs/wifi.conf" && [ "$SSID" = "$AKAM_WIFI_SSID" ] && [ "$PASS" = "$AKAM_WIFI_PASS" ] ); }
check "wifi.conf sources back to the exact SSID/password (quotes, \$, backtick, backslash)" wifi_ok "$B"
check "wifi.conf values were not executed" test ! -e "$WORK/pwned"
check "verify (stage dir, with toolkit hashes) passes" sd verify --mode backup --stage-dir "$B" --toolkit "$TK"
check "verify prints VERIFY: OK" has "VERIFY: OK"
check "verify without toolkit passes (hash step skipped)" sd verify --mode backup --stage-dir "$B"

# ------------------------------------------------------------ first-boot card
check "first-boot dry-run on mmcblk0" sd prepare --mode first-boot --device /dev/mmcblk0 --pubkey "$KEY" --dry-run --stage-dir "$WORK/f" --toolkit "$TK"
F=$WORK/f
check "first-boot: AKAM_MAINTENANCE empty" bash -c "test -f '$F/AKAM_MAINTENANCE' -a ! -s '$F/AKAM_MAINTENANCE'"
check "first-boot: authorized_keys copied" cmp -s "$KEY" "$F/akam/authorized_keys"
check "first-boot verify passes" sd verify --mode first-boot --stage-dir "$F" --toolkit "$TK"
check "backup verify rejects a first-boot tree" refused sd verify --mode backup --stage-dir "$F"
check "first-boot verify rejects a backup tree" refused sd verify --mode first-boot --stage-dir "$B"
check "first-boot needs --pubkey" refused sd prepare --mode first-boot --dry-run --stage-dir "$WORK/f2" --toolkit "$TK"
printf 'ssh-rsa AAAAB3NzaC1yc2ESYNTHETIC x\n' >"$WORK/rsa.pub"
check "RSA key refused" refused sd prepare --mode first-boot --pubkey "$WORK/rsa.pub" --dry-run --stage-dir "$WORK/f3" --toolkit "$TK"
check "wrong tree hash refused" refused bash -c "AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=deadbeef '$SCRIPT' prepare --mode backup --dry-run --stage-dir '$WORK/h' --toolkit '$TK' >/dev/null 2>&1"
check "non-empty stage dir refused" refused sd prepare --mode backup --dry-run --stage-dir "$B" --toolkit "$TK"

# tampering detected by verify
cp -a "$F" "$WORK/t1"
echo x >>"$WORK/t1/custom/scripts/other.sh"
check "verify catches a modified toolkit file" refused sd verify --mode first-boot --stage-dir "$WORK/t1" --toolkit "$TK"
cp -a "$F" "$WORK/t2"
: >"$WORK/t2/EVIL.BIN"
check "verify catches a root *.bin" refused sd verify --mode first-boot --stage-dir "$WORK/t2"
cp -a "$F" "$WORK/t3"
sed -i 's/^TELNET_PORT=24/TELNET_PORT=23/' "$WORK/t3/custom/configs/hack.conf"
check "verify catches a wrong flag" refused sd verify --mode first-boot --stage-dir "$WORK/t3"
cp -a "$F" "$WORK/t4"
printf 'x' >"$WORK/t4/hack.sh"
check "verify catches a missing hook" refused sd verify --mode first-boot --stage-dir "$WORK/t4"

# toolkit that ships a root .bin is refused even with a matching hash
cp -a "$TK" "$WORK/tk_bin"
: >"$WORK/tk_bin/sd_card/evil.BIN"
H2=$( (cd "$WORK/tk_bin/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1)
check "toolkit with a root .bin refused" refused bash -c "AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=$H2 '$SCRIPT' prepare --mode backup --dry-run --stage-dir '$WORK/tb' --toolkit '$WORK/tk_bin' >/dev/null 2>&1"

# Wi-Fi from nowhere: refused without a TTY
( unset AKAM_WIFI_SSID AKAM_WIFI_PASS; sd prepare --mode backup --dry-run --stage-dir "$WORK/nw" --toolkit "$TK" ); rc=$?
check "no Wi-Fi env and no TTY refused" test "$rc" -ne 0

# ------------------------------------------------------------ flash files
IMG=$WORK/ak_rtsp_firmware_20261108_120000.squashfs
printf 'hsqs-fake-image' >"$IMG"
(cd "$WORK" && sha256sum "$(basename "$IMG")" >"$IMG.sha256")
FT=$WORK/flash_tool
printf '\177ELF-fake' >"$FT"
BK=$WORK/backup
mkdir -p "$BK"
truncate -s 5242880 "$BK/mtdblock7.bin"
check "flash-files dry-run" sd prepare --mode flash-files --device /dev/sdb --image "$IMG" --flash-tool "$FT" --backup-dir "$BK" --dry-run --stage-dir "$WORK/ff"
FF=$WORK/ff
check "flash-files: image in custom_firmware/ only" bash -c "ls '$FF/custom_firmware' | grep -qx 'ak_rtsp_firmware_20261108_120000.squashfs' && [ \"\$(ls '$FF/custom_firmware' | wc -l)\" -eq 1 ]"
check "flash-files: scripts and flash_tool" bash -c "cd '$FF' && test -x flash_tool && test -s install_with_flash_tool.sh && test -s flash_guard.sh && test -s preflight_check.sh && test -s restore.sh && test -s akam_hwcheck.sh"
check "flash-files: mtdblock7.bin in original_firmware/" test -f "$FF/original_firmware/mtdblock7.bin"
check "flash-files: no *.bin at the root" bash -c "! find '$FF' -maxdepth 1 -iname '*.bin' | grep -q ."
check "flash-files verify passes" sd verify --mode flash-files --stage-dir "$FF"
cp -a "$FF" "$WORK/ff2"
cp "$IMG" "$WORK/ff2/custom_firmware/ak_rtsp_firmware_20261109_120000.squashfs"
check "verify rejects two images" refused sd verify --mode flash-files --stage-dir "$WORK/ff2"
cp -a "$FF" "$WORK/ff3"
cp "$BK/mtdblock7.bin" "$WORK/ff3/mtdblock7.bin"
check "verify rejects a *.bin at the root" refused sd verify --mode flash-files --stage-dir "$WORK/ff3"
cp -a "$FF" "$WORK/ff4"
: >"$WORK/ff4/flash_guard.sh"
check "verify rejects an empty script" refused sd verify --mode flash-files --stage-dir "$WORK/ff4"
cp "$IMG" "$WORK/bad.squashfs"
check "bad image name refused" refused sd prepare --mode flash-files --image "$WORK/bad.squashfs" --flash-tool "$FT" --backup-dir "$BK" --dry-run --stage-dir "$WORK/fb1"
mkdir -p "$WORK/shortbk"
truncate -s 1000 "$WORK/shortbk/mtdblock7.bin"
check "short mtdblock7.bin refused" refused sd prepare --mode flash-files --image "$IMG" --flash-tool "$FT" --backup-dir "$WORK/shortbk" --dry-run --stage-dir "$WORK/fb2"
printf 'corrupt' >>"$IMG"
check "image not matching its .sha256 refused" refused sd prepare --mode flash-files --image "$IMG" --flash-tool "$FT" --backup-dir "$BK" --dry-run --stage-dir "$WORK/fb3"
check "flash-files needs its three inputs" refused sd prepare --mode flash-files --dry-run --stage-dir "$WORK/fb4"
check "flash-files refuses an internal disk" refused sd prepare --mode flash-files --device /dev/sda --image "$IMG" --flash-tool "$FT" --backup-dir "$BK" --dry-run --stage-dir "$WORK/fb5"

# ------------------------------------------------------------ wipe (dry-run)
check "wipe dry-run on the card" sd wipe --device /dev/sdb --dry-run
check "wipe dry-run mentions the size" has "3965190144"

# ------------------------------------------------------------ secrets
check "no password sentinel in any output or log" bash -c "! grep -q '$SENT' '$OUT'"
check "no SSID in any output or log" bash -c "! grep -qF \"net'\\\$HOME\" '$OUT'"
check "wifi.conf is the only file holding the password" bash -c "[ \"\$(grep -rl '$SENT' '$WORK' --exclude=all.log --exclude=o | grep -vc 'wifi.conf')\" -eq 0 ]"

echo "sd_card_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
