#!/bin/sh
# akam_hostname.sh -- configurable network hostname (installed as /usr/sbin/akam_hostname.sh, 755).
#
# Usage: akam_hostname.sh set NAME | show | status | reset | apply
#   set NAME  validate, lowercase, store atomically in /etc/config/akam/hostname (0644),
#             sync, then "hostname NAME" at once. DHCP (udhcpc) is NOT restarted: the
#             router sees the new name at the next boot / DHCP DISCOVER.
#   show      print the effective name (stored name, else the default) and nothing else.
#   status    key=value lines: effective, configured, default, kernel.
#   reset     remove the stored name (back to the default), then "hostname DEFAULT".
#   apply     boot-time: "hostname <effective>". Always exits 0 unless the usage is wrong.
#
# Default: akam-XXXX, XXXX = last 4 hex digits of the wlan0 MAC (lowercase), "akam"
# when the MAC is unreadable. Rules: RFC 1123 single label, 1..32 characters of
# A-Za-z0-9-, no leading/trailing "-", not all digits, stored in lowercase.
# A stored name that fails the rules is ignored (default used).
#
# POSIX sh + sed only (BusyBox 1.30.1: no tr, no mktemp). Test hooks: AKAM_ROOT
# (prefix), AKAM_HOSTNAME_FILE, AKAM_MAC_FILE, AKAM_HOSTNAME_CMD.
# The 8091 control API (SET sys.hostname, V1.1) must reuse this script and file.

AKAM_ROOT=${AKAM_ROOT:-}
CONF_DIR=$AKAM_ROOT/etc/config/akam
HFILE=${AKAM_HOSTNAME_FILE:-$CONF_DIR/hostname}
MACFILE=${AKAM_MAC_FILE:-$AKAM_ROOT/sys/class/net/wlan0/address}
HCMD=${AKAM_HOSTNAME_CMD:-hostname}

lower() { printf '%s\n' "$1" | sed 'y/ABCDEFGHIJKLMNOPQRSTUVWXYZ/abcdefghijklmnopqrstuvwxyz/'; }

# valid NAME: status 0 when it follows the rules above
valid() {
    case $1 in
        '' | -* | *- | *[!A-Za-z0-9-]*) return 1 ;;
    esac
    case $1 in
        *[!0-9]*) ;;
        *) return 1 ;; # digits only
    esac
    [ "${#1}" -le 32 ]
}

default_name() {
    mac=
    [ -r "$MACFILE" ] && IFS= read -r mac < "$MACFILE"
    hex=$(printf '%s\n' "$mac" | sed 's/[^0-9A-Fa-f]//g')
    if [ "${#hex}" -ge 12 ]; then
        suffix=$(printf '%s\n' "$hex" | sed 's/.*\(....\)$/\1/')
        lower "akam-$suffix"
    else
        echo akam
    fi
}

configured_name() {
    c=
    [ -r "$HFILE" ] && IFS= read -r c < "$HFILE"
    c=${c%"$(printf '\r')"}
    if valid "$c"; then lower "$c"; fi
}

effective_name() {
    e=$(configured_name)
    [ -n "$e" ] || e=$(default_name)
    echo "$e"
}

set_kernel() { "$HCMD" "$1" 2>/dev/null; }

case ${1:-} in
    set)
        [ $# -eq 2 ] || { echo "usage: akam_hostname.sh set NAME" >&2; exit 2; }
        if ! valid "$2"; then
            echo "akam_hostname: invalid hostname '$2' (RFC 1123 label: 1-32 chars A-Z a-z 0-9 -, not starting/ending with -, not only digits)" >&2
            exit 2
        fi
        n=$(lower "$2")
        umask 022
        mkdir -p "$(dirname "$HFILE")" || exit 1
        tmp=$HFILE.new.$$
        if ! { printf '%s\n' "$n" > "$tmp" && chmod 644 "$tmp" && mv -f "$tmp" "$HFILE"; }; then
            rm -f "$tmp"
            echo "akam_hostname: cannot write $HFILE" >&2
            exit 1
        fi
        sync
        set_kernel "$n"
        echo "$n"
        ;;
    show)
        effective_name
        ;;
    status)
        printf 'effective=%s\n' "$(effective_name)"
        printf 'configured=%s\n' "$(configured_name)"
        printf 'default=%s\n' "$(default_name)"
        printf 'kernel=%s\n' "$("$HCMD" 2>/dev/null)"
        ;;
    reset)
        rm -f "$HFILE" || exit 1
        sync
        d=$(default_name)
        set_kernel "$d"
        echo "$d"
        ;;
    apply)
        set_kernel "$(effective_name)"
        exit 0
        ;;
    *)
        echo "usage: akam_hostname.sh set NAME | show | status | reset | apply" >&2
        exit 2
        ;;
esac
