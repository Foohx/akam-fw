#!/bin/sh
# akam_net.sh -- keep Wi-Fi alive: supervise wpa_supplicant and udhcpc on wlan0 (G03).
#
# Installed as /usr/sbin/akam_net.sh (mode 755) and launched detached by ak_rtsp_wrapper.sh, like
# akam_ssh.sh. It must NEVER delay or break ak_rtsp: it only sleeps, looks, and (when something is
# really down) restarts a daemon, with a growing delay between two repair attempts.
#
# Usage: akam_net.sh [start|status]      (default: start; "start" runs the loop in the foreground,
#                                          the wrapper detaches it)
#
# Every 30 s (AKAM_NET_INTERVAL):
#   - no /etc/config/wpa_supplicant.conf  -> does nothing at all (wired or unconfigured camera);
#   - wpa_supplicant for wlan0 and udhcpc for wlan0 must run, and wlan0 must have an IPv4 address;
#   - healthy -> resets its failure state; otherwise repairs, escalating:
#       a daemon missing            -> start it (wpa_supplicant first, then udhcpc);
#       both run but no IPv4 yet    -> restart udhcpc; from the 3rd attempt restart wpa_supplicant too.
#     Between two attempts it waits 1, 2, 4, 8 then 10 checks (30 s ... 5 min), so a router that is off
#     for an hour costs one cheap check every 5 minutes, not a restart storm.
# BusyBox applets only (no pgrep, no tr, no mktemp). Log: /tmp/akam_net.log (capped), and syslog when
# `logger` exists. All paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.

AKAM_ROOT=${AKAM_ROOT:-}
TMP_DIR=${AKAM_TMP_DIR:-$AKAM_ROOT/tmp}
WPA_CONF=${AKAM_WPA_CONF:-$AKAM_ROOT/etc/config/wpa_supplicant.conf}
HOSTNAME_TOOL=${AKAM_HOSTNAME_TOOL:-$AKAM_ROOT/usr/sbin/akam_hostname.sh}
IFACE=${AKAM_NET_IFACE:-wlan0}
INTERVAL=${AKAM_NET_INTERVAL:-30}
MAX_CYCLES=${AKAM_NET_MAX_CYCLES:-0} # 0 = forever (the tests set a number)
LOG=$TMP_DIR/akam_net.log
LOGGER=${AKAM_LOGGER:-logger}
KILL=${AKAM_KILL:-kill}
LOCKDIR=$TMP_DIR/akam_net.lock
LOG_MAX_LINES=100
LOG_KEEP_LINES=50

log() {
    echo "akam_net: $*" >>"$LOG" 2>/dev/null
    if [ "$(wc -l <"$LOG" 2>/dev/null)" -gt "$LOG_MAX_LINES" ] 2>/dev/null; then
        tail -n "$LOG_KEEP_LINES" "$LOG" >"$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
    fi
    command -v "$LOGGER" >/dev/null 2>&1 && "$LOGGER" -t akam_net "$*" 2>/dev/null
    return 0
}

# ps|grep with a bracket so the grep line itself never matches (no pgrep on the camera).
wpa_running() { ps 2>/dev/null | grep -q "[w]pa_supplicant.*$IFACE"; }
dhcp_running() { ps 2>/dev/null | grep -q "[u]dhcpc.*$IFACE"; }

has_ipv4() {
    if command -v ifconfig >/dev/null 2>&1; then
        ifconfig "$IFACE" 2>/dev/null | grep -q 'inet addr:[0-9]'
        return $?
    fi
    ip -4 addr show dev "$IFACE" 2>/dev/null | grep -q 'inet [0-9]'
}

kill_daemon() { # $1 = process name pattern ([x]yz form)
    ps 2>/dev/null | grep "$1.*$IFACE" | while read -r _pid _rest; do
        $KILL "$_pid" 2>/dev/null
    done
    sleep 1
}

start_wpa() {
    log "starting wpa_supplicant on $IFACE"
    wpa_supplicant -Dnl80211 -i "$IFACE" -c "$WPA_CONF" -B >/dev/null 2>&1
}

