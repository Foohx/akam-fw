#!/bin/sh
# akam_rtsp_auth.sh -- manage the optional RTSP Digest authentication of ak_rtsp.
#
# Installed as /usr/sbin/akam_rtsp_auth.sh (mode 755). Run it over SSH, as root:
#   akam_rtsp_auth.sh status           enabled (user NAME, ONVIF-ready | not ONVIF-ready) | disabled | INVALID
#   akam_rtsp_auth.sh show             the user name only ("user: NAME") or "disabled"
#   akam_rtsp_auth.sh set USER         asks for the password twice, without echo (>= 8 chars)
#   akam_rtsp_auth.sh set-creds        reads TWO lines on stdin, USER then PASSWORD (used by ./akam
#                                      rtsp-auth: typed on the PC, sent over the SSH channel, never in
#                                      argv); writes the htdigest file AND the plain-text copy
#   akam_rtsp_auth.sh set-hash         reads ONE line "USER:akam:HA1" on stdin (legacy; writes the
#                                      htdigest file only and removes the plain-text copy, which
#                                      could no longer match: ONVIF is then not ready)
#   akam_rtsp_auth.sh disable          removes both files: no authentication any more
# Exit status: 0 ok, 1 failure, 2 usage, 3 status/show found an INVALID file.
#
# The credentials are one line in /etc/config/akam/rtsp_auth (CONFIG partition,
# persistent, mode 0600, directory 0700):
#     USER:akam:HA1        HA1 = md5hex("USER:akam:PASSWORD"), 32 lowercase hex
# A second file, /etc/config/akam/credentials (same mode and directory), holds the
# clear-text password for the protocols that cannot work from the hash (ONVIF
# WS-Security PasswordDigest; specs/07 section 3.3, decision U2):
#     user=NAME
#     pass=PASSWORD        (8 to 128 characters, no control character)
# set, set-creds and disable keep the two files in step. The password is never put
# in argv and never printed. ak_rtsp reads the htdigest file again at every
# protected request: no restart is needed. A
# client that is streaming when authentication is switched on, or the
# credentials change, is disconnected within a second and must log in again.
# This command is the ONLY way to manage the credentials: the control port 8091
# has no authentication and never touches them.
#
# POSIX sh for the camera's BusyBox 1.30.1: no tr, no mktemp, no stat (ls -ld instead). md5sum is a
# BusyBox applet there. Paths are prefixed by $AKAM_ROOT (empty on the camera)
# so the host test can run the script in a fake root.

AKAM_ROOT=${AKAM_ROOT:-}
CONFIG_DIR=${AKAM_CONFIG_DIR:-$AKAM_ROOT/etc/config}
AUTH_DIR=$CONFIG_DIR/akam
AUTH_FILE=${AKAM_RTSP_AUTH_FILE:-$AUTH_DIR/rtsp_auth}
PLAIN_FILE=${AKAM_CRED_FILE:-$AUTH_DIR/credentials}
REALM=akam
MIN_PASS=8
MAX_PASS=128
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

# valid_pass: MIN_PASS..MAX_PASS characters, no control character (the plain-text file is line based).
valid_pass() {
    case $1 in
        *[[:cntrl:]]*) return 1 ;;
    esac
    [ "${#1}" -ge "$MIN_PASS" ] && [ "${#1}" -le "$MAX_PASS" ]
}

# Write TEXT (one line) to FILE.new.$$ with mode 0600, in the 0700 directory; the path stays in $_tmp.
stage() {
    _tmp=$1.new.$$
    rm -f "$_tmp"
    printf '%s\n' "$2" >"$_tmp" 2>/dev/null || { rm -f "$_tmp"; die "cannot write $_tmp"; }
    chmod 600 "$_tmp" 2>/dev/null
}

prepare_dir() {
    umask 077
    mkdir -p "$AUTH_DIR" 2>/dev/null || die "cannot create $AUTH_DIR (is /etc/config writable?)"
    chmod 700 "$AUTH_DIR" 2>/dev/null
}

# Write "USER:akam:HA1" atomically, 0600, directory 0700. The plain-text copy, which this
# line cannot vouch for, is removed.
install_line() {
    prepare_dir
    stage "$AUTH_FILE" "$1"
    _t1=$_tmp
    sync
    mv -f "$_t1" "$AUTH_FILE" 2>/dev/null || { rm -f "$_t1"; die "cannot replace $AUTH_FILE"; }
    rm -f "$PLAIN_FILE"
    sync
}

