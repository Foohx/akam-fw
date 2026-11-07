#!/bin/sh
# camyn_ssh.sh -- start Dropbear (public-key SSH, ed25519 only) at boot.
#
# Installed as /usr/sbin/camyn_ssh.sh (mode 755) and launched in the background
# by ak_rtsp_wrapper.sh. It must NEVER block or break ak_rtsp: every failure
# path only logs and returns 0.
#
# Usage: camyn_ssh.sh [start|status]      (default: start)
#
# Layout (all paths are prefixed by $CAMYN_ROOT, empty on the camera; the
# prefix and the variables below exist so the host-side test can run the
# script in a fake root):
#   /usr/bin/dropbear, /usr/bin/dropbearkey   binaries (from the Dropbear build)
#   /etc/config/dropbear/                     persistent state (CONFIG, JFFS2), 0700
#     dropbear_ed25519_host_key               host key, generated on first boot, 0600
#     authorized_keys                         client keys, 0600, root:root
#   /mnt/camyn/authorized_keys                provisioning file on the microSD
#                                             (mount point TO BE CONFIRMED on hardware)
#   /tmp/camyn_ssh.log                        one line per step, no secret logged
#   /tmp/dropbear.pid, /tmp/camyn_ssh.lock    runtime state
#
# Key policy: NO key is embedded in the firmware image. authorized_keys comes
# only from the microSD file, is validated (ssh-ed25519 lines only, no options,
# no "command="), and replaces the CONFIG copy when the content differs. The
# file on the SD card is never modified or deleted.
#
# Permissions: Dropbear refuses authorized_keys if any path component is
# writable by group/others, so the directory is 0700, the file 0600, and a
# umask of 077 is used for every write. Modes of /etc/config, /etc and / are
# logged for diagnosis; they are not changed.

CAMYN_ROOT=${CAMYN_ROOT:-}
CONFIG_DIR=${CAMYN_CONFIG_DIR:-$CAMYN_ROOT/etc/config}
DB_DIR=$CONFIG_DIR/dropbear
HOSTKEY=$DB_DIR/dropbear_ed25519_host_key
AUTHKEYS=$DB_DIR/authorized_keys
SD_DIR=${CAMYN_SD_DIR:-$CAMYN_ROOT/mnt/camyn}
BIN_DIR=${CAMYN_BIN_DIR:-$CAMYN_ROOT/usr/bin}
TMP_DIR=${CAMYN_TMP_DIR:-$CAMYN_ROOT/tmp}
SSH_PORT=${CAMYN_SSH_PORT:-22}
WAIT_TRIES=${CAMYN_WAIT_TRIES:-30}
LOG=$TMP_DIR/camyn_ssh.log
PIDFILE=$TMP_DIR/dropbear.pid
LOCKDIR=$TMP_DIR/camyn_ssh.lock

log() {
    echo "camyn_ssh: $*" >>"$LOG" 2>/dev/null
}

is_running() {
    [ -s "$PIDFILE" ] || return 1
    read -r _pid <"$PIDFILE" 2>/dev/null || return 1
    case "$_pid" in
        '' | *[!0-9]*) return 1 ;;
    esac
    kill -0 "$_pid" 2>/dev/null
}

log_mode() {
    _m=$(stat -c '%a %u' "$1" 2>/dev/null) || _m=unknown
    log "mode of $1: $_m"
}

# Validate the SD key file ($1) into $2. Accepts only plain
# "ssh-ed25519 <base64> [comment]" lines. Returns 0 if at least one valid
# line was written.
filter_keys() {
    _src=$1
    _dst=$2
    : >"$_dst"
    _n=0
    _bad=0
    while IFS= read -r _line || [ -n "$_line" ]; do
        _line=$(printf '%s' "$_line" | tr -d '\r')
        case "$_line" in
            '' | '#'*) continue ;;
            *command=*)
                _bad=$((_bad + 1))
                continue
                ;;
        esac
        if printf '%s\n' "$_line" | grep -Eq '^ssh-ed25519 AAAAC3NzaC1lZDI1NTE5[A-Za-z0-9+/]{43,}={0,2}( [^[:cntrl:]]*)?$'; then
            printf '%s\n' "$_line" >>"$_dst"
            _n=$((_n + 1))
        else
            _bad=$((_bad + 1))
        fi
    done <"$_src"
    log "SD keys: $_n accepted, $_bad rejected"
    [ "$_n" -gt 0 ]
}

