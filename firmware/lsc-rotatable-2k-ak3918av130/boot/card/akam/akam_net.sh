#!/bin/sh
# akam_net.sh -- network settings of the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130): hostname and Wi-Fi band.
# Installed next to the other overlay scripts (card: akam/akam_net.sh, appfs image: /appfs/akam/akam_net.sh), together
# with the UNCHANGED shared akam_hostname.sh (firmware/lsc-indoor-1080p-ak3918/overlay). Never writes flash, nvram or mtd: only
# /tmp and two small text files in /config/akam (jffs2, user actions only: `hostname`, `net`). Never prints wpa.conf.
#
# Usage:
#   akam_net.sh hostname set NAME | show | off | apply
#       set    validate (RFC 1123 label, done by akam_hostname.sh), store /config/akam/hostname, set the kernel hostname,
#              restart udhcpc with the new name (only when the name changed). show: effective name (stored, else
#              akam-rot2k). off: forget the stored name (back to akam-rot2k, applied at once). apply: boot, kernel
#              hostname := effective, prints it. set/off refuse (status 4) while a Wi-Fi change runs.
#   akam_net.sh wifi show | render | set country XX|none | set band 2.4|5 | fallback [save]
#       Config /config/akam/net: wifi_country=XX|none (default none), wifi_band=2.4|5 (default 2.4). Any other stored
#       value (legacy `auto`, garbage, CRLF residue) reads as the default. Band 5 without a country reads as 2.4.
#       render    writes $TMP/wpa.conf (0600). Band 2.4: BYTE-IDENTICAL copy of the user's wpa.conf (exactly the stock
#                 behaviour, no country, no key added). Band 5: the user's conf untouched + two appended GLOBAL keys
#                 `country=XX` and `freq_list=<5 GHz channels 36-48>` (both are parsed by the camera's wpa_supplicant 2.10, see
#                 docs/porting/lsc-rotatable-2k/net-settings.md). Never prints the content.
#       set       band 5 needs a country (status 2). Country change while on 2.4: saved at once, no restart (the
#                 country has no effect on 2.4). Anything that changes the rendered conf runs DETACHED (an SSH session
#                 over wlan0 dies with the link) with a DEAD-MAN: restart wpa_supplicant and udhcpc, and within 60 s an
#                 IPv4 on wlan0 plus a route over wlan0 must exist; wpa_supplicant failing to start or exiting within a
#                 few seconds reverts at once. Healthy: the value is saved (only AFTER success: a power cut mid-test keeps
#                 the old value). Otherwise: revert to the previous conf, then to the stock 2.4 bytes if that fails
#                 too; exit status 3. wpa_supplicant is never left stopped. The revert restores, byte for byte, the conf
#                 that was running (a copy kept in $TMP/wpa.conf.prev). If even that leaves no working link (no
#                 wpa_supplicant, no lease) the LAST RESORT resets the camera (watchdog feeder SIGKILL, else reboot): the
#                 saved file always holds the old value or 2.4, so the boot recovers.
#       fallback  boot-time last resort of the stub: render stock band 2.4, restart wpa_supplicant + udhcpc. With
#                 `save` (supplicant refused or died: the band is broken) band 2.4 is also saved (country kept); without
#                 it (no network within the budget: the router may just be slower than the camera) this boot only.
#   Progress and the dead-man result are logged to $TMP/akam_net.log (RAM).
# Result of the last `set`: first word of $TMP/akam_net.res (pending, ok, refused, failed, reverted), read by the control API.
# Exit: 0 ok, 2 usage / invalid value, 3 reverted by the dead-man, 1 other failure, 4 busy (another change running).
#
# Test hooks: AKAM_ROOT, AKAM_NET_CFG, AKAM_NET_WPA_SRC, AKAM_TMP_DIR, AKAM_WPAS, AKAM_IP, AKAM_HOSTNAME_SH,
# AKAM_NET_DEADMAN (s, 60), AKAM_NET_POLL (s, 2), AKAM_NET_FG=1 (no detach), AKAM_NET_STOP_WAIT (s, 5),
# AKAM_NET_ALIVE_WAIT (s, 3: wpa_supplicant must still run this long after its start), AKAM_NET_REBOOT_CMD (reboot),
# AKAM_NET_FEEDER_PID (file with the pid of the watchdog feeder, written by the stub), AKAM_PIDOF (pidof).
# BusyBox 1.30 ash/sed only.