# install_both USER HA1 PASSWORD: stage both files, then move both into place.
install_both() {
    prepare_dir
    stage "$AUTH_FILE" "$1:$REALM:$2"
    _t1=$_tmp
    _tmp=$PLAIN_FILE.new.$$
    rm -f "$_tmp"
    printf 'user=%s\npass=%s\n' "$1" "$3" >"$_tmp" 2>/dev/null || { rm -f "$_t1" "$_tmp"; die "cannot write $_tmp"; }
    chmod 600 "$_tmp" 2>/dev/null
    _t2=$_tmp
    sync
    mv -f "$_t2" "$PLAIN_FILE" 2>/dev/null || { rm -f "$_t1" "$_t2"; die "cannot replace $PLAIN_FILE"; }
    mv -f "$_t1" "$AUTH_FILE" 2>/dev/null || { rm -f "$_t1"; die "cannot replace $AUTH_FILE"; }
    sync
}

# Read the current file: sets CUR_USER and returns 0 (valid), 1 (absent), 3 (invalid).
read_current() {
    CUR_USER=
    CUR_HA1=
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
    CUR_HA1=$_h
    return 0
}

# onvif_ready: the plain-text copy exists, is exactly "user=NAME" / "pass=PASSWORD", has the mode 0600
# ak_rtsp insists on, and agrees with the htdigest file (md5 of NAME:akam:PASSWORD is its HA1).
# Needs read_current to have succeeded.
onvif_ready() {
    [ -f "$PLAIN_FILE" ] || return 1
    _lsl=$(ls -ld "$PLAIN_FILE" 2>/dev/null) || return 1
    case $_lsl in -rw-------* | -r--------*) ;; *) return 1 ;; esac
    _pl1=
    _pl2=
    _pl3=
    {
        IFS= read -r _pl1 || return 1
        IFS= read -r _pl2 || return 1
        if IFS= read -r _pl3; then return 1; fi
    } <"$PLAIN_FILE" 2>/dev/null
    [ "${_pl1#user=}" != "$_pl1" ] && [ "${_pl2#pass=}" != "$_pl2" ] || return 1
    [ "${_pl1#user=}" = "$CUR_USER" ] || return 1
    _pp=${_pl2#pass=}
    valid_pass "$_pp" || { _pp=; return 1; }
    _hh=$(printf '%s' "$CUR_USER:$REALM:$_pp" | md5_stdin) || { _pp=; return 1; }
    _pp=
    [ "$_hh" = "$CUR_HA1" ]
}

do_status() {
    read_current
    case $? in
        0)
            if onvif_ready; then echo "enabled (user $CUR_USER, ONVIF-ready)"
            else echo "enabled (user $CUR_USER, not ONVIF-ready)"; fi
            ;;
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
    if [ -e "$AUTH_FILE" ] || [ -e "$PLAIN_FILE" ]; then
        rm -f "$PLAIN_FILE" || die "cannot remove $PLAIN_FILE"
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
    valid_pass "$_p1" || { _p1=; _p2=; die "the password must have $MIN_PASS to $MAX_PASS characters, no control character; nothing changed"; }
    # printf is a shell builtin: the password is not in any command line.
    _ha1=$(printf '%s' "$_user:$REALM:$_p1" | md5_stdin) || { _p1=; _p2=; die "cannot compute the hash (md5sum)"; }
    install_both "$_user" "$_ha1" "$_p1"
    _p1=
    _p2=
    echo "RTSP authentication enabled for user $_user (Digest, realm $REALM)."
    echo "The password is also stored on the camera ($PLAIN_FILE, readable by root only) so that ONVIF can verify it."
    echo "Stream URL: rtsp://$_user:PASSWORD@<camera-ip>:554/  (URL-encode special characters)"
}

do_set_creds() {
    _u=
    _p=
    IFS= read -r _u || [ -n "$_u" ] || die "no input: expected the user name, then the password, one per line"
    IFS= read -r _p || [ -n "$_p" ] || die "no password on stdin"
    _u=${_u%"$(printf '\r')"}
    _p=${_p%"$(printf '\r')"}
    valid_user "$_u" || { _p=; die "invalid user name (1 to $USER_MAX characters among A-Z a-z 0-9 . _ @ + -)"; }
    valid_pass "$_p" || { _p=; die "the password must have $MIN_PASS to $MAX_PASS characters, no control character; nothing changed"; }
    _ha1=$(printf '%s' "$_u:$REALM:$_p" | md5_stdin) || { _p=; die "cannot compute the hash (md5sum)"; }
    install_both "$_u" "$_ha1" "$_p"
    _p=
    echo "RTSP authentication enabled for user $_u (Digest, realm $REALM)."
    echo "The password is also stored on the camera ($PLAIN_FILE, readable by root only) so that ONVIF can verify it."
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
    echo "No plain-text copy of the password: ONVIF cannot verify it (use set-creds or set)."
}

case ${1:-} in
    status) do_status ;;
    show) do_show ;;
    disable) do_disable ;;
    set)
        [ "$#" -eq 2 ] || { echo "usage: $0 set USER" >&2; exit 2; }
        do_set "$2"
        ;;
    set-creds) do_set_creds ;;
    set-hash) do_set_hash ;;
    *)
        echo "usage: $0 status | show | set USER | set-creds | set-hash | disable" >&2
        exit 2
        ;;
esac
