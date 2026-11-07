#!/usr/bin/env bash
# Host tests for tools/flash_scripts/preflight_check.sh (synthetic data only).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../tools/flash_scripts/preflight_check.sh
if [ -n "${CAMYN_BUSYBOX:-}" ]; then SHCMD=("$CAMYN_BUSYBOX" sh); else SHCMD=(sh); fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "  ok   - $d"; else FAIL=$((FAIL + 1)); echo "  FAIL - $d"; fi; }
mkroot() { R=$WORK/$1; rm -rf "$R"; mkdir -p "$R/etc/config" "$R/tmp" "$R/mnt" "$R/proc"
    echo 'mtd7: 00500000 00001000 "APP"' >"$R/proc/mtd"; echo 'root:x:0:0' >"$R/etc/config/passwd"; }
run() { CAMYN_ROOT=$R "${SHCMD[@]}" "$SCRIPT" >"$WORK/out" 2>&1; }

mkroot good
printf 'ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n    ssid="SYNTH_SSID"\n    key_mgmt=WPA-PSK\n    psk="SYNTH_SECRET_XYZ"\n}\n' >"$R/etc/config/wpa_supplicant.conf"
check "good tree: exit 0" run
check "good tree: ssid/psk never printed" bash -c "! grep -q 'SYNTH_' '$WORK/out'"
check "good tree: reports 1 network" grep -q 'network blocks: 1' "$WORK/out"

mkroot nowpa
check "missing wpa conf: exit 1" bash -c "! CAMYN_ROOT=$R ${SHCMD[*]} $SCRIPT >/dev/null 2>&1"

mkroot ap
cp "$WORK/good/etc/config/wpa_supplicant.conf" "$R/etc/config/"
: >"$R/mnt/_ht_ap_mode.conf"
check "AP-mode file on SD (toolkit card): still exit 0" run
check "AP-mode file in /tmp: warning only" bash -c ": >'$R/tmp/_ht_ap_mode.conf'; CAMYN_ROOT=$R ${SHCMD[*]} $SCRIPT | grep -q WARN"

mkroot nopsk
printf 'network={\n    ssid="SYNTH_SSID"\n    key_mgmt=WPA-PSK\n}\n' >"$R/etc/config/wpa_supplicant.conf"
check "network without psk: exit 1" bash -c "! CAMYN_ROOT=$R ${SHCMD[*]} $SCRIPT >/dev/null 2>&1"

echo; echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
