# flash_guard.sh — sourced (". flash_guard.sh") by every script that writes the APP
# partition (mtd7). POSIX sh / BusyBox ash. Defines camyn_flash_guard SCRIPT [TOOL].
#
# Refuses (returns 1) when the running system depends on the APP partition:
#   - the shell is inside an SSH session (SSH_CONNECTION / SSH_CLIENT set, or a
#     dropbear process among the ancestors), or
#   - /usr is a squashfs mounted from mtdblock7 AND the script or flash tool lives
#     under /usr (it would be erased while executing).
# Override: CAMYN_FLASH_I_KNOW=1 (prints a loud warning; meant for STOCK firmware
# over Telnet, where nothing runs from the APP partition being written).
#
# Second entry point, camyn_flash_guard_detached SCRIPT TOOL IMAGE CONFIRM, for
# the no-UART reflash over SSH (camyn_reflash_ssh.sh). It does NOT honour
# CAMYN_FLASH_I_KNOW and allows the write only when ALL of these hold:
#   1. CONFIRM is exactly "--i-understand-no-uart" (the operator accepted that
#      a partly written APP is recoverable only through UART);
#   2. SCRIPT, TOOL and IMAGE all resolve (symlinks followed) under the RAM
#      directory (/tmp), none under /usr, and that directory is a tmpfs/ramfs
#      mount, so nothing executed or read during the write lives on flash;
#   3. the process is detached from the SSH session: SSH_CONNECTION,
#      SSH_CLIENT and SSH_TTY are unset, no dropbear process is among its
#      ancestors, and it is a session leader (setsid), so the session's death
#      cannot signal it. A worker started from an SSH shell passes rule 3 only
#      once the launching shell has exited and it was re-parented to init.
# The plain camyn_flash_guard rule above (refuse inside SSH / tools under
# /usr) is unchanged and stays the default for every other script.
#
# Test hooks: CAMYN_PROC_MOUNTS (default /proc/mounts), CAMYN_PROC_DIR (default /proc),
# CAMYN_FLASH_TMPDIR (default /tmp, the RAM directory of rule 2).

_cfg_doc="docs/camyn_v1_flash_and_restore.md, section 'Reflashing later: never over an SSH session'"

_cfg_under_usr() {
    case "$1" in
        /usr | /usr/*) return 0 ;;
    esac
    return 1
}

# Absolute, physical path (symlinks resolved, up to 16 hops, using only
# shell builtins plus readlink when present) so /mnt/x -> /usr/... cannot slip
# through. No dirname/basename: the bare UART init=/bin/sh rootfs has neither
# (nor readlink, nor cut, wc, md5sum...): they live in the APP partition.
_cfg_abspath() {
    _f=$1
    _h=0
    while [ -L "$_f" ] && [ "$_h" -lt 16 ]; do
        _h=$((_h + 1))
        _t=$(readlink "$_f" 2>/dev/null) || break
        case "$_t" in
            /*) _f=$_t ;;
            */*) _f=${_f%/*}/$_t ;;
            *) case "$_f" in */*) _f=${_f%/*}/$_t ;; *) _f=$_t ;; esac ;;
        esac
    done
    case "$_f" in
        */*) _dir=${_f%/*}; [ -n "$_dir" ] || _dir=/ ;;
        *) _dir=. ;;
    esac
    _d=$(cd "$_dir" 2>/dev/null && pwd -P) || _d=$(pwd -P)
    printf '%s/%s\n' "$_d" "${_f##*/}"
}

# returns 0 if a dropbear process is among the ancestors of this shell
_cfg_dropbear_ancestor() {
    _p=$$
    _n=0
    while [ -n "$_p" ] && [ "$_p" -gt 1 ] 2>/dev/null && [ "$_n" -lt 64 ]; do
        _n=$((_n + 1))
        [ -r "${CAMYN_PROC_DIR:-/proc}/$_p/stat" ] || return 1
        _stat=$(cat "${CAMYN_PROC_DIR:-/proc}/$_p/stat" 2>/dev/null) || return 1
        _comm=${_stat#*\(}
        _comm=${_comm%\)*}
        case "$_comm" in
            dropbear*) return 0 ;;
        esac
        _rest=${_stat##*\) }
        # _rest is "STATE PPID ..."
        # shellcheck disable=SC2086
        set -- $_rest
        _p=${2:-}
    done
    return 1
}

_cfg_usr_is_app() {
    while read -r _dev _mp _type _rest; do
        if [ "$_mp" = "/usr" ] && [ "$_type" = "squashfs" ]; then
            case "$_dev" in
                *mtdblock7*) return 0 ;;
            esac
        fi
    done <"${CAMYN_PROC_MOUNTS:-/proc/mounts}"
    return 1
}

