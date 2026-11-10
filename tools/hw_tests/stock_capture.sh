#!/bin/sh
# stock_capture.sh - capture how the STOCK vendor app anyka_ipc drives the speaker, the
# GPIOs and the ISP, on a still-stock camera booted from the microSD toolkit (POSIX sh,
# BusyBox 1.30.1: no tr, mktemp, nohup). Run it from the toolkit telnet shell (port 24).
#
#   sh /tmp/sd/stock_capture/stock_capture.sh check          read-only verification, changes nothing
#   sh /tmp/sd/stock_capture/stock_capture.sh run [SECONDS]  capture (default 120 s), then restore
#   sh /tmp/sd/stock_capture/stock_capture.sh restore        undo everything (safe to repeat)
#   options (before the command): --force (ignore a stock-check mismatch)  --keep-mute
#
# How it works, and why it is safe:
#   * Nothing is written to flash or /etc/config. Everything is RAM (tmpfs /tmp) or the SD card.
#   * The watchdog (which feeds the nowayout hardware watchdog) is NEVER touched. It starts
#     anyka_ipc with execvp("anyka_ipc") from its own environment, and uClibc ignores
#     /etc/ld.so.preload, so a bind mount puts a 3-line wrapper over /usr/bin/anyka_ipc: the
#     wrapper sets LD_PRELOAD=stock_hook.so and execs the real binary (seen through a second,
#     non-recursive bind of /usr). Only anyka_ipc is killed; the watchdog respawns it.
#   * The toolkit replaces /etc/config/_ht_hw_settings.ini and _ht_sw_settings.ini (and mutes
#     the factory prompts). For a faithful capture the pristine files of the APP partition are
#     bind-mounted on top (RAM copies) and the muted prompts are un-muted; restore pops all of it.
#   * Restore: remove the wrapper bind, pop the config binds, re-mute, kill anyka_ipc again
#     (clean respawn without the hook). A power cycle also restores everything.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) so the host test can use a fake root.

ROOT=${AKAM_ROOT:-}
STOCK_SHA=${STOCK_CAPTURE_SHA:-14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294}
TICK=${STOCK_CAPTURE_TICK:-5}          # seconds between progress lines (tests use 1)
STATE=$ROOT/tmp/stock_capture
LSTATE=/tmp/stock_capture            # the same place as seen by the camera (used in the wrapper)
IPC=/usr/bin/anyka_ipc
MTDDEV=$ROOT/dev/mtdblock7
FORCE=0
KEEP_MUTE=0
MIN_FREE_KB=${STOCK_CAPTURE_MIN_FREE_KB:-30000}
WAIT_RESPAWN=${STOCK_CAPTURE_WAIT:-60}
ACTIVE=0
RESTORED=0
SD=
RUNDIR=
N_FAIL=0

ok()   { echo "OK      $*"; }
info() { echo "INFO    $*"; }
warn() { echo "WARN    $*"; }
fail() { N_FAIL=$((N_FAIL + 1)); echo "FAIL    $*"; }
die()  { echo "FAIL    $*" >&2; exit "${2:-1}"; }
have() { command -v "$1" >/dev/null 2>&1; }

mounts_file() { echo "$ROOT/proc/mounts"; }
# number of mounts whose target is exactly $1 (logical path)
mcount() { awk -v p="$1" '$2 == p { n++ } END { print n + 0 }' "$(mounts_file)" 2>/dev/null; }

first_pid() { pidof "$1" 2>/dev/null | awk '{ print $1 }'; }

# ----------------------------------------------------------------------------- stock check
find_sd() {
    for c in /tmp/sd /mnt; do
        if [ -f "$ROOT$c/stock_capture/stock_hook.so" ]; then SD=$ROOT$c; return 0; fi
    done
    return 1
}

