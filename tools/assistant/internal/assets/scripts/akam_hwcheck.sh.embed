#!/bin/sh
# akam_hwcheck.sh - read-only post-flash hardware check for Akam (POSIX sh,
# BusyBox ash). Not tested on hardware: written against the V1 spec only.
#
# It never writes to flash or CONFIG, never kills or starts anything, never
# prints key material. Output: PASS|FAIL|WARN|UNKNOWN|INFO lines plus a raw
# facts section for later work. Exit code: 0 if no FAIL, 1 otherwise.
#
# Usage:
#   akam_hwcheck.sh                     full report
#   akam_hwcheck.sh --boot-window [N]   sample listening ports every second
#                                        for N seconds (default 120)
#   akam_hwcheck.sh --help
#
# Run it without installing anything in the image:
#   ssh root@CAM 'sh -s' < tools/hw_tests/akam_hwcheck.sh
# or
#   ssh root@CAM 'cat > /tmp/hwcheck.sh' < tools/hw_tests/akam_hwcheck.sh
#   ssh root@CAM 'sh /tmp/hwcheck.sh'
# or copy it to the microSD (mounted on /mnt, to be confirmed) and run it
# from the UART console: sh /mnt/akam_hwcheck.sh
#
# Measuring the telnet exposure window (--boot-window): start it as early as
# possible after power-on, from the UART console as soon as a shell prompt
# exists, with the output kept off the volatile /tmp, e.g.
#   sh /mnt/akam_hwcheck.sh --boot-window 180 > /mnt/bootwindow.log 2>&1 &
# (the script itself writes nothing; the redirection is yours). Each sample
# carries the uptime: a forbidden port already open on the first line means
# the window began BEFORE the measurement and is longer than reported. The
# summary gives first/last seen uptime per watched port. If the UART shell
# only appears late, the result is a lower bound.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) so the host test can
# run the script against a fake root. $HWCHECK_INTERVAL (default 1) is the
# --boot-window sampling period in seconds.
#
# Other knobs: HWCHECK_CPU_WINDOW (default 2) is the ak_rtsp CPU sampling
# window in seconds; HWCHECK_NIGHT_SAMPLES (default 4, 0 disables) and
# HWCHECK_NIGHT_INTERVAL (default 6) drive the day/night flap check, which
# opens a short connection to 8091 per sample (the control port serves up
# to 4 clients at once, so a sample does not disturb the others).

ROOT=${AKAM_ROOT:-}
INTERVAL=${HWCHECK_INTERVAL:-1}
CPU_WINDOW=${HWCHECK_CPU_WINDOW:-2}
NIGHT_SAMPLES=${HWCHECK_NIGHT_SAMPLES:-4}
NIGHT_INTERVAL=${HWCHECK_NIGHT_INTERVAL:-6}
TMP_DIR=${AKAM_TMP_DIR:-$ROOT/tmp}
FLAG=${AKAM_MAINT_FLAG:-$ROOT/mnt/AKAM_MAINTENANCE}
MARK=${AKAM_MAINT_MARK:-$TMP_DIR/akam_maintenance}
# Deprecated alias (former name of the flag), honoured for one release only.
LEGACY_FLAG=${AKAM_MAINT_FLAG_LEGACY-$ROOT/mnt/CAMYN_MAINTENANCE}
N_PASS=0
N_FAIL=0
N_WARN=0
N_UNK=0
# The camera's BusyBox 1.30.1 has no mktemp (nor tr): mkdir a private dir by hand.
WORK=$(mktemp -d "${TMPDIR:-/tmp}/akam_hwcheck.XXXXXX" 2>/dev/null) || WORK=
if [ -z "$WORK" ]; then
    WORK=${TMPDIR:-/tmp}/akam_hwcheck.$$
    rm -rf "$WORK"
    (umask 077 && mkdir "$WORK" 2>/dev/null) || WORK=
fi
[ -n "$WORK" ] || { echo "FAIL    cannot create a work directory" >&2; exit 1; }
trap 'rm -rf "$WORK"' EXIT

pass() { N_PASS=$((N_PASS + 1)); echo "PASS    $*"; }
fail() { N_FAIL=$((N_FAIL + 1)); echo "FAIL    $*"; }
warn() { N_WARN=$((N_WARN + 1)); echo "WARN    $*"; }
unk()  { N_UNK=$((N_UNK + 1)); echo "UNKNOWN $*"; }
info() { echo "INFO    $*"; }
section() { echo; echo "== $* =="; }
have() { command -v "$1" >/dev/null 2>&1; }

# Print a file indented; optional $2 limits the line count.
show_file() {
    if [ -r "$1" ]; then
        if [ -n "${2:-}" ]; then head -n "$2" "$1"; else cat "$1"; fi | sed 's/^/    /'
    else
        echo "    ($1 not readable)"
    fi
}
show_cmd() { "$@" 2>&1 | sed 's/^/    /'; }

maint=0
{ [ -e "$FLAG" ] || [ -e "$MARK" ]; } && maint=1
legacy_flag=0
if [ -n "$LEGACY_FLAG" ] && [ -e "$LEGACY_FLAG" ]; then maint=1; legacy_flag=1; fi

