#!/bin/sh
# akam_debug.sh -- allow writes (SET) on the control port 8091 from the network, for a while.
#
# Installed as /usr/sbin/akam_debug.sh (mode 755). Run it over SSH, as root:
#   akam_debug.sh on [MINUTES]    remote writes allowed; default 60 minutes, 1 to 1440
#   akam_debug.sh off             read-only again, at once
#   akam_debug.sh status          "debug on, N s left" or "debug off"
# Exit status: 0 ok, 1 ak_rtsp did not answer or refused, 2 usage.
#
# Port 8091 is read-only for every client that is not on the camera itself: LIST, GET and the
# metrics work for everyone, SET/SAVE/RESET from the network answer "ERR read-only mode, see
# akam_debug.sh". Commands sent from the camera (127.0.0.1: SSH helpers such as ./akam led) always
# work. This script turns on a RAM-only flag in ak_rtsp (never saved, off at every start of ak_rtsp
# and at reboot, off by itself after the delay, measured with a monotonic clock) that lets the
# network write too: for example the PC tuning tool. It talks to ak_rtsp with its own multi-call
# alias "ak_rtsp ctl <line>" (connects to 127.0.0.1:8091), not with nc.
#
# POSIX sh for the camera's BusyBox 1.30.1.

AKAM_RTSP_BIN=${AKAM_RTSP_BIN:-/usr/bin/ak_rtsp}
MAX_MIN=1440
DEF_MIN=60

usage() {
    echo "usage: $0 on [MINUTES] | off | status    (MINUTES: 1 to $MAX_MIN, default $DEF_MIN)" >&2
    exit 2
}

# ctl LINE...: sends the line, puts the first reply line in REPLY; returns 1 when ak_rtsp did not
# answer or answered ERR.
ctl() {
    REPLY=$("$AKAM_RTSP_BIN" ctl "$@" 2>&1)
    _rc=$?
    REPLY=$(printf '%s\n' "$REPLY" | sed -n '1p')
    if [ "$_rc" -ne 0 ] || [ -z "$REPLY" ]; then
        echo "akam_debug: ak_rtsp did not accept the command${REPLY:+: $REPLY}" >&2
        echo "akam_debug: is ak_rtsp running, and is this image recent enough (ak_rtsp ctl)?" >&2
        return 1
    fi
    return 0
}

case ${1:-} in
    on)
        [ "$#" -le 2 ] || usage
        min=$DEF_MIN
        [ "$#" -lt 2 ] || min=$2
        case $min in
            '' | *[!0-9]*) echo "akam_debug: MINUTES must be a whole number from 1 to $MAX_MIN" >&2; exit 2 ;;
        esac
        if [ "$min" -lt 1 ] || [ "$min" -gt "$MAX_MIN" ]; then
            echo "akam_debug: MINUTES must be a whole number from 1 to $MAX_MIN" >&2
            exit 2
        fi
        ctl DEBUG on "$min" || exit 1
        case $REPLY in
            OK*) echo "debug on: the control port accepts writes from the network for $min minutes (off at reboot, or: akam_debug.sh off)" ;;
            *) echo "akam_debug: unexpected answer: $REPLY" >&2; exit 1 ;;
        esac
        ;;
    off)
        [ "$#" -eq 1 ] || usage
        ctl DEBUG off || exit 1
        case $REPLY in
            OK*) echo "debug off: the control port is read-only for the network" ;;
            *) echo "akam_debug: unexpected answer: $REPLY" >&2; exit 1 ;;
        esac
        ;;
    status)
        [ "$#" -eq 1 ] || usage
        ctl DEBUG status || exit 1
        case $REPLY in
            "ctl.debug=1 remaining="*)
                left=${REPLY#*remaining=}
                echo "debug on, $left s left (the control port accepts writes from the network)"
                ;;
            "ctl.debug=0"*) echo "debug off (the control port is read-only for the network)" ;;
            *) echo "akam_debug: unexpected answer: $REPLY" >&2; exit 1 ;;
        esac
        ;;
    *) usage ;;
esac