verify_stock() {
    if [ ! -x "$ROOT$IPC" ]; then fail "$IPC missing: not the stock firmware"; return 1; fi
    ok "$IPC present"
    if [ -n "$(first_pid ak_rtsp)" ] || [ -e "$ROOT/usr/bin/ak_rtsp" ] || [ -e "$ROOT/usr/sbin/akam_ssh.sh" ]; then
        fail "ak_rtsp / Akam files found: this is not a stock camera"; return 1
    fi
    ok "no ak_rtsp, no Akam helper"
    if [ -z "$(first_pid watchdog)" ]; then fail "watchdog is not running: killing anyka_ipc would not respawn it"; return 1; fi
    ok "watchdog running (left alone, it feeds the hardware watchdog)"
    if [ -z "$(first_pid anyka_ipc)" ]; then fail "anyka_ipc is not running"; return 1; fi
    ok "anyka_ipc running, pid $(first_pid anyka_ipc)"
    if ! grep -q '"APP"' "$ROOT/proc/mtd" 2>/dev/null; then warn "/proc/mtd shows no APP partition"; fi
    if have sha256sum && [ -r "$MTDDEV" ]; then
        got=$(dd if="$MTDDEV" bs=64k 2>/dev/null | sha256sum | awk '{ print $1 }')
        MTD_SHA=$got
        if [ "$got" = "$STOCK_SHA" ]; then
            ok "mtd7 (APP) sha256 matches the known stock image"
        elif [ "$FORCE" = 1 ]; then
            warn "mtd7 sha256 $got differs from the known stock image (--force given)"
        else
            fail "mtd7 sha256 $got differs from the known stock $STOCK_SHA (use --force to override)"
            return 1
        fi
    else
        MTD_SHA=unverified
        warn "sha256sum or $MTDDEV unavailable: stock image NOT verified, relying on the process checks"
    fi
    return 0
}

check_sd() {
    if ! find_sd; then fail "stock_hook.so not found in /tmp/sd/stock_capture or /mnt/stock_capture"; return 1; fi
    ok "capture kit found in $SD/stock_capture"
    if [ -f "$SD/stock_capture/SHA256SUMS" ] && have sha256sum; then
        if (cd "$SD/stock_capture" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
            ok "kit files match SHA256SUMS"
        else
            fail "SHA256SUMS mismatch: copy the kit to the card again"; return 1
        fi
    fi
    if ! (: >"$SD/stock_capture/.w") 2>/dev/null; then fail "SD card is not writable"; return 1; fi
    rm -f "$SD/stock_capture/.w"
    free=$(df -k "$SD" 2>/dev/null | awk 'NR > 1 { print $4 }' | tail -n 1)
    case $free in
        '' | *[!0-9]*) warn "cannot read the free space of the card" ;;
        *) if [ "$free" -lt "$MIN_FREE_KB" ]; then fail "only $free KiB free on the card (need $MIN_FREE_KB)"; return 1; fi
           ok "$free KiB free on the card" ;;
    esac
    for need in awk grep sed cp dd df find md5sum mount umount sleep kill pidof; do
        have "$need" || { fail "missing command: $need"; return 1; }
    done
    return 0
}

