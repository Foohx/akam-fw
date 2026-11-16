#!/bin/sh
# decloud.sh -- cut the camera off the Internet while keeping the LAN (Tuya cloud egress blocked).
#
# Usage: decloud.sh on | off | status        (default: on)
#   on      only when the card has the flag file akam/decloud (else it logs "not enabled" and does nothing).
#           Waits (bounded) for the Wi-Fi interface to get an address, then adds two blackhole routes.
#   off     stops the keeper and removes the routes (the same effect as a reboot without the flag)
#   keeper  (internal, started by "on") the background loop that re-adds missing routes every 30 s
#   status  prints whether the routes are present
#
# Mechanism: blackhole routes 0.0.0.0/1 and 128.0.0.0/1. Together they cover the whole IPv4 space, are more
# specific than the DHCP default route (0.0.0.0/0) so they override it, and are less specific than the LAN
# subnet route, so LAN traffic keeps working. They survive DHCP renewals. The camera has no iptables.
# Reversible: remove the flag and reboot, or run "decloud.sh off". Nothing is written to flash.
# Side effect to know: no public NTP pool either (it is cloud-side, so blackholed). Time sync from a server on the LAN
# (your router, listed in akam/ntp, see own_app_stub.sh) is NOT affected: this script only blackholes the Internet.
#
# UNVERIFIED (PLAN section 9): that these routes stop ALL cloud traffic (DNS to a public resolver, a hard
# coded IP, IPv6, a tunnel). DNS to the router (LAN) still resolves; the connection to the answer is blackholed. Check on the live camera from a LAN host with a packet capture.
#
# Paths prefixed by $AKAM_ROOT (see common.sh); $AKAM_IP overrides the ip binary, $AKAM_IFACE the interface
# (default wlan0, UNVERIFIED (PLAN section 9) for this model), $AKAM_WAIT_TRIES / $AKAM_WAIT_SLEEP the wait.

