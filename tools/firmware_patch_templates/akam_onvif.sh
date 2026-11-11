#!/bin/sh
# akam_onvif.sh -- switch the ONVIF service of ak_rtsp (Profile S: discovery, device, media) on or off.
#
# Installed as /usr/sbin/akam_onvif.sh (mode 755). Run it over SSH, as root:
#   akam_onvif.sh enable [PORT]   turn ONVIF on (HTTP port PORT, 1 to 65535 except 21-24, 554, 8091; default: the port already
#                                 saved, else 80). Refused unless a stream password is set
#                                 (./akam rtsp-auth set): ONVIF logins are checked against it
#   akam_onvif.sh disable         turn ONVIF off (the HTTP port and UDP 3702 are closed again)
#   akam_onvif.sh status          enabled/disabled, the port, the password, and what ak_rtsp reports
# Exit status: 0 ok, 1 failure, 2 usage, 3 status found an INVALID flag file.
#
# The flag is /etc/config/akam/onvif (CONFIG partition, persistent, mode 0600, directory 0700), two
# lines, LF only:
#     enabled=1            1 = on, 0 = off
#     port=80              HTTP port of the ONVIF service, 1 to 65535
# An absent file means disabled (the default of every image). The file is replaced atomically.
# ak_rtsp reads it at start and again on "ONVIF reload" (port 8091, from the camera only): this
# script sends it with the multi-call alias "ak_rtsp ctl ONVIF reload" after every change. An older
# ak_rtsp answers "ERR unknown command": the flag is saved anyway and applies at the next reboot.
# The password is never read out or printed here: only its presence and shape are checked.
#
# POSIX sh for the camera's BusyBox 1.30.1: no tr, no mktemp, no stat (ls -ld instead). Paths are
# prefixed by $AKAM_ROOT (empty on the camera) so the host test can run the script in a fake root.

AKAM_ROOT=${AKAM_ROOT:-}
CONFIG_DIR=${AKAM_CONFIG_DIR:-$AKAM_ROOT/etc/config}
AKAM_DIR=$CONFIG_DIR/akam
ONVIF_FILE=${AKAM_ONVIF_FILE:-$AKAM_DIR/onvif}
CRED_FILE=${AKAM_CRED_FILE:-$AKAM_DIR/credentials}
AKAM_RTSP_BIN=${AKAM_RTSP_BIN:-/usr/bin/ak_rtsp}
DEF_PORT=80
MIN_PASS=8
MAX_PASS=128
USER_MAX=32

die() {
    echo "akam_onvif: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 enable [PORT] | disable | status    (PORT: 1 to 65535, default $DEF_PORT)" >&2
    exit 2
}

valid_port() {
    case $1 in
        '' | *[!0-9]*) return 1 ;;
    esac
    [ "${#1}" -le 5 ] || return 1
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

# read_flag: sets CUR_EN (0/1) and CUR_PORT; returns 0 valid, 1 absent (disabled, default port), 3 invalid.
read_flag() {
    CUR_EN=0
    CUR_PORT=$DEF_PORT
    [ -e "$ONVIF_FILE" ] || return 1
    _en=
    _pt=
    _n=0
    while IFS= read -r _l || [ -n "$_l" ]; do
        _l=${_l%"$(printf '\r')"}
        _n=$((_n + 1))
        case $_l in
            enabled=0 | enabled=1) [ -z "$_en" ] || return 3; _en=${_l#enabled=} ;;
            port=*) [ -z "$_pt" ] || return 3; _pt=${_l#port=}; valid_port "$_pt" || return 3 ;;
            *) return 3 ;;
        esac
    done <"$ONVIF_FILE" 2>/dev/null
    [ -n "$_en" ] && [ -n "$_pt" ] && [ "$_n" -eq 2 ] || return 3
    CUR_EN=$_en
    CUR_PORT=$_pt
    return 0
}

# creds_state: sets CRED_MSG; returns 0 when /etc/config/akam/credentials looks valid (see
# akam_rtsp_auth.sh set-creds: "user=NAME" then "pass=PASSWORD", mode 0600), 1 otherwise.
creds_state() {
    CRED_MSG="no password set"
    [ -f "$CRED_FILE" ] || return 1
    _lsl=$(ls -ld "$CRED_FILE" 2>/dev/null) || return 1
    case $_lsl in
        -rw-------* | -r--------*) ;;
        *) CRED_MSG="the credentials file is not private (mode 0600 needed)"; return 1 ;;
    esac
    _c1=
    _c2=
    _c3=
    CRED_MSG="the credentials file is malformed"
    {
        IFS= read -r _c1 || return 1
        IFS= read -r _c2 || return 1
        if IFS= read -r _c3; then return 1; fi
    } <"$CRED_FILE" 2>/dev/null
    _cu=${_c1#user=}
    _cp=${_c2#pass=}
    [ "$_cu" != "$_c1" ] && [ "$_cp" != "$_c2" ] || { _cp=; return 1; }
    case $_cu in '' | *[!A-Za-z0-9._@+-]*) _cp=; return 1 ;; esac
    [ "${#_cu}" -le "$USER_MAX" ] || { _cp=; return 1; }
    case $_cp in *[[:cntrl:]]*) _cp=; return 1 ;; esac
    [ "${#_cp}" -ge "$MIN_PASS" ] && [ "${#_cp}" -le "$MAX_PASS" ] || { _cp=; return 1; }
    _cp=
    CRED_MSG="set"
    return 0
}

