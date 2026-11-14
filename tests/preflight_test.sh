#!/usr/bin/env bash
# Host tests for tools/flash_scripts/preflight_check.sh (synthetic data only).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT=$HERE/../tools/flash_scripts/preflight_check.sh
if [ -n "${AKAM_BUSYBOX:-}" ]; then SHCMD=("$AKAM_BUSYBOX" sh); else SHCMD=(sh); fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
check() { local d=$1; shift; if "$@"; then PASS=$((PASS + 1)); echo "  ok   - $d"; else FAIL=$((FAIL + 1)); echo "  FAIL - $d"; fi; }
mkroot() { R=$WORK/$1; rm -rf "$R"; mkdir -p "$R/etc/config" "$R/tmp" "$R/mnt" "$R/proc"
    echo 'mtd7: 00500000 00001000 "APP"' >"$R/proc/mtd"; echo 'root:x:0:0' >"$R/etc/config/passwd"; }
run() { AKAM_ROOT=$R "${SHCMD[@]}" "$SCRIPT" >"$WORK/out" 2>&1; }

mkroot good
printf 'ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n    ssid="SYNTH_SSID"\n    key_mgmt=WPA-PSK\n    psk="SYNTH_SECRET_XYZ"\n}\n' >"$R/etc/config/wpa_supplicant.conf"
check "good tree: exit 0" run
check "good tree: ssid/psk never printed" bash -c "! grep -q 'SYNTH_' '$WORK/out'"
check "good tree: reports 1 network" grep -q 'network blocks: 1' "$WORK/out"

mkroot nowpa
check "missing wpa conf: exit 1" bash -c "! AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT >/dev/null 2>&1"

mkroot ap
cp "$WORK/good/etc/config/wpa_supplicant.conf" "$R/etc/config/"
: >"$R/mnt/_ht_ap_mode.conf"
check "AP-mode file on SD (toolkit card): still exit 0" run
check "AP-mode file in /tmp: warning only" bash -c ": >'$R/tmp/_ht_ap_mode.conf'; AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT | grep -q WARN"

mkroot nopsk
printf 'network={\n    ssid="SYNTH_SSID"\n    key_mgmt=WPA-PSK\n}\n' >"$R/etc/config/wpa_supplicant.conf"
check "network without psk: exit 1" bash -c "! AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT >/dev/null 2>&1"

# --- --ensure-wifi: create wpa_supplicant.conf ON THE CAMERA from the card's wifi.conf ---------------
ens() { AKAM_ROOT=$R "${SHCMD[@]}" "$SCRIPT" --ensure-wifi >"$WORK/out" 2>&1; }
mkcard() { # root, wifi.conf content
    mkdir -p "$1/tmp/sd/custom/configs"
    printf '%s\n' "$2" >"$1/tmp/sd/custom/configs/wifi.conf"
}
mode_of() { stat -c %a "$1"; }

mkroot w1
mkcard "$R" "MODE=sta
SSID='Synth Net'
PASS='synth-pass-123'
SECURITY=3
DHCP=1"
check "ensure-wifi: creates the file from the card, exit 0" ens
check "ensure-wifi: reports CREATED" grep -q '^WIFI=CREATED' "$WORK/out"
check "ensure-wifi: the password and the SSID are never printed" bash -c "! grep -q -e 'synth-pass' -e 'Synth Net' '$WORK/out'"
W=$R/etc/config/wpa_supplicant.conf
check "ensure-wifi: ctrl_interface and update_config" bash -c "grep -qx 'ctrl_interface=/var/run/wpa_supplicant' '$W' && grep -qx 'update_config=1' '$W'"
check "ensure-wifi: a network block with ssid, psk and WPA-PSK" bash -c "grep -qx 'network={' '$W' && grep -qx '    ssid=\"Synth Net\"' '$W' && grep -qx '    psk=\"synth-pass-123\"' '$W' && grep -qx '    key_mgmt=WPA-PSK' '$W' && grep -qx '}' '$W'"
check "ensure-wifi: mode 0600" test "$(mode_of "$W")" = 600
check "ensure-wifi: no temporary file left behind" bash -c "[ \"\$(ls '$R/etc/config' | grep -c 'wpa_supplicant')\" = 1 ]"
check "ensure-wifi: the read-only gates now pass" run
check "ensure-wifi: a second run finds it PRESENT and changes nothing" bash -c "cp '$W' '$WORK/w1.keep'; AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=PRESENT' && cmp -s '$W' '$WORK/w1.keep'"