# ---------------------------------------------------------------- listening
hex_ip() { # 8 hex digits, little-endian as in /proc/net/tcp -> dotted quad
    h=$1
    echo "$((0x$(echo "$h" | cut -c7-8))).$((0x$(echo "$h" | cut -c5-6))).$((0x$(echo "$h" | cut -c3-4))).$((0x$(echo "$h" | cut -c1-2)))"
}
listen_from_proc() {
    for f in "$ROOT/proc/net/tcp" "$ROOT/proc/net/tcp6"; do
        [ -r "$f" ] || continue
        while read -r _sl lad _rad st _rest; do
            [ "$st" = "0A" ] || continue
            ip=${lad%:*}
            port=$((0x${lad##*:}))
            case ${#ip} in
                8) addr=$(hex_ip "$ip") ;;
                *) case $ip in
                       00000000000000000000000000000000) addr="::" ;;
                       *) addr="[v6:$ip]" ;;
                   esac ;;
            esac
            echo "$addr $port"
        done < "$f"
    done
}
listen_from_text() { # ss -ltn / netstat -ltn on stdin; local address is column 4
    while read -r c1 _c2 _c3 c4 _rest; do
        case $c1 in
            LISTEN|tcp|tcp6) ;;
            *) continue ;;
        esac
        case $c4 in *:*) ;; *) continue ;; esac
        echo "${c4%:*} ${c4##*:}"
    done
}
# Fills $WORK/listen with "addr port" lines; sets LISTEN_SRC. Order: ss,
# netstat, /proc/net/tcp (the guaranteed fallback).
collect_listen() {
    : > "$WORK/listen"
    LISTEN_SRC=
    if have ss; then
        ss -ltn > "$WORK/raw" 2>/dev/null
        listen_from_text < "$WORK/raw" > "$WORK/l.try"
        if [ -s "$WORK/l.try" ]; then LISTEN_SRC=ss; cp "$WORK/l.try" "$WORK/listen"; return 0; fi
    fi
    if have netstat; then
        netstat -ltn > "$WORK/raw" 2>/dev/null
        listen_from_text < "$WORK/raw" > "$WORK/l.try"
        if [ -s "$WORK/l.try" ]; then LISTEN_SRC=netstat; cp "$WORK/l.try" "$WORK/listen"; return 0; fi
    fi
    if [ -r "$ROOT/proc/net/tcp" ]; then
        listen_from_proc > "$WORK/listen"
        LISTEN_SRC=/proc/net/tcp
        return 0
    fi
    return 1
}
port_addrs() { # addresses listening on port $1, space separated
    out=
    while read -r a p; do
        [ "$p" = "$1" ] && out="$out $a"
    done < "$WORK/listen"
    echo "${out# }"
}

# ONVIF (akam_onvif.sh): the flag /etc/config/akam/onvif holds "enabled=1|0" and "port=N" (absent = off).
# Off: 80/tcp and 3702/udp must be closed. On: the HTTP port (80 by default) must listen, 3702/udp should
# (multicast WS-Discovery), and port 80 stays closed when another port was chosen.
onvif_udp_listening() { # is UDP port $1 bound? (/proc/net/udp: local_address is HEXIP:HEXPORT)
    _hx=$(printf '%04X' "$1")
    [ -r "$ROOT/proc/net/udp" ] || return 2
    while read -r _sl la _rest; do
        case $la in *:"$_hx") return 0 ;; esac
    done < "$ROOT/proc/net/udp"
    return 1
}
check_onvif_ports() {
    of=${AKAM_ONVIF_FILE:-$ROOT/etc/config/akam/onvif}
    on=0
    op=80
    if [ -r "$of" ]; then
        while IFS= read -r ol || [ -n "$ol" ]; do
            case $ol in
                enabled=1) on=1 ;;
                port=*)
                    ov=${ol#port=}
                    case $ov in ''|*[!0-9]*) ;; *) [ "${#ov}" -le 5 ] && [ "$ov" -ge 1 ] && [ "$ov" -le 65535 ] && op=$ov ;; esac
                    ;;
            esac
        done < "$of"
    fi
    if [ "$on" = 1 ]; then
        a=$(port_addrs "$op")
        if [ -n "$a" ]; then pass "ONVIF is on: port $op listening ($a)"; else fail "ONVIF is on but port $op is not listening"; fi
        onvif_udp_listening 3702; ur=$?
        case $ur in
            0) pass "ONVIF is on: UDP 3702 (WS-Discovery) bound" ;;
            1) warn "ONVIF is on but UDP 3702 is not bound (discovery will not find the camera; add it by address)" ;;
            *) unk "UDP sockets not readable: 3702 not checked" ;;
        esac
        if [ "$op" != 80 ]; then
            a=$(port_addrs 80)
            if [ -z "$a" ]; then pass "port 80 closed"; else fail "port 80 listening ($a): ONVIF uses port $op"; fi
        fi
    else
        a=$(port_addrs 80)
        if [ -z "$a" ]; then pass "port 80 closed (ONVIF off)"
        elif [ "$maint" = 1 ]; then warn "port 80 listening ($a) while the maintenance flag is present"
        else fail "port 80 listening ($a): must be closed while ONVIF is off (akam_onvif.sh enable opens it)"; fi
        if onvif_udp_listening 3702; then
            if [ "$maint" = 1 ]; then warn "UDP 3702 bound while ONVIF is off, with the maintenance flag present"
            else fail "UDP 3702 bound while ONVIF is off"; fi
        fi
    fi
}

# ---------------------------------------------------------------- MQTT (G17)
# MQTT is off by default (akam_mqtt.sh): no file, or enabled=0, means ak_rtsp has no MQTT thread and no socket, so it
# must hold no outbound TCP connection. On, it holds exactly one (to the broker). "Outbound" = a TCP socket of ak_rtsp
# (found through its /proc/<pid>/fd inodes) in state ESTABLISHED or SYN_SENT whose local port is ephemeral (32768 and
# up); RTSP and control-port clients are accepted on 554 and 8091 and do not count. Prints the number, or "?".
tcp_outbound_of_pid() {
    [ -d "$ROOT/proc/$1/fd" ] && [ -r "$ROOT/proc/net/tcp" ] || { echo "?"; return; }
    : > "$WORK/akino"
    # shellcheck disable=SC2012  # the socket links are named socket:[inode]: ls -l shows the link target, find would not
    ls -l "$ROOT/proc/$1/fd" 2>/dev/null | while read -r _l; do
        case $_l in
            *"socket:["*"]"*) _i=${_l##*socket:[}; echo "${_i%%]*}" >> "$WORK/akino" ;;
        esac
    done
    _n=0
    while read -r _sl _la _ra _st _q _tr _rt _uid _to _ino _rest; do
        case $_sl in *:) ;; *) continue ;; esac
        case $_st in 01 | 02) ;; *) continue ;; esac
        _lp=$((0x${_la##*:}))
        [ "$_lp" -ge 32768 ] || continue
        if grep -qx "$_ino" "$WORK/akino" 2>/dev/null; then _n=$((_n + 1)); fi
    done < "$ROOT/proc/net/tcp"
    echo "$_n"
}
check_mqtt() {
    section "MQTT"
    mf=${AKAM_MQTT_FILE:-$ROOT/etc/config/akam/mqtt}
    mon=0
    if [ -r "$mf" ]; then
        while IFS= read -r ml || [ -n "$ml" ]; do
            case $ml in enabled=1) mon=1 ;; esac
        done < "$mf"
    fi
    mp=$(pids_of ak_rtsp)
    mp=${mp%% *}
    if [ -z "$mp" ]; then unk "ak_rtsp not running: MQTT not checked"; return; fi
    mn=$(tcp_outbound_of_pid "$mp")
    if [ "$mn" = "?" ]; then unk "the sockets of ak_rtsp are not readable: MQTT not checked"; return; fi
    if [ "$mon" = 1 ]; then
        case $mn in
            1) pass "MQTT is on: ak_rtsp holds one outbound TCP connection (the broker)" ;;
            0) warn "MQTT is on but ak_rtsp holds no outbound TCP connection (broker unreachable or backing off: akam_mqtt.sh status)" ;;
            *) fail "MQTT is on but ak_rtsp holds $mn outbound TCP connections (one expected)" ;;
        esac
    else
        case $mn in
            0) pass "MQTT is off: ak_rtsp holds no outbound TCP connection" ;;
            *) fail "MQTT is off but ak_rtsp holds $mn outbound TCP connection(s): no socket is allowed (akam_mqtt.sh status)" ;;
        esac
    fi
}

