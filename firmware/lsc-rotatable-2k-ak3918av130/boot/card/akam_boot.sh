#!/bin/sh
# akam_boot.sh -- Akam overlay launcher for the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
# Run as root by YYOS /appfs/script/init.sh when the card's yy_debug.ini has mode=debug and
# start_sh=/mnt/sdcard/akam_boot.sh. In that mode init runs this INSTEAD of /appfs/script/start.sh, so
# handing off to start.sh is mandatory: the camera would never come up otherwise.
#
# ANTI-BRICK RULES (see README.md):
#   - writes only to /tmp (tmpfs) and the SD card; the single exception is the opt-in param.conf edit of
#     enable_local.sh (config partition, after a backup to the card)
#   - removing the card always returns the camera to stock behaviour
#   - this script ALWAYS reaches the hand-off to start.sh: no "set -e", every step runs in its own subshell and
#     logs its own failure, slow steps run in the background, and a card with missing or broken helpers still
#     boots the camera.
#
# NON-BLOCKING by design (the stock app probably feeds the watchdog, PLAN section 9). Order (PLAN section 5;
# steps 3 and 4 run after the hand-off because they need the network and yy_cam):
#   1  rescue: telnet (passwordless root shell) + tmpfs shadow root/root      [flag akam/no_telnet turns it off]
#   2  SSH: Dropbear, key-only root on :22, keys and host key from the card   (background, start_ssh.sh)
#   -  param.conf opt-in edit BEFORE the app starts                           (enable_local.sh apply)
#   -  PoC-1 only with flag akam/own_app: boot counter + bind mount of the stub over yy_cam (see 4b)
#   5  hand off to /appfs/script/start.sh (background)
#   3  decloud: WAN blackhole routes, only with flag akam/decloud             (background, decloud.sh)
#   4  report the local services RTSP + ONVIF                                 (background, enable_local.sh verify)
# Everything is logged to <card>/akam/akam_boot.log.
#
# Card layout: /mnt/sdcard/{yy_debug.ini,akam_boot.sh,busybox} and /mnt/sdcard/akam/{*.sh,dropbearmulti,
# authorized_keys,flag files,logs}. Paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.
# $AKAM_SH is the shell used for the helper scripts (default sh).

ROOT=${AKAM_ROOT:-}
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
START=$ROOT/appfs/script/start.sh
SH=${AKAM_SH:-sh}

# An incomplete or broken card must still boot the camera. (A syntax error in a sourced file would end this
# shell, hence the probe in a subshell first.)
# shellcheck disable=SC1091  # the helper lives on the card at run time
if ! ( . "$SD/akam/common.sh" ) >/dev/null 2>&1; then
    "$START" &
    exit 0
fi
# shellcheck disable=SC1091  # the helper lives on the card at run time
. "$SD/akam/common.sh"

log boot "akam_boot.sh start"

# 1) Maintenance / rescue: tmpfs shadow with a known root password (RAM only; the real /etc/shadow is on a
#    read-only squashfs and is bind-mounted over, never modified) and telnet with the card's static busybox.
(
    if flag no_telnet; then
        log boot "rescue telnet disabled by flag akam/no_telnet"
        exit 0
    fi
    mkdir -p "$TMP/etc" || exit 1
    # The $1$ hash below is the hash of "root".
    # shellcheck disable=SC2016  # literal crypt hash, nothing to expand
    echo 'root:$1$hack$26ZSPRIJi4E4euNXpW7mr1:10933:0:99999:7:::' >"$TMP/etc/shadow" || exit 1
    if mount --bind "$TMP/etc/shadow" "$ROOT/etc/shadow"; then
        log boot "shadow patched (root/root, RAM only)"
    else
        log boot "WARNING: shadow bind mount failed"
    fi
    if "$SD/busybox" telnetd -l /bin/sh -p 23 2>/dev/null; then
        log boot "telnetd on :23"
    else
        log boot "WARNING: telnetd did not start (busybox missing on the card?)"
    fi
) || log boot "WARNING: rescue step failed"

# 2) SSH, in the background: the first boot generates a host key, which can wait for entropy.
( $SH "$AK/start_ssh.sh" >/dev/null 2>&1 </dev/null & ) || log boot "WARNING: could not launch start_ssh.sh"

# 4a) param.conf opt-in, before the app starts so that it reads the new values at once (no-op without the flag).
( $SH "$AK/enable_local.sh" apply >/dev/null 2>&1 </dev/null ) || log boot "WARNING: enable_local.sh apply failed"

# 4b) PoC-1 "own app" (opt-in flag akam/own_app): replace yy_cam by akam/own_app_stub.sh for THIS boot only, with a
#     bind mount of a /tmp copy over /appfs/bin/yy_cam (RAM only; the squashfs file is untouched and a reboot or
#     the card's removal restores stock). Boot-loop guard: akam/own_app_boots counts boots that did not reach
#     "healthy" (the stub resets it to 0); at 3 the stock yy_cam is left alone. Delete the file to re-arm.
#     Without the flag nothing below runs.
if flag own_app; then
    (
        CNT=$AK/own_app_boots
        n=0
        if [ -e "$CNT" ]; then
            n=$(cat "$CNT" 2>/dev/null)
            case $n in '' | *[!0-9]* | ?????*) n=3 ;; esac
        fi
        if [ "$n" -ge 3 ]; then
            log boot "own_app disabled after 3 unfinished boots (delete akam/own_app_boots to re-arm)"
            exit 0
        fi
        echo $((n + 1)) >"$CNT" 2>/dev/null
        sync
        if [ "$(cat "$CNT" 2>/dev/null)" != $((n + 1)) ]; then
            log boot "own_app: cannot write the boot counter, stock yy_cam boots"
            exit 0
        fi
        if cp "$AK/own_app_stub.sh" "$TMP/own_app_stub.sh" 2>/dev/null && chmod 755 "$TMP/own_app_stub.sh" &&
            mount --bind "$TMP/own_app_stub.sh" "$ROOT/appfs/bin/yy_cam"; then
            log boot "own_app: stub bind-mounted over yy_cam (unfinished boots: $((n + 1)) of 3)"
        else
            echo "$n" >"$CNT" 2>/dev/null
            sync
            log boot "WARNING: own_app stub could not be installed, stock yy_cam boots"
        fi
    ) || log boot "WARNING: own_app step failed"
fi

# 5) Hand off to the stock boot (drivers, Wi-Fi, yy_cam).
"$START" &
log boot "handed off to $START"

# 3 + 4b) Network-dependent steps, in the background.
(
    (
        $SH "$AK/decloud.sh" on
        $SH "$AK/enable_local.sh" verify
    ) >/dev/null 2>&1 </dev/null &
) || log boot "WARNING: could not launch the background steps"

log boot "akam_boot.sh done"
exit 0
