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
#                                             (stock mounts the SD on /mnt only during
#                                             early boot, then umount -l; we re-mount it
#                                             read-only on /tmp/camyn_sd, see mount_sd_ro)
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
# logged for diagnosis; they are not changed (the stock / and /etc are 0775 and
# read-only: see prepare_home, which moves root's HOME instead).

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
    read -r _pid 2>/dev/null <"$PIDFILE" || return 1
    case "$_pid" in
        '' | *[!0-9]*) return 1 ;;
    esac
    kill -0 "$_pid" 2>/dev/null || return 1
    # A stale pidfile whose PID was reused must not count as "running".
    # (No "tr": the camera's BusyBox has no such applet. cmdline is NUL
    # separated; grep -a reads it fine.)
    grep -qa dropbear "${CAMYN_PROC_DIR:-/proc}/$_pid/cmdline" 2>/dev/null && return 0
    return 1
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
    _cr=$(printf '\r')
    while IFS= read -r _line || [ -n "$_line" ]; do
        # Strip CRs (DOS line endings) in pure shell: no "tr" on the camera.
        while [ "${_line%"$_cr"}" != "$_line" ]; do _line=${_line%"$_cr"}; done
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

# The stock boot (service.sh) mounts the SD on /mnt for its own checks and then
# lazily unmounts it, so by the time this script runs /mnt/camyn is normally
# NOT visible. Mount the card read-only on a private directory, same device and
# filesystem order as the stock tf_mount.sh, just to read the key file.
# Sets _sdmnt (to be unmounted by the caller) and _sdfile. Never fatal.
mount_sd_ro() {
    _sdmnt=$TMP_DIR/camyn_sd
    mkdir -p "$_sdmnt" 2>/dev/null || return 1
    for _try in "vfat $CAMYN_ROOT/dev/mmcblk0p1" "exfat $CAMYN_ROOT/dev/mmcblk0p1" \
        "vfat $CAMYN_ROOT/dev/mmcblk0" "exfat $CAMYN_ROOT/dev/mmcblk0"; do
        # shellcheck disable=SC2086
        set -- $_try
        [ -e "$2" ] || continue
        if mount -t "$1" -o ro "$2" "$_sdmnt" 2>/dev/null; then
            if [ -f "$_sdmnt/camyn/authorized_keys" ]; then
                _sdfile=$_sdmnt/camyn/authorized_keys
                log "SD mounted read-only ($1 $2) on $_sdmnt"
                return 0
            fi
            umount "$_sdmnt" 2>/dev/null
        fi
    done
    return 1
}

provision_keys() {
    _sdfile=$SD_DIR/authorized_keys
    _sdmnt=
    if [ ! -f "$_sdfile" ] && [ "${CAMYN_SD_AUTOMOUNT:-1}" = 1 ]; then
        mount_sd_ro || _sdmnt=
        [ -f "$_sdfile" ] || _sdmnt=
    fi
    if [ ! -f "$_sdfile" ]; then
        log "no $SD_DIR/authorized_keys on SD, keeping existing authorized_keys"
        return 0
    fi
    install_keys
    if [ -n "$_sdmnt" ]; then
        umount "$_sdmnt" 2>/dev/null || log "could not unmount $_sdmnt (ignored)"
        _sdmnt=
    fi
    return 0
}

