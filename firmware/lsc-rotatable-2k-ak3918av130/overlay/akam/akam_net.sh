#!/bin/sh
# akam_net.sh -- network settings of the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130): hostname and Wi-Fi band.
# Installed next to the other overlay scripts (card: akam/akam_net.sh, appfs image: /appfs/akam/akam_net.sh), together
# with the UNCHANGED shared akam_hostname.sh (tools/firmware_patch_templates). Never writes flash, nvram or mtd: only
# /tmp and two small text files in /config/akam (jffs2, user actions only: `hostname`, `net`). Never prints wpa.conf.
#
# Usage:
#   akam_net.sh hostname set NAME | show | off | apply
#       set    validate (RFC 1123 label, done by akam_hostname.sh), store /config/akam/hostname, set the kernel hostname,
#              restart udhcpc with the new name. show: effective name (stored, else akam-rot2k). off: forget the stored
#              name (back to akam-rot2k, applied at once). apply: boot, kernel hostname := effective, prints it.
#   akam_net.sh wifi show | render | set country XX | set band auto|2.4|5 | fallback
#       Config /config/akam/net: wifi_country=FR (default FR, two capitals), wifi_band=auto|2.4|5 (default auto).
#       render    writes $TMP/wpa.conf (0600) = the user's wpa.conf UNTOUCHED + appended global keys `country=` and, for
#                 2.4/5 only, `freq_list=` (wpa_supplicant 2.10 global keys; a later global key wins, network blocks
#                 are never edited). Used at boot by the stub.
#       set       runtime change with a DEAD-MAN, run detached (an SSH session over wlan0 dies with the link): render the
#                 new conf, restart wpa_supplicant and udhcpc, wait up to 60 s for an IPv4 on wlan0 plus a route over
#                 wlan0 (no default route needed: decloud replaces it). Healthy: the new value is saved in
#                 /config/akam/net (saved only AFTER success: a power cut mid-test keeps the old value). Not healthy:
#                 revert to the previous conf, restart again, log it, exit status 3. Wi-Fi is never left stopped.
#       fallback  boot-time last resort of the stub: render with band auto and restart wpa_supplicant + udhcpc.
#   Progress and the dead-man result are logged to $TMP/akam_net.log (RAM).
# Exit: 0 ok, 2 usage / invalid value, 3 reverted by the dead-man, 1 other failure, 4 busy (another change running).
#
# Test hooks: AKAM_ROOT, AKAM_NET_CFG, AKAM_NET_WPA_SRC, AKAM_TMP_DIR, AKAM_WPAS, AKAM_IP, AKAM_HOSTNAME_SH,
# AKAM_NET_DEADMAN (s, 60), AKAM_NET_POLL (s, 2), AKAM_NET_FG=1 (no detach), AKAM_NET_STOP_WAIT (s, 5).
# BusyBox 1.30 ash/sed only.

ROOT=${AKAM_ROOT:-}
CFG=${AKAM_NET_CFG:-$ROOT/config/akam}
TMP=${AKAM_TMP_DIR:-$ROOT/tmp}
WPA_SRC=${AKAM_NET_WPA_SRC:-$CFG/wpa.conf}
WPAS=${AKAM_WPAS:-$ROOT/appfs/bin/wpa_supplicant}
IP=${AKAM_IP:-ip}
SH=${AKAM_SH:-sh}
HERE=$(dirname "$0")
HSH=${AKAM_HOSTNAME_SH:-$HERE/akam_hostname.sh}
HFILE=$CFG/hostname
NETFILE=$CFG/net
IFACE=wlan0
DEF_HOST=akam-rot2k
DEF_COUNTRY=FR
DEF_BAND=auto
DEADMAN=${AKAM_NET_DEADMAN:-60}
POLL=${AKAM_NET_POLL:-2}
STOP_WAIT=${AKAM_NET_STOP_WAIT:-5}
LOG=$TMP/akam_net.log
LOCK=$TMP/akam_net.lock
WPA_CONF=$TMP/wpa.conf
WPA_PID=$TMP/wpa.pid
DHCP_PID=$TMP/udhcpc.pid
F24="2412 2417 2422 2427 2432 2437 2442 2447 2452 2457 2462 2467 2472"
F5="5180 5200 5220 5240 5260 5280 5300 5320 5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 5745 5765 5785 5805 5825"

nlog() {
    echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) akam_net: $*" >>"$LOG" 2>/dev/null
    echo "akam_net: $*" >&2
}
quiet() { echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) akam_net: $*" >>"$LOG" 2>/dev/null; }
die2() {
    echo "akam_net: $*" >&2
    exit 2
}
usage() {
    echo "usage: akam_net.sh hostname set NAME|show|off|apply | wifi show|render|set country XX|set band auto|2.4|5|fallback" >&2
    exit 2
}

upper() { printf '%s\n' "$1" | sed 'y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/'; }
valid_country() {
    case $1 in [A-Z][A-Z]) return 0 ;; esac
    return 1
}
valid_band() {
    case $1 in auto | 2.4 | 5) return 0 ;; esac
    return 1
}

