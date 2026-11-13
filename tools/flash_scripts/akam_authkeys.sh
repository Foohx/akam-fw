#!/bin/sh
# akam_authkeys.sh -- add or remove ONE ssh-ed25519 public key in the camera's Dropbear key list,
# run on the camera (BusyBox sh) from its RAM by the assistant ("akam key ..."), never installed.
#
# Usage: sh akam_authkeys.sh add       the key line is read from standard input
#        sh akam_authkeys.sh remove    same; only lines with the very same key are removed
#
# The input line is "ssh-ed25519 BASE64 [comment]". Anything else is refused (exit 2, nothing changed).
# Lines are compared on key type + base64 only; the comment is ignored.
#
# add     appends the key unless a line with that key is already there (idempotent).
# remove  deletes every line with that key, but REFUSES (exit 3, nothing changed) when no other
#         ssh-ed25519 line would remain: the list is never left empty.
#
# Result line (stdout): AUTHKEYS=added | present | removed | absent | REFUSED_LAST_KEY | BAD_KEY |
# WRITE_FAILED. A rewrite is made in a temp file of the SAME directory, mode 0600, synced, then moved
# into place (atomic: a power cut leaves the old file or the new one, never half of one).
#
# Why the second copy: akam_ssh.sh makes root's HOME a private tmpfs directory
# (/tmp/akam_home) that holds a COPY of authorized_keys, and Dropbear reads THAT copy
# (-D /tmp/akam_home). Without refreshing it a new key would only work after the next power cycle.
# When that directory exists the copy is replaced the same atomic way after every change.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.

ROOT=${AKAM_ROOT:-}
DB=$ROOT/etc/config/dropbear
KEYS=$DB/authorized_keys
HOMEDIR=${AKAM_HOME_DIR:-$ROOT/tmp/akam_home}
HOMEKEYS=$HOMEDIR/authorized_keys
umask 077

usage() {
    echo "usage: sh akam_authkeys.sh add|remove   (key line on standard input)" >&2
    exit 2
}

[ $# -eq 1 ] || usage
MODE=$1
case "$MODE" in
    add | remove) ;;
    *) usage ;;
esac

IFS= read -r LINE || [ -n "$LINE" ] || {
    echo "AUTHKEYS=BAD_KEY"
    exit 2
}
set -f
# shellcheck disable=SC2086
set -- $LINE
KTYPE=${1:-}
KB64=${2:-}
KCOMMENT=${3:-}
set +f
case "$KTYPE" in
    ssh-ed25519) ;;
    *)
        echo "AUTHKEYS=BAD_KEY"
        exit 2
        ;;
esac
case "$KB64" in
    '' | *[!A-Za-z0-9+/=]*)
        echo "AUTHKEYS=BAD_KEY"
        exit 2
        ;;
esac
[ "${#KB64}" -ge 60 ] || {
    echo "AUTHKEYS=BAD_KEY"
    exit 2
}
case "$KCOMMENT" in
    *[!A-Za-z0-9._@+-]*) KCOMMENT= ;;
esac
NEWLINE=$KTYPE' '$KB64
[ -z "$KCOMMENT" ] || NEWLINE=$NEWLINE' '$KCOMMENT

[ -d "$DB" ] || {
    echo "AUTHKEYS=WRITE_FAILED"
    echo "akam_authkeys: $DB does not exist" >&2
    exit 1
}

# line_fields LINE: F1 and F2 are the first two words of LINE (type and base64 of a key line).
line_fields() {
    set -f
    # shellcheck disable=SC2086
    set -- $1
    set +f
    F1=${1:-}
    F2=${2:-}
}

# has_key: the list holds the key of the input.
has_key() {
    [ -f "$KEYS" ] || return 1
    while IFS= read -r l || [ -n "$l" ]; do
        line_fields "$l"
        if [ "$F1" = "$KTYPE" ] && [ "$F2" = "$KB64" ]; then return 0; fi
    done <"$KEYS"
    return 1
}

# count_other: how many ssh-ed25519 lines other than the input key the list holds.
count_other() {
    n=0
    [ -f "$KEYS" ] || {
        echo 0
        return
    }
    while IFS= read -r l || [ -n "$l" ]; do
        line_fields "$l"
        if [ "$F1" = ssh-ed25519 ] && [ -n "$F2" ] && [ "$F2" != "$KB64" ]; then n=$((n + 1)); fi
    done <"$KEYS"
    echo "$n"
}

# install_file SRC DST: SRC is a finished temp file in the directory of DST; mode, sync, move.
install_file() {
    chmod 600 "$1" 2>/dev/null || return 1
    chown 0:0 "$1" 2>/dev/null
    sync
    mv -f "$1" "$2"
}

# refresh_copy: the login copy of Dropbear (see the header) follows the persistent file.
refresh_copy() {
    [ -d "$HOMEDIR" ] || return 0
    cp "$KEYS" "$HOMEDIR/.authorized_keys.new.$$" 2>/dev/null && install_file "$HOMEDIR/.authorized_keys.new.$$" "$HOMEKEYS" || {
        rm -f "$HOMEDIR/.authorized_keys.new.$$"
        return 1
    }
}

fail_write() {
    rm -f "$TMP"
    echo "AUTHKEYS=WRITE_FAILED"
    exit 1
}

TMP=$DB/.authorized_keys.new.$$
rm -f "$TMP"
case "$MODE" in
    add)
        if has_key; then
            refresh_copy || fail_write
            echo "AUTHKEYS=present"
            exit 0
        fi
        {
            if [ -f "$KEYS" ]; then
                while IFS= read -r l || [ -n "$l" ]; do printf '%s\n' "$l"; done <"$KEYS"
            fi
            printf '%s\n' "$NEWLINE"
        } >"$TMP" 2>/dev/null || fail_write
        install_file "$TMP" "$KEYS" || fail_write
        has_key || fail_write
        refresh_copy || fail_write
        echo "AUTHKEYS=added"
        ;;
    remove)
        if ! has_key; then
            echo "AUTHKEYS=absent"
            exit 0
        fi
        if [ "$(count_other)" -eq 0 ]; then
            echo "AUTHKEYS=REFUSED_LAST_KEY"
            echo "akam_authkeys: that is the only key on the camera; not removing it." >&2
            exit 3
        fi
        {
            while IFS= read -r l || [ -n "$l" ]; do
                line_fields "$l"
                if [ "$F1" = "$KTYPE" ] && [ "$F2" = "$KB64" ]; then continue; fi
                printf '%s\n' "$l"
            done <"$KEYS"
        } >"$TMP" 2>/dev/null || fail_write
        install_file "$TMP" "$KEYS" || fail_write
        ! has_key || fail_write
        refresh_copy || fail_write
        echo "AUTHKEYS=removed"
        ;;
esac