ROOT=${AKAM_ROOT:-}
CFG=${AKAM_NET_CFG:-$ROOT/config/akam}
TMP=${AKAM_TMP_DIR:-$ROOT/tmp}
WPAS=${AKAM_WPAS:-$ROOT/appfs/bin/wpa_supplicant}
IP=${AKAM_IP:-ip}
SH=${AKAM_SH:-sh}
HERE=$(dirname "$0")
# the conf the card/stub uses: wpa.conf next to this script (card mode) wins over /config/akam/wpa.conf (a stale file there);
# never the rendered $TMP/wpa.conf itself (a script run from $TMP would take its own output as the source)
if [ -n "${AKAM_NET_WPA_SRC:-}" ]; then WPA_SRC=$AKAM_NET_WPA_SRC
elif [ -f "$HERE/wpa.conf" ] && ! [ "$HERE/wpa.conf" -ef "$TMP/wpa.conf" ]; then WPA_SRC=$HERE/wpa.conf
else WPA_SRC=$CFG/wpa.conf; fi
HSH=${AKAM_HOSTNAME_SH:-$HERE/akam_hostname.sh}
HFILE=$CFG/hostname
NETFILE=$CFG/net
IFACE=wlan0
DEF_HOST=akam-rot2k
DEF_COUNTRY=none # no regulatory country unless the user sets one: the Wi-Fi config stays the stock one (2.4 GHz)
DEF_BAND=2.4
DEADMAN=${AKAM_NET_DEADMAN:-60}
POLL=${AKAM_NET_POLL:-2}
STOP_WAIT=${AKAM_NET_STOP_WAIT:-5}
ALIVE_WAIT=${AKAM_NET_ALIVE_WAIT:-3}
TAB=$(printf "\t")
CR=$(printf "\r")
LOG=$TMP/akam_net.log
RES=$TMP/akam_net.res
LOCK=$TMP/akam_net.lock
WPA_CONF=$TMP/wpa.conf
WPA_PID=$TMP/wpa.pid
DHCP_PID=$TMP/udhcpc.pid
PREV=$WPA_CONF.prev
FEEDER_PID=${AKAM_NET_FEEDER_PID:-$TMP/akam_feeder.pid}
REBOOT_CMD=${AKAM_NET_REBOOT_CMD:-reboot}
PIDOF=${AKAM_PIDOF:-pidof}
# 5 GHz channels 36-48 only: no DFS, accepted by every regulatory domain. nl80211 may reject a whole scan if one frequency
# of the list is not supported by the driver/country, so widening it needs a camera check (see net-settings.md).
F5="5180 5200 5220 5240"

nlog() {
    echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) akam_net: $*" >>"$LOG" 2>/dev/null
    echo "akam_net: $*" >&2
}
quiet() { echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) akam_net: $*" >>"$LOG" 2>/dev/null; }
# res WORD [DETAIL]: the outcome of the last `set` request, for the control API (state net.apply): pending | ok | refused | reverted
res() { echo "$*" >"$RES" 2>/dev/null; }
die2() {
    res refused "$*"
    echo "akam_net: $*" >&2
    exit 2
}
usage() {
    echo "usage: akam_net.sh hostname set NAME|show|off|apply | wifi show|render|set country XX|none|set band 2.4|5|fallback [save]" >&2
    exit 2
}

upper() { printf '%s\n' "$1" | sed 'y/abcdefghijklmnopqrstuvwxyz/ABCDEFGHIJKLMNOPQRSTUVWXYZ/'; }
valid_country() {
    case $1 in [A-Z][A-Z] | none) return 0 ;; esac
    return 1
}
valid_band() {
    case $1 in 2.4 | 5) return 0 ;; esac
    return 1
}

# cfg_get KEY DEFAULT VALIDATOR: the stored value, or the default when absent or invalid
cfg_get() {
    v=$(sed -n "s/^$1=//p" "$NETFILE" 2>/dev/null | head -n 1)
    v=${v%"$(printf '\r')"}
    if [ -n "$v" ] && $3 "$v"; then echo "$v"; else echo "$2"; fi
}
cur_country() { cfg_get wifi_country "$DEF_COUNTRY" valid_country; }
# The effective band: a stored 5 without a country reads as 2.4 (5 GHz is never rendered without a regulatory country)
cur_band() {
    v=$(cfg_get wifi_band "$DEF_BAND" valid_band)
    if [ "$v" = 5 ] && [ "$(cur_country)" = none ]; then v=2.4; fi
    echo "$v"
}

