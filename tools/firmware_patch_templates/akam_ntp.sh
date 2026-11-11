#!/bin/sh
# akam_ntp.sh -- opt-in time sync for a camera that has no clock (no RTC: it boots in the past) (G08).
#
# Installed as /usr/sbin/akam_ntp.sh (mode 755). Run it over SSH, as root:
#   akam_ntp.sh set SERVER [SERVER [SERVER]]   save 1 to 3 NTP servers (host names or IPv4 addresses) and start
#   akam_ntp.sh off                            remove the setting and stop the helper
#   akam_ntp.sh status                         servers, the camera clock, whether the helper runs, last log line
#   akam_ntp.sh run                            the loop (started by ak_rtsp_wrapper.sh at boot, only when the
#                                              file below exists; fire and forget, detached)
# Exit status: 0 ok, 1 failure, 2 usage or an invalid server.
#
# OFF BY DEFAULT: no file, no helper, no network traffic. The setting is /etc/config/akam/ntp (CONFIG partition,
# persistent, mode 0600, directory 0700): one `server=NAME` line per server, at most 3, LF only. A server is a
# host name (letters, digits, hyphens, dots) or a dotted IPv4 address; anything else is refused (a name cannot
# start with a hyphen, so it can never be taken as an option by ntpd).
#
# What `run` does: BusyBox ntpd in one-shot mode (`ntpd -n -q -p S ...`, killed after 60 s by `timeout` when that
# applet exists). One shot at a time, not a daemon, on purpose: no resident ntpd process (RAM), no UDP socket
# kept open on the network (the port audit sees none), and the camera crystal drifts about a second in 6 hours,
# which is enough for time stamps and the ONVIF/WS-Security window. Until the clock is believable (year 2024 or
# later) it retries after 15, 30, 60, 120, 300 then 600 s (the network may still be coming up); once set it syncs
# again every 6 hours. Low priority: `nice -n 10` when nice exists. The clock is stepped by ntpd (settimeofday);
# a step during a viewing session shifts the RTCP wall clock once (the stream itself uses the monotonic clock).
# Log: /tmp/akam_ntp.log (RAM, capped at 100 lines), and syslog when `logger` exists.
#
# POSIX sh for the camera's BusyBox 1.30.1: no tr, no mktemp, no stat. All paths are prefixed by $AKAM_ROOT (empty
# on the camera) and the commands are overridable, so tests/akam_ntp_test.sh can run it with stubs.

AKAM_ROOT=${AKAM_ROOT:-}
CONFIG_DIR=${AKAM_CONFIG_DIR:-$AKAM_ROOT/etc/config}
AKAM_DIR=$CONFIG_DIR/akam
NTP_FILE=${AKAM_NTP_FILE:-$AKAM_DIR/ntp}
TMP_DIR=${AKAM_TMP_DIR:-$AKAM_ROOT/tmp}
LOG=$TMP_DIR/akam_ntp.log
LOCKDIR=$TMP_DIR/akam_ntp.lock
NTPD=${AKAM_NTPD:-ntpd}
DATE=${AKAM_DATE:-date}
LOGGER=${AKAM_LOGGER:-logger}
KILL=${AKAM_KILL:-kill}
SYNC_TIMEOUT=${AKAM_NTP_TIMEOUT:-60}
TIMEOUT_BIN=${AKAM_TIMEOUT_BIN:-timeout}
RESYNC_S=${AKAM_NTP_RESYNC_S:-21600}
MAX_CYCLES=${AKAM_NTP_MAX_CYCLES:-0} # 0 = forever (the tests set a number)
MAX_SERVERS=3
LOG_MAX_LINES=100
LOG_KEEP_LINES=50
MIN_YEAR=2024

log() {
    echo "akam_ntp: $*" >>"$LOG" 2>/dev/null
    if [ "$(wc -l <"$LOG" 2>/dev/null)" -gt "$LOG_MAX_LINES" ] 2>/dev/null; then
        tail -n "$LOG_KEEP_LINES" "$LOG" >"$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
    fi
    command -v "$LOGGER" >/dev/null 2>&1 && "$LOGGER" -t akam_ntp "$*" 2>/dev/null
    return 0
}

die() {
    echo "akam_ntp: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 set SERVER [SERVER [SERVER]] | off | status | run    (SERVER: a host name or an IPv4 address)" >&2
    exit 2
}