check_ports() {
    section "listening sockets"
    if ! collect_listen; then
        unk "no way to list sockets (no ss, netstat or /proc/net/tcp)"
        return
    fi
    info "source: $LISTEN_SRC"
    sed 's/^/    /' "$WORK/listen"
    for p in 22 554 8091; do
        a=$(port_addrs $p)
        if [ -n "$a" ]; then pass "port $p listening ($a)"; else fail "port $p not listening"; fi
    done
    for p in 21 23 24; do
        a=$(port_addrs $p)
        if [ -z "$a" ]; then pass "port $p closed"
        elif [ "$maint" = 1 ]; then warn "port $p listening ($a) while the maintenance flag is present"
        else fail "port $p listening ($a): must be closed"; fi
    done
    check_onvif_ports
    a=$(port_addrs 8091)
    [ -n "$a" ] && info "8091 listen address: $a (must be LAN only)"
    return 0
}

# ---------------------------------------------------------------- processes
# read_cmdline DIR: sets CL to the space-joined argv of the process in DIR (empty for a kernel thread or when it
# vanished). /proc/<pid>/cmdline is NUL separated; the camera's BusyBox may lack strings, paste or tr, so the tool
# is picked once: strings+paste, else tr, else the process name from comm (argv0 only, no script name). CL_MODE is
# "full" or "comm"; with "comm" a process is only taken when its cmdline is not empty (kernel threads have none).
CL_MODE=
pick_cl_tool() {
    if have strings && have paste; then CL_TOOL="sp"; CL_MODE="full"
    elif have tr; then CL_TOOL="tr"; CL_MODE="full"
    else CL_TOOL="none"; CL_MODE="comm"; fi
}
read_cmdline() {
    CL=
    [ -n "$CL_MODE" ] || pick_cl_tool
    case $CL_TOOL in
        sp) CL=$({ strings -n 1 < "$1/cmdline" | paste -s -d ' ' - ; } 2>/dev/null) || CL= ;;
        tr) CL=$(tr '\0' ' ' < "$1/cmdline" 2>/dev/null) || CL= ;;
        *)
            _x=
            { read -r _x < "$1/cmdline"; } 2>/dev/null || true
            if [ -n "$_x" ]; then { read -r CL < "$1/comm"; } 2>/dev/null || CL=; fi ;;
    esac
    return 0
}
# Match $NAME against argv0 or argv1 of /proc/<pid>/cmdline (scripts run as
# "sh x.sh"), directory and ".sh" removed. Prints matching pids.
find_pids() {
    for d in "$ROOT"/proc/[0-9]*; do
        [ -d "$d" ] || continue
        pid=${d##*/}
        [ "$pid" = "$$" ] && continue
        # Each read may race with the process exiting: errors (including the
        # shell's own "can't open") go to /dev/null and the pid is skipped.
        read_cmdline "$d"
        cl=$CL
        if [ -z "$cl" ]; then { read -r cl < "$d/comm"; } 2>/dev/null || cl=; fi
        [ -n "$cl" ] || continue
        set -f
        # shellcheck disable=SC2086
        set -- $cl
        set +f
        for tok in "${1:-}" "${2:-}"; do
            [ -n "$tok" ] || continue
            b=${tok##*/}
            b=${b%.sh}
            if [ "$b" = "$NAME" ]; then echo "$pid"; break; fi
        done
    done
}
pids_of() { NAME=$1; find_pids | paste -s -d ' ' - ; }
rss_of() { # VmRSS (kB) of pid $1
    [ -r "$ROOT/proc/$1/status" ] || { echo "?"; return; }
    { while read -r k v _u; do
        [ "$k" = "VmRSS:" ] && { echo "$v"; return; }
    done < "$ROOT/proc/$1/status"; } 2>/dev/null
    echo "?"
}

check_procs() {
    section "processes"
    if ! ls "$ROOT"/proc/[0-9]* >/dev/null 2>&1; then
        unk "no /proc/<pid> entries readable; process checks skipped"
        return
    fi
    for n in ak_rtsp dropbear akam_lockdown; do
        p=$(pids_of $n)
        if [ -n "$p" ]; then pass "$n running (pid $p)"
        elif [ "$n" = akam_lockdown ] && [ "$maint" = 1 ]; then
            warn "akam_lockdown not running (expected: maintenance flag present)"
        else fail "$n not running"; fi
    done
    for n in wpa_supplicant udhcpc; do
        p=$(pids_of $n)
        if [ -n "$p" ]; then pass "$n running (pid $p)"; else warn "$n not running"; fi
    done
    for n in anyka_ipc telnetd utelnetd tcpsvd ftpd; do
        p=$(pids_of $n)
        if [ -z "$p" ]; then pass "$n absent"
        elif [ "$n" != anyka_ipc ] && [ "$maint" = 1 ]; then warn "$n running (pid $p) while the maintenance flag is present"
        else fail "$n running (pid $p)"; fi
    done
    section "resident memory (VmRSS)"
    for n in ak_rtsp dropbear akam_lockdown; do
        for p in $(pids_of $n); do info "$n pid $p VmRSS $(rss_of "$p") kB"; done
    done
}

# ---------------------------------------------------------------- UDP sockets (G14)
# Every bound UDP socket (/proc/net/udp and udp6) must be explained. 3702 belongs to ONVIF and is judged by
# check_onvif_ports above; 68 is the DHCP client (udhcpc); ports 32768 to 60999 are ephemeral client sockets (DNS,
# NTP of ./akam ntp, the UDP transport of an RTSP session) and are only reported; anything else is a listener
# nobody audited: FAIL (WARN while the maintenance flag is present). The owner is looked up through the socket
# inode in /proc/<pid>/fd when the shell can read it (best effort).
udp_bound() { # prints "port inode" per bound socket of udp and udp6
    for _uf in "$ROOT/proc/net/udp" "$ROOT/proc/net/udp6"; do
        [ -r "$_uf" ] || continue
        while read -r _sl _la _ra _st _q _tr _rt _uid _to _ino _rest; do
            case $_sl in *:) ;; *) continue ;; esac
            _lp=${_la##*:}
            case $_lp in *[!0-9A-Fa-f]* | '') continue ;; esac
            _pn=$((0x$_lp))
            [ "$_pn" -gt 0 ] || continue
            echo "$_pn ${_ino:-0}"
        done < "$_uf"
    done
}
udp_owner() { # process owning socket inode $1 (best effort)
    [ "$1" != 0 ] || { echo "?"; return; }
    for _d in "$ROOT"/proc/[0-9]*; do
        [ -d "$_d/fd" ] || continue
        # shellcheck disable=SC2010  # socket links are named socket:[inode], no glob can match them
        if ls -l "$_d/fd" 2>/dev/null | grep -q "socket:\[$1\]"; then
            read_cmdline "$_d"
            _c=$CL
            echo "${_c:-pid ${_d##*/}}"
            return
        fi
    done
    echo "?"
}
check_udp() {
    section "UDP sockets"
    if [ ! -r "$ROOT/proc/net/udp" ] && [ ! -r "$ROOT/proc/net/udp6" ]; then
        info "/proc/net/udp not readable: UDP listeners not audited"
        return 0
    fi
    udp_bound > "$WORK/udp" 2>/dev/null
    bad=0
    seen=" "
    while read -r up ui; do
        case $seen in *" $up "*) continue ;; esac
        seen="$seen$up "
        case $up in
            3702) continue ;;
            68) info "UDP 68 bound (DHCP client)"; continue ;;
        esac
        if [ "$up" -ge 32768 ] && [ "$up" -le 60999 ]; then
            info "UDP $up bound (ephemeral client socket)"
            continue
        fi
        bad=1
        who=$(udp_owner "$ui")
        if [ "$maint" = 1 ]; then warn "UDP $up bound ($who) while the maintenance flag is present: not an expected listener"
        else fail "UDP $up bound ($who): not an expected listener (only 68, 3702 with ONVIF on, and ephemeral client ports)"; fi
    done < "$WORK/udp"
    [ "$bad" = 1 ] || pass "no unexpected UDP listener"
    return 0
}