HERE=${0%/*}
[ -r "$HERE/common.sh" ] || HERE=${AKAM_SD:-${AKAM_ROOT:-}/mnt/sdcard}/akam
# shellcheck disable=SC1091  # the helper lives on the card at run time
. "$HERE/common.sh"

IP=${AKAM_IP:-$ROOT/sbin/ip}
IFACE=${AKAM_IFACE:-wlan0}
TRIES=${AKAM_WAIT_TRIES:-60}
NAP=${AKAM_WAIT_SLEEP:-1}
ROUTES="0.0.0.0/1 128.0.0.0/1"
# IPv6: every global unicast address (2000::/3) is blackholed; the on-link /64 the router announces is more
# specific, so the LAN keeps working; link-local (fe80::/10), ULA (fc00::/7) and multicast are not touched.
# Best effort: a kernel without IPv6 just refuses these routes.
ROUTES6="2000::/3"
# The IPv4 blackholes also cover multicast (224.0.0.0/4 is inside 128.0.0.0/1): ONVIF WS-Discovery (239.255.255.250)
# and mDNS would be cut for a socket that does not pick its interface. A more specific multicast route through
# the Wi-Fi interface is added unless one exists; the marker file tells "off" that it is ours.
MCAST=224.0.0.0/4
MCAST_MARK=$TMP/akam_decloud_mcast
# Keeper: stock yy_cam restarts Wi-Fi (link down/up, DHCP) and that drops the dev-bound multicast route (seen on
# hardware); the keeper puts back what is missing. Only our three routes are checked; the default route, the LAN
# route and yy_cam's own 239.255.255.250 host route are never touched.
KEEP_PID=$TMP/akam_decloud_keeper.pid
KEEP_SECS=${AKAM_KEEP_SECS:-30}

# has_blackhole <net>: a blackhole line for exactly this prefix ("blackhole 0.0.0.0/1 " in busybox, trailing blanks
# included; /10 or /16 must not match /1). $_r holds one "ip route show".
has_blackhole() {
    printf '%s\n' "$_r" | grep -Eq "^blackhole $1([[:space:]]|\$)"
}

have_routes() {
    _r=$("$IP" route show 2>/dev/null) || return 1
    for _net in $ROUTES; do
        has_blackhole "$_net" || return 1
    done
    return 0
}

# add_routes: the IPv4 and IPv6 blackholes and the multicast exception. Idempotent. Needs no address on the
# interface (a blackhole route has no device), so it can run at once.
add_routes() {
    for _net in $ROUTES; do
        if _e=$("$IP" route add blackhole "$_net" 2>&1); then
            log decloud "blackhole $_net added"
        else
            case $_e in
                *exists*) ;;
                *) log decloud "WARNING: could not add blackhole $_net: $_e" ;;
            esac
        fi
    done
    for _net in $ROUTES6; do
        if _e=$("$IP" -6 route add blackhole "$_net" 2>&1); then
            log decloud "IPv6 blackhole $_net added"
        else
            case $_e in
                *exists*) ;;
                *) log decloud "IPv6 blackhole $_net not added (no IPv6?): $_e" ;;
            esac
        fi
    done
    if ! "$IP" route show 2>/dev/null | grep -q "^$MCAST "; then
        if _e=$("$IP" route add "$MCAST" dev "$IFACE" 2>&1); then
            : >"$MCAST_MARK" 2>/dev/null
            log decloud "multicast route $MCAST dev $IFACE added (discovery keeps working)"
        else
            log decloud "WARNING: could not add the multicast route (interface not up yet?): $_e"
        fi
    fi
}

# missing_routes: true when a blackhole or the multicast route is absent from one "ip route show".
missing_routes() {
    _r=$("$IP" route show 2>/dev/null) || return 1
    for _net in $ROUTES; do
        has_blackhole "$_net" || return 0
    done
    printf '%s\n' "$_r" | grep -Eq "^$MCAST([[:space:]]|\$)" || return 0
    return 1
}

# keeper_pid: prints the pid in the pidfile when that process is really a decloud keeper (a stale file or a
# reused pid after a crash must not count; /proc/<pid>/cmdline is NUL separated).
keeper_pid() {
    _kp=$(cat "$KEEP_PID" 2>/dev/null)
    case $_kp in
        '' | *[!0-9]*) return 1 ;;
    esac
    kill -0 "$_kp" 2>/dev/null || return 1
    if [ -r "/proc/$_kp/cmdline" ]; then
        tr '\0' ' ' <"/proc/$_kp/cmdline" 2>/dev/null | grep -q "decloud.sh keeper" || return 1
    fi
    echo "$_kp"
}

keeper_alive() {
    keeper_pid >/dev/null
}

# add_routes_quiet: the IPv4 part of add_routes without per-route log lines (the keeper logs one summary line).
add_routes_quiet() {
    for _net in $ROUTES; do "$IP" route add blackhole "$_net" >/dev/null 2>&1; done
    if ! "$IP" route show 2>/dev/null | grep -Eq "^$MCAST([[:space:]]|\$)"; then
        "$IP" route add "$MCAST" dev "$IFACE" >/dev/null 2>&1 && : >"$MCAST_MARK" 2>/dev/null
    fi
}

# publish_keeper: creates the pidfile with its content already in it, exclusively (a hard link fails when the
# name exists), so a reader never sees an empty file and two claimants never both succeed.
publish_keeper() {
    echo $$ >"$KEEP_PID.$$" 2>/dev/null || return 1
    ln "$KEEP_PID.$$" "$KEEP_PID" 2>/dev/null
    _pr=$?
    rm -f "$KEEP_PID.$$"
    return $_pr
}

# claim_keeper: take the pidfile atomically, so two starts close together leave one keeper. A stale file (dead or
# foreign pid) is replaced under a mkdir lock: without it, two starts could both see the stale file and the
# second one's removal would delete the pidfile the first had just claimed (two keepers). The lock holder
# re-checks, and a start that finds the lock held leaves the claim to the holder.
claim_keeper() {
    publish_keeper && return 0
    keeper_alive && return 1
    mkdir "$KEEP_PID.lk" 2>/dev/null || return 1
    if keeper_alive; then
        rmdir "$KEEP_PID.lk" 2>/dev/null
        return 1
    fi
    rm -f "$KEEP_PID"
    publish_keeper
    _cr=$?
    rmdir "$KEEP_PID.lk" 2>/dev/null
    return $_cr
}

# do_keeper: runs in the background. One `ip route show` per tick, a write only when something is missing, a
# log line only on a change (lost -> restored, or the restore failing for the first time).
do_keeper() {
    claim_keeper || return 0
    _sp=
    trap '[ -n "$_sp" ] && kill "$_sp" 2>/dev/null; rm -f "$KEEP_PID"; exit 0' TERM INT HUP
    case $KEEP_SECS in
        '' | *[!0-9]* | 0) KEEP_SECS=30 ;;
    esac
    _bad=0
    while :; do
        sleep "$KEEP_SECS" &
        _sp=$!
        # A sleep that fails at once would turn this into a tight loop: give up instead.
        wait "$_sp" || { log decloud "keeper: sleep failed, keeper exits"; break; }
        _sp=
        flag decloud || break
        if missing_routes; then
            add_routes_quiet
            if missing_routes; then
                [ "$_bad" = 1 ] || log decloud "keeper: routes missing and could not be restored"
                _bad=1
            else
                log decloud "keeper: routes were missing, restored"
                _bad=0
            fi
        elif [ "$_bad" = 1 ]; then
            log decloud "keeper: routes back"
            _bad=0
        fi
    done
    rm -f "$KEEP_PID"
    return 0
}

start_keeper() {
    keeper_alive && return 0
    ${AKAM_SH:-sh} "$0" keeper >/dev/null 2>&1 </dev/null &
    log decloud "keeper started (every ${KEEP_SECS}s)"
}

do_on() {
    if ! flag decloud; then
        log decloud "not enabled (no flag akam/decloud on the card): the camera keeps its Internet access"
        return 0
    fi
    # At once, not after the wait: the stock app is starting and must not get a window to reach the cloud.
    add_routes
    _n=0
    while [ "$_n" -lt "$TRIES" ]; do
        "$IP" addr show "$IFACE" 2>/dev/null | grep -q "inet " && break
        sleep "$NAP"
        _n=$((_n + 1))
    done
    if [ "$_n" -ge "$TRIES" ]; then
        log decloud "no address on $IFACE after $TRIES tries"
    fi
    # Again, once the interface is up: the multicast route needs it, and it is the point where a DHCP client may
    # have touched the routes. Idempotent.
    add_routes
    if have_routes; then
        log decloud "WAN blackholed, LAN kept"
    else
        log decloud "WARNING: blackhole routes not visible in the routing table"
    fi
    # Evidence for the hardware validation (PLAN section 9).
    "$IP" route show 2>/dev/null | while IFS= read -r _l; do log decloud "route: $_l"; done
    start_keeper
    return 0
}

do_off() {
    if _kp=$(keeper_pid); then
        kill "$_kp" 2>/dev/null && log decloud "keeper stopped"
        # An iteration in progress finishes first; wait (bounded) so it cannot re-add what is removed below.
        _n=0
        while [ "$_n" -lt 30 ] && kill -0 "$_kp" 2>/dev/null; do
            sleep 0.1 2>/dev/null || sleep 1
            _n=$((_n + 1))
        done
    fi
    rm -f "$KEEP_PID"
    for _net in $ROUTES; do
        "$IP" route del blackhole "$_net" 2>/dev/null && log decloud "blackhole $_net removed"
    done
    for _net in $ROUTES6; do
        "$IP" -6 route del blackhole "$_net" 2>/dev/null && log decloud "IPv6 blackhole $_net removed"
    done
    if [ -e "$MCAST_MARK" ]; then
        "$IP" route del "$MCAST" dev "$IFACE" 2>/dev/null && log decloud "multicast route $MCAST removed"
        rm -f "$MCAST_MARK"
    fi
    return 0
}

case ${1:-on} in
    on) do_on ;;
    off) do_off ;;
    keeper) do_keeper ;;
    status)
        if have_routes; then
            echo "decloud: WAN blackholed"
        else
            echo "decloud: not active"
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 on|off|status" >&2
        exit 2
        ;;
esac
exit 0