# ----------------------------------------------------------------------------- mounts
# bind SRC DST: remember the mount count of DST to pop exactly what we added.
do_bind() {
    lp=${2#"$ROOT"}
    before=$(mcount "$lp")
    if mount --bind "$1" "$2" 2>/dev/null; then
        echo "bind $lp $before" >>"$STATE/state"
        return 0
    fi
    return 1
}

pop_binds() {
    [ -f "$STATE/state" ] || return 0
    # undo in reverse order
    sed -n '1!G;h;$p' "$STATE/state" | while read -r kind target before; do
        [ "$kind" = bind ] || continue
        tries=0
        while [ "$(mcount "$target")" -gt "$before" ] && [ "$tries" -lt 3 ]; do
            umount "$ROOT$target" 2>/dev/null || umount -l "$ROOT$target" 2>/dev/null
            tries=$((tries + 1))
        done
        if [ "$(mcount "$target")" -gt "$before" ]; then
            echo "WARN    could not unmount $target (a power cycle clears it)"
        fi
    done
}

snap_config() { # $1 = output file; skips the files that are bind mounts (SD files, they change)
    {
        ls -lR "$ROOT/etc/config" 2>&1
        find "$ROOT/etc/config" -type f 2>/dev/null | while read -r f; do
            lp=${f#"$ROOT"}
            if [ "$(mcount "$lp")" -gt 0 ]; then echo "(bind mount, skipped) $lp"; continue; fi
            md5sum "$f" 2>&1
        done
    } >"$1" 2>&1
}

# ----------------------------------------------------------------------------- restore
restore() {
    [ "$RESTORED" = 1 ] && return 0
    RESTORED=1
    if [ ! -f "$STATE/state" ] && [ ! -f "$STATE/muted" ]; then
        info "nothing to restore"
        return 0
    fi
    echo "== restore =="
    pop_binds
    if [ -f "$STATE/muted" ]; then
        sil=${SD:-$ROOT/tmp/sd}/custom/audio/silent.mp3
        while read -r f; do
            if [ -f "$sil" ]; then
                mount --bind "$sil" "$ROOT$f" 2>/dev/null && echo "OK      re-muted $f"
            else
                echo "WARN    $f stays un-muted (silent.mp3 not found; reboot restores it)"
            fi
        done <"$STATE/muted"
    fi
    old=$(first_pid anyka_ipc)
    if [ -n "$old" ]; then
        kill -TERM "$old" 2>/dev/null
        i=0
        while [ "$i" -lt 8 ] && kill -0 "$old" 2>/dev/null; do sleep 1; i=$((i + 1)); done
        kill -0 "$old" 2>/dev/null && kill -KILL "$old" 2>/dev/null
        i=0
        new=
        while [ "$i" -lt "$WAIT_RESPAWN" ]; do
            new=$(first_pid anyka_ipc)
            [ -n "$new" ] && [ "$new" != "$old" ] && break
            new=
            sleep 1
            i=$((i + 1))
        done
        if [ -n "$new" ]; then echo "OK      anyka_ipc respawned by the watchdog without the hook (pid $new)"; else echo "WARN    anyka_ipc not seen again after ${WAIT_RESPAWN}s: check with ps, a power cycle fixes it"; fi
    fi
    rm -f "$STATE/out" "$STATE/state" "$STATE/muted" "$STATE/anyka_ipc_wrap" "$STATE/stock_hook.so" \
          "$STATE/cfg/_ht_hw_settings.ini" "$STATE/cfg/_ht_sw_settings.ini" "$STATE/active"
    rmdir "$STATE/cfg" "$STATE/app" "$STATE" 2>/dev/null
    ACTIVE=0
}

on_signal() { echo "signal received: restoring"; restore; exit 130; }
on_exit()   { [ "$ACTIVE" = 1 ] && restore; }

# ----------------------------------------------------------------------------- run
do_run() {
    secs=$1
    case $secs in '' | *[!0-9]*) die "SECONDS must be a number" 2 ;; esac
    if [ -f "$STATE/active" ]; then
        info "a previous run left its state behind: restoring it first"
        find_sd
        restore
        RESTORED=0
    fi
    verify_stock || exit 2
    check_sd || exit 2
    n=1
    while :; do
        RUNDIR=$SD/stock_capture/run$(printf '%02d' "$n")
        [ -e "$RUNDIR" ] || break
        n=$((n + 1))
    done
    mkdir -p "$RUNDIR" || die "cannot create $RUNDIR"
    ok "capture folder $RUNDIR"
    mkdir -p "$STATE/cfg" "$STATE/app" || die "cannot create $STATE"
    : >"$STATE/state"
    trap on_signal INT TERM HUP
    trap on_exit EXIT
    ACTIVE=1
    echo "active" >"$STATE/active"

    cp "$(mounts_file)" "$RUNDIR/mounts_before.txt" 2>/dev/null
    snap_config "$RUNDIR/config_before.txt"
    ok "snapshot of /etc/config taken (compared again at the end)"

    # pristine view of /usr: a NON-recursive bind does not carry the toolkit's file binds along
    do_bind "$ROOT/usr" "$STATE/app" || { fail "cannot bind /usr"; return 1; }
    [ -x "$STATE/app/bin/anyka_ipc" ] || [ "$ROOT" != "" ] || { fail "real anyka_ipc not visible through the bind"; return 1; }
    # pristine settings, RAM copies on top of whatever (toolkit or not) is there now
    for f in _ht_hw_settings.ini _ht_sw_settings.ini; do
        if [ -f "$STATE/app/local/$f" ] && [ -e "$ROOT/etc/config/$f" ]; then
            cp "$STATE/app/local/$f" "$STATE/cfg/$f" && do_bind "$STATE/cfg/$f" "$ROOT/etc/config/$f" \
                && ok "pristine $f of the APP partition mounted over /etc/config/$f (RAM copy)"
        else
            warn "no pristine $f available: the settings in place stay as they are"
        fi
    done
    # un-mute the prompts the toolkit muted
    : >"$STATE/muted"
    if [ "$KEEP_MUTE" = 0 ]; then
        for f in "$ROOT"/usr/share/*.mp3; do
            [ -e "$f" ] || continue
            lp=${f#"$ROOT"}
            if [ "$(mcount "$lp")" -gt 0 ]; then
                umount "$f" 2>/dev/null && { echo "$lp" >>"$STATE/muted"; info "un-muted $lp"; }
            fi
        done
    else
        info "--keep-mute: the toolkit's muted prompts stay muted"
    fi

    # hook + wrapper
    cp "$SD/stock_capture/stock_hook.so" "$STATE/stock_hook.so" || die "cannot copy the hook to RAM"
    ln -s "$RUNDIR" "$STATE/out" 2>/dev/null || die "cannot create the log link"
    {
        echo '#!/bin/sh'
        echo "# generated by stock_capture.sh: preload the capture hook, then run the real anyka_ipc"
        echo "LD_PRELOAD=$LSTATE/stock_hook.so"
        echo 'export LD_PRELOAD'
        echo "exec $LSTATE/app/bin/anyka_ipc \"\$@\""
    } >"$STATE/anyka_ipc_wrap"
    chmod 755 "$STATE/anyka_ipc_wrap"
    do_bind "$STATE/anyka_ipc_wrap" "$ROOT$IPC" || { fail "cannot bind the wrapper over $IPC"; return 1; }
    ok "wrapper bound over $IPC (the watchdog is untouched)"

    old=$(first_pid anyka_ipc)
    {
        echo "script=stock_capture.sh"
        echo "date=$(date 2>/dev/null)"
        echo "uptime=$(awk '{ print $1 }' "$ROOT/proc/uptime" 2>/dev/null)"
        echo "kernel=$(cat "$ROOT/proc/version" 2>/dev/null)"
        echo "mtd7_sha256=${MTD_SHA:-unknown}"
        echo "stock_firmware=yes"
        echo "seconds=$secs"
        echo "keep_mute=$KEEP_MUTE"
        echo "old_pid=$old"
        echo "sd=$SD"
    } >"$RUNDIR/run.meta"

    info "killing only anyka_ipc (pid $old); the watchdog respawns it with the hook"
    kill -TERM "$old" 2>/dev/null
    i=0
    while [ "$i" -lt 8 ] && kill -0 "$old" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    kill -0 "$old" 2>/dev/null && kill -KILL "$old" 2>/dev/null
    i=0
    new=
    while [ "$i" -lt "$WAIT_RESPAWN" ]; do
        new=$(first_pid anyka_ipc)
        [ -n "$new" ] && [ "$new" != "$old" ] && break
        new=
        sleep 1
        i=$((i + 1))
    done
    [ -n "$new" ] || { fail "anyka_ipc did not come back within ${WAIT_RESPAWN}s"; return 1; }
    ok "anyka_ipc respawned, pid $new"
    echo "new_pid=$new" >>"$RUNDIR/run.meta"
    i=0
    while [ "$i" -lt 30 ]; do
        if grep -q stock_hook.so "$ROOT/proc/$new/maps" 2>/dev/null; then break; fi
        sleep 1
        i=$((i + 1))
    done
    if ! grep -q stock_hook.so "$ROOT/proc/$new/maps" 2>/dev/null; then
        fail "the hook is not loaded in anyka_ipc (pid $new); restoring"
        echo "hook_loaded=no" >>"$RUNDIR/run.meta"
        return 1
    fi
    ok "stock_hook.so is loaded in anyka_ipc"
    echo "hook_loaded=yes" >>"$RUNDIR/run.meta"
    echo "Listen now: the camera may speak (factory prompts are un-muted). Capturing for ${secs}s."

    t=0
    snapped=0
    while [ "$t" -lt "$secs" ]; do
        sleep "$TICK"
        t=$((t + TICK))
        if [ "$snapped" = 0 ] && [ "$t" -ge 20 ]; then
            ls -l "$ROOT/proc/$new/fd" >"$RUNDIR/fd_start.txt" 2>&1
            snapped=1
        fi
        sz=$(wc -c <"$RUNDIR/stock_hook.log" 2>/dev/null)
        echo "  t=${t}s stock_hook.log=${sz:-0} bytes, anyka_ipc $(kill -0 "$new" 2>/dev/null && echo alive || echo GONE)"
    done
    ls -l "$ROOT/proc/$new/fd" >"$RUNDIR/fd_end.txt" 2>&1
    cp "$ROOT/proc/$new/maps" "$RUNDIR/maps_end.txt" 2>/dev/null

    # kernel and system side
    dmesg >"$RUNDIR/dmesg.txt" 2>&1
    tail -n 300 "$ROOT/var/log/messages" >"$RUNDIR/messages_tail.txt" 2>/dev/null
    tail -n 300 "$ROOT/etc/config/log_debug" >"$RUNDIR/anyka_debug_tail.txt" 2>/dev/null
    cp "$ROOT/proc/mtd" "$RUNDIR/proc_mtd.txt" 2>/dev/null
    cp "$ROOT/proc/devices" "$RUNDIR/proc_devices.txt" 2>/dev/null
    if [ -r "$ROOT/sys/kernel/debug/gpio" ]; then cp "$ROOT/sys/kernel/debug/gpio" "$RUNDIR/debug_gpio.txt"; fi
    ls -l "$ROOT"/dev/pcm* "$ROOT"/dev/snd "$ROOT"/dev/akpcm* >"$RUNDIR/dev_audio.txt" 2>&1
    cp "$(mounts_file)" "$RUNDIR/mounts_during.txt" 2>/dev/null
    return 0
}

finish() {
    rc=$1
    restore
    if [ -n "$RUNDIR" ] && [ -d "$RUNDIR" ]; then
        cp "$(mounts_file)" "$RUNDIR/mounts_after.txt" 2>/dev/null
        snap_config "$RUNDIR/config_after.txt"
        if diff "$RUNDIR/config_before.txt" "$RUNDIR/config_after.txt" >"$RUNDIR/config_diff.txt" 2>&1; then
            ok "/etc/config unchanged during the capture (flash not written)"
        else
            warn "/etc/config differs before/after (see config_diff.txt); a stock process may have saved a setting"
        fi
        sync
        echo
        echo "Logs are on the card: $RUNDIR"
        ls -l "$RUNDIR" 2>/dev/null
        echo "Power the camera off, take the card, copy the folder $(basename "$SD")/stock_capture/$(basename "$RUNDIR") to the PC."
    fi
    exit "$rc"
}

# ----------------------------------------------------------------------------- main
CMD=
SECS=120
while [ $# -gt 0 ]; do
    case $1 in
        --force) FORCE=1 ;;
        --keep-mute) KEEP_MUTE=1 ;;
        -h | --help | help) sed -n '2,12p' "$0"; exit 0 ;;
        check | run | restore | status) CMD=$1 ;;
        *) if [ "$CMD" = run ]; then SECS=$1; else die "unknown argument: $1" 2; fi ;;
    esac
    shift
done
[ -n "$CMD" ] || { sed -n '2,12p' "$0"; exit 2; }

case $CMD in
    check)
        verify_stock
        check_sd
        if [ "$N_FAIL" -eq 0 ]; then echo "CHECK: OK, nothing was changed"; exit 0; fi
        echo "CHECK: $N_FAIL problem(s)"
        exit 1
        ;;
    status)
        if [ -f "$STATE/active" ]; then echo "a capture state is active (run: restore)"; else echo "no capture state"; fi
        ;;
    restore)
        find_sd
        restore
        ;;
    run)
        do_run "$SECS"
        finish $?
        ;;
esac