# running_band: the band of the conf wpa_supplicant was last started with ($WPA_CONF, rendered by this script): 5 when it
# carries the global 5 GHz freq_list, else 2.4 (also before any render: the stock conf is 2.4). It differs from the saved band
# after a boot fallback without save.
running_band() {
    if grep -q '^freq_list=' "$WPA_CONF" 2>/dev/null; then echo 5; else echo 2.4; fi
}

# save_cfg COUNTRY BAND: atomic replace of the two-line file
save_cfg() {
    mkdir -p "$CFG" || return 1
    rm -f "$NETFILE".new.* 2>/dev/null # leftovers of a power cut between write and rename
    t=$NETFILE.new.$$
    if printf 'wifi_country=%s\nwifi_band=%s\n' "$1" "$2" >"$t" && chmod 644 "$t" && mv -f "$t" "$NETFILE"; then
        sync
        return 0
    fi
    rm -f "$t"
    return 1
}

# blocks_ok: every `network={` of $WPA_SRC is closed by a lone `}` (a file that ends inside a block would swallow the
# global keys we append). Only whole lines count, as in wpa_supplicant's own parser (blanks / CR around are fine).
# A conf with blob-base64 sections (also closed by `}`) is refused too: nobody has one on this camera.
blocks_ok() {
    bo_ws="[ $TAB$CR]"
    bo_n=$(grep -c "^$bo_ws*network={$bo_ws*\$" "$WPA_SRC")
    bo_c=$(grep -c "^$bo_ws*}$bo_ws*\$" "$WPA_SRC")
    [ "$bo_n" = "$bo_c" ]
}

# render COUNTRY BAND: band 2.4 = byte-identical copy of the user's wpa.conf (stock). Band 5 (with a country) = the
# user's conf UNTOUCHED, then a blank line (the last line of the user's file may lack its newline) and the two global
# keys `country=XX` and `freq_list=<5 GHz channels>` (global freq_list restricts the frequencies wpa_supplicant scans;
# a later global key wins). The content of wpa.conf is never printed.
render() {
    [ -f "$WPA_SRC" ] || {
        nlog "render: $WPA_SRC missing"
        return 1
    }
    mkdir -p "$TMP" 2>/dev/null
    rt=$WPA_CONF.new.$$
    (
        umask 077
        if [ "$2" = 5 ] && [ "$1" != none ]; then
            blocks_ok || exit 1 # refuse rather than append inside an open block
            { cat "$WPA_SRC" && printf '\ncountry=%s\nfreq_list=%s\n' "$1" "$F5"; } >"$rt" || exit 1
        else
            cat "$WPA_SRC" >"$rt" || exit 1
        fi
        mv -f "$rt" "$WPA_CONF"
    ) || {
        rm -f "$rt"
        nlog "render: failed (band $2)"
        return 1
    }
    chmod 600 "$WPA_CONF" 2>/dev/null
    return 0
}

# pid_named PID NAME: the process still is what we think (a stale pidfile may name a recycled pid)
pid_named() {
    [ -r "/proc/$1/cmdline" ] || return 0
    # cmdline is NUL separated: BusyBox 1.30 grep matches across NUL, but do not rely on it (argv[1] of `busybox watchdog` too)
    tr '\0' ' ' <"/proc/$1/cmdline" 2>/dev/null | grep -q "$2"
}
# find_pid PIDFILE NAME: the pid of a live NAME process: the pidfile's, else the first one pidof finds ("" if none)
find_pid() {
    fp=
    [ -r "$1" ] && IFS= read -r fp <"$1"
    case $fp in '' | *[!0-9]*) fp= ;; esac
    if [ -n "$fp" ] && { ! kill -0 "$fp" 2>/dev/null || ! pid_named "$fp" "$2"; }; then fp=; fi
    if [ -z "$fp" ]; then
        fp=$($PIDOF "$2" 2>/dev/null | sed 's/ .*//')
        case $fp in '' | *[!0-9]*) fp= ;; esac
        if [ -n "$fp" ] && ! kill -0 "$fp" 2>/dev/null; then fp=; fi
    fi
    echo "$fp"
}

