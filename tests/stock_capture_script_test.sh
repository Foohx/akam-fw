#!/usr/bin/env bash
# Host test of tools/hw_tests/stock_capture.sh against a fake camera root ($AKAM_ROOT) with
# stub mount/umount/pidof and a fake watchdog that respawns "anyka_ipc". The script runs under
# the pinned BusyBox ash with a PATH made only of applets the camera's BusyBox 1.30.1 has
# (no tr, mktemp, nohup). Checks: stock verification, wrapper/LD_PRELOAD set-up, only
# anyka_ipc is killed (the watchdog survives), config binds and mutes are popped, nothing under
# /etc/config changes, signal handling, running twice. Touches no hardware.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
SCRIPT=$REPO/tools/hw_tests/stock_capture.sh

if [ -n "${AKAM_BUSYBOX:-}" ]; then SHCMD=("$AKAM_BUSYBOX" sh)
elif command -v busybox >/dev/null 2>&1; then SHCMD=(busybox sh)
else echo "skip: no BusyBox (run tools/build/bootstrap.sh)"; exit 0; fi
BB=${AKAM_BUSYBOX:-$(command -v busybox)}

WORK=$(mktemp -d)
FAKEPIDS=()
cleanup() {
    for p in "${FAKEPIDS[@]}"; do kill "$p" 2>/dev/null; done
    pkill -f "$WORK/respawn.sh" 2>/dev/null
    [ -s "$WORK/root/tmp/ipc.pid" ] && kill "$(cat "$WORK/root/tmp/ipc.pid")" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }
has() { if grep -Eq -- "$2" "$3"; then ok "$1"; else ko "$1 (no match: $2)"; sed 's/^/      | /' "$3" | head -n 40; fi; }
hasnt() { if grep -Eq -- "$2" "$3"; then ko "$1 (unexpected: $2)"; else ok "$1"; fi; }

# --- PATH: only what the camera has ------------------------------------------------------
BIN=$WORK/bin
mkdir -p "$BIN"
# (no cut, dirname, basename, tr, mktemp, nohup, flock, gzip, wget: the script must not need them)
for a in awk cat chmod cp date df diff dd find grep ln ls md5sum mkdir rm rmdir sed sha256sum \
         sleep sync tail wc; do
    ln -s "$BB" "$BIN/$a"
done
# host tools for the stubs and the fake watchdog only (not on the script's PATH)
HOSTBIN=/usr/bin:/bin

R=$WORK/root
STATE=$R/tmp/stock_capture
mkdir -p "$R/usr/bin" "$R/usr/share" "$R/usr/local" "$R/etc/config" "$R/proc" "$R/dev" "$R/var/log" \
         "$R/tmp/sd/stock_capture" "$R/tmp/sd/custom/audio" "$R/tmp/stubstack"
printf 'REAL-ANYKA\n' >"$R/usr/bin/anyka_ipc"; chmod 755 "$R/usr/bin/anyka_ipc"
printf 'ding\n' >"$R/usr/share/boot.mp3"
printf 'dong\n' >"$R/usr/share/other.mp3"
printf 'PRISTINE-HW\n' >"$R/usr/local/_ht_hw_settings.ini"
printf 'PRISTINE-SW\n' >"$R/usr/local/_ht_sw_settings.ini"
printf 'TOOLKIT-HW\n' >"$R/etc/config/_ht_hw_settings.ini"
printf 'TOOLKIT-SW\n' >"$R/etc/config/_ht_sw_settings.ini"
printf 'keep\n' >"$R/etc/config/keep.txt"
printf 'silent\n' >"$R/tmp/sd/custom/audio/silent.mp3"
printf 'hook\n' >"$R/tmp/sd/stock_capture/stock_hook.so"
( cd "$R/tmp/sd/stock_capture" && sha256sum stock_hook.so >SHA256SUMS )
printf 'mtd7: 00500000 00001000 "APP"\n' >"$R/proc/mtd"
printf 'fake-app-partition' >"$R/dev/mtdblock7"
SHA=$(sha256sum "$R/dev/mtdblock7" | cut -d' ' -f1)
echo "12.34 5.67" >"$R/proc/uptime"
printf '0.52 0.40 0.30 1/80 1234\n' >"$R/proc/loadavg"
printf 'MemTotal:       22000 kB\nMemFree:         9000 kB\n' >"$R/proc/meminfo"
echo 3 >"$R/tmp/camera_start_times"
cat >"$R/proc/mounts" <<'EOF'
/dev/mtdblock7 /usr squashfs ro 0 0
tmpfs /tmp tmpfs rw 0 0
/dev/mmcblk0p1 /tmp/sd vfat rw 0 0
/dev/mmcblk0p1 /etc/config/_ht_sw_settings.ini vfat rw 0 0
/dev/mmcblk0p1 /usr/share/boot.mp3 vfat rw 0 0
EOF
cp "$R/proc/mounts" "$WORK/mounts.initial"

