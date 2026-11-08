#!/usr/bin/env bash
# Host-side tests for tools/flash_scripts/flash_guard.sh (and its wiring in the
# flash scripts). Runs under $SH_UNDER_TEST (default: "$AKAM_BUSYBOX sh", else
# tools/.toolchain/busybox-1.35.0-x86_64/busybox sh if built, else "sh") with a
# fake /proc/mounts and fake /proc/PID/stat. Nothing here writes any device.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
FS=$REPO/tools/flash_scripts
TCBB=$REPO/tools/.toolchain/busybox-1.35.0-x86_64/busybox

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif [ -x "$TCBB" ]; then SH_UNDER_TEST="$TCBB sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }

printf 'rootfs / squashfs ro 0 0\n/dev/mtdblock7 /usr squashfs ro,relatime 0 0\n' >"$WORK/mounts_app"
printf 'rootfs / squashfs ro 0 0\n/dev/mmcblk0p1 /mnt vfat rw 0 0\n' >"$WORK/mounts_stock"
printf '/dev/mtdblock6 /usr squashfs ro 0 0\n' >"$WORK/mounts_other"

# Driver: optional fake ancestor chain, then guard on $1 (script) $2 (tool).
cat >"$WORK/driver.sh" <<'DRV'
. "$FS/flash_guard.sh"
if [ -n "${FAKE_ANCESTOR:-}" ]; then
    mkdir -p "$AKAM_PROC_DIR/$$" "$AKAM_PROC_DIR/900" "$AKAM_PROC_DIR/800"
    echo "$$ (sh) S 800 1 1 0" >"$AKAM_PROC_DIR/$$/stat"
    echo "800 (my shell) S 900 1 1 0" >"$AKAM_PROC_DIR/800/stat"
    echo "900 ($FAKE_ANCESTOR) S 1 1 1 0" >"$AKAM_PROC_DIR/900/stat"
fi
akam_flash_guard "$1" "${2:-}"
DRV

# run NAME EXPECT(0|1) MOUNTS SCRIPT TOOL [VAR=VAL...]
run() {
    local name=$1 want=$2 mounts=$3 script=$4 tool=$5
    shift 5
    rm -rf "$WORK/proc"
    mkdir -p "$WORK/proc"
    env -u SSH_CONNECTION -u SSH_CLIENT -u AKAM_FLASH_I_KNOW -u CAMYN_FLASH_I_KNOW \
        FS="$FS" AKAM_PROC_MOUNTS="$WORK/$mounts" AKAM_PROC_DIR="$WORK/proc" "$@" \
        "${SHCMD[@]}" "$WORK/driver.sh" "$script" "$tool" >"$WORK/out" 2>&1
    local rc=$?
    if [ "$want" = 0 ] && [ $rc -eq 0 ]; then ok "$name"
    elif [ "$want" = 1 ] && [ $rc -ne 0 ]; then ok "$name"
    else ko "$name (rc=$rc)"; sed 's/^/      | /' "$WORK/out"; fi
}
outhas() { if grep -q -- "$2" "$WORK/out"; then ok "$1"; else ko "$1 (no '$2')"; fi; }

ln -s /usr/bin/env "$WORK/sd_link.sh"
run "symlink on SD pointing into /usr: blocked" 1 mounts_app "$WORK/sd_link.sh" /mnt/flash_tool
run "symlink on SD pointing into /usr, stock mounts: allowed" 0 mounts_stock "$WORK/sd_link.sh" /mnt/flash_tool

