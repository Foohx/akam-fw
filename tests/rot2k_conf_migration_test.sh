#!/usr/bin/env bash
# The rename akam2k.conf -> akam.conf of the 2K (akamd): akam_flash_boot.sh copies the old conf file ONCE on an update from
# 0.1.x, keeps the old file, and never touches an existing akam.conf. Runs the real script on a fake root, no camera.
set -u
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/flash/akam_flash_boot.sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/akam-confmig.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
check() {
    local d=$1
    shift
    if "$@"; then PASS=$((PASS + 1)); echo "  ok   - $d"; else FAIL=$((FAIL + 1)); echo "  FAIL - $d"; fi
}

R=$WORK/root
mkdir -p "$R/appfs/akam" "$R/appfs/script" "$R/config/akam" "$R/mnt/sdcard" "$R/tmp" "$WORK/bin"
printf '#!/bin/sh\nexit 0\n' >"$R/appfs/script/start.sh"
chmod 755 "$R/appfs/script/start.sh"
printf '#!/bin/sh\nexit 1\n' >"$WORK/bin/mount" # no bind mount on the host: the script only warns
chmod 755 "$WORK/bin/mount"
run_boot() { PATH="$WORK/bin:$PATH" AKAM_ROOT="$R" AKAM_SH=true bash "$SCRIPT" >"$WORK/boot.out" 2>&1; }

printf 'audio=1\nonvif=1\n' >"$R/config/akam/akam2k.conf"
run_boot
check "update from 0.1.x: akam.conf is a copy of akam2k.conf" cmp -s "$R/config/akam/akam.conf" "$R/config/akam/akam2k.conf"
check "the old akam2k.conf is kept" test -f "$R/config/akam/akam2k.conf"
check "the boot says it migrated" grep -q 'akam2k.conf migrated to akam.conf' "$WORK/boot.out"
check "no temporary file is left" test ! -e "$R/config/akam/.akam.conf.new"

printf 'audio=0\n' >"$R/config/akam/akam.conf"
run_boot
check "an existing akam.conf is never overwritten" test "$(cat "$R/config/akam/akam.conf")" = "audio=0"
check "the new name is linked into the card-shaped tree" test "$(readlink "$R/tmp/akamsd/akam/akam.conf")" = "$R/config/akam/akam.conf"

rm -f "$R/config/akam/akam2k.conf" "$R/config/akam/akam.conf"
run_boot
check "a fresh install (neither file) creates nothing" test ! -e "$R/config/akam/akam.conf"

echo "rot2k_conf_migration_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