mkroot w2
mkcard "$R" "SSID=Net
PASS=password1
SECURITY=3"
cp "$WORK/good/etc/config/wpa_supplicant.conf" "$R/etc/config/"
check "ensure-wifi: an existing network is never overwritten" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=PRESENT' && grep -q SYNTH_SSID '$R/etc/config/wpa_supplicant.conf'"

mkroot w3
printf 'ctrl_interface=/var/run/wpa_supplicant\n' >"$R/etc/config/wpa_supplicant.conf"
mkcard "$R" "SSID=Net
PASS=password1
SECURITY=3"
check "ensure-wifi: a file without a network block is replaced" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=CREATED' && grep -qx '    psk=\"password1\"' '$R/etc/config/wpa_supplicant.conf'"

mkroot w4
check "ensure-wifi: no file and no card config: exit 1, NO_CARD_CONFIG, nothing created" bash -c "! AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi >'$WORK/out' 2>&1 && grep -q '^WIFI=NO_CARD_CONFIG' '$WORK/out' && [ ! -e '$R/etc/config/wpa_supplicant.conf' ]"

mkroot w5
mkcard "$R" "SSID=Net
PASS=\"pass'word1\"
SECURITY=3"
# a single quote is fine; a double quote, backslash or newline is refused
check "ensure-wifi: a single quote in the password is accepted" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=CREATED'"
bad() { # name, wifi.conf content, expected code
    mkroot "$1"
    mkcard "$R" "$2"
    check "ensure-wifi: $1 refused ($3), nothing written" bash -c "! AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi >'$WORK/out' 2>&1 && grep -q '^WIFI=$3' '$WORK/out' && [ ! -e '$R/etc/config/wpa_supplicant.conf' ] && [ -z \"\$(ls '$R/etc/config' | grep 'wpa_supplicant')\" ]"
}
bad dquote_pass 'SSID=Net
PASS='"'"'pass"word1'"'"'
SECURITY=3' BAD_CHARS
bad backslash_pass 'SSID=Net
PASS='"'"'pass\word1'"'"'
SECURITY=3' BAD_CHARS
bad dquote_ssid 'SSID='"'"'My "Net"'"'"'
PASS=password1
SECURITY=3' BAD_CHARS
bad newline_pass 'SSID=Net
PASS='"'"'password1
second'"'"'
SECURITY=3' BAD_CHARS
bad short_psk 'SSID=Net
PASS=short
SECURITY=3' BAD_PSK_LENGTH
bad long_psk "SSID=Net
PASS=$(printf 'a%.0s' $(seq 1 64))
SECURITY=3" BAD_PSK_LENGTH
bad wep 'SSID=Net
PASS=password1
SECURITY=2' UNSUPPORTED_SECURITY
bad empty_ssid 'SSID=
PASS=password1
SECURITY=3' NO_CARD_CONFIG
mkroot w6
mkcard "$R" "SSID=Net
PASS=$(printf 'b%.0s' $(seq 1 63))
SECURITY=3"
check "ensure-wifi: a 63-character password is accepted" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=CREATED'"
mkroot w7
mkcard "$R" "SSID=OpenNet
SECURITY=1"
check "ensure-wifi: an open network (SECURITY=1) gets key_mgmt=NONE" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=CREATED' && grep -qx '    key_mgmt=NONE' '$R/etc/config/wpa_supplicant.conf' && ! grep -q psk '$R/etc/config/wpa_supplicant.conf'"
mkroot w8
mkdir -p "$R/mnt/custom/configs"
printf 'SSID=Net\nPASS=password1\nSECURITY=3\n' >"$R/mnt/custom/configs/wifi.conf"
check "ensure-wifi: the card may be on /mnt as well as /tmp/sd" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --ensure-wifi | grep -q '^WIFI=CREATED'"
check "usage: an unknown option exits 2" bash -c "AKAM_ROOT=$R ${SHCMD[*]} $SCRIPT --nope >/dev/null 2>&1; [ \$? = 2 ]"

echo; echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