start_dhcp() {
    _hn=
    [ -x "$HOSTNAME_TOOL" ] && _hn=$("$HOSTNAME_TOOL" show 2>/dev/null)
    [ -n "$_hn" ] || _hn=akam
    log "starting udhcpc on $IFACE"
    udhcpc -i "$IFACE" -x "hostname:$_hn" >/dev/null 2>&1 &
}

# Cycles to wait after repair attempt number $1 (1-based).
gap_for() {
    case $1 in
        1) _gap=1 ;;
        2) _gap=2 ;;
        3) _gap=4 ;;
        4) _gap=8 ;;
        *) _gap=10 ;;
    esac
}

repair() { # $1 = attempt number
    _w=1
    _d=1
    wpa_running || _w=0
    dhcp_running || _d=0
    if [ "$_w" = 0 ]; then
        start_wpa
        sleep 2
        [ "$_d" = 1 ] && kill_daemon '[u]dhcpc' && _d=0
        dhcp_running || start_dhcp
    elif [ "$_d" = 0 ]; then
        start_dhcp
    elif [ "$1" -ge 3 ]; then
        log "no IPv4 on $IFACE after $1 attempts: restarting wpa_supplicant and udhcpc"
        kill_daemon '[u]dhcpc'
        kill_daemon '[w]pa_supplicant'
        start_wpa
        sleep 2
        start_dhcp
    else
        log "no IPv4 on $IFACE: restarting udhcpc"
        kill_daemon '[u]dhcpc'
        start_dhcp
    fi
}

take_lock() {
    mkdir -p "$TMP_DIR" 2>/dev/null
    if ! mkdir "$LOCKDIR" 2>/dev/null; then
        _lp=
        read -r _lp 2>/dev/null <"$LOCKDIR/pid"
        case "$_lp" in '' | *[!0-9]*) _lp= ;; esac
        if [ -n "$_lp" ] && kill -0 "$_lp" 2>/dev/null &&
            grep -qa akam_net "${AKAM_PROC_DIR:-/proc}/$_lp/cmdline" 2>/dev/null; then
            return 1 # another supervisor runs
        fi
        rm -rf "$LOCKDIR"
        mkdir "$LOCKDIR" 2>/dev/null || return 1
    fi
    echo $$ >"$LOCKDIR/pid"
    return 0
}

do_start() {
    if ! take_lock; then
        log "another instance holds the lock, exiting"
        return 0
    fi
    trap 'rm -rf "$LOCKDIR"' EXIT
    trap 'exit 0' INT TERM HUP
    log "start (pid $$, every ${INTERVAL}s)"
    _fails=0   # consecutive unhealthy checks
    _attempt=0 # repair attempts of the current outage
    _skip=0    # checks to let pass before the next attempt
    _n=0
    while :; do
        sleep "$INTERVAL" # first: the wrapper is bringing Wi-Fi up itself right now
        _n=$((_n + 1))
        if [ -f "$WPA_CONF" ]; then
            if wpa_running && dhcp_running && has_ipv4; then
                if [ "$_fails" -gt 0 ]; then log "network back after $_fails unhealthy check(s)"; fi
                _fails=0
                _attempt=0
                _skip=0
            else
                _fails=$((_fails + 1))
                if [ "$_skip" -gt 0 ]; then
                    _skip=$((_skip - 1))
                else
                    _attempt=$((_attempt + 1))
                    log "unhealthy (check $_fails): wpa_supplicant/udhcpc/IPv4 missing, repair attempt $_attempt"
                    repair "$_attempt"
                    gap_for "$_attempt"
                    _skip=$_gap
                fi
            fi
        fi
        if [ "$MAX_CYCLES" -gt 0 ] && [ "$_n" -ge "$MAX_CYCLES" ]; then break; fi
    done
    return 0
}

do_status() {
    _lp=
    read -r _lp 2>/dev/null <"$LOCKDIR/pid"
    case "$_lp" in '' | *[!0-9]*) _lp= ;; esac
    if [ -n "$_lp" ] && kill -0 "$_lp" 2>/dev/null; then
        echo "akam_net: supervisor running (pid $_lp)"
        return 0
    fi
    echo "akam_net: supervisor not running"
    return 1
}

case "${1:-start}" in
    start)
        do_start
        exit 0
        ;;
    status)
        do_status
        exit $?
        ;;
    *)
        echo "usage: $0 [start|status]" >&2
        exit 2
        ;;
esac