# named_pids PIDFILE NAME: every live NAME process: the pidfile's and all those pidof finds (one line, no duplicates)
named_pids() {
    np=
    fp=
    [ -r "$1" ] && IFS= read -r fp <"$1"
    for q in $fp $($PIDOF "$2" 2>/dev/null); do
        case $q in '' | *[!0-9]*) continue ;; esac
        kill -0 "$q" 2>/dev/null || continue
        pid_named "$q" "$2" || continue
        case " $np " in *" $q "*) continue ;; esac
        np="$np $q"
    done
    # shellcheck disable=SC2086 # a list of pids: plain word splitting
    echo $np
}
any_alive() {
    for q in "$@"; do kill -0 "$q" 2>/dev/null && return 0; done
    return 1
}

# stop_pid PIDFILE NAME: SIGTERM to EVERY NAME process (a leftover twin must not survive), wait up to STOP_WAIT s, then SIGKILL.
stop_pid() {
    sps=$(named_pids "$1" "$2")
    [ -n "$sps" ] || return 0
    for p in $sps; do kill -TERM "$p" 2>/dev/null; done
    i=0
    # shellcheck disable=SC2086
    while any_alive $sps && [ "$i" -lt "$STOP_WAIT" ]; do
        sleep 1
        i=$((i + 1))
    done
    for p in $sps; do kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null; done
    return 0
}

wpa_alive() { [ -n "$(find_pid "$WPA_PID" wpa_supplicant)" ]; }

# reset_camera: LAST RESORT when no network client can be brought up. Same as the stub's `fail`: SIGKILL of the watchdog
# feeder (hardware reset in ~20 s), else a plain reboot. The saved /config/akam/net always holds the old value or 2.4,
# so the next boot comes up on a working conf; a regulatory domain set by country=XX is also gone after the reboot.
reset_camera() {
    nlog "LAST RESORT: no working Wi-Fi client, forcing a reset (the saved settings are safe)"
    rf=$(find_pid "$FEEDER_PID" watchdog)
    if [ -n "$rf" ]; then
        kill -KILL "$rf" 2>/dev/null
        nlog "watchdog feeder $rf killed: hardware reset in about 20 s"
    else
        nlog "no watchdog feeder: $REBOOT_CMD"
        $REBOOT_CMD >/dev/null 2>&1
    fi
    return 0
}

# start_wpa: stock command line. Status 0 started and still alive ALIVE_WAIT s later; 1 would not start (3 attempts);
# 2 started then exited (depending on the build, a refused conf makes it exit before or after it daemonized: both
# are handled, the second one is only visible here; no retry).
start_wpa() {
    a=0
    while [ "$a" -lt 3 ]; do
        a=$((a + 1))
        rm -f "$WPA_PID"
        if "$WPAS" -Dnl80211 -i "$IFACE" -c "$WPA_CONF" -B -P "$WPA_PID" >"$TMP/wpa.out" 2>&1; then
            sleep "$ALIVE_WAIT"
            wpa_alive && return 0
            quiet "wpa_supplicant exited right after its start"
            return 2
        fi
        quiet "wpa_supplicant attempt $a of 3 failed"
        sleep 1
    done
    return 1
}

effective_host() {
    eh=$(AKAM_HOSTNAME_FILE=$HFILE $SH "$HSH" status 2>/dev/null | sed -n 's/^configured=//p')
    [ -n "$eh" ] || eh=$DEF_HOST
    echo "$eh"
}

# flush_addr: drop every IPv4 address of wlan0. `ip addr flush` is verified, an address that survives is deleted by hand.
flush_addr() {
    $IP addr flush dev "$IFACE" >/dev/null 2>&1
    $IP addr show "$IFACE" 2>/dev/null | grep -q 'inet ' || return 0
    quiet "addr flush left an address: deleting it explicitly"
    $IP addr show "$IFACE" 2>/dev/null | sed -n 's/^ *inet \([0-9./]*\).*/\1/p' | while IFS= read -r fa; do
        $IP addr del "$fa" dev "$IFACE" >/dev/null 2>&1
    done
    $IP addr show "$IFACE" 2>/dev/null | grep -q 'inet ' && return 1
    return 0
}