# ---------------------------------------------------------------- process allow-list (G14)
# Everything running after boot must be one of ours, a stock piece we keep on purpose, or a short-lived tool our own
# scripts call. Names are argv0, or argv1 for "sh script": directory and .sh removed (as find_pids). The vendor
# daemons msgserver and cmdserver are kept deliberately (local IPC, docs/building_your_own_firmware.md) and are
# reported as WARN "review": whether they can go is a hardware decision. Kernel threads have no cmdline: skipped.
PROC_ALLOW=" init syslogd klogd wpa_supplicant udhcpc watchdog ak_rtsp_wrapper ak_rtsp dropbear dropbearmulti sh ash sleep drop_caches ntpd timeout nice ps grep logger kill cat date wc tail sync mv "
proc_name() { # name of the process in dir $1, empty when none
    read_cmdline "$1"
    _cl=$CL
    [ -n "$_cl" ] || return 1
    set -f
    # shellcheck disable=SC2086
    set -- $_cl
    set +f
    _a0=${1##*/}
    _a0=${_a0%.sh}
    case $_a0 in
        sh | ash | busybox | -sh | -ash)
            _a1=${2:-}
            case $_a1 in '' | -*) ;; *) _a0=${_a1##*/}; _a0=${_a0%.sh} ;; esac ;;
    esac
    echo "${_a0#-}"
}
check_proc_allowlist() {
    section "process allow-list"
    pick_cl_tool
    if ! ls "$ROOT"/proc/[0-9]* >/dev/null 2>&1; then
        info "no /proc/<pid> entries readable: process allow-list skipped"
        return 0
    fi
    n_all=0
    n_odd=0
    for d in "$ROOT"/proc/[0-9]*; do
        [ -d "$d" ] || continue
        pid=${d##*/}
        [ "$pid" = "$$" ] && continue
        nm=$(proc_name "$d") || continue
        [ -n "$nm" ] || continue
        # our own forked helpers (command substitutions) show up as copies of this script
        case $nm in akam_hwcheck) continue ;; esac
        n_all=$((n_all + 1))
        case $nm in
            akam_*) continue ;;
            anyka_ipc | telnetd | utelnetd | tcpsvd | ftpd) continue ;; # judged (FAIL) by check_procs
            msgserver | cmdserver)
                warn "vendor daemon, review: $nm (pid $pid) is kept on purpose (local IPC); decide whether it can go"
                n_odd=$((n_odd + 1))
                continue ;;
        esac
        case "$PROC_ALLOW" in
            *" $nm "*) ;;
            *) warn "unexpected process: $nm (pid $pid); not on the allow-list, find who starts it (service.sh, main.sh, ethernet.sh, _ht_*.ini)"
               n_odd=$((n_odd + 1)) ;;
        esac
    done
    if [ "$n_all" -eq 0 ]; then
        # Nothing readable: a vacuous "all 0 processes are on the allow-list" would be a false PASS.
        fail "process allow-list could not be checked: no process command line was readable (cmdline reader: ${CL_TOOL:-?}, needs strings+paste, or tr, or comm)"
    elif [ "$n_odd" -eq 0 ]; then pass "all $n_all processes are on the allow-list"
    else info "$n_all processes, $n_odd to review (WARN above)"; fi
    return 0
}

