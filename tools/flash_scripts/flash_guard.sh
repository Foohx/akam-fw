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
# Test hooks: CAMYN_PROC_MOUNTS (default /proc/mounts), CAMYN_PROC_DIR (default /proc).

_cfg_doc="docs/camyn_v1_flash_and_restore.md, section 'Reflashing later: never over an SSH session'"

_cfg_under_usr() {
    case "$1" in
        /usr | /usr/*) return 0 ;;
    esac
    return 1
}

# Absolute, physical path (symlinks resolved, up to 16 hops, using only
# cd/pwd -P and readlink when present) so /mnt/x -> /usr/... cannot slip through.
_cfg_abspath() {
    _f=$1
    _h=0
    while [ -L "$_f" ] && [ "$_h" -lt 16 ]; do
        _h=$((_h + 1))
        _t=$(readlink "$_f" 2>/dev/null) || break
        case "$_t" in
            /*) _f=$_t ;;
            *) _f=$(dirname "$_f")/$_t ;;
        esac
    done
    case "$_f" in
        */*) _d=$(cd "$(dirname "$_f")" 2>/dev/null && pwd -P) || _d=$(pwd -P) ;;
        *) _d=$(pwd -P) ;;
    esac
    printf '%s/%s\n' "$_d" "$(basename "$_f")"
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