# dhcp_restart NAME [flush]: udhcpc has the hostname fixed on its command line (-x hostname:), so SIGUSR1 (renew) and
# SIGUSR2 (release) cannot change the name it sends: stop it and start it again (fresh DISCOVER). flush = also drop
# the old address first, so that "has an IPv4" afterwards proves a new lease. Status 1 if udhcpc will not start
# (3 attempts).
dhcp_restart() {
    stop_pid "$DHCP_PID" udhcpc
    [ "${2:-}" != flush ] || flush_addr || quiet "an old address could not be removed"
    d=0
    while [ "$d" -lt 3 ]; do
        d=$((d + 1))
        udhcpc -b -p "$DHCP_PID" -i "$IFACE" -x "hostname:$1" >/dev/null 2>&1 && return 0
        quiet "udhcpc attempt $d of 3 failed"
        sleep 1
    done
    return 1
}

# healthy: the link is up (carrier 1, else operstate up), IPv4 on wlan0 and a route over wlan0 (the LAN route; no
# default route needed)
healthy() {
    hc=$(cat "$ROOT/sys/class/net/$IFACE/carrier" 2>/dev/null)
    if [ -n "$hc" ]; then
        [ "$hc" = 1 ] || return 1
    else
        [ "$(cat "$ROOT/sys/class/net/$IFACE/operstate" 2>/dev/null)" = up ] || return 1
    fi
    $IP addr show "$IFACE" 2>/dev/null | grep -q 'inet ' && $IP route show 2>/dev/null | grep -q " dev $IFACE"
}
# wait_healthy SECONDS: 0 healthy; 1 not within the time, or wpa_supplicant died meanwhile (no point waiting)
wait_healthy() {
    t=0
    while :; do
        healthy && return 0
        wpa_alive || {
            quiet "wpa_supplicant is not running"
            return 1
        }
        [ "$t" -lt "$1" ] || return 1
        sleep "$POLL"
        t=$((t + POLL))
    done
}

# restart_wifi: wpa_supplicant (new $WPA_CONF) then udhcpc. Status 1 wpa_supplicant is not running afterwards, 2 udhcpc
# would not start (counts as not healthy).
restart_wifi() {
    stop_pid "$WPA_PID" wpa_supplicant
    start_wpa || return 1
    dhcp_restart "$(effective_host)" flush || {
        quiet "udhcpc did not start"
        return 2
    }
    return 0
}

lock_pid() {
    lp=
    [ -r "$LOCK/pid" ] && IFS= read -r lp <"$LOCK/pid"
}
take_lock() {
    mkdir -p "$TMP" 2>/dev/null
    if mkdir "$LOCK" 2>/dev/null; then
        echo $$ >"$LOCK/pid"
        return 0
    fi
    w=0
    while :; do
        lock_pid
        if [ -n "$lp" ]; then
            if kill -0 "$lp" 2>/dev/null; then return 1; fi
            break # its owner is gone: stale
        fi
        # a lock directory without a pid: its owner is between mkdir and echo (a few ms), unless it died there
        [ "$w" -lt 3 ] || break
        [ -d "$LOCK" ] || break
        sleep 1
        w=$((w + 1))
    done
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null && echo $$ >"$LOCK/pid"
}
drop_lock() { rm -rf "$LOCK"; }
lock_busy() { # a change holds the lock; a lock directory without a pid gets the same 3 s grace as take_lock, then it is stale
    lb=0
    while :; do
        [ -d "$LOCK" ] || return 1
        lock_pid
        [ -z "$lp" ] || break
        [ "$lb" -lt 3 ] || return 1
        sleep 1
        lb=$((lb + 1))
    done
    kill -0 "$lp" 2>/dev/null
}
busy() {
    res refused busy
    echo "akam_net: another network change is running" >&2
    exit 4
}

# try_conf COUNTRY BAND: render + restart + wait. 0 healthy, 1 running but no network in time, 2 render or start failed
try_conf() {
    render "$1" "$2" && restart_wifi || return 2
    wait_healthy "$DEADMAN" && return 0
    return 1
}
# try_prev: put back, byte for byte, the conf that was RUNNING before the change (not a re-render of the source, which
# may have changed meanwhile), restart, wait. Same statuses as try_conf.
try_prev() {
    if cp -p "$PREV" "$WPA_CONF.rst.$$" && mv -f "$WPA_CONF.rst.$$" "$WPA_CONF"; then :; else
        rm -f "$WPA_CONF.rst.$$"
        return 2
    fi
    restart_wifi || return 2
    wait_healthy "$DEADMAN" && return 0
    return 1
}

