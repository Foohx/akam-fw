#!/bin/sh
# stock_capture.sh - capture how the STOCK vendor app anyka_ipc drives the speaker, the
# GPIOs and the ISP, on a still-stock camera booted from the microSD toolkit (POSIX sh,
# BusyBox 1.30.1: no tr, mktemp, nohup). Run it from the toolkit telnet shell (port 24).
#
#   sh /tmp/sd/stock_capture/stock_capture.sh check             verification, changes nothing on the camera
#   sh /tmp/sd/stock_capture/stock_capture.sh run --detach      capture (default 120 s) in the background,
#                                                               then restore: THE WAY TO RUN IT
#   sh /tmp/sd/stock_capture/stock_capture.sh status | watch    where is the run (watch: every 2 s, Ctrl-C only
#                                                               stops the watching)
#   sh /tmp/sd/stock_capture/stock_capture.sh note TEXT         record an event (with its time) in the run
#   sh /tmp/sd/stock_capture/stock_capture.sh restore           stop a run / undo everything (safe to repeat)
#   sh /tmp/sd/stock_capture/stock_capture.sh run [SECONDS]     the same in the foreground (dies with the telnet)
#   options: --detach (run under busybox setsid, HUP ignored, output in runNN/console.txt)
#            --force (ignore a stock-check mismatch)  --keep-mute
#            --daynight          image / day-night run (default 300 s) with two prompts: cover the lens
#                                at t=60 s, uncover it at t=180 s
#            --phase SEC:TEXT    a prompt of your own at SEC seconds (repeatable; "_" shows as a space)
#            --keep-start-counter  do not reset /tmp/camera_start_times (RAM) before the restart
#            --no-sync           do not fsync the logs on the card once every 2 s
#   In a foreground run on a terminal, a line you type (then Enter) is recorded as a note with its time.
#
# Why --detach: the stock anyka_ipc kills telnetd when it starts (the toolkit's telnet.sh restart loop
# launches it again a few seconds later; it also happens at a normal boot). The telnet session therefore
# drops 5 to 8 s after the hooked anyka_ipc starts, the script gets SIGHUP and its HUP trap restores
# everything: the capture ends (status=signal). The camera does NOT reboot (uptime keeps running). A
# detached run is in its own session, ignores HUP, writes to runNN/console.txt; reconnect telnet and use
# "watch" / "status" / "note".
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
#     (clean respawn without the hook). A power cycle also restores everything. INT and TERM (and
#     HUP in a foreground run: lost telnet) run the restore; "restore" from another shell stops a
#     detached run (it sends TERM to it); after a SIGKILL of the script, run "restore".
#   * If the watchdog does not respawn anyka_ipc, the script gives up after 60 s and restores;
#     the stock watchdog may then reboot the board, which just returns it to the stock state.
#   * If the camera does reboot DURING a run (it did not in the two runs seen so far), nothing has to
#     be undone: the binds, the wrapper and the state live in RAM and are gone. The logs written so far
#     stay on the card, fsync'd every 2 s, and timeline.log has a line per second (uptime, anyka_ipc
#     alive or not, load, free RAM, /tmp/camera_start_times) so the last moment is known. run.meta
#     says "status=running" until the run ends; the next check/run reports it.
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
MIN_FREE_KB=${STOCK_CAPTURE_MIN_FREE_KB:-80000}   # caps: 12 + 12 + 24 MiB of logs + small files
FSYNC_EVERY=${STOCK_CAPTURE_FSYNC:-2}   # seconds between fsyncs of the logs on the card (0 = never)
MAX_SECS=3600
MIN_SECS=${STOCK_CAPTURE_MIN_SECS:-10}
PHASES=
FIRED=
KEEP_COUNTER=0
INTERACTIVE=0
SECS_GIVEN=0
WAIT_RESPAWN=${STOCK_CAPTURE_WAIT:-60}
ACTIVE=0
RESTORED=0
SD=
RUNDIR=
N_FAIL=0
DETACH=0                 # --detach: re-execute under busybox setsid, in the background
DETACHED_CHILD=0         # internal: this process IS the detached run
NOTE_TEXT=
BUSYBOX=${STOCK_CAPTURE_BUSYBOX:-busybox}

ok()   { echo "OK      $*"; }
info() { echo "INFO    $*"; }
warn() { echo "WARN    $*"; }
fail() { N_FAIL=$((N_FAIL + 1)); echo "FAIL    $*"; }
die()  { echo "FAIL    $*" >&2; exit "${2:-1}"; }
have() { command -v "$1" >/dev/null 2>&1; }
# need_cmds NAME...: every applet the script relies on, checked up front (BusyBox 1.30.1 has no
# tr, mktemp, nohup, flock, gzip, wget, dirname or cut and this script uses none of them).
need_cmds() {
    for need in "$@"; do
        have "$need" || { fail "missing command: $need"; return 1; }
    done
    return 0
}