# --- stubs ----------------------------------------------------------------------------------
cat >"$BIN/mount" <<'EOF'
#!/bin/sh
# stub: mount --bind SRC DST on the fake root (copies content, keeps a stack to undo it)
PATH=/usr/bin:/bin
[ "$1" = "--bind" ] || exit 1
src=$2; dst=$3; lp=${dst#"$AKAM_ROOT"}
echo "mount $lp" >>"$AKAM_ROOT/tmp/mount.log"
key=$(echo "$lp" | sed 's,/,_,g')
if [ -d "$src" ]; then
    cp -a "$src"/. "$dst"/ || exit 1
else
    n=$(ls "$AKAM_ROOT/tmp/stubstack" | grep -c "^$key\.")
    cp -p "$dst" "$AKAM_ROOT/tmp/stubstack/$key.$n"
    cp "$src" "$dst"; touch -r "$AKAM_ROOT/tmp/stubstack/$key.$n" "$dst"
fi
echo "/dev/mmcblk0p1 $lp vfat rw 0 0" >>"$AKAM_ROOT/proc/mounts"
EOF
cat >"$BIN/umount" <<'EOF'
#!/bin/sh
PATH=/usr/bin:/bin
[ "$1" = "-l" ] && shift
dst=$1; lp=${dst#"$AKAM_ROOT"}
[ -e "$AKAM_ROOT/tmp/umount.fail" ] && [ "$lp" = /usr/bin/anyka_ipc ] && exit 1
echo "umount $lp" >>"$AKAM_ROOT/tmp/mount.log"
key=$(echo "$lp" | sed 's,/,_,g')
grep -v "^[^ ]* $lp " "$AKAM_ROOT/proc/mounts" >"$AKAM_ROOT/proc/mounts.new" || true
last=$(grep -n "^[^ ]* $lp " "$AKAM_ROOT/proc/mounts" | tail -n 1 | cut -d: -f1)
[ -n "$last" ] || exit 1
awk -v n="$last" 'NR != n' "$AKAM_ROOT/proc/mounts" >"$AKAM_ROOT/proc/mounts.new"
mv "$AKAM_ROOT/proc/mounts.new" "$AKAM_ROOT/proc/mounts"
if [ -d "$dst" ]; then rm -rf "$dst"/*; mkdir -p "$dst"; exit 0; fi
top=$(ls "$AKAM_ROOT/tmp/stubstack" | grep "^$key\." | sort | tail -n 1)
if [ -n "$top" ]; then
    cp "$AKAM_ROOT/tmp/stubstack/$top" "$dst"; touch -r "$AKAM_ROOT/tmp/stubstack/$top" "$dst"
    rm -f "$AKAM_ROOT/tmp/stubstack/$top"
fi
exit 0
EOF
cat >"$BIN/pidof" <<'EOF'
#!/bin/sh
case $1 in
    anyka_ipc) cat "$AKAM_ROOT/tmp/ipc.pid" 2>/dev/null ;;
    watchdog)  cat "$AKAM_ROOT/tmp/wd.pid" ;;
    *) exit 1 ;;
esac
EOF
cat >"$BIN/dmesg" <<'EOF'
#!/bin/sh
echo "[    1.0] ak_pcm: SPEAKER @69(lv 1)"
EOF
# the camera's "busybox": the script calls "busybox setsid" and "busybox sh"; this stub logs the call
# and runs the pinned test BusyBox (the camera's BusyBox 1.30.1 has the setsid applet)
cat >"$BIN/busybox" <<EOF
#!/bin/sh
echo "\$*" >>"$WORK/busybox.log"
exec "$BB" "\$@"
EOF
chmod +x "$BIN/mount" "$BIN/umount" "$BIN/pidof" "$BIN/dmesg" "$BIN/busybox"

# fake watchdog: a process that must survive, plus a respawner for anyka_ipc
PATH=$HOSTBIN sleep 600 >/dev/null 2>&1 & WDPID=$!
FAKEPIDS+=("$WDPID")
echo "$WDPID" >"$R/tmp/wd.pid"
cat >"$WORK/respawn.sh" <<'EOF'
#!/bin/sh
# the "watchdog": respawn anyka_ipc when it dies; with the wrapper in place the new process
# "has the hook" (its maps show stock_hook.so and it writes a first log line).
PATH=/usr/bin:/bin
R=$AKAM_ROOT
while :; do
    pid=$(cat "$R/tmp/ipc.pid" 2>/dev/null)
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        sleep 600 >/dev/null 2>&1 & new=$!
        echo "$new" >"$R/tmp/ipc.pid"
        mkdir -p "$R/proc/$new"
        if head -n 1 "$R/usr/bin/anyka_ipc" | grep -q '^#!/bin/sh'; then
            cp "$R/usr/bin/anyka_ipc" "$R/tmp/wrapper.seen"
            ld=$(grep '^LD_PRELOAD=' "$R/usr/bin/anyka_ipc" | cut -d= -f2)
            echo "00000000-00001000 r-xp 00000000 00:00 0 $ld" >"$R/proc/$new/maps"
            echo "T=0.000 tid=$new HOOK_START version=1 arch=arm comm=anyka_ipc pid=$new wall=1" >>"$R/tmp/stock_capture/out/stock_hook.log"
        else
            echo "00000000-00001000 r-xp 00000000 00:00 0 /lib/libc.so.0" >"$R/proc/$new/maps"
        fi
        mkdir -p "$R/proc/$new/fd"
    fi
    sleep 1
done
EOF
chmod +x "$WORK/respawn.sh"
env PATH=$HOSTBIN AKAM_ROOT="$R" "$WORK/respawn.sh" &
FAKEPIDS+=($!)
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$R/tmp/ipc.pid" ] && break; sleep 0.3; done
IPC0=$(cat "$R/tmp/ipc.pid")

XENV=()    # extra environment for runscript (name=value ...)
runscript() { # args -> script args; output in $WORK/out.txt
    env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 "${XENV[@]+${XENV[@]}}" \
        STOCK_CAPTURE_WAIT=15 STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" "$@" >"$WORK/out.txt" 2>&1
}

cfg_sum() { ( cd "$R/etc/config" && sha256sum _ht_hw_settings.ini _ht_sw_settings.ini keep.txt | sort ); }
cfg_sum >"$WORK/cfg.initial"

# --- check -------------------------------------------------------------------------------------
runscript check; rc=$?
if [ "$rc" -eq 0 ]; then ok "check passes on a stock camera"; else ko "check exit $rc"; cat "$WORK/out.txt"; fi
has "check verifies the APP image hash" 'OK      mtd7 \(APP\) sha256 matches' "$WORK/out.txt"
has "check says nothing was changed" 'CHECK: OK, nothing was changed' "$WORK/out.txt"
has "check leaves the watchdog alone (wording)" 'watchdog running \(left alone' "$WORK/out.txt"
if [ ! -e "$STATE" ]; then ok "check creates no state"; else ko "check created $STATE"; fi

STOCK_SHA_SAVE=$SHA; SHA=0000
runscript check; rc=$?
if [ "$rc" -ne 0 ]; then ok "a different APP hash fails the check"; else ko "hash mismatch accepted"; fi
runscript --force check; rc=$?
if [ "$rc" -eq 0 ]; then ok "--force accepts it with a warning"; else ko "--force exit $rc"; fi
SHA=$STOCK_SHA_SAVE

touch "$R/usr/bin/ak_rtsp"
runscript check; rc=$?
if [ "$rc" -ne 0 ]; then ok "an Akam camera is refused"; else ko "ak_rtsp present but check passed"; fi
rm -f "$R/usr/bin/ak_rtsp"

# --- full run ------------------------------------------------------------------------------------
: >"$R/tmp/mount.log"
runscript run 2; rc=$?
if [ "$rc" -eq 0 ]; then ok "run exits 0"; else ko "run exit $rc"; fi
has "stock verified" 'OK      mtd7 \(APP\) sha256 matches' "$WORK/out.txt"
has "pristine hw settings stacked" 'pristine _ht_hw_settings.ini of the APP partition mounted over /etc/config/_ht_hw_settings.ini' "$WORK/out.txt"
has "pristine sw settings stacked" 'pristine _ht_sw_settings.ini' "$WORK/out.txt"
has "the toolkit's mute is lifted" 'un-muted /usr/share/boot.mp3' "$WORK/out.txt"
has "wrapper bound, watchdog untouched" 'wrapper bound over /usr/bin/anyka_ipc \(the watchdog is untouched\)' "$WORK/out.txt"
has "only anyka_ipc killed and respawned" 'anyka_ipc respawned, pid [0-9]+' "$WORK/out.txt"
has "hook loaded check" 'stock_hook.so is loaded in anyka_ipc' "$WORK/out.txt"
has "restore re-mutes" 're-muted /usr/share/boot.mp3' "$WORK/out.txt"
has "restore respawns without the hook" 'respawned by the watchdog without the hook' "$WORK/out.txt"
has "the start counter is reported and reset" 'start counter /tmp/camera_start_times was 3, reset to 0' "$WORK/out.txt"
has "a foreground run says what to do when telnet drops" 'Use .run --detach. to avoid that' "$WORK/out.txt"
has "progress line shows the heartbeat data" 't=2s stock_hook.log=[0-9]+ bytes, anyka_ipc alive \(pid [0-9]+\), start_times=0, free=9000 kB' "$WORK/out.txt"
has "config unchanged verdict" '/etc/config unchanged during the capture' "$WORK/out.txt"
RUN=$R/tmp/sd/stock_capture/run01
if [ -d "$RUN" ]; then ok "capture folder run01 created on the card"; else ko "no run01"; fi
has "wrapper preloads the RAM copy of the hook" '^LD_PRELOAD=/tmp/stock_capture/stock_hook.so$' "$R/tmp/wrapper.seen"
has "wrapper execs the real binary through the /usr bind" '^exec /tmp/stock_capture/app/bin/anyka_ipc "\$@"$' "$R/tmp/wrapper.seen"
has "run.meta records the hook" 'hook_loaded=yes' "$RUN/run.meta"
has "run.meta records the stock hash" "mtd7_sha256=$SHA" "$RUN/run.meta"
has "run.meta status is finished at the end" '^status=finished rc=0$' "$RUN/run.meta"
has "run.meta says the run was in progress first" '^status=running$' "$RUN/run.meta"
has "run.meta: start counter before the restart" '^start_times_before=3$' "$RUN/run.meta"
has "run.meta: start counter reset" '^start_times_reset=yes$' "$RUN/run.meta"
has "run.meta: fsync interval" '^fsync_every=2$' "$RUN/run.meta"
if [ "$(cat "$R/tmp/camera_start_times")" = 0 ]; then ok "RAM start counter file reset to 0"; else ko "start counter file: $(cat "$R/tmp/camera_start_times")"; fi
has "timeline: a start line" 'up=12.34 t=0 ev=START pid=[0-9]+ hook=yes' "$RUN/timeline.log"
has "timeline: one heartbeat per second with uptime, load, free RAM and the start counter" 'up=12.34 t=1 ev=TICK ipc=alive pid=[0-9]+ load=0.52 memfree_kb=9000 start_times=0' "$RUN/timeline.log"
has "timeline: an end line" 'ev=END t=2' "$RUN/timeline.log"
has "the fsync helper (dd conv=notrunc) did not truncate the hook log" 'HOOK_START' "$RUN/stock_hook.log"
if [ -s "$RUN/dmesg_live.txt" ] || [ "$(wc -l <"$RUN/timeline.log")" -ge 3 ]; then ok "rolling snapshots or timeline present"; else ko "no live data"; fi
if [ -s "$RUN/stock_hook.log" ]; then ok "log written through the card symlink"; else ko "no log in run01"; fi
if [ -s "$RUN/dmesg.txt" ]; then ok "dmesg saved"; else ko "no dmesg"; fi
if [ -f "$RUN/fd_end.txt" ]; then ok "descriptor listing saved"; else ko "no fd_end.txt"; fi
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table identical to the one before the run"; else ko "mount table differs after restore"; diff "$WORK/mounts.initial" "$R/proc/mounts"; fi
cfg_sum >"$WORK/cfg.after"
if diff "$WORK/cfg.initial" "$WORK/cfg.after" >/dev/null; then ok "files under /etc/config are byte-identical afterwards"; else ko "/etc/config content changed"; fi
if kill -0 "$WDPID" 2>/dev/null; then ok "the watchdog process is still alive"; else ko "the watchdog was killed"; fi
IPC1=$(cat "$R/tmp/ipc.pid")
if [ "$IPC1" != "$IPC0" ]; then ok "anyka_ipc was restarted (pid $IPC0 -> $IPC1)"; else ko "anyka_ipc not restarted"; fi
if ls "$STATE" >/dev/null 2>&1; then ko "state dir left behind: $(ls "$STATE")"; else ok "no state left in /tmp/stock_capture"; fi
if grep -q 'umount /usr/bin/anyka_ipc' "$R/tmp/mount.log"; then ok "wrapper unmounted"; else ko "wrapper not unmounted"; fi
if head -n 1 "$R/usr/bin/anyka_ipc" | grep -q REAL-ANYKA; then ok "the real anyka_ipc is visible again"; else ko "wrapper content still on /usr/bin/anyka_ipc"; fi

# --- second run (safe to repeat) -------------------------------------------------------------------
echo 5 >"$R/tmp/camera_start_times"
runscript run 5 --phase 2:COVER_THE_LENS --phase 4:uncover --keep-start-counter --no-sync; rc=$?
if [ "$rc" -eq 0 ]; then ok "second run (with phases) exits 0"; else ko "second run exit $rc"; cat "$WORK/out.txt"; fi
if [ -d "$R/tmp/sd/stock_capture/run02" ]; then ok "second run gets its own folder run02"; else ko "no run02"; fi
RUN2=$R/tmp/sd/stock_capture/run02
has "the prompts are announced before the capture" 't=2s  COVER THE LENS' "$WORK/out.txt"
has "a prompt is shown at its time" '>>>>>>>>>> t=2s: COVER THE LENS <<<<<<<<<<' "$WORK/out.txt"
has "the second prompt too" '>>>>>>>>>> t=4s: uncover <<<<<<<<<<' "$WORK/out.txt"
has "timeline records the prompt with the uptime" 'up=12.34 t=2 ev=PHASE text=COVER THE LENS' "$RUN2/timeline.log"
has "run.meta records the prompts" '^phase_4=uncover \(really at t=4\)$' "$RUN2/run.meta"
has "--keep-start-counter: no reset" '^start_times_before=5$' "$RUN2/run.meta"
hasnt "--keep-start-counter: no reset line in run.meta" 'start_times_reset' "$RUN2/run.meta"
if [ "$(cat "$R/tmp/camera_start_times")" = 5 ]; then ok "--keep-start-counter leaves the file alone"; else ko "counter changed: $(cat "$R/tmp/camera_start_times")"; fi
has "--no-sync is recorded" '^fsync_every=0$' "$RUN2/run.meta"
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table identical after the second run"; else ko "mount table differs after run 2"; fi

# --- argument checks ---------------------------------------------------------------------------------
runscript run 6 --daynight; rc=$?
if [ "$rc" -eq 2 ]; then ok "a prompt after the end of the run is refused"; else ko "late prompt accepted (exit $rc)"; fi
has "...with a clear message" 'phase 60:COVER.* is not before the end of the run \(6s\)' "$WORK/out.txt"
runscript run 99999; rc=$?
if [ "$rc" -eq 2 ]; then ok "an absurd duration is refused"; else ko "99999 s accepted"; fi
runscript run 5 --phase abc; rc=$?
if [ "$rc" -eq 2 ]; then ok "a malformed --phase is refused"; else ko "--phase abc accepted"; fi
if [ ! -d "$R/tmp/sd/stock_capture/run03" ]; then ok "refused runs create no folder"; else ko "a refused run made run03"; fi

# --- a day/night run: default 300 s, prompts at 60 s and 180 s listed up front; then SIGTERM -----------------
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" run --daynight >"$WORK/dn.txt" 2>&1 &
SP=$!
for _ in $(seq 1 40); do grep -q 'Capturing for' "$WORK/dn.txt" 2>/dev/null && break; sleep 0.5; done
has "--daynight defaults to 300 s" 'Capturing for 300s' "$WORK/dn.txt"
has "--daynight announces the cover prompt at 60 s" 't=60s  COVER THE LENS NOW' "$WORK/dn.txt"
has "--daynight announces the uncover prompt at 180 s" 't=180s  UNCOVER THE LENS NOW' "$WORK/dn.txt"
kill -TERM "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
has "SIGTERM ends a day/night run with a restore" 'signal received: restoring' "$WORK/dn.txt"
RUN3=$R/tmp/sd/stock_capture/run03
has "a signal is recorded in run.meta" '^status=signal$' "$RUN3/run.meta"

# --- the camera reboots DURING a run: simulated by SIGKILL of the script (no restore, no status) ------------
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/kill.txt" 2>&1 &
SP=$!
for _ in $(seq 1 40); do grep -q 't=3s' "$WORK/kill.txt" 2>/dev/null && break; sleep 0.5; done
kill -KILL "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
RUN4=$R/tmp/sd/stock_capture/run04
has "after the kill the card has the heartbeat up to the last second" 'ev=TICK ipc=alive' "$RUN4/timeline.log"
has "after the kill run.meta still says running" '^status=running$' "$RUN4/run.meta"
if [ -f "$STATE/active" ]; then ok "RAM state is still there (a real reboot would have cleared it)"; else ko "no state after SIGKILL"; fi
runscript restore; rc=$?
if [ "$rc" -eq 0 ]; then ok "restore after the kill exits 0"; else ko "restore after the kill exit $rc"; fi
runscript check; rc=$?
if [ "$rc" -eq 0 ]; then ok "check still passes"; else ko "check exit $rc"; fi
has "the interrupted run is reported by the next check" 'run04 never finished' "$WORK/out.txt"
has "...with its last heartbeat" 'last heartbeat: up=12.34 t=[0-9]+ ev=TICK' "$WORK/out.txt"
has "...and marked in run.meta" '^status=interrupted$' "$RUN4/run.meta"
runscript check; rc=$?
hasnt "an interrupted run is reported once" 'run04 never finished' "$WORK/out.txt"
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table restored after the kill"; else ko "mounts not restored after the kill"; fi

# --- a typed note is saved with its time (needs a terminal: python3 pty) ---------------------------------------------
if command -v python3 >/dev/null 2>&1; then
    python3 -I - "$BB" "$SCRIPT" "$BIN" "$R" "$SHA" >"$WORK/pty.txt" 2>&1 <<'PYEOF'
import os, pty, sys, time, select
bb, script, binp, root, sha = sys.argv[1:6]
env = {"PATH": binp, "AKAM_ROOT": root, "STOCK_CAPTURE_SHA": sha, "STOCK_CAPTURE_TICK": "1",
       "STOCK_CAPTURE_WAIT": "15", "STOCK_CAPTURE_MIN_FREE_KB": "1", "STOCK_CAPTURE_MIN_SECS": "1"}
pid, fd = pty.fork()
if pid == 0:
    os.execve(bb, [bb, "sh", script, "run", "8"], env)
out = b""
sent = False
deadline = time.time() + 60
while time.time() < deadline:
    r, _, _ = select.select([fd], [], [], 0.5)
    if r:
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        out += d
        if not sent and b"Capturing for" in out:
            time.sleep(1.2)
            os.write(fd, b"room is dark now\n")
            sent = True
sys.stdout.write(out.decode("utf-8", "replace"))
PYEOF
    has "a terminal run offers notes" 'type a note and press Enter' "$WORK/pty.txt"
    has "a typed note is acknowledged" 'note saved at t=[0-9]+s' "$WORK/pty.txt"
    LASTRUN=$(find "$R/tmp/sd/stock_capture" -maxdepth 1 -name "run*" -type d | sort | tail -n 1)
    has "the note is in the timeline with its time" 'up=12.34 t=[0-9]+ ev=NOTE text=room is dark now' "$LASTRUN/timeline.log"
    has "...and in run.meta" '^note_t[0-9]+=room is dark now$' "$LASTRUN/run.meta"
    has "the run still ends normally after a note" '^status=finished rc=0$' "$LASTRUN/run.meta"
fi

# --- restore on a signal ----------------------------------------------------------------------------
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/sig.txt" 2>&1 &
SP=$!
for _ in $(seq 1 40); do grep -q 'Capturing for' "$WORK/sig.txt" 2>/dev/null && break; sleep 0.5; done
kill -TERM "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
has "SIGTERM triggers the restore" 'signal received: restoring' "$WORK/sig.txt"
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table restored after a signal"; else ko "mounts not restored after a signal"; fi
if kill -0 "$WDPID" 2>/dev/null; then ok "watchdog still alive after the signal"; else ko "watchdog killed"; fi

# a wrapper that cannot be unmounted: nothing else is removed, a second restore finishes the job
touch "$R/tmp/umount.fail"
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/sig2.txt" 2>&1 &
SP=$!
for _ in $(seq 1 40); do grep -q 'Capturing for' "$WORK/sig2.txt" 2>/dev/null && break; sleep 0.5; done
kill -TERM "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
has "a stuck bind is reported" 'could not unmount /usr/bin/anyka_ipc' "$WORK/sig2.txt"
if [ -f "$STATE/anyka_ipc_wrap" ] && [ -f "$STATE/stock_hook.so" ] && [ -f "$STATE/state" ]; then ok "wrapper, hook and state are kept while a bind is still mounted"; else ko "state removed although the wrapper is still bound"; fi
rm -f "$R/tmp/umount.fail"
runscript restore; rc=$?
if [ "$rc" -eq 0 ]; then ok "restore run again exits 0"; else ko "second restore exit $rc"; fi
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table restored by the second restore"; else ko "mounts not restored by the second restore"; fi
if ls "$STATE" >/dev/null 2>&1; then ko "state left after the second restore"; else ok "state removed by the second restore"; fi

# explicit restore with nothing to do
runscript restore; rc=$?
if [ "$rc" -eq 0 ]; then ok "restore with nothing to undo exits 0"; else ko "restore exit $rc"; fi
has "restore reports nothing to do" 'nothing to restore' "$WORK/out.txt"

# the start counter is only ever reset when it is a plain file on a RAM file system: a symlink
# (which could lead into /etc/config, flash) is left alone
printf 'precious\n' >"$R/etc/config/flash_file.txt"
rm -f "$R/tmp/camera_start_times"
ln -s "$R/etc/config/flash_file.txt" "$R/tmp/camera_start_times"
runscript run 3; rc=$?
if [ "$rc" -eq 0 ]; then ok "run with a symlinked start counter exits 0"; else ko "symlink run exit $rc"; cat "$WORK/out.txt"; fi
has "a symlinked start counter is refused" 'NOT touched \(flash must never be written\)' "$WORK/out.txt"
if [ "$(cat "$R/etc/config/flash_file.txt")" = precious ]; then ok "the file behind the symlink was not written"; else ko "symlink target overwritten"; fi
rm -f "$R/tmp/camera_start_times" "$R/etc/config/flash_file.txt"
echo 3 >"$R/tmp/camera_start_times"
cp "$R/proc/mounts" "$WORK/mounts.save"
grep -v ' /tmp tmpfs ' "$WORK/mounts.save" >"$R/proc/mounts"
runscript run 3; rc=$?
has "/tmp not being a RAM file system refuses the reset" 'NOT touched \(flash must never be written\)' "$WORK/out.txt"
if [ "$(cat "$R/tmp/camera_start_times")" = 3 ]; then ok "the counter is untouched when /tmp is not tmpfs"; else ko "counter changed on a non-tmpfs /tmp"; fi
cp "$WORK/mounts.save" "$R/proc/mounts"
# the end-of-run flush never needs a global sync
LASTRUN=$(find "$R/tmp/sd/stock_capture" -maxdepth 1 -name "run*" -type d | sort | tail -n 1)
if [ -s "$LASTRUN/stock_hook.log" ]; then ok "the run folder is complete after the final per-file flush"; else ko "empty log after the final flush"; fi

# --- detached runs (--detach, the documented way to run it) ----------------------------------------------
# The stock anyka_ipc kills telnetd when it starts, so the telnet session (and a foreground script) dies
# a few seconds later. A detached run lives in its own session, ignores HUP, writes runNN/console.txt.
wait_for() { # FILE REGEX [HALF-SECONDS]
    for _ in $(seq 1 "${3:-60}"); do grep -Eq -- "$2" "$1" 2>/dev/null && return 0; sleep 0.5; done
    return 1
}
newest_run() { find "$R/tmp/sd/stock_capture" -maxdepth 1 -name "run*" -type d | sort | tail -n 1; }
wait_state_gone() { for _ in $(seq 1 80); do [ -e "$STATE/active" ] || return 0; sleep 0.5; done; return 1; }
mounts_ok() { diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; }

runscript note "nothing runs"; rc=$?
if [ "$rc" -eq 1 ]; then ok "note without a run in progress is refused"; else ko "note without a run: exit $rc"; fi
has "...with a clear message" 'no capture is running' "$WORK/out.txt"
runscript note; rc=$?
if [ "$rc" -eq 2 ]; then ok "note without a text is refused"; else ko "empty note: exit $rc"; fi

# no usable busybox setsid: a clear error, nothing created, nothing mounted
NRUNS=$(find "$R/tmp/sd/stock_capture" -maxdepth 1 -name "run*" -type d | wc -l)
XENV=(STOCK_CAPTURE_BUSYBOX=/nonexistent/busybox)
runscript run 5 --detach; rc=$?
XENV=()
if [ "$rc" -eq 2 ]; then ok "--detach without busybox setsid is refused (exit 2)"; else ko "--detach without busybox: exit $rc"; fi
has "...with a clear error" 'cannot detach' "$WORK/out.txt"
if [ "$(find "$R/tmp/sd/stock_capture" -maxdepth 1 -name "run*" -type d | wc -l)" = "$NRUNS" ]; then ok "...before any run folder is created"; else ko "a folder was created"; fi
if [ ! -e "$STATE" ] && mounts_ok; then ok "...and nothing mounted"; else ko "state or mounts left after the refused --detach"; fi
# busybox is there but has no setsid applet
printf '#!/bin/sh\nexit 127\n' >"$WORK/busybox_nosetsid"; chmod +x "$WORK/busybox_nosetsid"
XENV=(STOCK_CAPTURE_BUSYBOX="$WORK/busybox_nosetsid")
runscript run 5 --detach; rc=$?
XENV=()
if [ "$rc" -eq 2 ]; then ok "a busybox without the setsid applet is refused (exit 2)"; else ko "no-setsid busybox: exit $rc"; fi
has "...saying setsid does not work" 'setsid. does not work here' "$WORK/out.txt"
runscript status --detach; rc=$?
if [ "$rc" -eq 2 ]; then ok "--detach only goes with run"; else ko "status --detach: exit $rc"; fi

# the detached run itself: 14 s, a prompt at 8 s
: >"$WORK/busybox.log"
T_A=$(date +%s)
runscript run 14 --detach --phase 8:COVER_NOW --keep-start-counter; rc=$?
T_B=$(date +%s)
if [ "$rc" -eq 0 ]; then ok "run --detach returns 0"; else ko "run --detach exit $rc"; cat "$WORK/out.txt"; fi
if [ $((T_B - T_A)) -le 6 ]; then ok "...at once (before the capture is over)"; else ko "run --detach took $((T_B - T_A)) s"; fi
has "it says the run is in the background" 'started in the background' "$WORK/out.txt"
has "it says how to follow it" 'stock_capture.sh watch' "$WORK/out.txt"
has "it says how to note an event" 'stock_capture.sh note' "$WORK/out.txt"
has "it says where the console is" 'console.txt' "$WORK/out.txt"
has "it warns that telnet will probably drop" 'telnet session will probably' "$WORK/out.txt"
has "the script re-executed itself under busybox setsid" '^setsid .*sh .*stock_capture.sh --detached-child run 14' "$WORK/busybox.log"
hasnt "the detach flag is not passed on" 'setsid .* --detach( |$)' "$WORK/busybox.log"
DRUN=$(newest_run)
CPID=$(cat "$STATE/pid" 2>/dev/null)
if [ -n "$CPID" ] && kill -0 "$CPID" 2>/dev/null; then ok "the detached run is alive after run --detach returned (pid $CPID)"; else ko "no live detached run"; fi
if [ "$(awk '{ print $6 }' "/proc/$CPID/stat" 2>/dev/null)" = "$CPID" ]; then ok "it is the leader of its own session (setsid)"; else ko "not a session leader"; fi
if [ "$(readlink "/proc/$CPID/fd/0" 2>/dev/null)" = /dev/null ]; then ok "its stdin is /dev/null"; else ko "stdin: $(readlink "/proc/$CPID/fd/0")"; fi
if [ "$(readlink "/proc/$CPID/fd/1" 2>/dev/null)" = "$DRUN/console.txt" ]; then ok "its output goes to runNN/console.txt"; else ko "stdout: $(readlink "/proc/$CPID/fd/1")"; fi
has "run.meta says detached" '^detached=1$' "$DRUN/run.meta"
has "run.meta names its folder" "^rundir=${DRUN##*/}\$" "$DRUN/run.meta"
wait_for "$DRUN/timeline.log" 'ev=TICK'
runscript status; rc=$?
if [ "$rc" -eq 0 ]; then ok "status exits 0"; else ko "status exit $rc"; fi
has "status says it is running" 'state     : RUNNING \(script pid [0-9]+\)' "$WORK/out.txt"
has "status shows the elapsed time" 'elapsed   : t=[0-9]+ s of 14 s' "$WORK/out.txt"
has "status shows anyka_ipc" 'anyka_ipc : alive, pid [0-9]+' "$WORK/out.txt"
has "status lists the prompt and when it is due" 't=8s  COVER NOW  -> (DUE in [0-9]+ s|DONE)' "$WORK/out.txt"
has "status shows the console tail" "last lines of ${DRUN##*/}/console.txt" "$WORK/out.txt"
has "status says how the stopwatch relates to t" 'clock     : t=0 came [0-9]+ s after you pressed Enter' "$WORK/out.txt"
runscript check; rc=$?
has "check says a capture is running" 'a capture is running right now' "$WORK/out.txt"
runscript run 5 --detach; rc=$?
if [ "$rc" -eq 2 ]; then ok "a second run is refused while one is going"; else ko "second run exit $rc"; fi
has "...and says how to follow or stop it" 'already running \(pid [0-9]+\).*status or watch.*restore' "$WORK/out.txt"
runscript note lens is "covered" now; rc=$?
if [ "$rc" -eq 0 ]; then ok "note from another shell exits 0"; else ko "note exit $rc"; fi
has "note acknowledges with the time" 'note saved at t=[0-9]+s in run' "$WORK/out.txt"
has "the note is in the timeline with its time" 'up=12.34 t=[0-9]+ ev=NOTE text=lens is covered now' "$DRUN/timeline.log"
has "...and in run.meta" '^note_t[0-9]+=lens is covered now$' "$DRUN/run.meta"
# the HUP of a lost telnet session must not touch it
kill -HUP "$CPID" 2>/dev/null
sleep 1.5
if kill -0 "$CPID" 2>/dev/null; then ok "SIGHUP is ignored by the detached run"; else ko "the detached run died of SIGHUP"; fi
hasnt "...and it did not start restoring" 'signal received' "$DRUN/console.txt"
# a stray Ctrl-C (SIGINT) cannot reach it from a terminal; the shell also starts background jobs with
# SIGINT ignored, so it stays up. TERM (what "restore" sends) is the way to stop it, tested below.
kill -INT "$CPID" 2>/dev/null
sleep 1
if kill -0 "$CPID" 2>/dev/null; then ok "a stray SIGINT does not kill the detached run"; else ko "the detached run died of SIGINT"; fi
# watch: follows the run to its end, shows the prompt as a banner
timeout 90 env -i PATH="$BIN" AKAM_ROOT="$R" "${SHCMD[@]}" "$SCRIPT" watch >"$WORK/watch.txt" 2>&1; rc=$?
if [ "$rc" -eq 0 ]; then ok "watch ends with the run (exit 0)"; else ko "watch exit $rc"; fi
has "watch prints short progress lines" '^  t=[0-9]+/14 s  anyka_ipc alive' "$WORK/watch.txt"
has "watch shows the prompt as a banner" '>>>>>>>>>> t=[0-9]+s: COVER NOW <<<<<<<<<<' "$WORK/watch.txt"
has "watch ends with the final state" 'state     : finished rc=0' "$WORK/watch.txt"
has "the run finished normally although the session was 'hung up'" '^status=finished rc=0$' "$DRUN/run.meta"
has "console.txt has the prompt" '>>>>>>>>>> t=8s: COVER NOW <<<<<<<<<<' "$DRUN/console.txt"
has "console.txt has the restore" 'respawned by the watchdog without the hook' "$DRUN/console.txt"
has "console.txt says it is a detached run" 'Detached run: the prompts appear here' "$DRUN/console.txt"
has "run.meta records the gap between Enter and t=0" '^t0_after_launch=[0-9]+$' "$DRUN/run.meta"
has "--keep-start-counter passed on to the detached run" '^start_times_before=' "$DRUN/run.meta"
hasnt "...no reset" 'start_times_reset' "$DRUN/run.meta"
if mounts_ok && [ ! -e "$STATE" ]; then ok "mounts and state restored after the detached run"; else ko "detached run left mounts or state"; fi
runscript status; rc=$?
has "status after the end says finished" 'state     : finished rc=0' "$WORK/out.txt"
runscript note too late; rc=$?
if [ "$rc" -eq 1 ]; then ok "note after the end is refused"; else ko "late note exit $rc"; fi
if [ -f "$DRUN/console.txt" ] && [ -s "$DRUN/console.txt" ]; then ok "console.txt is on the card"; else ko "no console.txt"; fi

# restore from another shell stops a detached run; Ctrl-C of watch does not touch it
if command -v python3 >/dev/null 2>&1; then
    runscript run 60 --detach; rc=$?
    DRUN2=$(newest_run)
    wait_for "$DRUN2/timeline.log" 'ev=TICK'
    python3 -I - "$BB" "$SCRIPT" "$BIN" "$R" >"$WORK/ctrlc.txt" 2>&1 <<'PYEOF'
import os, signal, subprocess, sys, time
bb, script, binp, root = sys.argv[1:5]
env = {"PATH": binp, "AKAM_ROOT": root}
p = subprocess.Popen([bb, "sh", script, "watch"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                     preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
time.sleep(3.5)
p.send_signal(signal.SIGINT)
try:
    out, _ = p.communicate(timeout=20)
except subprocess.TimeoutExpired:
    p.kill()
    out, _ = p.communicate()
sys.stdout.write(out.decode("utf-8", "replace"))
print("RC=%s" % p.returncode)
PYEOF
    has "Ctrl-C of watch says the run is not affected" 'watch stopped; the run itself is not affected' "$WORK/ctrlc.txt"
    has "...and watch exits 0" '^RC=0$' "$WORK/ctrlc.txt"
    CPID2=$(cat "$STATE/pid" 2>/dev/null)
    if [ -n "$CPID2" ] && kill -0 "$CPID2" 2>/dev/null; then ok "the detached run survived the Ctrl-C of watch"; else ko "the run died with the watch"; fi
    runscript restore; rc=$?
    if [ "$rc" -eq 0 ]; then ok "restore from another shell exits 0"; else ko "restore exit $rc"; fi
    has "restore says it stops the running capture" 'a capture is running \(pid [0-9]+\): stopping it' "$WORK/out.txt"
    has "...and that it stopped" 'OK      the capture stopped' "$WORK/out.txt"
    has "the stopped run recorded the signal" '^status=signal$' "$DRUN2/run.meta"
    has "...and restored in its console" 'signal received: restoring' "$DRUN2/console.txt"
    if mounts_ok && [ ! -e "$STATE" ]; then ok "mounts and state restored after a restore from another shell"; else ko "restore from another shell left mounts or state"; fi
    if kill -0 "$WDPID" 2>/dev/null; then ok "the watchdog survived the stopped detached run"; else ko "watchdog killed"; fi
fi

# a detached run that is killed hard (SIGKILL) leaves a stale state: check and status say so
runscript run 60 --detach; rc=$?
DRUN3=$(newest_run)
wait_for "$DRUN3/timeline.log" 'ev=TICK'
CPID3=$(cat "$STATE/pid" 2>/dev/null)
kill -KILL "$CPID3" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$CPID3" 2>/dev/null || break; sleep 0.2; done
runscript check; rc=$?
has "check detects the stale state of a dead run" 'stale state of a capture that did not finish' "$WORK/out.txt"
runscript status
has "status says STALE" 'state     : STALE' "$WORK/out.txt"
runscript note anything; rc=$?
if [ "$rc" -eq 1 ]; then ok "note on a dead run is refused"; else ko "note on a dead run: exit $rc"; fi
runscript restore; rc=$?
if [ "$rc" -eq 0 ]; then ok "restore after the SIGKILL of a detached run exits 0"; else ko "restore exit $rc"; fi
if mounts_ok && [ ! -e "$STATE" ]; then ok "mounts and state restored after the SIGKILL"; else ko "SIGKILL left mounts or state"; fi
runscript status
has "status then says INTERRUPTED" 'state     : INTERRUPTED' "$WORK/out.txt"

# a lost telnet session in a FOREGROUND run: HUP still restores (that is the failure --detach avoids)
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 STOCK_CAPTURE_MIN_SECS=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/hup.txt" 2>&1 &
SP=$!
wait_for "$WORK/hup.txt" 'Capturing for'
kill -HUP "$SP" 2>/dev/null
wait "$SP" 2>/dev/null
has "a foreground run restores on SIGHUP (the lost telnet)" 'signal received: restoring' "$WORK/hup.txt"
has "...and records status=signal" '^status=signal$' "$(newest_run)/run.meta"
if mounts_ok; then ok "mounts restored after SIGHUP"; else ko "mounts left after SIGHUP"; fi

# --- only camera applets used ---------------------------------------------------------------------------
if grep -v '^ *#' "$SCRIPT" | grep -Eq '(^|[|;&( ])(tr|mktemp|nohup|flock|gzip|wget|dirname|basename|cut) '; then ko "script uses an applet the camera's BusyBox lacks"; else ok "script uses none of tr, mktemp, nohup, flock, gzip, wget, dirname, cut"; fi

echo
echo "stock_capture_script_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