# last_resort: after every revert failed. wpa_supplicant is started on the stock bytes (3 rounds) if it is not running;
# if the link still is not healthy the camera is reset (reset_camera).
last_resort() {
    k=0
    lr=0
    while [ "$k" -lt 3 ] && ! wpa_alive; do
        k=$((k + 1))
        nlog "last resort: stock wpa.conf, wpa_supplicant start round $k"
        render "$1" 2.4 && restart_wifi && lr=1
    done
    # after a restart the lease needs its time: the dead-man wait, not one sample
    if [ "$lr" = 1 ]; then wait_healthy "$DEADMAN" && return 0; else healthy && return 0; fi
    reset_camera
    return 1
}

# change_wifi COUNTRY BAND: the dead-man run (see the header)
change_wifi() {
    nc=$1
    nb=$2
    oc=$(cur_country)
    ob=$(cur_band)
    rm -f "$PREV"
    [ ! -f "$WPA_CONF" ] || cp -p "$WPA_CONF" "$PREV" 2>/dev/null
    nlog "wifi change country $oc -> $nc, band $ob -> $nb: dead-man ${DEADMAN}s"
    try_conf "$nc" "$nb"
    r=$?
    if [ "$r" = 0 ]; then
        rm -f "$PREV"
        if save_cfg "$nc" "$nb"; then
            nlog "wifi change healthy: saved country=$nc band=$nb"
            res ok
        else
            nlog "wifi change healthy but /config/akam/net could not be written (not persistent)"
            res failed not-persistent
        fi
        return 0
    fi
    if [ "$r" = 2 ]; then
        nlog "wifi change FAILED (render or wpa_supplicant start): REVERTING to the running conf (band $ob)"
    else
        nlog "wifi change NOT healthy within ${DEADMAN}s: REVERTING to the running conf (band $ob)"
    fi
    if [ -f "$PREV" ]; then
        try_prev
        r=$?
    else
        try_conf "$oc" "$ob"
        r=$?
    fi
    if [ "$r" != 0 ] && [ "$ob" != 2.4 ]; then
        # the saved band 5 does not work any more: the saved value follows what runs (as the boot fallback does)
        save_cfg "$oc" 2.4 || nlog "revert: /config/akam/net not writable"
        nlog "revert to band $ob failed too (status $r): STOCK band 2.4"
        try_conf "$oc" 2.4
        r=$?
    fi
    case $r in
        0) nlog "revert: wifi is back (IPv4 + route on $IFACE)" ;;
        1) nlog "revert: wpa_supplicant runs with the previous conf, no IPv4 yet (check the AP)" ;;
        *) nlog "revert: wpa_supplicant did not start with the previous conf" ;;
    esac
    [ "$r" = 0 ] || last_resort "$oc"
    rm -f "$PREV"
    res reverted
    return 3
}