# ----------------------------------------------------------------------------- run record
# meta LINE: append to run.meta (SD card). The status line is the last "status=" one.
meta() { [ -n "$RUNDIR" ] && echo "$*" >>"$RUNDIR/run.meta"; }
# stamp: "up=<uptime of the camera> t=<seconds since the capture began>" (up is the same clock as
# mono_ms of HOOK_START in stock_hook.log, so the parser can line the two up)
stamp() {
    up=
    read -r up _ 2>/dev/null <"$ROOT/proc/uptime"
    echo "up=${up:-0} t=${T_NOW:-0}"
}
# tl EVENT [TEXT...]: one line in timeline.log (SD card)
tl() {
    ev=$1; shift
    [ -n "$RUNDIR" ] && echo "$(stamp) ev=$ev $*" >>"$RUNDIR/timeline.log"
}
# sync_files FILE...: fsync those files of the run (file by file: dd conv=fsync on an empty read;
# the global sync would also flush the flash file systems). A hard reboot then loses ~2 s at most.
FSYNC_FAILS=0
sync_files() {
    [ "$FSYNC_EVERY" -gt 0 ] && [ "$FSYNC_FAILS" -lt 2 ] || return 0
    for f in "$@"; do
        [ -f "$RUNDIR/$f" ] || continue
        dd if=/dev/null of="$RUNDIR/$f" conv=notrunc,fsync >/dev/null 2>&1 || FSYNC_FAILS=$((FSYNC_FAILS + 1))
    done
    if [ "$FSYNC_FAILS" -ge 2 ]; then warn "dd conv=fsync does not work here: logs are no longer fsync'd"; fi
}
# flush_run: fsync every file of the run folder, whatever --no-sync says (a global "sync" would also
# push the flash file systems, which this tool never wants). When dd cannot fsync here, fall back to
# one global sync (the only way to get the card's files out before it is removed) and say so.
flush_run() {
    bad=0
    for f in "$RUNDIR"/*; do
        [ -f "$f" ] || continue
        dd if=/dev/null of="$f" conv=notrunc,fsync >/dev/null 2>&1 || bad=1
    done
    if [ "$bad" = 1 ]; then
        warn "dd conv=fsync failed: using a global sync to flush the card"
        sync
    fi
}
# runs left behind by a camera that rebooted (or a script that was killed) during a capture
scan_interrupted() {
    [ -n "$SD" ] || return 0
    [ -f "$STATE/active" ] && return 0      # a capture is going on in another shell: not an orphan
    for m in "$SD"/stock_capture/run*/run.meta; do
        [ -f "$m" ] || continue
        st=$(sed -n 's/^status=//p' "$m" | tail -n 1)
        [ "$st" = running ] || continue
        d=${m%/run.meta}
        last=$(tail -n 1 "$d/timeline.log" 2>/dev/null)
        warn "${d##*/} never finished (script killed, session lost without --detach, or camera rebooted; see its timeline.log)"
        info "   its logs are complete up to the last fsync; last heartbeat: ${last:-none}"
        echo "status=interrupted" >>"$m"
    done
}

