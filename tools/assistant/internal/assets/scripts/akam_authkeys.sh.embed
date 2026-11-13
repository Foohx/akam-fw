#!/bin/sh
# akam_authkeys.sh -- add or remove ONE ssh-ed25519 public key in the camera's Dropbear key list,
# run on the camera (BusyBox sh) from its RAM by the assistant ("akam key ..."), never installed.
#
# Usage: sh akam_authkeys.sh add       the key line is read from standard input
#        sh akam_authkeys.sh remove    same; only lines with the very same key are removed
#        sh akam_authkeys.sh card      read-only: is a provisioning card with akam/authorized_keys in?
#
# The input line is "ssh-ed25519 BASE64 [comment]". Anything else is refused (exit 2, nothing changed).
# Lines are compared on the key (the base64) only: the comment is ignored, an option prefix of an
# existing line is skipped, CRs are ignored, and only a well-formed ed25519 key counts as a key.
#
# add     appends the key unless a line with that key is already there (idempotent).
# remove  deletes every line with that key, but REFUSES (exit 3, nothing changed) when no other
#         ssh-ed25519 line would remain: the list is never left empty.
#
# Result lines (stdout):
#   AUTHKEYS=added | present | removed | absent               done (or nothing to do)
#   AUTHKEYS=REFUSED_LAST_KEY                                 remove: it is the only key (exit 3)
#   AUTHKEYS=BAD_KEY                                          bad input (exit 2)
#   AUTHKEYS=OUT_OF_SYNC                                      the login copy differs from the stored list:
#                                                             nothing was changed (a power cycle fixes it)
#   AUTHKEYS=WRITE_FAILED                                     nothing was changed (exit 1)
#   AUTHKEYS=ADDED_BUT_HOME_FAILED | REMOVED_BUT_HOME_FAILED the stored list WAS changed, the login
#                                                             copy could not follow AND the old list could
#                                                             not be put back (a power cycle makes them agree)
#   CARD=present | absent | unknown                           mode card only: a card with
#                                                             akam/authorized_keys is in the camera
#
# The file is rewritten in a temp file of the SAME directory (mode 0600). Every write error is
# tracked, and the temp file is checked (it must hold the expected number of ed25519 keys, at least
# one, and the key must be there or gone) BEFORE it is moved into place, so a failed write can never
# replace the list with a short or empty one. Then sync, mv (atomic: a power cut leaves the old
# file or the new one) and sync again.
#
# Why the second copy: akam_ssh.sh makes root's HOME a private tmpfs directory
# (/tmp/akam_home) that holds a COPY of authorized_keys, and Dropbear reads THAT copy
# (-D /tmp/akam_home). Without refreshing it a new key would only work after the next power cycle.
# When that copy exists it must equal the stored list before a change (else OUT_OF_SYNC), and it is
# replaced the same atomic way after it. Without the copy (bind mount failed) only the stored file is used.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.

ROOT=${AKAM_ROOT:-}
DB=$ROOT/etc/config/dropbear
KEYS=$DB/authorized_keys
HOMEDIR=${AKAM_HOME_DIR:-$ROOT/tmp/akam_home}
HOMEKEYS=$HOMEDIR/authorized_keys
TMPDIR_=${AKAM_TMP_DIR:-$ROOT/tmp}
SD_DIR=${AKAM_SD_DIR:-$ROOT/mnt/akam}
LEGACY_SD_DIR=${AKAM_SD_DIR_LEGACY:-$ROOT/mnt/camyn}
umask 077

usage() {
    echo "usage: sh akam_authkeys.sh add|remove|card   (key line on standard input for add and remove)" >&2
    exit 2
}

[ $# -eq 1 ] || usage
MODE=$1
case "$MODE" in
    add | remove | card) ;;
    *) usage ;;
esac

CR=$(printf '\r')
TAB=$(printf '\t')

_mnt=
_mounted=0
# cleanup_mnt: unmount (when mounted) and remove the private mount point of the card probe.
cleanup_mnt() {
    [ -n "$_mnt" ] || return 0
    if [ "$_mounted" = 1 ]; then
        umount "$_mnt" 2>/dev/null
        _mounted=0
    fi
    rmdir "$_mnt" 2>/dev/null
    _mnt=
}