# valid_server NAME: a host name (labels of letters, digits and inner hyphens, dots between, at most 253 characters)
# or a dotted IPv4 address. Never starts with a hyphen or a dot.
valid_server() {
    case $1 in
        '' | -* | .* | *. | *..* | *[!A-Za-z0-9.-]*) return 1 ;;
    esac
    [ "${#1}" -le 253 ] || return 1
    printf '%s\n' "$1" | grep -Eq '^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$'
}

# read_servers: sets SERVERS (space separated, at most 3, invalid or extra lines ignored) and NSERV.
# Returns 0 when the file exists, 1 when absent.
read_servers() {
    SERVERS=
    NSERV=0
    [ -f "$NTP_FILE" ] || return 1
    while IFS= read -r _l || [ -n "$_l" ]; do
        _l=${_l%"$(printf '\r')"}
        case $_l in
            server=*)
                _s=${_l#server=}
                valid_server "$_s" || continue
                [ "$NSERV" -lt "$MAX_SERVERS" ] || continue
                SERVERS="$SERVERS $_s"
                NSERV=$((NSERV + 1))
                ;;
        esac
    done <"$NTP_FILE" 2>/dev/null
    SERVERS=${SERVERS# }
    return 0
}

# clock_set: 0 when the camera clock year is MIN_YEAR or later.
clock_set() {
    _y=$($DATE -u +%Y 2>/dev/null)
    case $_y in '' | *[!0-9]*) return 1 ;; esac
    [ "${#_y}" -eq 4 ] && [ "$_y" -ge "$MIN_YEAR" ]
}

# helper_pid: sets HPID to the pid of a running `run` (from the lock), empty when none.
helper_pid() {
    HPID=
    read -r _lp 2>/dev/null <"$LOCKDIR/pid"
    case "$_lp" in '' | *[!0-9]*) return 1 ;; esac
    kill -0 "$_lp" 2>/dev/null || return 1
    grep -qa akam_ntp "${AKAM_PROC_DIR:-/proc}/$_lp/cmdline" 2>/dev/null || return 1
    HPID=$_lp
    return 0
}

take_lock() {
    mkdir -p "$TMP_DIR" 2>/dev/null
    if ! mkdir "$LOCKDIR" 2>/dev/null; then
        helper_pid && return 1 # another helper runs
        rm -rf "$LOCKDIR"
        mkdir "$LOCKDIR" 2>/dev/null || return 1
    fi
    echo $$ >"$LOCKDIR/pid"
    return 0
}

# sync_once: one ntpd -q pass against $SERVERS. 0 when ntpd reported success.
sync_once() {
    _args=
    for _s in $SERVERS; do _args="$_args -p $_s"; done
    _pre=
    command -v nice >/dev/null 2>&1 && _pre="nice -n 10"
    if command -v "$TIMEOUT_BIN" >/dev/null 2>&1; then
        # shellcheck disable=SC2086  # word list on purpose: no server name contains a space or starts with -
        $_pre "$TIMEOUT_BIN" "$SYNC_TIMEOUT" "$NTPD" -n -q $_args >/dev/null 2>&1
    else
        # No `timeout` applet (BusyBox 1.30.1 may lack it): ntpd -q can wait forever on an unreachable server
        # (or a name that does not resolve), so a shell watchdog kills it after SYNC_TIMEOUT seconds.
        # shellcheck disable=SC2086
        $_pre "$NTPD" -n -q $_args >/dev/null 2>&1 &
        _np=$!
        _t=0
        while kill -0 "$_np" 2>/dev/null; do
            if [ "$_t" -ge "$SYNC_TIMEOUT" ]; then
                kill "$_np" 2>/dev/null
                wait "$_np" 2>/dev/null
                return 1
            fi
            sleep 1
            _t=$((_t + 1))
        done
        wait "$_np"
    fi
}

# retry_wait N: seconds to wait after the Nth consecutive failed pass (1-based).
retry_wait() {
    case $1 in
        1) WAIT=15 ;;
        2) WAIT=30 ;;
        3) WAIT=60 ;;
        4) WAIT=120 ;;
        5) WAIT=300 ;;
        *) WAIT=600 ;;
    esac
}

release_lock() {
    _mp=
    read -r _mp <"$LOCKDIR/pid" 2>/dev/null
    [ "$_mp" = "$$" ] && rm -rf "$LOCKDIR"
    return 0
}

