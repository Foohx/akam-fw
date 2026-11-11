#!/bin/sh
# preflight_check.sh -- pre-flash gates, run on the camera (stock firmware or Akam,
# BusyBox sh) before writing Akam's APP and again before the card is removed. Never
# prints the SSID or the key: only presence and counts.
#
# Usage: sh preflight_check.sh                read-only gates; exit 0 = all pass, 1 = a gate failed
#        sh preflight_check.sh --ensure-wifi  make sure /etc/config/wpa_supplicant.conf holds a
#                                             network; creates it from the card when it does not
#
# Why: Akam removes anyka_ipc. The Wi-Fi comes back at boot only if the stock
# ethernet.sh finds /etc/config/wpa_supplicant.conf (station mode, DHCP) and
# nothing forces AP mode (/tmp/_ht_ap_mode.conf, copied from the SD at boot; reported as
# a warning only, because the toolkit card used for the flash carries that file).
#
# A camera that was never paired with the vendor app has NO wpa_supplicant.conf: the toolkit
# card brings Wi-Fi up from its own custom/configs/wifi.conf, so everything works with the card
# in, and the camera has no network at all once the card is removed. --ensure-wifi writes the
# file ON THE CAMERA from that card config (the password never travels through the PC):
#   ctrl_interface=/var/run/wpa_supplicant / update_config=1 / network={ ssid psk key_mgmt }
# Refused (exit 1, nothing written): SSID or password with a double quote, a backslash or a
# line break; a WPA password that is not 8..63 characters; SECURITY other than 3 (WPA-PSK) or
# 1 (open). Result line: WIFI=PRESENT | CREATED | NO_CARD_CONFIG | BAD_CHARS | BAD_PSK_LENGTH |
# UNSUPPORTED_SECURITY | WRITE_FAILED. The file is written with umask 077, synced, moved into
# place atomically and left at mode 0600.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.

ROOT=${AKAM_ROOT:-}
WPA=$ROOT/etc/config/wpa_supplicant.conf
FAIL=0

ok() { echo "PASS  $*"; }
ko() { echo "FAIL  $*"; FAIL=1; }
note() { echo "WARN  $*"; }

# wifi_has_network: the file holds a network block with an ssid.
wifi_has_network() {
    [ -s "$WPA" ] || return 1
    grep -q '^[[:space:]]*network[[:space:]]*=' "$WPA" || return 1
    grep -q '^[[:space:]]*ssid[[:space:]]*=[[:space:]]*[^[:space:]]' "$WPA"
}

# wifi_write CONF TMP: runs in a subshell; the secrets never leave it. Exit codes: 0 written,
# 11 no usable config, 12 bad characters, 13 bad psk length, 14 unsupported security, 15 write error.
wifi_write() {
    (
        umask 077
        SSID="" PASS="" SECURITY=""
        # shellcheck disable=SC1090  # the card's wifi.conf, written by the SD tool with shell quoting
        . "$1" >/dev/null 2>&1 || exit 11
        nl=$(printf '\nX')
        nl=${nl%X}
        cr=$(printf '\rX')
        cr=${cr%X}
        case $SSID in '') exit 11 ;; esac
        case $SSID in *\"* | *\\* | *"$nl"* | *"$cr"*) exit 12 ;; esac
        open=0
        case ${SECURITY:-3} in
            3) ;;
            1) open=1 ;;
            *) exit 14 ;;
        esac
        if [ "$open" = 0 ]; then
            case $PASS in *\"* | *\\* | *"$nl"* | *"$cr"*) exit 12 ;; esac
            len=${#PASS}
            if [ "$len" -lt 8 ] || [ "$len" -gt 63 ]; then exit 13; fi
        fi
        {
            echo 'ctrl_interface=/var/run/wpa_supplicant'
            echo 'update_config=1'
            echo 'network={'
            printf '    ssid="%s"\n' "$SSID"
            if [ "$open" = 1 ]; then
                echo '    key_mgmt=NONE'
            else
                printf '    psk="%s"\n' "$PASS"
                echo '    key_mgmt=WPA-PSK'
            fi
            echo '}'
        } >"$2" || exit 15
        chmod 600 "$2" || exit 15
        exit 0
    )
}

ensure_wifi() {
    local_conf="" tmp="" rc=0
    if wifi_has_network; then
        echo "WIFI=PRESENT"
        echo "wpa_supplicant.conf already holds a network."
        return 0
    fi
    for c in "$ROOT/tmp/sd/custom/configs/wifi.conf" "$ROOT/mnt/custom/configs/wifi.conf"; do
        if [ -r "$c" ]; then
            local_conf=$c
            break
        fi
    done
    if [ -z "$local_conf" ] || [ ! -d "$ROOT/etc/config" ]; then
        echo "WIFI=NO_CARD_CONFIG"
        echo "no network in wpa_supplicant.conf and no wifi.conf on the card to create it from."
        return 1
    fi
    tmp=$WPA.new.$$
    rm -f "$tmp"
    wifi_write "$local_conf" "$tmp" || rc=$?
    case $rc in
        0) ;;
        12)
            rm -f "$tmp"
            echo "WIFI=BAD_CHARS"
            echo "the SSID or password holds a double quote, a backslash or a line break: wpa_supplicant.conf cannot be written safely."
            return 1
            ;;
        13)
            rm -f "$tmp"
            echo "WIFI=BAD_PSK_LENGTH"
            echo "a WPA password must be 8 to 63 characters."
            return 1
            ;;
        14)
            rm -f "$tmp"
            echo "WIFI=UNSUPPORTED_SECURITY"
            echo "only SECURITY=3 (WPA-PSK) and SECURITY=1 (open) are handled."
            return 1
            ;;
        15)
            rm -f "$tmp"
            echo "WIFI=WRITE_FAILED"
            echo "could not write $tmp."
            return 1
            ;;
        *)
            rm -f "$tmp"
            echo "WIFI=NO_CARD_CONFIG"
            echo "the card's wifi.conf has no usable SSID."
            return 1
            ;;
    esac
    sync
    if ! mv "$tmp" "$WPA"; then
        rm -f "$tmp"
        echo "WIFI=WRITE_FAILED"
        echo "could not move the new file into place."
        return 1
    fi
    chmod 600 "$WPA" 2>/dev/null
    sync
    if ! wifi_has_network; then
        echo "WIFI=WRITE_FAILED"
        echo "the new file does not hold a network."
        return 1
    fi
    echo "WIFI=CREATED"
    echo "created /etc/config/wpa_supplicant.conf from the card's Wi-Fi settings."
    return 0
}

case ${1:-} in
    --ensure-wifi)
        ensure_wifi
        exit $?
        ;;
    "") ;;
    *)
        echo "usage: sh preflight_check.sh [--ensure-wifi]" >&2
        exit 2
        ;;
esac

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
    ko "wpa_supplicant.conf missing or empty: Akam would boot without Wi-Fi (fix: sh preflight_check.sh --ensure-wifi)"
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
    note "/etc/config/passwd missing or empty (akam_ssh.sh will create one)"
fi

if grep -q '"APP"' "$ROOT/proc/mtd" 2>/dev/null; then
    ok "/proc/mtd lists APP"
else
    ko "/proc/mtd has no APP partition"
fi

if [ "$FAIL" = 0 ]; then echo "preflight: all gates passed"; else echo "preflight: FAILED, do not flash"; fi
exit "$FAIL"