# ----------------------------------------------------------------------------- finding a run
# pid_is_run PID: PID is alive AND is this script (a stale pid file must never make us signal an
# unrelated process whose pid was reused; /proc is the real one, not $ROOT/proc). Not this very
# process either: a "status" that got the recycled pid of a dead run must not call itself a run.
pid_is_run() {
    [ -n "$1" ] && [ "$1" != "$$" ] && kill -0 "$1" 2>/dev/null && grep -q 'stock_capture\.sh' "/proc/$1/cmdline" 2>/dev/null
}
# state_pid: the pid recorded by the run that owns $STATE (empty when none or when it is gone)
state_pid() {
    sp=
    [ -f "$STATE/pid" ] && { read -r sp <"$STATE/pid"; } 2>/dev/null
    pid_is_run "$sp" && echo "$sp"
}
# locate_run: sets RD (run folder) and RNAME. A run in progress is $STATE/out (a link to its folder
# on the card, so no readlink is needed); otherwise the highest runNN on the card.
locate_run() {
    RD=
    RNAME=
    if [ -d "$STATE/out" ]; then
        RD=$STATE/out
        RNAME=$(sed -n 's/^rundir=//p' "$RD/run.meta" 2>/dev/null | tail -n 1)
        return 0
    fi
    [ -n "$SD" ] || find_sd || return 1
    best=-1
    for d in "$SD"/stock_capture/run*; do
        [ -f "$d/run.meta" ] || continue
        n=${d##*/run}
        case $n in '' | *[!0-9]*) continue ;; esac
        while [ "${n#0}" != "$n" ]; do n=${n#0}; done
        n=${n:-0}
        if [ "$n" -gt "$best" ]; then best=$n; RD=$d; fi
    done
    [ -n "$RD" ] || return 1
    RNAME=${RD##*/}
    return 0
}
# read_run: facts about the run in $RD (R_ST status, R_SECS, R_PHASES, last heartbeat R_UP / R_T / R_IPC)
read_run() {
    R_ST=$(sed -n 's/^status=//p' "$RD/run.meta" 2>/dev/null | tail -n 1)
    R_SECS=$(sed -n 's/^seconds=//p' "$RD/run.meta" 2>/dev/null | tail -n 1)
    R_PHASES=$(sed -n 's/^phases=//p' "$RD/run.meta" 2>/dev/null | tail -n 1)
    R_T0=$(sed -n 's/^t0_after_launch=//p' "$RD/run.meta" 2>/dev/null | tail -n 1)
    tick=$(grep 'ev=TICK' "$RD/timeline.log" 2>/dev/null | tail -n 1)
    R_UP=$(echo "$tick" | sed -n 's/^up=\([0-9.]*\) .*/\1/p')
    R_T=$(echo "$tick" | sed -n 's/^up=[0-9.]* t=\([0-9]*\) .*/\1/p')
    R_IPC=$(echo "$tick" | sed -n 's/.* ipc=\([A-Za-z]*\) pid=\([0-9]*\).*/\1 (pid \2)/p')
    R_PID=
    if [ "$RD" = "$STATE/out" ]; then R_PID=$(state_pid); fi
    if [ -n "$R_PID" ]; then
        R_LABEL="RUNNING (script pid $R_PID)"
    elif [ "$RD" = "$STATE/out" ]; then
        R_LABEL="STALE: the script is gone but its state is still mounted (run: restore)"
    else
        case $R_ST in
            running)
                R_LABEL="INTERRUPTED: it did not finish cleanly (script killed, or camera rebooted; see timeline.log)"
                if [ -n "$R_UP" ] && awk -v a="$(up_now)" -v b="$R_UP" 'BEGIN { exit (a < int(b)) ? 0 : 1 }'; then
                    R_LABEL="INTERRUPTED: the camera REBOOTED (its uptime, $(up_now) s, is below the last heartbeat, $R_UP s)"
                fi ;;
            starting) R_LABEL="FAILED TO START (see console.txt)" ;;
            '') R_LABEL="unknown" ;;
            *) R_LABEL=$R_ST ;;
        esac
    fi
}
# up_now: the camera's uptime in whole seconds
up_now() { u=; read -r u _ 2>/dev/null <"$ROOT/proc/uptime"; echo "${u:-0}" | sed 's/\..*//'; }

show_status() {
    echo "Run $RNAME  ($RD)"
    echo "  state     : $R_LABEL"
    if [ -n "$R_T" ]; then
        age=
        if [ -n "$R_PID" ]; then
            age=$(awk -v a="$(up_now)" -v b="$R_UP" 'BEGIN { d = a - b; if (d < 0) d = 0; printf "  (last heartbeat %d s ago)", d }')
        fi
        echo "  elapsed   : t=${R_T} s of ${R_SECS:-?} s$age"
    elif [ -n "$R_PID" ]; then
        echo "  elapsed   : starting (restarting anyka_ipc, loading the hook: t=0 comes soon)"
    fi
    [ -n "$R_T0" ] && echo "  clock     : t=0 came ${R_T0} s after you pressed Enter on run (for a stopwatch)"
    if [ -n "$R_PID" ]; then
        live=$(first_pid anyka_ipc)
        if [ -n "$live" ]; then live="alive, pid $live"; else live="NOT RUNNING"; fi
        echo "  anyka_ipc : $live   (last heartbeat: ${R_IPC:-none})"
    elif [ -n "$R_IPC" ]; then
        echo "  anyka_ipc : last heartbeat ${R_IPC}"
    fi
    if [ -n "$R_PHASES" ] && [ "$R_PHASES" != none ]; then
        echo "  prompts   :"
        for ph in $R_PHASES; do
            sec=${ph%%:*}
            txt=$(echo "${ph#*:}" | sed 's/_/ /g')
            if [ -z "$R_T" ]; then when="not yet"
            elif [ "$R_T" -ge "$sec" ]; then when="DONE (it was ${sec} s, now t=${R_T})"
            else when="DUE in $((sec - R_T)) s"; fi
            echo "      t=${sec}s  $txt  -> $when"
        done
    fi
    echo "  last lines of $RNAME/console.txt:"
    tail -n 5 "$RD/console.txt" 2>/dev/null | sed 's/^/      | /'
    n_notes=$(grep -c 'ev=NOTE' "$RD/timeline.log" 2>/dev/null)
    echo "  notes     : ${n_notes:-0}"
}

do_status() {
    find_sd
    if ! locate_run; then
        echo "no capture found (no run in ${SD:-the card}/stock_capture, none in progress)"
        return 1
    fi
    read_run
    show_status
    return 0
}

