#!/bin/sh
# akam_rtsp_auth.sh -- manage the optional RTSP Digest authentication of ak_rtsp.
#
# Installed as /usr/sbin/akam_rtsp_auth.sh (mode 755). Run it over SSH, as root:
#   akam_rtsp_auth.sh status           enabled (user NAME) | disabled | INVALID
#   akam_rtsp_auth.sh show             the user name only ("user: NAME") or "disabled"
#   akam_rtsp_auth.sh set USER         asks for the password twice, without echo (>= 8 chars)
#   akam_rtsp_auth.sh set-hash         reads ONE line "USER:akam:HA1" on stdin (used by ./akam
#                                      rtsp-auth: the password is typed and hashed on the PC)
#   akam_rtsp_auth.sh disable          removes the credentials: no authentication any more
# Exit status: 0 ok, 1 failure, 2 usage, 3 status/show found an INVALID file.
#
# The credentials are one line in /etc/config/akam/rtsp_auth (CONFIG partition,
# persistent, mode 0600, directory 0700):
#     USER:akam:HA1        HA1 = md5hex("USER:akam:PASSWORD"), 32 lowercase hex
# The password itself is never stored, never put in argv, never printed. ak_rtsp
# reads the file again at every protected request: no restart is needed. A
# client that is streaming when authentication is switched on, or the
# credentials change, is disconnected within a second and must log in again.
# This command is the ONLY way to manage the credentials: the control port 8091
# has no authentication and never touches them.
#
# POSIX sh for the camera's BusyBox 1.30.1: no tr, no mktemp. md5sum is a
# BusyBox applet there. Paths are prefixed by $AKAM_ROOT (empty on the camera)
# so the host test can run the script in a fake root.

AKAM_ROOT=${AKAM_ROOT:-}
CONFIG_DIR=${AKAM_CONFIG_DIR:-$AKAM_ROOT/etc/config}
AUTH_DIR=$CONFIG_DIR/akam
AUTH_FILE=${AKAM_RTSP_AUTH_FILE:-$AUTH_DIR/rtsp_auth}
REALM=akam
MIN_PASS=8
USER_MAX=32

PATH=$PATH:$AKAM_ROOT/bin:$AKAM_ROOT/sbin:$AKAM_ROOT/usr/bin:$AKAM_ROOT/usr/sbin

die() {
    echo "akam_rtsp_auth: $*" >&2
    exit 1
}

# md5hex of the text on stdin (first field of md5sum).
md5_stdin() {
    if command -v md5sum >/dev/null 2>&1; then
        _o=$(md5sum) || return 1
    else
        _o=$(busybox md5sum) || return 1
    fi
    _o=${_o%% *}
    [ "${#_o}" -eq 32 ] || return 1
    printf '%s\n' "$_o"
}

valid_user() {
    case $1 in
        '' | *[!A-Za-z0-9._@+-]*) return 1 ;;
    esac
    [ "${#1}" -le "$USER_MAX" ]
}

valid_hash() {
    case $1 in
        *[!0-9a-f]*) return 1 ;;
    esac
    [ "${#1}" -eq 32 ]
}

# Write "USER:akam:HA1" atomically, 0600, directory 0700.
install_line() {
    _line=$1
    umask 077
    mkdir -p "$AUTH_DIR" 2>/dev/null || die "cannot create $AUTH_DIR (is /etc/config writable?)"
    chmod 700 "$AUTH_DIR" 2>/dev/null
    _tmp=$AUTH_FILE.new.$$
    rm -f "$_tmp"
    printf '%s\n' "$_line" >"$_tmp" 2>/dev/null || { rm -f "$_tmp"; die "cannot write $_tmp"; }
    chmod 600 "$_tmp" 2>/dev/null
    sync
    mv -f "$_tmp" "$AUTH_FILE" 2>/dev/null || { rm -f "$_tmp"; die "cannot replace $AUTH_FILE"; }
    sync
}

