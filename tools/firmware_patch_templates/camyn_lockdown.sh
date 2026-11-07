#!/bin/sh
# camyn_lockdown.sh -- runtime safety net: keep telnet and FTP daemons dead.
#
# Installed as /usr/sbin/camyn_lockdown.sh (mode 755) and launched in the
# background by ak_rtsp_wrapper.sh. Second layer only: the first layer is the
# build-time patch of the APP's service.sh (patch_service_sh.sh), which stops
# the daemons from being launched at all. This script covers a patch that had
# no effect, or a late start by some other script.
#
# Usage: camyn_lockdown.sh            (no arguments)
#
# Behaviour:
#   - anti-duplicate lock ($TMP/camyn_lockdown.lock/pid; stale locks taken over)
#   - maintenance flag present (/mnt/CAMYN_MAINTENANCE on the microSD, mount
#     point TO BE CONFIRMED on hardware): log, kill nothing for that round, and
#     KEEP LOOPING. The flag is re-checked at every round: inserting the SD
#     pauses the killing, removing it (or deleting the file) resumes it at the
#     next round, so telnet/ftp cannot outlive the card.
#     Mount point: the flag sits at the ROOT of the microSD, which the stock
#     firmware mounts on /mnt. camyn_ssh.sh reads its provisioning files from
#     the camyn/ subdirectory of the same card (CAMYN_SD_DIR, /mnt/camyn): both
#     assume the same /mnt mount point, to be confirmed together on hardware.
#   - otherwise, each round: take one `ps` snapshot and SIGTERM (then SIGKILL on
#     the next round if the pid is still there) every process whose command
#     line has a token whose basename is telnetd, utelnetd, ftpd, bftpd,
#     vsftpd, or one of the SD-hook respawn scripts (telnet.sh, hack.sh).
#     The respawn scripts are killed first so they cannot restart a daemon.
#   - NEVER touches ak_rtsp, dropbear, wpa_supplicant, cmdserver, msgserver,
#     camyn_* (exclusion list checked before anything else), nor itself.
#   - no pkill/killall (not guaranteed on the target BusyBox); only ps + kill.
#   - never opens or listens on a network socket.
#
# Schedule (low cost; boot phase bounded, then unbounded):
#   fast phase: every 5 s for 2 min (24 rounds). Late starts by the stock boot
#     scripts (network bring-up, SD hook) happen within the first minute or
#     two; 5 s keeps the exposure of a late daemon to a few seconds.
#   slow phase: every 60 s with NO end (CAMYN_SLOW_ROUNDS=-1, the default), so
#     a restart after a network reconnect is always caught. The loop only ends
#     when the maintenance flag appears or the process is signalled.
#     CAMYN_SLOW_ROUNDS>=0 bounds it (used by the tests only).
#   - Dropbear respawn: every slow round, camyn_ssh.sh start is launched fully
#     detached (it has its own anti-duplicate guard and is a no-op when dropbear
#     runs), so a dead dropbear comes back within ~60 s. Never waited for.
#   Cost: one sleeping sh plus one `ps` fork per round (~0.2 per second in the
#     fast phase, ~0.017 per second after). RAM cost to be measured on hardware.
#
# Every failure path only logs and returns 0: it must never break ak_rtsp.
# Paths are prefixed by $CAMYN_ROOT (empty on the camera) so the host test can
# run it in a fake root.

CAMYN_ROOT=${CAMYN_ROOT:-}
TMP_DIR=${CAMYN_TMP_DIR:-$CAMYN_ROOT/tmp}
SSH_HELPER=${CAMYN_SSH_HELPER:-$CAMYN_ROOT/usr/sbin/camyn_ssh.sh}
FLAG=${CAMYN_MAINT_FLAG:-$CAMYN_ROOT/mnt/CAMYN_MAINTENANCE}
FAST_ROUNDS=${CAMYN_FAST_ROUNDS:-24}
FAST_PERIOD=${CAMYN_FAST_PERIOD:-5}
SLOW_ROUNDS=${CAMYN_SLOW_ROUNDS:--1}
SLOW_PERIOD=${CAMYN_SLOW_PERIOD:-60}
LOG=$TMP_DIR/camyn_lockdown.log
LOCKDIR=$TMP_DIR/camyn_lockdown.lock
TERMED=' '

log() {
    echo "camyn_lockdown: $*" >>"$LOG" 2>/dev/null
}

# Token classification of a command line ($1 = "ps" row without the pid).
# Prints: protect | script | daemon | none
classify() {
    _kind=none
    for _tok in $1; do
        _b=${_tok##*/}
        case "$_b" in
            ak_rtsp* | dropbear* | wpa_supplicant* | cmdserver | msgserver | camyn_*)
                echo protect
                return 0
                ;;
            telnet.sh | hack.sh)
                [ "$_kind" = none ] && _kind=script
                ;;
            telnetd | utelnetd | ftpd | bftpd | vsftpd)
                _kind=daemon
                ;;
        esac
    done
    echo "$_kind"
}

# One pass over a ps snapshot ($1) for the given kind ($2: script|daemon).
kill_pass() {
    while read -r _pid _rest; do
        case "$_pid" in
            '' | *[!0-9]*) continue ;;
        esac
        [ "$_pid" = "$$" ] && continue
        [ "$(classify "$_rest")" = "$2" ] || continue
        case "$TERMED" in
            *" $_pid "*)
                kill -9 "$_pid" 2>/dev/null && log "SIGKILL pid $_pid ($2): $_rest"
                ;;
            *)
                kill "$_pid" 2>/dev/null && log "SIGTERM pid $_pid ($2): $_rest"
                TERMED="$TERMED$_pid "
                ;;
        esac
    done <<EOF
$1
EOF
}

# Returns 0 when the maintenance flag exists.
maintenance() {
    [ -e "$FLAG" ]
}

# Detached, output discarded, never waited for: cannot block the loop.
respawn_ssh() {
    [ -x "$SSH_HELPER" ] || return 0
    ( "$SSH_HELPER" start </dev/null >/dev/null 2>&1 & )
}

round() {
    _snap=$(ps 2>/dev/null) || return 0
    kill_pass "$_snap" script
    kill_pass "$_snap" daemon
}

MAINT_LOGGED=0
# One round, unless the maintenance flag is present (then kill nothing).
guarded_round() {
    if maintenance; then
        if [ "$MAINT_LOGGED" = 0 ]; then
            log "maintenance mode: telnet not touched"
            MAINT_LOGGED=1
        fi
        return 0
    fi
    if [ "$MAINT_LOGGED" = 1 ]; then
        log "maintenance flag gone: lockdown resumed"
        MAINT_LOGGED=0
    fi
    round
}

main() {
    mkdir -p "$TMP_DIR" 2>/dev/null

    if ! mkdir "$LOCKDIR" 2>/dev/null; then
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
    trap 'rm -rf "$LOCKDIR"' EXIT
    trap 'exit 0' INT TERM HUP
    echo $$ >"$LOCKDIR/pid"

    : >"$LOG" 2>/dev/null
    log "start (pid $$)"

    _i=0
    while [ "$_i" -lt "$FAST_ROUNDS" ]; do
        guarded_round
        _i=$((_i + 1))
        sleep "$FAST_PERIOD"
    done
    _i=0
    while [ "$SLOW_ROUNDS" -lt 0 ] || [ "$_i" -lt "$SLOW_ROUNDS" ]; do
        guarded_round
        respawn_ssh
        _i=$((_i + 1))
        sleep "$SLOW_PERIOD"
    done
    log "done (bounded loop finished)"
    return 0
}

main
exit 0
