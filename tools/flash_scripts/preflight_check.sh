#!/bin/sh
# preflight_check.sh -- READ-ONLY pre-flash gates, run on the camera (stock
# firmware, BusyBox sh) before writing Camyn's APP. Changes nothing and never
# prints the SSID or the key: only presence and counts.
#
# Usage: sh preflight_check.sh        exit 0 = all gates pass, 1 = a gate failed
#
# Why: Camyn removes anyka_ipc. The Wi-Fi comes back at boot only if the stock
# ethernet.sh finds /etc/config/wpa_supplicant.conf (station mode, DHCP) and
# nothing forces AP mode (/tmp/_ht_ap_mode.conf, copied from the SD at boot; reported as
# a warning only, because the toolkit card used for the flash carries that file).
#
# Paths are prefixed by $CAMYN_ROOT (empty on the camera) for the host test.

ROOT=${CAMYN_ROOT:-}
WPA=$ROOT/etc/config/wpa_supplicant.conf
FAIL=0

ok() { echo "PASS  $*"; }
ko() { echo "FAIL  $*"; FAIL=1; }
note() { echo "WARN  $*"; }

if [ -s "$WPA" ]; then
    ok "wpa_supplicant.conf exists and is non-empty"
    nets=$(grep -c '^[[:space:]]*network[[:space:]]*=' "$WPA")
    ssids=$(grep -c '^[[:space:]]*ssid[[:space:]]*=[[:space:]]*[^[:space:]]' "$WPA")
    psks=$(grep -c '^[[:space:]]*psk[[:space:]]*=[[:space:]]*[^[:space:]]' "$WPA")
    open=$(grep -c '^[[:space:]]*key_mgmt[[:space:]]*=[[:space:]]*NONE[[:space:]]*$' "$WPA")
    if [ "$nets" -ge 1 ]; then ok "network blocks: $nets"; else ko "no network block in wpa_supplicant.conf"; fi
    if [ "$ssids" -ge 1 ]; then ok "ssid present"; else ko "no ssid line in wpa_supplicant.conf"; fi
    if [ "$psks" -ge 1 ] || [ "$open" -ge 1 ]; then
        ok "psk present (or open network)"
    else
        ko "no psk and not an open network"
    fi
    grep -q '^[[:space:]]*ctrl_interface=' "$WPA" || note "no ctrl_interface line (wpa_cli would not work)"
else
    ko "wpa_supplicant.conf missing or empty: Camyn would boot without Wi-Fi"
fi

# The toolkit card itself carries _ht_ap_mode.conf (that is how its hook runs), so
# this is only informative: a boot WITHOUT the card is in station mode.
if [ -e "$ROOT/tmp/_ht_ap_mode.conf" ]; then
    note "this boot has /tmp/_ht_ap_mode.conf (toolkit card): stock Wi-Fi script ran in AP branch; without the card it is station mode"
else
    ok "no /tmp/_ht_ap_mode.conf (station mode)"
fi

if [ -s "$ROOT/etc/config/passwd" ]; then
    ok "/etc/config/passwd present"
else
    note "/etc/config/passwd missing or empty (camyn_ssh.sh will create one)"
fi

if grep -q '"APP"' "$ROOT/proc/mtd" 2>/dev/null; then
    ok "/proc/mtd lists APP"
else
    ko "/proc/mtd has no APP partition"
fi

if [ "$FAIL" = 0 ]; then echo "preflight: all gates passed"; else echo "preflight: FAILED, do not flash"; fi
exit "$FAIL"