# watch: every 2 s a short line, the prompts as banners; ends when the run ends. Ctrl-C stops only
# this command: a detached run is in another session and does not see the keyboard.
do_watch() {
    find_sd
    locate_run || { echo "no capture found (no run in ${SD:-the card}/stock_capture, none in progress)"; return 1; }
    trap 'echo; echo "watch stopped; the run itself is not affected (status / watch to look again)"; exit 0' INT
    seen=$(grep -c 'ev=PHASE' "$RD/timeline.log" 2>/dev/null)
    seen=${seen:-0}
    read_run
    show_status
    [ -n "$R_PID" ] || return 0
    echo
    echo "watching every 2 s (Ctrl-C stops the watching only):"
    while :; do
        sleep 2
        locate_run
        read_run
        np=$(grep -c 'ev=PHASE' "$RD/timeline.log" 2>/dev/null)
        np=${np:-0}
        if [ "$np" -gt "$seen" ]; then
            grep 'ev=PHASE' "$RD/timeline.log" | tail -n $((np - seen)) | sed -n 's/.* ev=PHASE text=\(.*\)$/\1/p' | while read -r l; do
                echo
                echo ">>>>>>>>>> t=${R_T:-?}s: $l <<<<<<<<<<"
                echo
            done
            seen=$np
        fi
        if [ -z "$R_PID" ]; then
            echo
            show_status
            return 0
        fi
        next=
        for ph in $R_PHASES; do
            [ "$ph" = none ] && continue
            sec=${ph%%:*}
            if [ -n "$R_T" ] && [ "$sec" -gt "$R_T" ] && { [ -z "$next" ] || [ "$sec" -lt "$next" ]; }; then next=$sec; fi
        done
        echo "  t=${R_T:-starting}/${R_SECS:-?} s  anyka_ipc ${R_IPC:-?}${next:+  next prompt in $((next - R_T)) s}"
    done
}

# note TEXT: append a timestamped note to the run in progress (from any shell)
do_note() {
    [ -n "$NOTE_TEXT" ] || die "note needs a text: note \"room dark now\"" 2
    pid=$(state_pid)
    { [ -n "$pid" ] && [ -d "$STATE/out" ]; } || die "no capture is running (nothing to attach a note to)" 1
    RUNDIR=$STATE/out
    T_NOW=$(tail -n 1 "$RUNDIR/timeline.log" 2>/dev/null | sed -n 's/^up=[0-9.]* t=\([0-9]*\) .*/\1/p')
    T_NOW=${T_NOW:-0}
    tl NOTE "text=$NOTE_TEXT"
    echo "note_t${T_NOW}=$NOTE_TEXT" >>"$RUNDIR/run.meta"
    echo "note saved at t=${T_NOW}s in $(sed -n 's/^rundir=//p' "$RUNDIR/run.meta" | tail -n 1)"
}

# stop_live: a run in progress (another shell, or detached) is asked to stop with TERM, which makes
# it restore itself, and waited for.
stop_live() {
    pid=$(state_pid)
    [ -n "$pid" ] || return 0
    echo "a capture is running (pid $pid): stopping it (it restores itself)"
    kill -TERM "$pid" 2>/dev/null
    i=0
    while [ "$i" -lt $((WAIT_RESPAWN + 30)) ] && pid_is_run "$pid"; do sleep 1; i=$((i + 1)); done
    if pid_is_run "$pid"; then die "the capture (pid $pid) did not stop within $((WAIT_RESPAWN + 30)) s" 1; fi
    echo "OK      the capture stopped"
}

mounts_file() { echo "$ROOT/proc/mounts"; }
# ram_plain_file FILE: FILE is a regular file (not a symlink) and /tmp is a tmpfs/ramfs mount, so
# writing it cannot reach the flash (/etc/config is jffs2 on the camera)
ram_plain_file() {
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    awk '$2 == "/tmp" { t = $3 } END { exit (t == "tmpfs" || t == "ramfs") ? 0 : 1 }' "$(mounts_file)" 2>/dev/null
}
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
    need_cmds awk pidof grep || return 1
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
    need_cmds awk chmod grep sed cp dd df find md5sum mount umount sleep kill pidof ls rm rmdir ln wc tail date sync || return 1
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

# Returns 1 when a bind we added is still mounted (the caller then keeps the wrapper, the hook
# and the state so that "restore" can be run again).
pop_binds() {
    [ -f "$STATE/state" ] || return 0
    left=0
    # undo in reverse order; read from a file, not a pipe, so that $left survives the loop
    sed -n '1!G;h;$p' "$STATE/state" >"$STATE/state.rev" || return 1
    while read -r kind target before; do
        [ "$kind" = bind ] || continue
        tries=0
        while [ "$(mcount "$target")" -gt "$before" ] && [ "$tries" -lt 3 ]; do
            umount "$ROOT$target" 2>/dev/null || umount -l "$ROOT$target" 2>/dev/null
            tries=$((tries + 1))
        done
        if [ "$(mcount "$target")" -gt "$before" ]; then
            echo "WARN    could not unmount $target (a power cycle clears it)"
            left=1
        fi
    done <"$STATE/state.rev"
    rm -f "$STATE/state.rev"
    return "$left"
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
    need_cmds awk sed mount umount rm kill sleep || { echo "WARN    restore needs those applets; power-cycle the camera instead"; return 1; }
    if ! pop_binds; then
        echo "WARN    some binds are still mounted: nothing else is removed, run restore again or power-cycle"
        RESTORED=0
        return 1
    fi
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
    rm -f "$STATE/out" "$STATE/state" "$STATE/state.rev" "$STATE/muted" "$STATE/anyka_ipc_wrap" "$STATE/stock_hook.so" \
          "$STATE/cfg/_ht_hw_settings.ini" "$STATE/cfg/_ht_sw_settings.ini" "$STATE/active" "$STATE/pid"
    rmdir "$STATE/cfg" "$STATE/app" "$STATE" 2>/dev/null
    ACTIVE=0
}