# cfg_get KEY DEFAULT VALIDATOR: the stored value, or the default when absent or invalid
cfg_get() {
    v=$(sed -n "s/^$1=//p" "$NETFILE" 2>/dev/null | head -n 1)
    v=${v%"$(printf '\r')"}
    if [ -n "$v" ] && $3 "$v"; then echo "$v"; else echo "$2"; fi
}
cur_country() { cfg_get wifi_country "$DEF_COUNTRY" valid_country; }
cur_band() { cfg_get wifi_band "$DEF_BAND" valid_band; }

# save_cfg COUNTRY BAND: atomic replace of the two-line file
save_cfg() {
    mkdir -p "$CFG" || return 1
    t=$NETFILE.new.$$
    if printf 'wifi_country=%s\nwifi_band=%s\n' "$1" "$2" >"$t" && chmod 644 "$t" && mv -f "$t" "$NETFILE"; then
        sync
        return 0
    fi
    rm -f "$t"
    return 1
}

# render COUNTRY BAND: the user's wpa.conf + our global keys. The content of wpa.conf is never printed.
render() {
    [ -f "$WPA_SRC" ] || {
        nlog "render: $WPA_SRC missing"
        return 1
    }
    mkdir -p "$TMP" 2>/dev/null
    case $2 in
        2.4) fl=$F24 ;;
        5) fl=$F5 ;;
        *) fl= ;;
    esac
    (
        umask 077
        {
            cat "$WPA_SRC"
            printf '\n# akam_net.sh: global keys (a later global key wins; the network blocks above are the users)\n'
            printf 'country=%s\n' "$1"
            [ -z "$fl" ] || printf 'freq_list=%s\n' "$fl"
        } >"$WPA_CONF.new.$$" && mv -f "$WPA_CONF.new.$$" "$WPA_CONF"
    ) || {
        rm -f "$WPA_CONF.new.$$"
        return 1
    }
    chmod 600 "$WPA_CONF" 2>/dev/null
    return 0
}

# stop_pid PIDFILE [name]: SIGTERM, wait up to STOP_WAIT s, then SIGKILL. A missing/stale pidfile is fine.
stop_pid() {
    p=
    [ -r "$1" ] && IFS= read -r p <"$1"
    if [ -z "$p" ] && [ -n "${2:-}" ]; then p=$(pidof "$2" 2>/dev/null | sed 's/ .*//'); fi
    case $p in '' | *[!0-9]*) return 0 ;; esac
    kill -0 "$p" 2>/dev/null || return 0
    kill -TERM "$p" 2>/dev/null
    i=0
    while kill -0 "$p" 2>/dev/null && [ "$i" -lt "$STOP_WAIT" ]; do
        sleep 1
        i=$((i + 1))
    done
    kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null
    return 0
}

# start_wpa: stock command line (3 attempts)
start_wpa() {
    a=0
    while [ "$a" -lt 3 ]; do
        a=$((a + 1))
        if "$WPAS" -Dnl80211 -i "$IFACE" -c "$WPA_CONF" -B -P "$WPA_PID" >"$TMP/wpa.out" 2>&1; then return 0; fi
        quiet "wpa_supplicant attempt $a of 3 failed"
        sleep 1
    done
    return 1
}

effective_host() {
    c=$(AKAM_HOSTNAME_FILE=$HFILE $SH "$HSH" status 2>/dev/null | sed -n 's/^configured=//p')
    [ -n "$c" ] || c=$DEF_HOST
    echo "$c"
}

# dhcp_restart NAME [flush]: udhcpc has the hostname fixed on its command line (-x hostname:), so SIGUSR1 (renew) and
# SIGUSR2 (release) cannot change the name it sends: stop it and start it again (fresh DISCOVER). flush = also drop
# the old address first, so that "has an IPv4" afterwards proves a new lease.
dhcp_restart() {
    stop_pid "$DHCP_PID" udhcpc
    [ "${2:-}" != flush ] || $IP addr flush dev "$IFACE" >/dev/null 2>&1
    udhcpc -b -p "$DHCP_PID" -i "$IFACE" -x "hostname:$1" >/dev/null 2>&1
}

# healthy: IPv4 on wlan0 and a route over wlan0 (the LAN route; no default route needed)
healthy() {
    $IP addr show "$IFACE" 2>/dev/null | grep -q 'inet ' && $IP route show 2>/dev/null | grep -q " dev $IFACE"
}
wait_healthy() { # wait_healthy SECONDS
    t=0
    while :; do
        healthy && return 0
        [ "$t" -lt "$1" ] || return 1
        sleep "$POLL"
        t=$((t + POLL))
    done
}

# restart_wifi: wpa_supplicant (new $WPA_CONF) then udhcpc
restart_wifi() {
    stop_pid "$WPA_PID" wpa_supplicant
    start_wpa || return 1
    dhcp_restart "$(effective_host)" flush
    return 0
}

