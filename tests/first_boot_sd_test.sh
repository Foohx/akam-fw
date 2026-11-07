#!/usr/bin/env bash
# Host tests for tools/sd/prepare_first_boot_sd.sh, dry-run mode only (no device,
# no network, synthetic toolkit tree, synthetic key).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../tools/sd/prepare_first_boot_sd.sh
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "  ok   - $d"; else FAIL=$((FAIL + 1)); echo "  FAIL - $d"; fi; }

TK=$WORK/toolkit
mkdir -p "$TK/sd_card/custom/scripts" "$TK/sd_card/custom/configs" "$TK/sd_card/custom/state"
printf '#!/bin/sh\necho hack' >"$TK/sd_card/hack.sh"
printf 'TELNET=0\nTELNET_PORT=23\nOFFLINE=0\nONVIF=1\n' >"$TK/sd_card/custom/configs/hack.conf"
printf '#!/bin/sh\necho entry\n' >"$TK/sd_card/custom/scripts/entrypoint.sh"
: >"$TK/sd_card/custom/state/shadow.done"
treehash() { (cd "$1/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1; }
HASH=$(treehash "$TK")

KEY=$WORK/k.pub
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq camyn-test' >"$KEY"
echo 'ssh-rsa AAAAB3NzaC1yc2ESYNTHETIC x' >"$WORK/rsa.pub"

dry() { # dry <stagedir> [extra args]
    local st=$1; shift
    CAMYN_TEST_DRYRUN_ONLY=1 CAMYN_TEST_PIN_TREE_SHA=$HASH CAMYN_SD_SSID="Synth SSID" CAMYN_SD_PASS="syn'th pass" \
        "$SCRIPT" --dry-run --stage-dir "$st" --toolkit "$TK" --pubkey "$KEY" "$@" >"$WORK/out" 2>&1
}

check "dry run succeeds" dry "$WORK/s1"
S=$WORK/s1
check "CAMYN_MAINTENANCE present and empty" test -f "$S/CAMYN_MAINTENANCE" -a ! -s "$S/CAMYN_MAINTENANCE"
check "authorized_keys copied" cmp -s "$KEY" "$S/camyn/authorized_keys"
check "telnet 24 / offline / telnet on" bash -c "grep -qx 'TELNET=1' '$S/custom/configs/hack.conf' && grep -qx 'TELNET_PORT=24' '$S/custom/configs/hack.conf' && grep -qx 'OFFLINE=1' '$S/custom/configs/hack.conf'"
check "dump.done present, shadow.done removed" bash -c "test -f '$S/custom/state/dump.done' && test ! -e '$S/custom/state/shadow.done'"
check "entrypoint lock inserted" grep -q DECLOUD_ENTRYPOINT_LOCK "$S/custom/scripts/entrypoint.sh"
check "hack.sh hook appended on its own line" grep -qx '# DECLOUD_WIZARD_HOOK' "$S/hack.sh"
# shellcheck disable=SC1090,SC1091
wifi_ok() {
    ( . "$S/custom/configs/wifi.conf" && [ "$SSID" = "Synth SSID" ] && [ "$PASS" = "syn'th pass" ] ); }
check "wifi.conf sources back to the typed values" wifi_ok
check "no *.bin at the card root" bash -c "! find '$S' -maxdepth 1 -iname '*.bin' | grep -q ."
check "password never echoed" bash -c "! grep -q 'syn' '$WORK/out'"

check "wrong tree hash refused" bash -c "! CAMYN_TEST_DRYRUN_ONLY=1 CAMYN_TEST_PIN_TREE_SHA=deadbeef CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --dry-run --stage-dir '$WORK/s2' --toolkit '$TK' --pubkey '$KEY' >/dev/null 2>&1"

# A toolkit that ships a top-level .bin (even with a matching hash) must be refused.
cp -a "$TK" "$WORK/tk_bin"
: >"$WORK/tk_bin/sd_card/evil.BIN"
H2=$(treehash "$WORK/tk_bin")
check "top-level .bin refused" bash -c "! CAMYN_TEST_DRYRUN_ONLY=1 CAMYN_TEST_PIN_TREE_SHA=$H2 CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --dry-run --stage-dir '$WORK/s3' --toolkit '$WORK/tk_bin' --pubkey '$KEY' >/dev/null 2>&1"

printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5SHORT x\n' >"$WORK/short.pub"
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq a\nssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq b' >"$WORK/two.pub"
printf 'command="x" ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq a\n' >"$WORK/opts.pub"
for k in short two opts; do
    check "bad key refused: $k" bash -c "! CAMYN_TEST_DRYRUN_ONLY=1 CAMYN_TEST_PIN_TREE_SHA=$HASH CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --dry-run --stage-dir '$WORK/sk_$k' --toolkit '$TK' --pubkey '$WORK/$k.pub' >/dev/null 2>&1"
done
check "RSA key refused" bash -c "! CAMYN_TEST_DRYRUN_ONLY=1 CAMYN_TEST_PIN_TREE_SHA=$HASH CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --dry-run --stage-dir '$WORK/s4' --toolkit '$TK' --pubkey '$WORK/rsa.pub' >/dev/null 2>&1"
check "test pin override refused without --dry-run" bash -c "! CAMYN_TEST_DRYRUN_ONLY=1 '$SCRIPT' --device /dev/sdz --toolkit '$TK' --pubkey '$KEY' >/dev/null 2>&1"
check "real run refuses a partition node" bash -c "! CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --device /dev/sda1 --toolkit '$TK' --pubkey '$KEY' >/dev/null 2>&1"
check "real run refuses a missing device" bash -c "! CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --device /dev/sdzz --toolkit '$TK' --pubkey '$KEY' >/dev/null 2>&1"
check "real pin rejects the synthetic toolkit" bash -c "! CAMYN_SD_SSID=a CAMYN_SD_PASS=b '$SCRIPT' --dry-run --stage-dir '$WORK/s5' --toolkit '$TK' --pubkey '$KEY' >/dev/null 2>&1"

echo; echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