# CPU use of a process from /proc/PID/stat deltas (no top dependency).
# utime and stime are fields 14 and 15 of the stat line, i.e. the 12th and 13th
# after the ") " that ends the command name (which may contain spaces).
proc_jiffies() { # utime+stime of pid $1; fails if unreadable
    [ -r "$ROOT/proc/$1/stat" ] || return 1
    { read -r st < "$ROOT/proc/$1/stat"; } 2>/dev/null || return 1
    st=${st##*) }
    set -f
    # shellcheck disable=SC2086
    set -- $st
    set +f
    [ -n "${13:-}" ] || return 1
    echo $((${12} + ${13}))
}
total_jiffies() { # sum of the aggregate "cpu" line of /proc/stat
    [ -r "$ROOT/proc/stat" ] || return 1
    while read -r k a1 a2 a3 a4 a5 a6 a7 _rest; do
        if [ "$k" = cpu ]; then echo $((a1 + a2 + a3 + a4 + a5 + a6 + a7)); return 0; fi
    done < "$ROOT/proc/stat"
    return 1
}
proc_wakeups() { # sum of voluntary_ctxt_switches over all threads of pid $1; empty when unreadable
    _t=0
    _any=0
    for _s in "$ROOT/proc/$1"/task/[0-9]*/status; do
        [ -r "$_s" ] || continue
        while read -r _k _v _r; do
            if [ "$_k" = "voluntary_ctxt_switches:" ]; then _t=$((_t + _v)); _any=1; fi
        done < "$_s"
    done
    [ "$_any" = 1 ] && echo "$_t"
    return 0
}
check_cpu() {
    section "CPU use (ak_rtsp, ${CPU_WINDOW}s window)"
    if ! ls "$ROOT"/proc/[0-9]* >/dev/null 2>&1; then unk "no /proc/<pid> entries: CPU use skipped"; return; fi
    p=$(pids_of ak_rtsp)
    p=${p%% *}
    if [ -z "$p" ]; then unk "ak_rtsp not running: CPU use skipped"; return; fi
    if ! j1=$(proc_jiffies "$p") || ! t1=$(total_jiffies); then unk "/proc/$p/stat or /proc/stat unreadable: CPU use skipped"; return; fi
    w1=$(proc_wakeups "$p")
    sleep "$CPU_WINDOW"
    w2=$(proc_wakeups "$p")
    # G10: wakeups per second = voluntary context switches of all threads (each sleep that ended); the budget of
    # specs/10 is about 80/s with no client, the estimate from the code is about 140/s before the cuts.
    if [ -n "$w1" ] && [ -n "$w2" ] && [ "$CPU_WINDOW" -gt 0 ]; then
        info "ak_rtsp pid $p wakeups $(( (w2 - w1) / CPU_WINDOW ))/s over ${CPU_WINDOW}s (voluntary context switches, all threads)"
    fi
    if ! j2=$(proc_jiffies "$p") || ! t2=$(total_jiffies); then unk "ak_rtsp (pid $p) vanished during the CPU sample"; return; fi
    dt=$((t2 - t1))
    dj=$((j2 - j1))
    if [ "$dt" -le 0 ]; then unk "CPU clock did not advance during the sample"; return; fi
    tenths=$((dj * 1000 / dt))
    info "ak_rtsp pid $p CPU $((tenths / 10)).$((tenths % 10))% of the machine over ${CPU_WINDOW}s ($dj of $dt jiffies)"
    if [ "$tenths" -ge 900 ]; then warn "ak_rtsp uses $((tenths / 10))% CPU: no headroom"; fi
}

# ------------------------------------------------------------------ files
mode_of() { # "mode owner:group" of $1; octal via stat, else the ls string
    if have stat && m=$(stat -c '%a %U:%G' "$1" 2>/dev/null) && [ -n "$m" ]; then echo "$m"; return; fi
    # shellcheck disable=SC2012
    ls -ldn "$1" 2>/dev/null | { read -r perm _l o g _rest; echo "$perm $o:$g (ls)"; }
}
wr_go() { # true if the mode string from mode_of is group/other writable
    wm=${1%% *}
    case $wm in
        [0-7]*)
            while [ ${#wm} -gt 3 ]; do wm=${wm#?}; done
            g=$(echo "$wm" | cut -c2)
            o=$(echo "$wm" | cut -c3)
            case $g$o in [2367]?|?[2367]) return 0 ;; esac
            return 1 ;;
        *)
            [ "$(echo "$wm" | cut -c6)" = w ] && return 0
            [ "$(echo "$wm" | cut -c9)" = w ] && return 0
            return 1 ;;
    esac
}
root_home() { # HOME field of root in $ROOT/etc/passwd, as seen at runtime
    [ -r "$ROOT/etc/passwd" ] || return 1
    while IFS=: read -r u _p _uid _gid _gecos h _s; do
        if [ "$u" = root ]; then echo "$h"; return 0; fi
    done < "$ROOT/etc/passwd"
    return 1
}
mode_line() { # mode_line LABEL FULLPATH ; sets MODE_BAD=1 if group/other writable
    m=$(mode_of "$2")
    if wr_go "$m"; then fail "$1 mode $m is group/other writable"; MODE_BAD=1; else info "$1 mode $m"; fi
}
count_keys() { # number of public-key lines in file $1
    grep -cE '^(ssh-|ecdsa-|sk-|[^#[:space:]].* (ssh-|ecdsa-|sk-))' "$1" 2>/dev/null
}
check_modes() {
    section "permissions (Dropbear rejects authorized_keys under group/other-writable paths)"
    # Dropbear walks from the key directory up to root's HOME (or "/"), so the
    # chain to check depends on HOME. akam_ssh.sh moves root's HOME to
    # /tmp/akam_home (private 0700 tmpfs): "/" and "/etc" are then not on the
    # path, and the stock 0775 there is expected.
    home=$(root_home) || home=
    info "root HOME in passwd: '${home:-?}'"
    for p in / /etc; do
        full=$ROOT$p
        [ "$p" = / ] && full=${ROOT:-/}
        if [ -e "$full" ]; then info "$p mode $(mode_of "$full") (informational: only on the key path if root's HOME is not the key directory)"
        else unk "$p missing"; fi
    done
    case $home in
        /?*) hk=$ROOT$home/authorized_keys ;;
        *) hk= ;;
    esac
    if [ -n "$hk" ] && [ -e "$hk" ]; then
        info "key path: root HOME $home holds authorized_keys"
        MODE_BAD=0
        mode_line "$home" "$ROOT$home"
        mode_line "$home/authorized_keys" "$hk"
        [ "$MODE_BAD" = 0 ] && pass "key path ($home) is not group/other writable"
        if [ -r "$hk" ]; then
            n=$(count_keys "$hk")
            if [ "${n:-0}" -gt 0 ]; then pass "$home/authorized_keys holds $n key line(s) (content not shown)"
            else fail "$home/authorized_keys has no key lines"; fi
        else unk "$home/authorized_keys not readable"; fi
        for p in /etc/config /etc/config/dropbear; do
            if [ -e "$ROOT$p" ]; then info "$p mode $(mode_of "$ROOT$p")"; else warn "$p missing"; fi
        done
    else
        warn "root HOME '${home:-?}' holds no authorized_keys: Dropbear walks the persistent path, which must not be group/other writable"
        for p in /etc/config /etc/config/dropbear /etc/config/dropbear/authorized_keys; do
            full=$ROOT$p
            if [ ! -e "$full" ]; then
                fail "$p missing"
                continue
            fi
            m=$(mode_of "$full")
            if wr_go "$m"; then fail "$p mode $m is group/other writable"; else info "$p mode $m"; fi
        done
        for p in / /etc; do
            full=$ROOT$p
            [ "$p" = / ] && full=${ROOT:-/}
            if [ -e "$full" ] && wr_go "$(mode_of "$full")"; then
                fail "$p is group/other writable and on the key path (root HOME is not the key directory)"
            fi
        done
    fi
    ak=$ROOT/etc/config/dropbear/authorized_keys
    if [ -r "$ak" ]; then
        n=$(count_keys "$ak")
        if [ "${n:-0}" -gt 0 ]; then pass "persistent authorized_keys holds $n key line(s) (content not shown)"
        else fail "persistent authorized_keys has no key lines"; fi
    elif [ -e "$ak" ]; then unk "persistent authorized_keys not readable"
    else fail "persistent authorized_keys missing"; fi
    echo "  -- /etc/config/dropbear"
    # shellcheck disable=SC2012
    ls "$ROOT/etc/config/dropbear/" 2>/dev/null | sed 's/^/    /'
    rsh=
    if [ -r "$ROOT/etc/passwd" ]; then
        while IFS=: read -r u _p _uid _gid _gecos _home s; do
            if [ "$u" = root ]; then rsh=$s; break; fi
        done < "$ROOT/etc/passwd"
        info "root shell in passwd: '$rsh' (empty means /bin/sh)"
        if [ -n "$rsh" ] && [ -r "$ROOT/etc/shells" ]; then
            if grep -qxF "$rsh" "$ROOT/etc/shells"; then pass "root shell is listed in /etc/shells"
            else fail "root shell $rsh is not in /etc/shells: Dropbear would refuse the login"; fi
        elif [ -n "$rsh" ]; then
            warn "/etc/shells missing; Dropbear falls back to /bin/sh, /bin/csh"
        fi
    else
        unk "/etc/passwd not readable"
    fi
}

