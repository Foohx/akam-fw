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
for a in awk cat cp date df diff dd find grep ln ls md5sum mkdir rm rmdir sed sha256sum \
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
cat >"$R/proc/mounts" <<'EOF'
/dev/mtdblock7 /usr squashfs ro 0 0
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
chmod +x "$BIN/mount" "$BIN/umount" "$BIN/pidof" "$BIN/dmesg"

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

runscript() { # args -> script args; output in $WORK/out.txt
    env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 \
        STOCK_CAPTURE_WAIT=15 STOCK_CAPTURE_MIN_FREE_KB=1 "${SHCMD[@]}" "$SCRIPT" "$@" >"$WORK/out.txt" 2>&1
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
has "config unchanged verdict" '/etc/config unchanged during the capture' "$WORK/out.txt"
RUN=$R/tmp/sd/stock_capture/run01
if [ -d "$RUN" ]; then ok "capture folder run01 created on the card"; else ko "no run01"; fi
has "wrapper preloads the RAM copy of the hook" '^LD_PRELOAD=/tmp/stock_capture/stock_hook.so$' "$R/tmp/wrapper.seen"
has "wrapper execs the real binary through the /usr bind" '^exec /tmp/stock_capture/app/bin/anyka_ipc "\$@"$' "$R/tmp/wrapper.seen"
has "run.meta records the hook" 'hook_loaded=yes' "$RUN/run.meta"
has "run.meta records the stock hash" "mtd7_sha256=$SHA" "$RUN/run.meta"
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
runscript run 2; rc=$?
if [ "$rc" -eq 0 ]; then ok "second run exits 0"; else ko "second run exit $rc"; cat "$WORK/out.txt"; fi
if [ -d "$R/tmp/sd/stock_capture/run02" ]; then ok "second run gets its own folder run02"; else ko "no run02"; fi
if diff "$WORK/mounts.initial" "$R/proc/mounts" >/dev/null; then ok "mount table identical after the second run"; else ko "mount table differs after run 2"; fi

# --- restore on a signal ----------------------------------------------------------------------------
env -i PATH="$BIN" AKAM_ROOT="$R" STOCK_CAPTURE_SHA="$SHA" STOCK_CAPTURE_TICK=1 STOCK_CAPTURE_WAIT=15 \
    STOCK_CAPTURE_MIN_FREE_KB=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/sig.txt" 2>&1 &
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
    STOCK_CAPTURE_MIN_FREE_KB=1 "${SHCMD[@]}" "$SCRIPT" run 60 >"$WORK/sig2.txt" 2>&1 &
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

# --- only camera applets used ---------------------------------------------------------------------------
if grep -v '^ *#' "$SCRIPT" | grep -Eq '(^|[|;&( ])(tr|mktemp|nohup|flock|gzip|wget|dirname|basename|cut) '; then ko "script uses an applet the camera's BusyBox lacks"; else ok "script uses none of tr, mktemp, nohup, flock, gzip, wget, dirname, cut"; fi

echo
echo "stock_capture_script_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