run "stock, SD script: allowed" 0 mounts_stock /mnt/x.sh /mnt/flash_tool
run "APP on /usr, SD script+tool (UART shell): allowed" 0 mounts_app /mnt/x.sh /mnt/flash_tool
run "SSH_CONNECTION: refused" 1 mounts_stock /mnt/x.sh /mnt/flash_tool SSH_CONNECTION="1.2.3.4 5 6.7.8.9 22"
outhas "refusal points to the doc" "never over an SSH session"
run "SSH_CLIENT: refused" 1 mounts_stock /mnt/x.sh "" SSH_CLIENT="1.2.3.4 5 22"
run "dropbear ancestor: refused" 1 mounts_stock /mnt/x.sh /mnt/flash_tool FAKE_ANCESTOR=dropbear
run "other ancestor: allowed" 0 mounts_stock /mnt/x.sh /mnt/flash_tool FAKE_ANCESTOR=telnetd
run "APP on /usr, script under /usr: refused" 1 mounts_app /usr/bin/x.sh /mnt/flash_tool
run "APP on /usr, tool under /usr: refused" 1 mounts_app /mnt/x.sh /usr/bin/flash_tool
run "other mtd on /usr, script under /usr: allowed" 0 mounts_other /usr/bin/x.sh ""
run "/usr not mounted from mtdblock7: allowed" 0 mounts_stock /usr/bin/x.sh ""
run "override on SSH: allowed" 0 mounts_stock /mnt/x.sh "" SSH_CONNECTION=x AKAM_FLASH_I_KNOW=1
outhas "override prints loud warning" "WARNING"
run "override on /usr script: allowed" 0 mounts_app /usr/bin/x.sh "" AKAM_FLASH_I_KNOW=1
run "override must be exactly 1" 1 mounts_stock /mnt/x.sh "" SSH_CONNECTION=x AKAM_FLASH_I_KNOW=yes
# Deprecated alias (one release): the former CAMYN_FLASH_I_KNOW=1 still works, with a warning.
run "deprecated CAMYN_FLASH_I_KNOW=1: allowed" 0 mounts_stock /mnt/x.sh "" SSH_CONNECTION=x CAMYN_FLASH_I_KNOW=1
outhas "deprecated override says so" "CAMYN_FLASH_I_KNOW is deprecated, use AKAM_FLASH_I_KNOW=1"
outhas "deprecated override still prints the loud warning" "AKAM_FLASH_I_KNOW=1 set, flashing anyway"
run "deprecated override must be exactly 1" 1 mounts_stock /mnt/x.sh "" SSH_CONNECTION=x CAMYN_FLASH_I_KNOW=yes

# Wiring: every APP-writing script refuses under SSH before doing anything else.
for s in install_with_flash_tool quiet_and_flash_test restore install_custom_firmware \
    install_padded_firmware install_ak_rtsp_firmware restore_full_firmware; do
    d=$WORK/sd_$s
    mkdir -p "$d"
    cp "$FS/$s.sh" "$FS/flash_guard.sh" "$d/"
    SSH_CONNECTION=x AKAM_PROC_MOUNTS="$WORK/mounts_stock" AKAM_PROC_DIR="$WORK/proc" \
        "${SHCMD[@]}" "$d/$s.sh" >"$WORK/out" 2>&1
    rc=$?
    if [ $rc -ne 0 ] && grep -q "refusing to write the APP" "$WORK/out"; then ok "$s.sh guarded"
    else ko "$s.sh guarded (rc=$rc)"; fi
    rm "$d/flash_guard.sh"
    SSH_CONNECTION=x "${SHCMD[@]}" "$d/$s.sh" >"$WORK/out" 2>&1
    rc=$?
    if [ $rc -ne 0 ] && grep -q "flash_guard.sh not found" "$WORK/out"; then ok "$s.sh fails closed without guard file"
    else ko "$s.sh fails closed (rc=$rc)"; fi
done

# Bare UART init=/bin/sh recovery shell: only the ROOTFS applets exist (no dirname,
# basename, readlink, cut, wc, md5sum: they live in the APP partition). The guard
# must still load and work, and restore_full_firmware.sh must parse its table.
RB=$WORK/rootbin
mkdir -p "$RB"
for a in sh cat grep sed stat mount sleep kill dd touch sync echo ls ps mkdir mv rm cp mknod date chmod; do
    ln -s "${SHCMD[0]}" "$RB/$a"
done
if [ "${#SHCMD[@]}" -gt 1 ]; then
    for a in dirname basename cut wc; do [ -e "$RB/$a" ] && ko "$a must not be in the fake rootfs"; done
    for s in install_with_flash_tool quiet_and_flash_test restore install_custom_firmware \
        install_padded_firmware install_ak_rtsp_firmware restore_full_firmware; do
        d=$WORK/sd_$s
        cp "$FS/flash_guard.sh" "$d/"
        (cd "$d" && env -i PATH="$RB" SSH_CONNECTION=x AKAM_PROC_MOUNTS="$WORK/mounts_stock" \
            AKAM_PROC_DIR="$WORK/proc" "${SHCMD[0]}" sh "./$s.sh" >"$WORK/out" 2>&1)
        rc=$?
        if [ $rc -ne 0 ] && grep -q "refusing to write the APP" "$WORK/out"; then ok "$s.sh: guard loads without dirname (rootfs applets only)"
        else ko "$s.sh: guard loads without dirname (rc=$rc)"; sed 's/^/      | /' "$WORK/out"; fi
    done
fi

echo "passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ]