# Validate $_sdfile and install it as authorized_keys.
install_keys() {
    _new=$DB_DIR/.authorized_keys.new.$$
    if filter_keys "$_sdfile" "$_new"; then
        if [ -f "$AUTHKEYS" ] && cmp -s "$_new" "$AUTHKEYS"; then
            log "authorized_keys unchanged"
            rm -f "$_new"
        else
            chmod 600 "$_new"
            chown 0:0 "$_new" 2>/dev/null || log "chown 0:0 failed (ignored)"
            sync
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

# Dropbear (musl getusershell) refuses a login when /etc/shells EXISTS and does
# not list the user's shell. If so, bind-mount a private list that has /bin/sh
# over it (the real file is untouched). Never fatal; no file = musl default
# list, which already contains /bin/sh.
fix_shells() {
    _shells=$CAMYN_ROOT/etc/shells
    [ -f "$_shells" ] || return 0
    grep -qx /bin/sh "$_shells" 2>/dev/null && return 0
    _sd=$TMP_DIR/camyn
    _sf=$_sd/shells
    if mkdir -p "$_sd" 2>/dev/null && chmod 755 "$_sd" 2>/dev/null &&
        echo /bin/sh >"$_sf" 2>/dev/null && chmod 644 "$_sf" 2>/dev/null &&
        mount --bind "$_sf" "$_shells" 2>/dev/null; then
        log "/etc/shells lacked /bin/sh, bind-mounted $_sf over it"
    else
        log "/etc/shells lacks /bin/sh and the bind mount failed (ignored); root login may be refused"
    fi
    return 0
}

# Dropbear walks from the authorized_keys directory up to root's HOME (or "/")
# and ignores the key file if ANY directory on the way is writable by group or
# others. On the real camera "/" and /etc come from the read-only ROOTFS
# squashfs where they are mode 0775 (observed on the extracted stock tree), and
# that cannot be changed at runtime: every key login would be refused. The walk
# stops at root's HOME, so make HOME the key directory itself: a private 0700
# directory on tmpfs holding a copy of authorized_keys, plus a private copy of
# passwd (bind-mounted over /etc/passwd; the real file is untouched) whose root
# entry has that HOME. If /etc/passwd has no root entry at all (dropbear would
# reject every login as "nonexistent user") the private copy gets one.
# Sets _keydir (the directory for -D): the tmpfs HOME when the bind mount took
# effect, else the unchanged persistent directory. Never fatal.
prepare_home() {
    _keydir=$1
    _home=$TMP_DIR/camyn_home
    _pw=$CAMYN_ROOT/etc/passwd
    _pwd=$TMP_DIR/camyn
    _pwn=$_pwd/passwd
    mkdir -p "$_home" "$_pwd" 2>/dev/null || return 0
    chmod 700 "$_home" 2>/dev/null
    chmod 755 "$_pwd" 2>/dev/null
    if ! grep -q "^root:[^:]*:[^:]*:[^:]*:[^:]*:$_home:" "$_pw" 2>/dev/null; then
        [ -e "$_pw" ] || : >"$_pw" 2>/dev/null
        if grep -q '^root:' "$_pw" 2>/dev/null; then
            sed "s|^\\(root:[^:]*:[^:]*:[^:]*:[^:]*:\\)[^:]*:|\\1$_home:|" "$_pw" >"$_pwn" 2>/dev/null
        else
            { cat "$_pw" 2>/dev/null; echo "root:x:0:0:root:$_home:/bin/sh"; } >"$_pwn" 2>/dev/null
        fi
        chmod 644 "$_pwn" 2>/dev/null
        if [ -s "$_pwn" ] && mount -o bind "$_pwn" "$_pw" 2>/dev/null &&
            grep -q "^root:[^:]*:[^:]*:[^:]*:[^:]*:$_home:" "$_pw" 2>/dev/null; then
            log "root HOME is now $_home (private passwd bind-mounted over $_pw)"
        else
            log "could not set root HOME to $_home, keeping $_keydir (login may be refused if / or /etc is group-writable)"
            return 0
        fi
    fi
    if [ -s "$AUTHKEYS" ]; then
        if cp "$AUTHKEYS" "$_home/.authorized_keys.new" 2>/dev/null &&
            chmod 600 "$_home/.authorized_keys.new" &&
            mv -f "$_home/.authorized_keys.new" "$_home/authorized_keys"; then
            chown 0:0 "$_home" "$_home/authorized_keys" 2>/dev/null
            _keydir=$_home
        else
            log "could not copy authorized_keys to $_home, keeping $_keydir"
            rm -f "$_home/.authorized_keys.new"
        fi
    else
        rm -f "$_home/authorized_keys"
        _keydir=$_home
    fi
    return 0
}

do_start() {
    umask 077
    mkdir -p "$TMP_DIR" 2>/dev/null

    if ! mkdir "$LOCKDIR" 2>/dev/null; then
        # Another instance is running; take over only a stale lock.
        # The holder writes its pid just after mkdir: if the file is not there
        # yet, give it a moment instead of declaring the lock stale.
        _lp=
        _try=0
        while [ "$_try" -lt 3 ]; do
            read -r _lp 2>/dev/null <"$LOCKDIR/pid"
            case "$_lp" in
                '' | *[!0-9]*) _lp= ;;
            esac
            [ -n "$_lp" ] && break
            _try=$((_try + 1))
            sleep 1
        done
        if [ -n "$_lp" ] && kill -0 "$_lp" 2>/dev/null; then
            log "another instance holds the lock, exiting"
            return 0
        fi
        rm -rf "$LOCKDIR"
        mkdir "$LOCKDIR" 2>/dev/null || return 0
    fi
    # Also drop the read-only SD mount if we are killed while it is mounted.
    trap 'rm -rf "$LOCKDIR"; [ -z "${_sdmnt:-}" ] || umount "$_sdmnt" 2>/dev/null' EXIT
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
        log "$CONFIG_DIR not writable, using ephemeral host key and authorized_keys in $_dir"
        mkdir -p "$_dir" 2>/dev/null
        chmod 700 "$_dir" 2>/dev/null
        # Keep SD provisioning working (else nobody could log in at all).
        DB_DIR=$_dir
        AUTHKEYS=$_dir/authorized_keys
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

    provision_keys
    [ "$_persistent" = 1 ] && fix_perms
    if [ ! -s "$AUTHKEYS" ]; then
        log "WARNING: no authorized key, nobody can log in (put authorized_keys on the SD card)"
    fi

    fix_shells
    prepare_home "$_dir"

    # Dropbear daemonizes itself (no -F); -E sends its log to stderr -> $LOG.
    "$BIN_DIR/dropbear" -r "$_hostkey" -D "$_keydir" -p "$SSH_PORT" \
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