# Read the current file: sets CUR_USER and returns 0 (valid), 1 (absent), 3 (invalid).
read_current() {
    CUR_USER=
    [ -e "$AUTH_FILE" ] || return 1
    _l=
    _more=
    # Exactly one line, as ak_rtsp requires (a second line makes it refuse everything).
    {
        IFS= read -r _l || [ -n "$_l" ] || return 3
        if IFS= read -r _more; then return 3; fi
    } <"$AUTH_FILE" 2>/dev/null
    [ -n "$_l" ] || return 3
    _l=${_l%"$(printf '\r')"}
    _u=${_l%%:*}
    _rest=${_l#*:}
    [ "$_rest" != "$_l" ] || return 3
    _r=${_rest%%:*}
    _h=${_rest#*:}
    [ "$_h" != "$_rest" ] || return 3
    valid_user "$_u" || return 3
    [ "$_r" = "$REALM" ] || return 3
    valid_hash "$_h" || return 3
    CUR_USER=$_u
    return 0
}

do_status() {
    read_current
    case $? in
        0) echo "enabled (user $CUR_USER)" ;;
        1) echo "disabled" ;;
        *) echo "INVALID (ak_rtsp refuses every protected request; run set or disable)"; return 3 ;;
    esac
}

do_show() {
    read_current
    case $? in
        0) echo "user: $CUR_USER" ;;
        1) echo "disabled" ;;
        *) echo "INVALID"; return 3 ;;
    esac
}

do_disable() {
    if [ -e "$AUTH_FILE" ]; then
        rm -f "$AUTH_FILE" || die "cannot remove $AUTH_FILE"
        sync
        echo "RTSP authentication disabled: the stream is open again."
    else
        echo "RTSP authentication was already disabled."
    fi
}

do_set() {
    _user=$1
    valid_user "$_user" || die "invalid user name (1 to $USER_MAX characters among A-Z a-z 0-9 . _ @ + -)"
    _tty=0
    _saved=
    if [ -t 0 ]; then
        _tty=1
        _saved=$(stty -g 2>/dev/null)
        trap 'stty "$_saved" 2>/dev/null; echo >&2; exit 1' INT TERM HUP
        stty -echo 2>/dev/null
        printf 'New RTSP password for %s: ' "$_user" >&2
    fi
    _p1=
    IFS= read -r _p1
    if [ "$_tty" = 1 ]; then
        echo >&2
        printf 'Repeat it: ' >&2
    fi
    _p2=
    IFS= read -r _p2
    if [ "$_tty" = 1 ]; then
        echo >&2
        stty "$_saved" 2>/dev/null
        trap - INT TERM HUP
    fi
    _p1=${_p1%"$(printf '\r')"}
    _p2=${_p2%"$(printf '\r')"}
    [ "$_p1" = "$_p2" ] || { _p1=; _p2=; die "the two passwords differ; nothing changed"; }
    if [ "${#_p1}" -lt "$MIN_PASS" ]; then
        _p1=
        _p2=
        die "the password must have at least $MIN_PASS characters; nothing changed"
    fi
    # printf is a shell builtin: the password is not in any command line.
    _ha1=$(printf '%s' "$_user:$REALM:$_p1" | md5_stdin) || { _p1=; _p2=; die "cannot compute the hash (md5sum)"; }
    _p1=
    _p2=
    install_line "$_user:$REALM:$_ha1"
    echo "RTSP authentication enabled for user $_user (Digest, realm $REALM)."
    echo "Stream URL: rtsp://$_user:PASSWORD@<camera-ip>:554/  (URL-encode special characters)"
}

do_set_hash() {
    _line=
    IFS= read -r _line || [ -n "$_line" ] || die "no input: expected one line USER:$REALM:HA1"
    _line=${_line%"$(printf '\r')"}
    _u=${_line%%:*}
    _rest=${_line#*:}
    [ "$_rest" != "$_line" ] || die "malformed line (expected USER:$REALM:HA1)"
    _r=${_rest%%:*}
    _h=${_rest#*:}
    [ "$_h" != "$_rest" ] || die "malformed line (expected USER:$REALM:HA1)"
    valid_user "$_u" || die "invalid user name (1 to $USER_MAX characters among A-Z a-z 0-9 . _ @ + -)"
    [ "$_r" = "$REALM" ] || die "the realm must be $REALM"
    valid_hash "$_h" || die "the hash must be 32 lowercase hex characters"
    install_line "$_u:$REALM:$_h"
    echo "RTSP authentication enabled for user $_u (Digest, realm $REALM)."
}

case ${1:-} in
    status) do_status ;;
    show) do_show ;;
    disable) do_disable ;;
    set)
        [ "$#" -eq 2 ] || { echo "usage: $0 set USER" >&2; exit 2; }
        do_set "$2"
        ;;
    set-hash) do_set_hash ;;
    *)
        echo "usage: $0 status | show | set USER | set-hash | disable" >&2
        exit 2
        ;;
esac