on_signal() {
    trap '' INT TERM HUP         # ignore a second signal: it must not interrupt the restore
    echo "signal received: restoring"
    meta "status=signal"
    restore
    exit 130
}
on_exit()   { [ "$ACTIVE" = 1 ] && restore; }

# ----------------------------------------------------------------------------- run
# wait_tick: one second. With a terminal, a typed line (Enter) is recorded as a note. busybox ash
# has "read -t"; it returns 1 on a timeout (like on end of input), so an immediate return (the
# clock second did not change) three times in a row means there is no usable terminal after all:
# plain sleep from then on. A quick return is always padded with sleep so a tick stays ~1 s.
QUICK=0
wait_tick() {
    if [ "$INTERACTIVE" != 1 ]; then
        sleep 1
        return
    fi
    note=
    t_a=$(date +%s)
    # shellcheck disable=SC3045
    read -r -t 1 note 2>/dev/null
    rc=$?
    if [ "$rc" -eq 0 ] && [ -n "$note" ]; then
        tl NOTE "text=$note"
        echo "note_t${T_NOW}=$note" >>"$RUNDIR/run.meta"
        echo "  (note saved at t=${T_NOW}s)"
    fi
    if [ "$rc" -ne 0 ] && [ "$(date +%s)" = "$t_a" ]; then
        QUICK=$((QUICK + 1))
        [ "$QUICK" -ge 3 ] && INTERACTIVE=0
        sleep 1
    else
        QUICK=0
    fi
}

check_run_args() {
    secs=$1
    case $secs in '' | *[!0-9]*) die "SECONDS must be a number" 2 ;; esac
    { [ "$secs" -ge "$MIN_SECS" ] && [ "$secs" -le "$MAX_SECS" ]; } || die "SECONDS must be between $MIN_SECS and $MAX_SECS" 2
    for ph in $PHASES; do
        [ "${ph%%:*}" -lt "$secs" ] || die "phase $ph is not before the end of the run (${secs}s)" 2
    done
}

# preflight_run: everything that can be refused, shown on the terminal; ends with the run folder
# created on the card (RUNDIR). Nothing on the camera is changed here (a stale state is cleaned).
preflight_run() {
    if [ -f "$STATE/active" ]; then
        lp=$(state_pid)
        [ -z "$lp" ] || die "a capture is already running (pid $lp): follow it with status or watch, stop it with restore" 2
        info "a previous run left its state behind: restoring it first"
        find_sd
        restore
        RESTORED=0
    fi
    verify_stock || exit 2
    check_sd || exit 2
    scan_interrupted
    n=1
    while :; do
        RUNDIR=$SD/stock_capture/run$(printf '%02d' "$n")
        [ -e "$RUNDIR" ] || break
        n=$((n + 1))
    done
    mkdir -p "$RUNDIR" || die "cannot create $RUNDIR"
    ok "capture folder $RUNDIR"
}

# check_setsid: --detach needs "busybox setsid" (an applet of the camera's BusyBox 1.30.1); say so clearly
# BEFORE anything is created when it is missing
check_setsid() {
    have "$BUSYBOX" || die "$BUSYBOX not found: cannot detach. Run without --detach from a session that does not drop (it ends when telnet drops)." 2
    "$BUSYBOX" setsid "$BUSYBOX" sh -c : >/dev/null 2>&1 || die "'$BUSYBOX setsid' does not work here (applet missing?): cannot detach. Run without --detach from a session that does not drop." 2
}