take_lock() {
    if mkdir "$LOCK" 2>/dev/null; then
        echo $$ >"$LOCK/pid"
        return 0
    fi
    lp=
    [ -r "$LOCK/pid" ] && IFS= read -r lp <"$LOCK/pid"
    if [ -n "$lp" ] && kill -0 "$lp" 2>/dev/null; then return 1; fi
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null && echo $$ >"$LOCK/pid"
}
drop_lock() { rm -rf "$LOCK"; }

# change_wifi COUNTRY BAND: the dead-man run (see the header)
change_wifi() {
    nc=$1
    nb=$2
    oc=$(cur_country)
    ob=$(cur_band)
    nlog "wifi change country $oc -> $nc, band $ob -> $nb: dead-man ${DEADMAN}s"
    if render "$nc" "$nb" && restart_wifi && wait_healthy "$DEADMAN"; then
        if save_cfg "$nc" "$nb"; then
            nlog "wifi change healthy: saved country=$nc band=$nb"
        else
            nlog "wifi change healthy but /config/akam/net could not be written (not persistent)"
        fi
        return 0
    fi
    nlog "wifi change NOT healthy within ${DEADMAN}s: REVERTING to country=$oc band=$ob"
    render "$oc" "$ob"
    if restart_wifi; then
        if wait_healthy "$DEADMAN"; then
            nlog "revert: wifi is back (IPv4 + route on $IFACE)"
        else
            nlog "revert: wpa_supplicant restarted with the previous conf, no IPv4 yet (check the AP)"
        fi
    else
        nlog "revert: wpa_supplicant did not start with the previous conf"
    fi
    return 3
}

case ${1:-} in
    hostname)
        case ${2:-} in
            show)
                [ $# -eq 2 ] || usage
                effective_host
                ;;
            set)
                [ $# -eq 3 ] || usage
                AKAM_HOSTNAME_FILE=$HFILE $SH "$HSH" set "$3" >/dev/null || exit $?
                n=$(effective_host)
                dhcp_restart "$n"
                quiet "hostname set to $n (kernel + DHCP restarted)"
                echo "$n"
                ;;
            off)
                [ $# -eq 2 ] || usage
                rm -f "$HFILE" || exit 1
                sync
                "${AKAM_HOSTNAME_CMD:-hostname}" "$DEF_HOST" 2>/dev/null
                dhcp_restart "$DEF_HOST"
                quiet "hostname back to $DEF_HOST"
                echo "$DEF_HOST"
                ;;
            apply)
                n=$(effective_host)
                "${AKAM_HOSTNAME_CMD:-hostname}" "$n" 2>/dev/null
                echo "$n"
                ;;
            *) usage ;;
        esac
        ;;
    wifi)
        case ${2:-} in
            show)
                printf 'country=%s\nband=%s\n' "$(cur_country)" "$(cur_band)"
                ;;
            render)
                render "$(cur_country)" "$(cur_band)" || exit 1
                ;;
            fallback)
                quiet "fallback: band auto, wpa_supplicant + udhcpc restarted"
                render "$(cur_country)" auto && restart_wifi || exit 1
                ;;
            set)
                [ $# -eq 4 ] || usage
                c=$(cur_country)
                b=$(cur_band)
                case $3 in
                    country)
                        c=$(upper "$4")
                        valid_country "$c" || die2 "invalid country '$4' (two letters, e.g. FR)"
                        ;;
                    band)
                        b=$4
                        valid_band "$b" || die2 "invalid band '$4' (auto, 2.4 or 5)"
                        ;;
                    *) usage ;;
                esac
                [ -f "$WPA_SRC" ] || {
                    nlog "no $WPA_SRC: nothing to render"
                    exit 1
                }
                if [ "${AKAM_NET_FG:-}" = 1 ]; then
                    take_lock || {
                        echo "akam_net: another network change is running" >&2
                        exit 4
                    }
                    change_wifi "$c" "$b"
                    rc=$?
                    drop_lock
                    exit $rc
                fi
                if [ -d "$LOCK" ]; then
                    lp=
                    [ -r "$LOCK/pid" ] && IFS= read -r lp <"$LOCK/pid"
                    if [ -n "$lp" ] && kill -0 "$lp" 2>/dev/null; then
                        echo "akam_net: another network change is running" >&2
                        exit 4
                    fi
                fi
                # detached worker: survives the SSH session that dies with the link
                (
                    trap '' HUP
                    exec $SH "$0" wifi run "$c" "$b"
                ) </dev/null >/dev/null 2>&1 &
                echo "pending: dead-man ${DEADMAN}s, result in $LOG"
                ;;
            run) # internal: the detached worker
                [ $# -eq 4 ] || usage
                take_lock || exit 4
                change_wifi "$3" "$4"
                rc=$?
                drop_lock
                exit $rc
                ;;
            *) usage ;;
        esac
        ;;
    *) usage ;;
esac