# write_flag ENABLED PORT: atomic, 0600, directory 0700.
write_flag() {
    umask 077
    mkdir -p "$AKAM_DIR" 2>/dev/null || die "cannot create $AKAM_DIR (is /etc/config writable?)"
    chmod 700 "$AKAM_DIR" 2>/dev/null
    _tmp=$ONVIF_FILE.new.$$
    rm -f "$_tmp"
    printf 'enabled=%s\nport=%s\n' "$1" "$2" >"$_tmp" 2>/dev/null || { rm -f "$_tmp"; die "cannot write $_tmp"; }
    chmod 600 "$_tmp" 2>/dev/null
    sync
    mv -f "$_tmp" "$ONVIF_FILE" 2>/dev/null || { rm -f "$_tmp"; die "cannot replace $ONVIF_FILE"; }
    sync
}

# ctl LINE...: first reply line in REPLY, exit status of "ak_rtsp ctl" in CTL_RC.
ctl() {
    REPLY=$("$AKAM_RTSP_BIN" ctl "$@" 2>&1)
    CTL_RC=$?
    REPLY=$(printf '%s\n' "$REPLY" | sed -n '1p')
}

# reload: tell ak_rtsp to read the flag again. Returns 0 when applied or only pending a reboot.
reload() {
    ctl ONVIF reload
    case $REPLY in
        OK*) echo "Applied now (ak_rtsp reloaded the setting)."; return 0 ;;
        *"unknown command"*) echo "Saved. This ak_rtsp is older and cannot reload: reboot the camera to apply."; return 0 ;;
    esac
    if [ "$CTL_RC" -ne 0 ] || [ -z "$REPLY" ]; then
        echo "Saved. ak_rtsp did not answer${REPLY:+ ($REPLY)}: it applies the next time ak_rtsp starts (reboot the camera)."
        return 0
    fi
    echo "akam_onvif: saved, but ak_rtsp refused the reload: $REPLY (reboot the camera to apply)" >&2
    return 1
}

do_enable() {
    port=
    if [ "$#" -ge 1 ]; then
        valid_port "$1" || { echo "akam_onvif: PORT must be a whole number from 1 to 65535" >&2; exit 2; }
        case $1 in
            21 | 22 | 23 | 24 | 554 | 8091) echo "akam_onvif: port $1 is used by another Akam service: choose another" >&2; exit 2 ;;
        esac
        port=$1
    fi
    creds_state || {
        echo "akam_onvif: ONVIF needs a password ($CRED_MSG); nothing changed." >&2
        echo "akam_onvif: set one first, from your computer: ./akam rtsp-auth set" >&2
        exit 1
    }
    read_flag
    case $? in
        3) [ -n "$port" ] || port=$DEF_PORT ;;
        *) [ -n "$port" ] || port=$CUR_PORT ;;
    esac
    write_flag 1 "$port"
    echo "ONVIF enabled on port $port (TCP $port, and UDP 3702 for discovery)."
    reload
}

do_disable() {
    read_flag
    case $? in
        1) echo "ONVIF was already disabled."; return 0 ;;
        3) _p=$DEF_PORT ;;
        *) _p=$CUR_PORT ;;
    esac
    write_flag 0 "$_p"
    echo "ONVIF disabled."
    reload
}

do_status() {
    read_flag
    _rc=$?
    case $_rc in
        1) echo "ONVIF: disabled (default)" ;;
        3) echo "ONVIF: INVALID flag file $ONVIF_FILE (treated as disabled; run enable or disable)" ;;
        *) if [ "$CUR_EN" = 1 ]; then echo "ONVIF: enabled, port $CUR_PORT"; else echo "ONVIF: disabled, port $CUR_PORT"; fi ;;
    esac
    creds_state
    echo "password: $CRED_MSG"
    ctl GET onvif.state
    case $REPLY in
        onvif.state=*) echo "state: ${REPLY#onvif.state=}" ;;
        *) echo "state: unknown (this ak_rtsp does not report it)" ;;
    esac
    [ "$_rc" -ne 3 ] || return 3
    return 0
}

case ${1:-} in
    enable) shift; [ "$#" -le 1 ] || usage; do_enable "$@" ;;
    disable) [ "$#" -eq 1 ] || usage; do_disable ;;
    status) [ "$#" -eq 1 ] || usage; do_status ;;
    *) usage ;;
esac