# spawn_detached ARGS...: run this script again, in its own session (busybox setsid), HUP ignored,
# no terminal (stdin /dev/null), output appended to RUNDIR/console.txt; wait until it reports its pid.
# $@ is the original command line without --detach.
spawn_detached() {
    case $0 in /*) self=$0 ;; *) self=$PWD/$0 ;; esac
    {
        echo "rundir=${RUNDIR##*/}"
        echo "status=starting"
    } >"$RUNDIR/run.meta"
    : >>"$RUNDIR/console.txt"
    trap '' HUP          # inherited by the child (the child also ignores it itself)
    STOCK_CAPTURE_RUNDIR=$RUNDIR STOCK_CAPTURE_MTD_SHA=${MTD_SHA:-unknown} STOCK_CAPTURE_LAUNCH_UP=$(up_now) \
        "$BUSYBOX" setsid "$BUSYBOX" sh "$self" --detached-child "$@" >>"$RUNDIR/console.txt" 2>&1 </dev/null &
    i=0
    pid=
    while [ "$i" -lt 40 ]; do
        pid=$(state_pid)
        [ -n "$pid" ] && break
        sleep 0.2 2>/dev/null || sleep 1
        i=$((i + 1))
    done
    if [ -z "$pid" ]; then
        fail "the detached run did not report within a few seconds; its output so far:"
        tail -n 10 "$RUNDIR/console.txt" 2>/dev/null | sed 's/^/      | /'
        exit 1
    fi
    echo
    ok "capture ${RUNDIR##*/} started in the background (pid $pid); it keeps going if telnet drops"
    echo "The stock anyka_ipc kills telnetd when it restarts: in 10 to 40 s this telnet session will probably"
    echo "drop. That is expected and harmless. Wait a few seconds, telnet in again, and use:"
    echo "    sh $self watch                 follow it (Ctrl-C only stops the watching)"
    echo "    sh $self status                one look: time, prompts due, notes"
    echo "    sh $self note \"room dark\"      record an event with its time"
    echo "    sh $self restore               stop it early (restores everything)"
    echo "Output of the run: $RUNDIR/console.txt (the prompts appear there too)"
    exit 0
}