provision_keys() {
    _sdfile=$SD_DIR/authorized_keys
    if [ ! -f "$_sdfile" ]; then
        log "no $_sdfile on SD, keeping existing authorized_keys"
        return 0
    fi
    _new=$DB_DIR/.authorized_keys.new.$$
    if filter_keys "$_sdfile" "$_new"; then
        if [ -f "$AUTHKEYS" ] && cmp -s "$_new" "$AUTHKEYS"; then
            log "authorized_keys unchanged"
            rm -f "$_new"
        else
            chmod 600 "$_new"
            chown 0:0 "$_new" 2>/dev/null || log "chown 0:0 failed (ignored)"
            if mv -f "$_new" "$AUTHKEYS"; then
                log "authorized_keys installed from SD"
            else
                log "could not install authorized_keys"
                rm -f "$_new"
            fi
        fi
    else
        log "no valid key on SD, keeping existing authorized_keys"
        rm -f "$_new"
    fi
}

fix_perms() {
    chmod 700 "$DB_DIR" 2>/dev/null
    [ -f "$AUTHKEYS" ] && chmod 600 "$AUTHKEYS" 2>/dev/null
    [ -f "$HOSTKEY" ] && chmod 600 "$HOSTKEY" 2>/dev/null
    chown 0:0 "$DB_DIR" 2>/dev/null
    [ -f "$AUTHKEYS" ] && chown 0:0 "$AUTHKEYS" 2>/dev/null
    [ -f "$HOSTKEY" ] && chown 0:0 "$HOSTKEY" 2>/dev/null
    return 0
}

wait_config_writable() {
    _i=0
    while [ "$_i" -lt "$WAIT_TRIES" ]; do
        if mkdir -p "$DB_DIR" 2>/dev/null && : >"$DB_DIR/.rwtest" 2>/dev/null; then
            rm -f "$DB_DIR/.rwtest"
            return 0
        fi
        _i=$((_i + 1))
        sleep 1
    done
    return 1
}

do_start() {
    umask 077
    mkdir -p "$TMP_DIR" 2>/dev/null

    if ! mkdir "$LOCKDIR" 2>/dev/null; then
        # Another instance is running; take over only a stale lock.
        _lp=
        read -r _lp <"$LOCKDIR/pid" 2>/dev/null
        case "$_lp" in
            '' | *[!0-9]*) _lp= ;;
        esac
        if [ -n "$_lp" ] && kill -0 "$_lp" 2>/dev/null; then
            log "another instance holds the lock, exiting"
            return 0
        fi
        rm -rf "$LOCKDIR"
        mkdir "$LOCKDIR" 2>/dev/null || return 0
    fi
    trap 'rm -rf "$LOCKDIR"' EXIT
    trap 'exit 0' INT TERM HUP
    echo $$ >"$LOCKDIR/pid"

    if is_running; then
        log "dropbear already running, nothing to do"
        return 0
    fi

    : >"$LOG" 2>/dev/null
    log "start (pid $$)"

    if [ ! -x "$BIN_DIR/dropbear" ] || [ ! -x "$BIN_DIR/dropbearkey" ]; then
        log "dropbear or dropbearkey missing/not executable in $BIN_DIR, giving up"
        return 0
    fi

    log_mode "$CONFIG_DIR"
    log_mode "$CAMYN_ROOT/etc"
    log_mode "$CAMYN_ROOT/"

    if wait_config_writable; then
        _dir=$DB_DIR
        _persistent=1
        fix_perms
    else
        _dir=$TMP_DIR/camyn_dropbear
        _persistent=0
        log "$CONFIG_DIR not writable, using ephemeral host key in $_dir and no persistent authorized_keys"
        mkdir -p "$_dir" 2>/dev/null
        chmod 700 "$_dir" 2>/dev/null
    fi
    _hostkey=$_dir/dropbear_ed25519_host_key

    if [ ! -s "$_hostkey" ]; then
        log "generating ed25519 host key (may wait for entropy)"
        _tmpkey=$_dir/.hostkey.new.$$
        rm -f "$_tmpkey"
        if "$BIN_DIR/dropbearkey" -t ed25519 -f "$_tmpkey" >/dev/null 2>&1 && [ -s "$_tmpkey" ]; then
            chmod 600 "$_tmpkey"
            mv -f "$_tmpkey" "$_hostkey" && log "host key generated"
            sync
        else
            rm -f "$_tmpkey"
            log "host key generation failed, giving up"
            return 0
        fi
    fi

    if [ "$_persistent" = 1 ]; then
        provision_keys
        fix_perms
        if [ ! -s "$AUTHKEYS" ]; then
            log "WARNING: no authorized key, nobody can log in (put authorized_keys on the SD card)"
        fi
    fi

    # Dropbear daemonizes itself (no -F); -E sends its log to stderr -> $LOG.
    "$BIN_DIR/dropbear" -r "$_hostkey" -D "$_dir" -p "$SSH_PORT" \
        -P "$PIDFILE" -T 3 -E >/dev/null 2>>"$LOG" </dev/null
    log "dropbear launcher returned status $? (daemon detached)"
    return 0
}

do_status() {
    if is_running; then
        echo "camyn_ssh: dropbear running (pid $_pid)"
        return 0
    fi
    echo "camyn_ssh: dropbear not running"
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