# ---- is a provisioning card in? (same places as akam_ssh.sh: mounted somewhere, else mounted read-only
# for a moment). Prints present | absent | unknown.
card_state() {
    for _d in "$SD_DIR" "$LEGACY_SD_DIR"; do
        [ -f "$_d/authorized_keys" ] && {
            echo present
            return
        }
    done
    _mounts=${AKAM_PROC_MOUNTS:-/proc/mounts}
    _seen=0
    if [ -r "$_mounts" ]; then
        while read -r _msrc _mtgt _mrest; do
            case "$_msrc" in
                /dev/mmcblk0p1 | /dev/mmcblk0) ;;
                *) continue ;;
            esac
            [ -d "$ROOT$_mtgt" ] || continue
            _seen=1
            if [ -f "$ROOT$_mtgt/akam/authorized_keys" ] || [ -f "$ROOT$_mtgt/camyn/authorized_keys" ]; then
                echo present
                return
            fi
        done <"$_mounts"
    fi
    [ "$_seen" = 1 ] && {
        echo absent
        return
    }
    _node=
    for _n in "$ROOT/dev/mmcblk0p1" "$ROOT/dev/mmcblk0"; do
        [ -e "$_n" ] && {
            _node=$_n
            break
        }
    done
    [ -n "$_node" ] || {
        echo absent
        return
    }
    # a card is in but not mounted: look at it read-only for a moment, like the boot does. The mount
    # is always undone (exit, Ctrl-C, hangup, TERM) by cleanup_mnt.
    _mnt=$TMPDIR_/akam_authkeys_sd.$$
    trap cleanup_mnt EXIT
    trap 'exit 1' INT TERM HUP
    mkdir -p "$_mnt" 2>/dev/null || {
        _mnt=
        echo unknown
        return
    }
    for _fs in vfat exfat; do
        if mount -t "$_fs" -o ro "$_node" "$_mnt" 2>/dev/null; then
            _mounted=1
            _r=absent
            if [ -f "$_mnt/akam/authorized_keys" ] || [ -f "$_mnt/camyn/authorized_keys" ]; then _r=present; fi
            cleanup_mnt
            echo "$_r"
            return
        fi
    done
    cleanup_mnt
    echo unknown
}

if [ "$MODE" = card ]; then
    echo "CARD=$(card_state)"
    exit 0
fi

finish() { # WORD STATUS: print the result, drop any temp file, exit
    echo "AUTHKEYS=$1"
    [ -z "${TMP:-}" ] || rm -f "$TMP"
    [ -z "${OLD:-}" ] || rm -f "$OLD"
    exit "$2"
}

IFS= read -r LINE || [ -n "$LINE" ] || finish BAD_KEY 2
while [ "${LINE%"$CR"}" != "$LINE" ]; do LINE=${LINE%"$CR"}; done
set -f
# shellcheck disable=SC2086
set -- $LINE
KTYPE=${1:-}
KB64=${2:-}
KCOMMENT=${3:-}
set +f
[ "$KTYPE" = ssh-ed25519 ] || finish BAD_KEY 2
echo "$KTYPE $KB64" | grep -Eq '^ssh-ed25519 AAAAC3NzaC1lZDI1NTE5[A-Za-z0-9+/]{48}$' || finish BAD_KEY 2
[ "${#KB64}" -eq 68 ] || finish BAD_KEY 2
case "$KCOMMENT" in
    *[!A-Za-z0-9._@+-]*) KCOMMENT= ;;
esac
NEWLINE=$KTYPE' '$KB64
[ -z "$KCOMMENT" ] || NEWLINE=$NEWLINE' '$KCOMMENT

[ -d "$DB" ] || {
    echo "akam_authkeys: $DB does not exist" >&2
    finish WRITE_FAILED 1
}

# line_key LINE: LK is the base64 of the ed25519 key of the line ("" when there is none or it is not a
# well-formed ed25519 key). An option prefix is skipped, like Dropbear does; comments never match.
line_key() {
    LK=
    _l=$1
    while [ "${_l%"$CR"}" != "$_l" ]; do _l=${_l%"$CR"}; done
    case "$_l" in
        '' | '#'*) return ;;
        # leading blanks and tab separators: Dropbear does not read such a line as a key
        ' '* | "$TAB"* | *"$TAB"*) return ;;
    esac
    set -f
    # shellcheck disable=SC2086
    set -- $_l
    set +f
    while [ $# -gt 0 ]; do
        if [ "$1" = ssh-ed25519 ]; then
            case "${2:-}" in
                AAAAC3NzaC1lZDI1NTE5*)
                    [ "${#2}" -eq 68 ] && LK=$2
                    ;;
            esac
            return
        fi
        shift
    done
}

# has_key FILE: the file holds the key of the input.
has_key() {
    [ -f "$1" ] || return 1
    while IFS= read -r l || [ -n "$l" ]; do
        line_key "$l"
        [ "$LK" = "$KB64" ] && return 0
    done <"$1"
    return 1
}

# count_keys FILE: how many well-formed ed25519 keys the file holds.
count_keys() {
    n=0
    if [ -f "$1" ]; then
        while IFS= read -r l || [ -n "$l" ]; do
            line_key "$l"
            [ -n "$LK" ] && n=$((n + 1))
        done <"$1"
    fi
    echo "$n"
}