# change_dhcp_name NEW OLD: restart udhcpc with the new name; if it will not start, with the old one; if neither starts
# the camera would have no DHCP client: LAST RESORT reset. Status 1 when the new name is not the one in use.
change_dhcp_name() {
    dhcp_restart "$1" && return 0
    quiet "udhcpc would not start with the name $1: trying the previous name"
    dhcp_restart "$2" && return 1
    reset_camera
    return 1
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
                trap '' HUP # udhcpc inherits it: it must outlive an SSH session over wlan0
                take_lock || busy
                prev=$(effective_host)
                AKAM_HOSTNAME_FILE=$HFILE $SH "$HSH" set "$3" >/dev/null || {
                    rc=$?
                    res refused hostname
                    drop_lock
                    exit $rc
                }
                n=$(effective_host)
                rc=0
                if [ "$n" != "$prev" ]; then change_dhcp_name "$n" "$prev" || rc=1; fi
                drop_lock
                quiet "hostname set to $n (kernel + DHCP restarted)"
                echo "$n"
                if [ "$rc" = 0 ]; then res ok; else res failed udhcpc; fi
                [ "$rc" = 0 ] || echo "akam_net: stored, but udhcpc would not restart with the new name (restarted with the old one, or the camera is being reset)" >&2
                exit $rc
                ;;
            off)
                [ $# -eq 2 ] || usage
                trap '' HUP
                take_lock || busy
                rc=0
                prev=$(effective_host)
                rm -f "$HFILE" || {
                    drop_lock
                    exit 1
                }
                sync
                "${AKAM_HOSTNAME_CMD:-hostname}" "$DEF_HOST" 2>/dev/null
                if [ "$prev" != "$DEF_HOST" ]; then change_dhcp_name "$DEF_HOST" "$prev" || rc=1; fi
                drop_lock
                quiet "hostname back to $DEF_HOST"
                echo "$DEF_HOST"
                exit $rc
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
                printf 'country=%s\nband=%s\nrunning=%s\n' "$(cur_country)" "$(cur_band)" "$(running_band)"
                ;;
            render)
                render "$(cur_country)" "$(cur_band)" || exit 1
                ;;
            fallback)
                [ $# -le 3 ] || usage
                [ -z "${3:-}" ] || [ "$3" = save ] || usage
                take_lock || busy
                c=$(cur_country)
                quiet "fallback: stock band 2.4, wpa_supplicant + udhcpc restarted"
                if render "$c" 2.4; then
                    restart_wifi
                    rc=$?
                    # save: the band itself does not work (refused / died): do not retry it at every boot
                    if [ "${3:-}" = save ]; then
                        save_cfg "$c" 2.4 || quiet "fallback: /config/akam/net not writable"
                    fi
                else
                    rc=1
                fi
                drop_lock
                exit $rc
                ;;
            set)
                [ $# -eq 4 ] || usage
                c=$(cur_country)
                b=$(cur_band)
                oc=$c
                ob=$b
                case $3 in
                    country)
                        c=$(upper "$4")
                        [ "$c" != NONE ] || c=none
                        valid_country "$c" || die2 "invalid country '$4' (two letters, e.g. FR, or none)"
                        ;;
                    band)
                        b=$4
                        valid_band "$b" || die2 "invalid band '$4' (2.4 or 5)"
                        ;;
                    *) usage ;;
                esac
                [ "$b" != 5 ] || [ "$c" != none ] || die2 "band 5 needs a country (wifi set country XX), and a country cannot be removed while on band 5"
                [ -f "$WPA_SRC" ] || {
                    res refused no-wpa-conf
                    nlog "no $WPA_SRC: nothing to render"
                    exit 1
                }
                if [ "$c" = "$oc" ] && [ "$b" = "$ob" ] && [ "$b" = "$(running_band)" ]; then
                    res ok unchanged
                    echo "unchanged: country=$c band=$b"
                    exit 0
                fi
                if [ "$b" = 5 ] && ! blocks_ok; then
                    # refuse before touching anything: a render would fail and the revert would restart Wi-Fi for nothing
                    res refused wpa-conf
                    echo "akam_net: wpa.conf has a network={ block that is not closed by a lone } line (or blob sections): band 5 refused" >&2
                    exit 1
                fi
                if [ "$b" = 2.4 ] && [ "$ob" = 2.4 ]; then
                    # the country has no effect on 2.4: the rendered conf does not change, nothing to restart
                    take_lock || busy
                    save_cfg "$c" "$b"
                    rc=$?
                    drop_lock
                    [ "$rc" = 0 ] || {
                        res refused not-writable
                        echo "akam_net: /config/akam/net not writable" >&2
                        exit 1
                    }
                    res ok
                    echo "saved: country=$c band=2.4 (no effect on 2.4, Wi-Fi not restarted)"
                    exit 0
                fi
                if [ "${AKAM_NET_FG:-}" = 1 ]; then
                    take_lock || busy
                    change_wifi "$c" "$b"
                    rc=$?
                    drop_lock
                    exit $rc
                fi
                lock_busy && busy
                res pending
                # detached worker: survives the SSH session that dies with the link
                (
                    trap '' HUP
                    exec $SH "$0" wifi run "$c" "$b"
                ) </dev/null >/dev/null 2>&1 &
                echo "pending: dead-man ${DEADMAN}s, result in $LOG"
                ;;
            run) # internal: the detached worker
                [ $# -eq 4 ] || usage
                valid_country "$3" || usage
                valid_band "$4" || usage
                take_lock || {
                    quiet "worker: another change holds the lock, nothing done"
                    res refused busy
                    exit 4
                }
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