# Network hostname: information only. Effective = stored name or akam-XXXX (see
# akam_hostname.sh); the router shows a new name after the next boot / DHCP lease.
check_hostname() {
    section "network hostname"
    hn_f=$ROOT/etc/config/akam/hostname
    hn_s=$ROOT/usr/sbin/akam_hostname.sh
    hn_cfg=
    [ -r "$hn_f" ] && IFS= read -r hn_cfg < "$hn_f"
    hn_eff=
    [ -x "$hn_s" ] && hn_eff=$(AKAM_ROOT=${AKAM_ROOT:-} "$hn_s" show 2>/dev/null)
    hn_k=
    [ -r "$ROOT/proc/sys/kernel/hostname" ] && IFS= read -r hn_k < "$ROOT/proc/sys/kernel/hostname"
    if [ -z "$hn_eff" ]; then
        info "hostname: akam_hostname.sh not installed (kernel name: ${hn_k:-unknown})"
        return 0
    fi
    info "hostname: effective $hn_eff, configured ${hn_cfg:-none (default akam-XXXX)}, kernel ${hn_k:-unknown}"
    if [ -n "$hn_k" ] && [ "$hn_k" != "$hn_eff" ]; then
        warn "hostname: kernel name '$hn_k' differs from the effective name '$hn_eff' (applied at boot; was it changed by hand?)"
    fi
}

# RTSP Digest authentication (src/ak_rtsp/rtsp_auth.h): information only, never a FAIL. The
# credentials file is "user:akam:HA1"; only the user name is shown, never the hash.
check_rtsp_auth() {
    section "RTSP authentication (optional)"
    f=${AKAM_RTSP_AUTH_FILE:-$ROOT/etc/config/akam/rtsp_auth}
    if [ ! -e "$f" ]; then
        info "RTSP auth: disabled (no credentials file): the video stream is open to anyone who can reach port 554"
        info "talk: not offered (two-way audio needs the RTSP password)"
        return 0
    fi
    line=
    IFS= read -r line < "$f" 2>/dev/null
    line=${line%"$(printf '\r')"}
    ra_u=${line%%:*}
    ra_rest=${line#*:}
    ra_r=${ra_rest%%:*}
    ra_h=${ra_rest#*:}
    if [ "$ra_rest" = "$line" ] || [ -z "$ra_u" ] || [ "$ra_r" != akam ] || [ "${#ra_h}" -ne 32 ]; then
        warn "RTSP auth: INVALID credentials file: ak_rtsp refuses every protected request (run akam_rtsp_auth.sh set USER, or disable)"
        return 0
    fi
    case $ra_h in
        *[!0-9a-f]*) warn "RTSP auth: INVALID credentials file (hash is not 32 lowercase hex characters): ak_rtsp refuses every protected request"; return 0 ;;
    esac
    info "RTSP auth: enabled (user $ra_u, Digest)"
    info "talk: offered on port 554 to ONVIF backchannel clients (audio.talk_enable and audio out permitting)"
    ra_m=$(mode_of "$f")
    case ${ra_m%% *} in
        *00) ;;
        *) warn "RTSP auth: $f mode $ra_m is readable by group/others (expected 600)" ;;
    esac
}