do_run() {
    check_run_args "$1"
    if [ "$DETACHED_CHILD" = 1 ]; then
        RUNDIR=${STOCK_CAPTURE_RUNDIR:-}
        [ -d "$RUNDIR" ] || die "detached run without its folder"
        find_sd || die "capture kit not found"
        MTD_SHA=${STOCK_CAPTURE_MTD_SHA:-unknown}
        ok "detached run in $RUNDIR (pid $$)"
    else
        preflight_run
    fi
    mkdir -p "$STATE/cfg" "$STATE/app" || die "cannot create $STATE"
    : >"$STATE/state"
    if [ "$DETACHED_CHILD" = 1 ]; then trap on_signal INT TERM; trap '' HUP; else trap on_signal INT TERM HUP; fi
    trap on_exit EXIT
    ACTIVE=1
    echo "$$" >"$STATE/pid"           # pid first: whoever sees "active" also finds the pid
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
            if cp "$STATE/app/local/$f" "$STATE/cfg/$f" && do_bind "$STATE/cfg/$f" "$ROOT/etc/config/$f"; then
                ok "pristine $f of the APP partition mounted over /etc/config/$f (RAM copy)"
            else
                warn "could not mount the pristine $f: the settings in place stay as they are"
            fi
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
    SC=$ROOT/tmp/camera_start_times
    sc_before=$(cat "$SC" 2>/dev/null)
    # appended (>>), never truncated: a detached run's run.meta already has its first lines, and a
    # "note" made in the first seconds must not be lost; the last "status=" line is the status
    {
        echo "script=stock_capture.sh"
        [ "$DETACHED_CHILD" = 1 ] || echo "rundir=${RUNDIR##*/}"
        echo "detached=$DETACHED_CHILD"
        echo "status=running"
        echo "start_times_before=${sc_before:-absent}"
        echo "phases=${PHASES:-none}"
        echo "fsync_every=$FSYNC_EVERY"
        echo "date=$(date 2>/dev/null)"
        echo "uptime=$(awk '{ print $1 }' "$ROOT/proc/uptime" 2>/dev/null)"
        echo "kernel=$(cat "$ROOT/proc/version" 2>/dev/null)"
        echo "mtd7_sha256=${MTD_SHA:-unknown}"
        echo "stock_firmware=yes"
        echo "seconds=$secs"
        echo "keep_mute=$KEEP_MUTE"
        echo "old_pid=$old"
        echo "sd=$SD"
    } >>"$RUNDIR/run.meta"

    # anyka_ipc counts its starts in /tmp/camera_start_times (RAM; the stock service.sh writes 0 there
    # at every boot) and its start-up code can run "sh recover_cfg.sh; reboot -f". That was a suspect for
    # the first run's "reboot"; it is DISPROVED for that symptom (the camera did not reboot: the telnet
    # session was lost, see "Why --detach"). The reset is kept because it is harmless (RAM only, plain
    # file on tmpfs, the value a fresh boot has) and it keeps a restart of anyka_ipc from counting
    # towards whatever limit the stock app has. The value is recorded (run.meta, every timeline line).
    if [ "$KEEP_COUNTER" = 0 ] && [ -n "$sc_before" ] && ! ram_plain_file "$SC"; then
        warn "/tmp/camera_start_times is not a plain file on a RAM file system: NOT touched (flash must never be written)"
        echo "start_times_reset=refused" >>"$RUNDIR/run.meta"
    elif [ "$KEEP_COUNTER" = 0 ] && [ -n "$sc_before" ]; then
        echo 0 >"$SC" 2>/dev/null && info "start counter /tmp/camera_start_times was $sc_before, reset to 0 (RAM only; --keep-start-counter skips this)"
        echo "start_times_reset=yes" >>"$RUNDIR/run.meta"
    else
        info "start counter /tmp/camera_start_times: ${sc_before:-absent}, left as it is"
    fi
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
    echo "start_times_after_respawn=$(cat "$SC" 2>/dev/null)" >>"$RUNDIR/run.meta"
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
    if [ -n "$PHASES" ]; then
        echo "Prompts that will appear (then do what they say, nothing else):"
        for ph in $PHASES; do echo "    t=${ph%%:*}s  $(echo "${ph#*:}" | sed 's/_/ /g')"; done
    fi
    if [ -t 0 ]; then
        INTERACTIVE=1
        echo "You can type a note and press Enter at any time; it is saved with its time (e.g. \"room dark now\")."
    fi
    if [ "$DETACHED_CHILD" = 1 ]; then
        echo "Detached run: the prompts appear here and in timeline.log; follow it with 'watch' / 'status', add events with 'note'."
    else
        echo "Foreground run: if this telnet session drops (the stock anyka_ipc kills telnetd when it restarts),"
        echo "the run is stopped and restored. Use 'run --detach' to avoid that."
    fi
    echo "If the camera itself reboots during the run, nothing needs undoing; logs on the card are fsync'd every ${FSYNC_EVERY}s."
    echo

    T_NOW=0
    if [ -n "${STOCK_CAPTURE_LAUNCH_UP:-}" ]; then
        t0gap=$(( $(up_now) - STOCK_CAPTURE_LAUNCH_UP ))
        echo "t0_after_launch=$t0gap" >>"$RUNDIR/run.meta"
        echo "Capture clock t=0 starts now, $t0gap s after run was launched (a stopwatch started at Enter reads $t0gap s at t=0)."
    fi
    tl START "pid=$new hook=yes"
    t=0
    snapped=0
    cur=$new
    while [ "$t" -lt "$secs" ]; do
        wait_tick
        t=$((t + 1))
        T_NOW=$t
        # one heartbeat per second: this is what tells the last moment alive after a reboot
        mem=
        while read -r k v _; do [ "$k" = MemFree: ] && { mem=$v; break; }; done 2>/dev/null <"$ROOT/proc/meminfo"
        read -r load _ 2>/dev/null <"$ROOT/proc/loadavg"
        st=
        read -r st 2>/dev/null <"$SC"
        if kill -0 "$cur" 2>/dev/null; then alive_s=alive; else alive_s=GONE; fi
        tl TICK "ipc=$alive_s pid=$cur load=${load:-?} memfree_kb=${mem:-?} start_times=${st:-absent}"
        for ph in $PHASES; do
            case " $FIRED " in *" $ph "*) continue ;; esac
            if [ "$t" -ge "${ph%%:*}" ]; then
                FIRED="$FIRED $ph"
                txt=$(echo "${ph#*:}" | sed 's/_/ /g')
                echo
                echo ">>>>>>>>>> t=${t}s: $txt <<<<<<<<<<"
                echo
                tl PHASE "text=$txt"
                echo "phase_${ph%%:*}=$txt (really at t=$t)" >>"$RUNDIR/run.meta"
            fi
        done
        if [ $((t % 5)) -eq 0 ]; then       # a restart of anyka_ipc in the middle of a run = it crashed
            now=$(first_pid anyka_ipc)
            if [ -n "$now" ] && [ "$now" != "$cur" ]; then
                warn "anyka_ipc restarted: pid $cur -> $now (it crashed or was killed); the hook keeps capturing"
                tl IPC_RESTART "old=$cur new=$now"
                cur=$now
                alive_s=alive
            fi
        fi
        if [ "$FSYNC_EVERY" -gt 0 ] && [ $((t % FSYNC_EVERY)) -eq 0 ]; then
            sync_files stock_hook.log isp_hook.log isp_blocks.bin timeline.log
        fi
        if [ $((t % 10)) -eq 0 ]; then     # small rolling snapshots, overwritten, kept after a reboot
            dmesg 2>/dev/null | tail -n 60 >"$RUNDIR/dmesg_live.txt"
            tail -n 30 "$ROOT/etc/config/log_debug" >"$RUNDIR/anyka_debug_live.txt" 2>/dev/null
            # shellcheck disable=SC2009
            if have ps; then { echo "== $(stamp)"; ps 2>/dev/null | grep -v '\['; } >>"$RUNDIR/ps_live.txt"; fi
            sync_files run.meta console.txt dmesg_live.txt anyka_debug_live.txt ps_live.txt
        fi
        if [ "$snapped" = 0 ] && [ "$t" -ge 20 ]; then
            ls -l "$ROOT/proc/$cur/fd" >"$RUNDIR/fd_start.txt" 2>&1
            snapped=1
        fi
        if [ $((t % TICK)) -eq 0 ]; then
            sz=$(wc -c <"$RUNDIR/stock_hook.log" 2>/dev/null)
            echo "  t=${t}s stock_hook.log=${sz:-0} bytes, anyka_ipc $alive_s (pid $cur), start_times=${st:-absent}, free=${mem:-?} kB"
        fi
    done
    tl END "t=$t"
    new=$cur
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
    meta "status=finished rc=$rc"
    if [ -n "$RUNDIR" ] && [ -d "$RUNDIR" ]; then
        cp "$(mounts_file)" "$RUNDIR/mounts_after.txt" 2>/dev/null
        snap_config "$RUNDIR/config_after.txt"
        if diff "$RUNDIR/config_before.txt" "$RUNDIR/config_after.txt" >"$RUNDIR/config_diff.txt" 2>&1; then
            ok "/etc/config unchanged during the capture (flash not written)"
        else
            warn "/etc/config differs before/after (see config_diff.txt); a stock process may have saved a setting"
        fi
        flush_run
        echo
        echo "Logs are on the card: $RUNDIR"
        ls -l "$RUNDIR" 2>/dev/null
        echo "Power the camera off, take the card, copy the folder stock_capture/${RUNDIR##*/} to the PC."
    fi
    exit "$rc"
}