camyn_flash_guard() {
    _script=$(_cfg_abspath "$1")
    _tool=${2:-}
    _why=""

    if [ -n "${SSH_CONNECTION:-}" ] || [ -n "${SSH_CLIENT:-}" ]; then
        _why="running inside an SSH session (SSH_CONNECTION/SSH_CLIENT set)"
    elif _cfg_dropbear_ancestor; then
        _why="running under a dropbear process (SSH session)"
    elif _cfg_usr_is_app; then
        if _cfg_under_usr "$_script"; then
            _why="/usr is the APP squashfs (mtdblock7) and $_script lives under /usr"
        elif [ -n "$_tool" ] && _cfg_under_usr "$_tool"; then
            _why="/usr is the APP squashfs (mtdblock7) and $_tool lives under /usr"
        fi
    fi

    [ -z "$_why" ] && return 0

    if [ "${CAMYN_FLASH_I_KNOW:-}" = "1" ]; then
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" >&2
        echo "WARNING: $_why" >&2
        echo "WARNING: CAMYN_FLASH_I_KNOW=1 set, flashing anyway. The session or" >&2
        echo "WARNING: the board may die mid-write; only UART recovers a partial APP." >&2
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!" >&2
        return 0
    fi

    echo "ERROR: refusing to write the APP partition: $_why." >&2
    echo "The running system executes from that partition. Use the UART init=/bin/sh" >&2
    echo "shell with the scripts and flash_tool on the SD card. See $_cfg_doc." >&2
    echo "(Stock firmware over Telnet only: re-run with CAMYN_FLASH_I_KNOW=1.)" >&2
    return 1
}

# ---------------------------------------------------------------------------
# Detached (no-UART) variant, see the header.

_cfg_under_dir() { # path $1 is strictly below directory $2
    case "$1" in
        "$2"/*) return 0 ;;
    esac
    return 1
}

_cfg_is_ram_dir() { # $1 is a mount point of type tmpfs or ramfs
    while read -r _dev _mp _type _rest; do
        if [ "$_mp" = "$1" ]; then
            case "$_type" in
                tmpfs | ramfs) return 0 ;;
            esac
        fi
    done <"${CAMYN_PROC_MOUNTS:-/proc/mounts}"
    return 1
}

# returns 0 if this shell is the leader of its own session (sid == pid)
_cfg_session_leader() {
    [ -r "${CAMYN_PROC_DIR:-/proc}/$$/stat" ] || return 1
    _stat=$(cat "${CAMYN_PROC_DIR:-/proc}/$$/stat" 2>/dev/null) || return 1
    _rest=${_stat##*\) }
    # _rest is "STATE PPID PGRP SESSION ..."
    # shellcheck disable=SC2086
    set -- $_rest
    [ "${4:-}" = "$$" ]
}

camyn_flash_guard_detached() {
    _dscript=$1
    _dtool=$2
    _dimage=$3
    _confirm=${4:-}
    _ramdir=${CAMYN_FLASH_TMPDIR:-/tmp}
    _dwhy=""

    if [ "$_confirm" != "--i-understand-no-uart" ]; then
        _dwhy="missing --i-understand-no-uart confirmation"
    else
        _dscript=$(_cfg_abspath "$_dscript")
        _dtool=$(_cfg_abspath "$_dtool")
        _dimage=$(_cfg_abspath "$_dimage")
        _ramdir=$(cd "$_ramdir" 2>/dev/null && pwd -P) || _ramdir=""
        if [ -z "$_ramdir" ]; then
            _dwhy="RAM directory ${CAMYN_FLASH_TMPDIR:-/tmp} does not exist"
        elif ! _cfg_is_ram_dir "$_ramdir"; then
            _dwhy="$_ramdir is not a tmpfs/ramfs mount point"
        else
            for _p in "$_dscript" "$_dtool" "$_dimage"; do
                if _cfg_under_usr "$_p"; then
                    _dwhy="$_p lives under /usr (the APP squashfs)"
                elif ! _cfg_under_dir "$_p" "$_ramdir"; then
                    _dwhy="$_p is not under $_ramdir"
                fi
                [ -z "$_dwhy" ] || break
            done
        fi
        if [ -z "$_dwhy" ]; then
            if [ -n "${SSH_CONNECTION:-}" ] || [ -n "${SSH_CLIENT:-}" ] || [ -n "${SSH_TTY:-}" ]; then
                _dwhy="still carries the SSH session environment (SSH_CONNECTION/SSH_CLIENT/SSH_TTY)"
            elif _cfg_dropbear_ancestor; then
                _dwhy="still a descendant of a dropbear process (not detached)"
            elif ! _cfg_session_leader; then
                _dwhy="not a session leader (not started with setsid)"
            fi
        fi
    fi

    [ -z "$_dwhy" ] && return 0
    echo "ERROR: refusing the detached APP write: $_dwhy." >&2
    echo "See $_cfg_doc." >&2
    return 1
}