# Night mode/state through 8091, plus a short flap check: sample night.state a
# few times and WARN if it changed more than once (a day/night flap).
night_get() { # prints the value of GET $1 ("night.state=night" -> night)
    # shellcheck disable=SC2086
    printf 'GET %s\n' "$1" | $NIGHT_TMO nc -w 2 127.0.0.1 8091 2>/dev/null | head -n 3 > "$WORK/night"
    while IFS= read -r l; do
        case $l in "$1="*) v=${l#*=}; echo "${v%% *}"; return 0 ;; esac
    done < "$WORK/night"
    return 1
}
# Debug flag of the control port (writes from the network, akam_debug.sh): information only.
check_debug() {
    dbg_v=$(night_get ctl.debug 2>/dev/null)
    case $dbg_v in
        0) info "debug: off (8091 is read-only for the network; writes only from the camera itself)" ;;
        1) dbg_r=$(night_get ctl.debug_remaining 2>/dev/null)
           warn "debug: ON, the network may write to 8091 (${dbg_r:-?} s left); akam_debug.sh off to close it" ;;
        *) info "debug: not reported (image without the 8091 read-only mode, akam_debug.sh)" ;;
    esac
}
check_night() {
    section "night mode and day/night flap check"
    if [ "$NIGHT_SAMPLES" -le 0 ] 2>/dev/null; then info "night check disabled (HWCHECK_NIGHT_SAMPLES=0)"; return; fi
    if ! have nc; then unk "nc not available: night check skipped"; return; fi
    if [ -z "$(port_addrs 8091)" ]; then unk "8091 not listening: night check skipped"; return; fi
    NIGHT_TMO=
    have timeout && NIGHT_TMO="timeout 6"
    mode=$(night_get night.mode) || mode=
    if [ -n "$mode" ]; then info "night.mode=$mode"; else unk "night.mode not readable through 8091"; fi
    seq_= ; prev= ; changes=0 ; got=0 ; i=0
    while [ "$i" -lt "$NIGHT_SAMPLES" ]; do
        i=$((i + 1))
        st=$(night_get night.state) || st=
        if [ -n "$st" ]; then
            got=$((got + 1))
            seq_="$seq_ $st"
            if [ -n "$prev" ] && [ "$st" != "$prev" ]; then changes=$((changes + 1)); fi
            prev=$st
        else
            seq_="$seq_ ?"
        fi
        [ "$i" -lt "$NIGHT_SAMPLES" ] && sleep "$NIGHT_INTERVAL"
    done
    if [ "$got" -eq 0 ]; then unk "night.state not readable through 8091"; return; fi
    info "night.state samples (${NIGHT_SAMPLES} x ${NIGHT_INTERVAL}s):$seq_"
    if [ "$changes" -gt 1 ]; then warn "night.state changed $changes times in $NIGHT_SAMPLES samples: day/night flapping"
    else pass "night.state stable ($changes change(s) in $got sample(s)); now $prev"; fi
}

check_misc() {
    section "maintenance flag and SD"
    if [ "$maint" = 1 ]; then warn "maintenance mode ($MARK or $FLAG present): telnet/ftp are NOT locked down in this mode"
    else pass "maintenance mode off ($MARK and $FLAG absent)"; fi
    if [ "$legacy_flag" = 1 ]; then warn "DEPRECATED flag name ${LEGACY_FLAG##*/}: rename it to ${FLAG##*/} (the old name is removed in the next release)"; fi
    if [ -r "$ROOT/proc/mounts" ]; then
        if grep -q ' /mnt ' "$ROOT/proc/mounts"; then info "SD mount: $(grep ' /mnt ' "$ROOT/proc/mounts" | head -n 1)"
        else info "nothing mounted on /mnt (no SD card, or another mount point: see mounts below)"; fi
    fi
    section "8091 control port"
    if ! have nc; then unk "nc not available: cannot query 8091 LIST locally"
    elif [ -z "$(port_addrs 8091)" ]; then unk "8091 not listening, LIST skipped"
    else
        tmo=
        have timeout && tmo="timeout 8"
        # bounded in time (timeout, nc -w) and in size (head)
        # shellcheck disable=SC2086
        printf 'LIST\n' | $tmo nc -w 3 127.0.0.1 8091 2>&1 | head -n 50 > "$WORK/list"
        if [ -s "$WORK/list" ]; then pass "8091 answered LIST ($(wc -l < "$WORK/list") line(s))"; show_file "$WORK/list" 8
        else fail "8091 gave no answer to LIST"; fi
        check_debug
    fi
    check_night
    section "entropy and Dropbear re-exec"
    if [ -r "$ROOT/proc/sys/kernel/random/entropy_avail" ]; then
        read -r e < "$ROOT/proc/sys/kernel/random/entropy_avail"
        if [ "$e" -ge 64 ] 2>/dev/null; then pass "entropy_avail=$e"; else warn "entropy_avail=$e (low; key generation may be slow)"; fi
    else unk "entropy_avail unreadable"; fi
    if [ -e "$ROOT/proc/self/exe" ] || [ -L "$ROOT/proc/self/exe" ]; then pass "/proc/self/exe available (Dropbear re-exec)"
    else warn "/proc/self/exe missing: Dropbear re-exec may fail"; fi
    section "akam logs (last 15 lines)"
    for l in akam_ssh.log akam_lockdown.log; do
        echo "  -- $TMP_DIR/$l"
        if [ -r "$TMP_DIR/$l" ]; then tail -n 15 "$TMP_DIR/$l" | sed 's/^/    /'; else echo "    (absent)"; fi
    done
}