do_run() {
    read_servers || exit 0 # no file: nothing to do (off)
    if [ "$NSERV" -eq 0 ]; then
        log "no valid server in $NTP_FILE, nothing to do"
        exit 0
    fi
    take_lock || exit 0
    # remove the lock only while it is still ours (stop_helper may have removed it and a new helper taken it)
    trap release_lock EXIT
    trap 'exit 0' INT TERM HUP
    log "start (pid $$), servers: $SERVERS"
    _fails=0
    _n=0
    while :; do
        read_servers || break
        [ "$NSERV" -gt 0 ] || break
        if sync_once; then
            if [ "$_fails" -gt 0 ]; then log "sync ok after $_fails failed pass(es)"; else log "sync ok"; fi
            _fails=0
            WAIT=$RESYNC_S
        else
            _fails=$((_fails + 1))
            retry_wait "$_fails"
            # a clock that is already believable (set by hand) is only re-checked at the slow rate
            if clock_set; then WAIT=$RESYNC_S; fi
            log "sync failed (pass $_fails), next try in ${WAIT}s"
        fi
        _n=$((_n + 1))
        if [ "$MAX_CYCLES" -gt 0 ] && [ "$_n" -ge "$MAX_CYCLES" ]; then break; fi
        # sleep in the background and wait for it: a TERM from `off`/`set` interrupts `wait` at once, while a
        # foreground sleep would delay the trap by up to 6 hours
        sleep "$WAIT" &
        wait $!
    done
    return 0
}

# write_file SERVER...: atomic, 0600, directory 0700.
write_file() {
    umask 077
    mkdir -p "$AKAM_DIR" 2>/dev/null || die "cannot create $AKAM_DIR (is /etc/config writable?)"
    chmod 700 "$AKAM_DIR" 2>/dev/null
    _tmp=$NTP_FILE.new.$$
    rm -f "$_tmp"
    : >"$_tmp" 2>/dev/null || { rm -f "$_tmp"; die "cannot write $_tmp"; }
    for _s in "$@"; do printf 'server=%s\n' "$_s" >>"$_tmp"; done
    chmod 600 "$_tmp" 2>/dev/null
    sync
    mv -f "$_tmp" "$NTP_FILE" 2>/dev/null || { rm -f "$_tmp"; die "cannot replace $NTP_FILE"; }
    sync
}

stop_helper() {
    if helper_pid; then
        $KILL "$HPID" 2>/dev/null
        sleep 1
        rm -rf "$LOCKDIR"
        return 0
    fi
    return 1
}

do_set() {
    [ "$#" -ge 1 ] && [ "$#" -le "$MAX_SERVERS" ] || { echo "akam_ntp: give 1 to $MAX_SERVERS servers" >&2; exit 2; }
    for _s in "$@"; do
        valid_server "$_s" || { echo "akam_ntp: '$_s' is not a host name or IPv4 address; nothing changed." >&2; exit 2; }
    done
    write_file "$@"
    echo "Time sync on, servers: $*. The camera asks them for the time at start and every 6 hours (UDP 123 outgoing)."
    stop_helper
    if [ -z "${AKAM_NTP_NO_START:-}" ]; then
        "$0" run </dev/null >/dev/null 2>&1 &
        echo "Started now; see: $0 status"
    else
        echo "Starts at the next boot."
    fi
}

do_off() {
    if [ -e "$NTP_FILE" ]; then
        rm -f "$NTP_FILE" || die "cannot remove $NTP_FILE"
        sync
        echo "Time sync off: no more traffic to time servers. The clock keeps its current value until the next reboot."
    else
        echo "Time sync was already off."
    fi
    stop_helper
    return 0
}

do_status() {
    if read_servers; then
        if [ "$NSERV" -gt 0 ]; then echo "time sync: on, servers: $SERVERS"; else echo "time sync: file present but no valid server (treated as off)"; fi
    else
        echo "time sync: off (default)"
    fi
    echo "camera clock: $($DATE -u '+%Y-%m-%d %H:%M:%S' 2>/dev/null) UTC"
    if clock_set; then echo "sys.time_set: 1"; else echo "sys.time_set: 0 (the clock is before $MIN_YEAR: no RTC, nothing set it yet)"; fi
    if helper_pid; then echo "helper: running (pid $HPID)"; else echo "helper: not running"; fi
    [ ! -f "$LOG" ] || { _last=$(tail -n 1 "$LOG" 2>/dev/null); [ -z "$_last" ] || echo "last log: $_last"; }
    return 0
}

case ${1:-} in
    set) shift; do_set "$@" ;;
    off) [ "$#" -eq 1 ] || usage; do_off ;;
    status) [ "$#" -eq 1 ] || usage; do_status ;;
    run) [ "$#" -eq 1 ] || usage; do_run; exit 0 ;;
    *) usage ;;
esac