# count_other FILE: the same, without the key of the input.
count_other() {
    n=0
    if [ -f "$1" ]; then
        while IFS= read -r l || [ -n "$l" ]; do
            line_key "$l"
            [ -n "$LK" ] && [ "$LK" != "$KB64" ] && n=$((n + 1))
        done <"$1"
    fi
    echo "$n"
}

# install_file SRC DST: SRC is a finished temp file in the directory of DST; mode, sync, move, sync.
install_file() {
    chmod 600 "$1" 2>/dev/null || return 1
    chown 0:0 "$1" 2>/dev/null
    sync
    mv -f "$1" "$2" || return 1
    sync
}

# refresh_copy: the login copy of Dropbear (see the header) follows the stored file; none = nothing to do.
refresh_copy() {
    [ -f "$HOMEKEYS" ] || return 0
    if cp "$KEYS" "$HOMEDIR/.authorized_keys.new.$$" 2>/dev/null && install_file "$HOMEDIR/.authorized_keys.new.$$" "$HOMEKEYS" && cmp -s "$KEYS" "$HOMEKEYS"; then
        return 0
    fi
    rm -f "$HOMEDIR/.authorized_keys.new.$$"
    return 1
}

TMP=$DB/.authorized_keys.new.$$
# stale temp files of an earlier run that died
for _s in "$DB"/.authorized_keys.new.* "$DB"/.authorized_keys.old.* "$HOMEDIR"/.authorized_keys.new.*; do
    [ -f "$_s" ] && rm -f "$_s"
done

# the login copy, when in use, must equal the stored list: else a change would be half effective
if [ -f "$HOMEKEYS" ] && ! cmp -s "$KEYS" "$HOMEKEYS"; then
    echo "akam_authkeys: the login copy of the key list differs from $KEYS; nothing was changed (a power cycle makes them agree)." >&2
    finish OUT_OF_SYNC 4
fi

fail_write() {
    rm -f "$TMP" "$OLD"
    finish WRITE_FAILED 1
}

OLD=$DB/.authorized_keys.old.$$
HAD=0
[ -f "$KEYS" ] && HAD=1
# backup_keys: keep the list as it is, to put it back if the login copy cannot follow the change.
backup_keys() {
    [ "$HAD" = 0 ] || cp "$KEYS" "$OLD" 2>/dev/null
}
# undo_or WORD: the login copy could not be refreshed: put the old list back (then nothing changed:
# WRITE_FAILED), and only when even that fails report WORD (the lists disagree until a power cycle).
undo_or() {
    if [ "$HAD" = 0 ]; then
        rm -f "$KEYS" && finish WRITE_FAILED 1
    elif mv -f "$OLD" "$KEYS"; then
        sync
        finish WRITE_FAILED 1
    fi
    finish "$1" 1
}

BEFORE=$(count_keys "$KEYS")
case "$MODE" in
    add)
        if has_key "$KEYS"; then finish present 0; fi
        werr=0
        {
            if [ -f "$KEYS" ]; then
                while IFS= read -r l || [ -n "$l" ]; do printf '%s\n' "$l" || werr=1; done <"$KEYS"
            fi
            printf '%s\n' "$NEWLINE" || werr=1
        } >"$TMP" 2>/dev/null || werr=1
        [ "$werr" = 0 ] || fail_write
        # the temp file must be the old list plus the key
        [ "$(count_keys "$TMP")" -eq $((BEFORE + 1))  ] || fail_write
        has_key "$TMP" || fail_write
        backup_keys || fail_write
        install_file "$TMP" "$KEYS" || fail_write
        has_key "$KEYS" || finish WRITE_FAILED 1
        refresh_copy || undo_or ADDED_BUT_HOME_FAILED
        rm -f "$OLD"
        finish added 0
        ;;
    remove)
        if ! has_key "$KEYS"; then finish absent 0; fi
        OTHERS=$(count_other "$KEYS")
        if [ "$OTHERS" -eq 0 ]; then
            echo "akam_authkeys: that is the only key on the camera; not removing it." >&2
            finish REFUSED_LAST_KEY 3
        fi
        werr=0
        {
            while IFS= read -r l || [ -n "$l" ]; do
                line_key "$l"
                if [ "$LK" = "$KB64" ]; then continue; fi
                printf '%s\n' "$l" || werr=1
            done <"$KEYS"
        } >"$TMP" 2>/dev/null || werr=1
        [ "$werr" = 0 ] || fail_write
        # the temp file must hold exactly the other keys, and at least one
        [ "$(count_keys "$TMP")" -eq "$OTHERS" ] || fail_write
        ! has_key "$TMP" || fail_write
        backup_keys || fail_write
        install_file "$TMP" "$KEYS" || fail_write
        ! has_key "$KEYS" || finish WRITE_FAILED 1
        refresh_copy || undo_or REMOVED_BUT_HOME_FAILED
        rm -f "$OLD"
        finish removed 0
        ;;
esac