# ----------------------------------------------------------------------------- main
CMD=
SECS=120

usage() { sed -n '2,/^# Why --detach/{/^# Why --detach/!p}' "$0"; }

parse_args() {
    while [ $# -gt 0 ]; do
        if [ "$CMD" = note ]; then      # everything after "note" is the text
            NOTE_TEXT="${NOTE_TEXT:+$NOTE_TEXT }$1"
            shift
            continue
        fi
        case $1 in
            --force) FORCE=1 ;;
            --keep-mute) KEEP_MUTE=1 ;;
            --keep-start-counter) KEEP_COUNTER=1 ;;
            --no-sync) FSYNC_EVERY=0 ;;
            --detach) DETACH=1 ;;
            --detached-child) DETACHED_CHILD=1 ;;
            --daynight) PHASES="$PHASES 60:COVER_THE_LENS_NOW_(and_keep_the_room_dark) 180:UNCOVER_THE_LENS_NOW" ;;
            --phase)
                [ $# -ge 2 ] || die "--phase needs SEC:TEXT" 2
                shift
                case $1 in [0-9]*:?*) ;; *) die "--phase needs SEC:TEXT, got: $1" 2 ;; esac
                case ${1%%:*} in *[!0-9]*) die "--phase needs SEC:TEXT, got: $1" 2 ;; esac
                PHASES="$PHASES $(echo "$1" | sed 's/[[:space:]][[:space:]]*/_/g')"
                ;;
            -h | --help | help) usage; exit 0 ;;
            check | run | restore | stop | status | watch | note) CMD=$1 ;;
            *) if [ "$CMD" = run ]; then SECS=$1; SECS_GIVEN=1; else die "unknown argument: $1" 2; fi ;;
        esac
        shift
    done
}
parse_args "$@"
[ -n "$CMD" ] || { usage; exit 2; }
# a day/night run needs the time to cover and uncover the lens
case $PHASES in *COVER_THE_LENS_NOW*) [ "$SECS_GIVEN" = 1 ] || SECS=300 ;; esac
[ "$DETACH" = 0 ] || [ "$CMD" = run ] || die "--detach only goes with run" 2
# restore / stop and the start of a run kill and respawn anyka_ipc, which makes the stock app kill
# telnetd: the session that typed the command is hung up a few seconds later and must not take the
# restore down with it (a half-done restore leaves a stale state, and repeating it would hang up
# again). A foreground run replaces this by its own trap once it has started; watch keeps default HUP.
case $CMD in restore | stop | run) trap '' HUP ;; esac

case $CMD in
    check)
        verify_stock
        check_sd && scan_interrupted
        if [ -f "$STATE/active" ]; then
            if [ -n "$(state_pid)" ]; then
                info "a capture is running right now (pid $(state_pid)): follow it with status / watch"
            else
                warn "stale state of a capture that did not finish (the script is gone): run 'restore' (the next run does it too)"
            fi
        fi
        if [ "$N_FAIL" -eq 0 ]; then echo "CHECK: OK, nothing was changed"; exit 0; fi
        echo "CHECK: $N_FAIL problem(s)"
        exit 1
        ;;
    status)
        do_status
        exit $?
        ;;
    watch)
        do_watch
        exit $?
        ;;
    note)
        do_note
        ;;
    restore | stop)
        find_sd
        stop_live
        restore
        ;;
    run)
        if [ "$DETACH" = 1 ] && [ "$DETACHED_CHILD" = 0 ]; then
            check_run_args "$SECS"
            check_setsid
            preflight_run
            # the original command line without --detach (the child gets --detached-child in front)
            n=$#
            while [ "$n" -gt 0 ]; do
                a=$1
                shift
                [ "$a" = --detach ] || set -- "$@" "$a"
                n=$((n - 1))
            done
            spawn_detached "$@"
        fi
        do_run "$SECS"
        finish $?
        ;;
esac