# ------------------------------------------------------------------ facts
check_facts() {
    section "FACTS: system"
    show_cmd uname -a
    show_file "$ROOT/proc/version"
    for k in Processor "model name" Hardware "CPU architecture" Features; do
        grep -i "^$k" "$ROOT/proc/cpuinfo" 2>/dev/null | head -n 1 | sed 's/^/    /'
    done
    echo "  -- uptime / load"
    show_file "$ROOT/proc/uptime"
    show_file "$ROOT/proc/loadavg"
    echo "  -- memory"
    have free && show_cmd free
    grep -E '^(MemTotal|MemFree|Cached|Buffers|SwapTotal):' "$ROOT/proc/meminfo" 2>/dev/null | sed 's/^/    /'
    echo "  -- /proc/mtd"
    show_file "$ROOT/proc/mtd"
    echo "  -- mounts"
    show_file "$ROOT/proc/mounts"
    if have dmesg; then
        echo "  -- dmesg: squashfs/jffs2/watchdog (compression shows here if logged)"
        dmesg 2>/dev/null | grep -iE 'squashfs|jffs2|watchdog' | tail -n 10 | sed 's/^/    /'
    fi
    echo "  -- df"
    have df && show_cmd df
    echo "  -- /etc/shells"
    show_file "$ROOT/etc/shells"
    section "FACTS: BusyBox and applets"
    : > "$WORK/applets"
    if have busybox; then
        busybox 2>&1 | head -n 1 | sed 's/^/    /'
        busybox --list > "$WORK/applets" 2>/dev/null
    else
        info "no busybox binary on PATH"
    fi
    for a in stat cmp grep strings paste kill ps mount nc netstat ss sleep tail head cut sed wc mktemp tr; do
        if have "$a"; then
            if grep -qx "$a" "$WORK/applets" 2>/dev/null; then info "applet $a: present (busybox)"
            else info "applet $a: present"; fi
        else
            case $a in
                ss|nc|netstat|cmp|mktemp|tr) warn "applet $a: MISSING (optional, a fallback exists or the check is skipped)" ;;
                *) warn "applet $a: MISSING" ;;
            esac
        fi
    done
    if have grep; then
        if echo ab | grep -E 'a(b|c)' >/dev/null 2>&1; then pass "grep -E works"; else fail "grep -E unsupported"; fi
    fi
    if have stat; then
        if stat -c '%a' "${ROOT:-/}" >/dev/null 2>&1; then pass "stat -c works"; else warn "stat has no -c"; fi
    fi
    if have mount; then
        if mount --help 2>&1 | grep -qi bind; then info "mount help mentions bind (mount --bind not exercised: read-only script)"
        else unk "mount --bind support not visible in help (not exercised)"; fi
    fi
    echo "  -- ps format"
    have ps && { ps 2>&1 | head -n 3 | sed 's/^/    /'; }
    echo "  -- uptime / top"
    have uptime && show_cmd uptime
    have top && { top -bn1 2>/dev/null | head -n 8 | sed 's/^/    /'; }
    return 0
}

# ------------------------------------------------------------ boot window
boot_window() {
    dur=${1:-120}
    case $dur in ''|*[!0-9]*) echo "bad duration: $dur" >&2; exit 2 ;; esac
    echo "boot window: sampling every ${INTERVAL}s for ${dur}s (u = uptime seconds)"
    watched="21 23 24 80 22 554 8091"
    for w in $watched; do eval "first_$w=; last_$w="; done
    i=0
    while [ "$i" -le "$dur" ]; do
        up="?"
        [ -r "$ROOT/proc/uptime" ] && read -r up _idle < "$ROOT/proc/uptime"
        if collect_listen; then
            line=
            while read -r _a p; do
                case " $line " in *" $p "*) ;; *) line="$line $p" ;; esac
                for w in $watched; do
                    if [ "$p" = "$w" ]; then
                        eval "[ -n \"\$first_$w\" ] || first_$w=\$up; last_$w=\$up"
                    fi
                done
            done < "$WORK/listen"
            echo "t=+${i}s u=$up listening:${line:- (none)}"
        else
            echo "t=+${i}s u=$up listening: UNKNOWN (no socket source)"
        fi
        i=$((i + 1))
        [ "$i" -le "$dur" ] && sleep "$INTERVAL"
    done
    echo "summary (uptime seconds first seen / last seen):"
    for w in $watched; do
        eval "f=\$first_$w; l=\$last_$w"
        if [ -z "$f" ]; then echo "  port $w: never seen"
        else echo "  port $w: first u=$f last u=$l"; fi
    done
    echo "note: a port seen at the first sample was already open before sampling began."
    exit 0
}

usage() { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; }

case ${1:-} in
    --boot-window) boot_window "${2:-}" ;;
    -h|--help) usage; exit 0 ;;
    '') ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
esac

echo "akam_hwcheck: read-only check; results are evidence, not validation"
check_ports
check_udp
check_mqtt
check_procs
check_proc_allowlist
check_cpu
check_modes
check_misc
check_rtsp_auth
check_hostname
check_facts
section "manual tests still to do (not automated)"
cat <<'MSG'
    - microphone audible, night mode/IR cut, H.264 1080p stream in a player
    - 80/tcp and 3702/udp: closed by default; open only after ./akam onvif enable
    - MQTT (./akam mqtt set): off = ak_rtsp holds no outbound TCP connection; on = exactly one (to the broker)
    - 8091: LIST / GET from the PC (read-only), SET refused, SET after akam_debug.sh on
    - SSH login by key from another machine, and persistence across a power cut
    - boot without microSD, Wi-Fi auto-reconnect, Telnet never reachable
    - run with --boot-window right after power-on (see header)
MSG
section "summary"
echo "PASS=$N_PASS FAIL=$N_FAIL WARN=$N_WARN UNKNOWN=$N_UNK"
[ "$N_FAIL" -eq 0 ]
