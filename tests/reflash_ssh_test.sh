#!/usr/bin/env bash
# Host-side tests for the no-UART reflash over SSH:
#   tools/flash_scripts/flash_guard.sh      akam_flash_guard_detached decisions
#   tools/flash_scripts/akam_reflash_ssh.sh  (camera side, run under BusyBox sh)
#   tools/flash_scripts/reflash_over_ssh.sh   (host helper, with a fake ssh that
#                                              runs the "camera" commands locally)
# Everything is fake: the "device" is a regular file, flash_tool and dropbear are
# shell stubs, /tmp is a directory under a scratch dir. Nothing touches hardware,
# a block device or the network.
# Camera-side scripts run under $SH_UNDER_TEST (default: the bootstrap BusyBox
# 1.35.0, else "$AKAM_BUSYBOX sh", else sh).
# shellcheck disable=SC2015  # ok/ko never fail, so "A && ok || ko" is safe
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
FS=$REPO/tools/flash_scripts
TCBB=$REPO/tools/.toolchain/busybox-1.35.0-x86_64/busybox

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif [ -x "$TCBB" ]; then SH_UNDER_TEST="$TCBB sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"
BB=${SHCMD[0]}
if [ "${#SHCMD[@]}" -lt 2 ]; then
    echo "SKIP: a BusyBox is required (run tools/build/bootstrap.sh)"; exit 0
fi

WORK=$(mktemp -d)
cleanup() {
    if [ -r "$WORK/pids" ]; then
        while read -r p; do kill "$p" 2>/dev/null; done <"$WORK/pids"
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }
has() { if grep -Eq -- "$2" "$3"; then ok "$1"; else ko "$1 (no match: $2)"; sed 's/^/      | /' "$3" | tail -n 25; fi; }
hasnt() { if grep -Eq -- "$2" "$3"; then ko "$1 (unexpected: $2)"; else ok "$1"; fi; }
rc_is() { if [ "$2" -eq "$3" ]; then ok "$1"; else ko "$1 (rc=$2, wanted $3)"; fi; }
rc_not0() { if [ "$2" -ne 0 ]; then ok "$1"; else ko "$1 (rc=0)"; fi; }

# =============================================================== A. guard
RAM=$WORK/ram
mkdir -p "$RAM"
RAMP=$(cd "$RAM" && pwd -P)
printf '/dev/mtdblock7 /usr squashfs ro 0 0\ntmpfs %s tmpfs rw 0 0\n' "$RAMP" >"$WORK/mounts_ok"
printf '/dev/mtdblock7 /usr squashfs ro 0 0\n/dev/root %s jffs2 rw 0 0\n' "$RAMP" >"$WORK/mounts_notram"
printf '/dev/mtdblock7 /usr squashfs ro 0 0\n' >"$WORK/mounts_nomount"
touch "$RAMP/script.sh" "$RAMP/tool" "$RAMP/image"
ln -s /usr/bin/env "$RAMP/tool_link"

# Driver: fake proc entries for this shell ($$), optionally a dropbear ancestor
# and a session id different from the pid.
cat >"$WORK/gdriver.sh" <<'DRV'
. "$FS/flash_guard.sh"
mkdir -p "$AKAM_PROC_DIR/$$"
sid=${FAKE_SID:-$$}
echo "$$ (${FAKE_COMM:-sh}) S 800 $$ $sid 0" >"$AKAM_PROC_DIR/$$/stat"
if [ -n "${FAKE_ANCESTOR:-}" ]; then
    mkdir -p "$AKAM_PROC_DIR/900" "$AKAM_PROC_DIR/800"
    echo "800 (my shell) S 900 1 1 0" >"$AKAM_PROC_DIR/800/stat"
    echo "900 ($FAKE_ANCESTOR) S 1 1 1 0" >"$AKAM_PROC_DIR/900/stat"
fi
akam_flash_guard_detached "$1" "$2" "$3" "${4:-}"
DRV

# grun NAME WANT(0|1) MOUNTS SCRIPT TOOL IMAGE CONFIRM [VAR=VAL...]
grun() {
    local name=$1 want=$2 mounts=$3 script=$4 tool=$5 image=$6 conf=$7
    shift 7
    rm -rf "$WORK/gproc"
    mkdir -p "$WORK/gproc"
    env -u SSH_CONNECTION -u SSH_CLIENT -u SSH_TTY -u AKAM_FLASH_I_KNOW \
        FS="$FS" AKAM_PROC_MOUNTS="$WORK/$mounts" AKAM_PROC_DIR="$WORK/gproc" \
        AKAM_FLASH_TMPDIR="$RAMP" "$@" \
        "${SHCMD[@]}" "$WORK/gdriver.sh" "$script" "$tool" "$image" "$conf" >"$WORK/gout" 2>&1
    local rc=$?
    if [ "$want" = 0 ] && [ $rc -eq 0 ]; then ok "guard: $name"
    elif [ "$want" = 1 ] && [ $rc -ne 0 ]; then ok "guard: $name"
    else ko "guard: $name (rc=$rc)"; sed 's/^/      | /' "$WORK/gout"; fi
}
C=--i-understand-no-uart
S=$RAMP/script.sh T=$RAMP/tool I=$RAMP/image
grun "RAM tools, confirmed, detached: allowed" 0 mounts_ok "$S" "$T" "$I" "$C"
grun "no confirmation: refused" 1 mounts_ok "$S" "$T" "$I" ""
grep -q "missing --i-understand-no-uart" "$WORK/gout" && ok "guard: refusal names the confirmation" || ko "guard: refusal names the confirmation"
grun "wrong confirmation: refused" 1 mounts_ok "$S" "$T" "$I" "--yes"
grun "AKAM_FLASH_I_KNOW does not replace the confirmation" 1 mounts_ok "$S" "$T" "$I" "" AKAM_FLASH_I_KNOW=1
grun "tool under /usr: refused" 1 mounts_ok "$S" /usr/bin/flash_tool "$I" "$C"
grun "script under /usr: refused" 1 mounts_ok /usr/bin/x.sh "$T" "$I" "$C"
grun "image outside RAM (SD): refused" 1 mounts_ok "$S" "$T" /mnt/image "$C"
grun "tool symlink into /usr: refused" 1 mounts_ok "$S" "$RAMP/tool_link" "$I" "$C"
grun "RAM dir not a tmpfs mount: refused" 1 mounts_notram "$S" "$T" "$I" "$C"
grun "RAM dir not a mount point at all: refused" 1 mounts_nomount "$S" "$T" "$I" "$C"
grun "SSH_CONNECTION still set: refused" 1 mounts_ok "$S" "$T" "$I" "$C" SSH_CONNECTION="1.2.3.4 5 6.7.8.9 22"
grun "SSH_CLIENT still set: refused" 1 mounts_ok "$S" "$T" "$I" "$C" SSH_CLIENT="1.2.3.4 5 22"
grun "SSH_TTY still set: refused" 1 mounts_ok "$S" "$T" "$I" "$C" SSH_TTY=/dev/pts/0
grun "dropbear ancestor: refused" 1 mounts_ok "$S" "$T" "$I" "$C" FAKE_ANCESTOR=dropbear
grun "non-dropbear ancestor: allowed" 0 mounts_ok "$S" "$T" "$I" "$C" FAKE_ANCESTOR=init
grun "not a session leader (no setsid): refused" 1 mounts_ok "$S" "$T" "$I" "$C" FAKE_SID=777
grep -q "session leader" "$WORK/gout" && ok "guard: refusal names setsid" || ko "guard: refusal names setsid"
# /proc/PID/stat parsing with spaces and parentheses in the command name
grun "comm with spaces and parens, leader: allowed" 0 mounts_ok "$S" "$T" "$I" "$C" FAKE_COMM='a) (b c) d'
grun "comm with spaces and parens, not leader: refused" 1 mounts_ok "$S" "$T" "$I" "$C" FAKE_COMM='a) (b c) d' FAKE_SID=777
grun "ancestor named 'dropbear-x': refused" 1 mounts_ok "$S" "$T" "$I" "$C" FAKE_ANCESTOR=dropbear-x
grun "ancestor 'my dropbear' (does not start with it): allowed" 0 mounts_ok "$S" "$T" "$I" "$C" FAKE_ANCESTOR='my dropbear'

# =============================================================== B. camera side
CAM=$WORK/cam
PB=$CAM/pathbin
mkdir -p "$PB" "$CAM/usr/bin" "$CAM/etc/config/dropbear" "$CAM/home"
ln -s "$BB" "$PB/busybox"
ln -s "$BB" "$PB/sh"
# Stubs that would record any attempt to reboot the camera.
for a in reboot poweroff halt; do
    # shellcheck disable=SC2016  # $0 must expand in the stub, not here
    printf '#!/bin/sh\necho "$0" >>"%s/rebooted"\n' "$CAM" >"$PB/$a"
    chmod +x "$PB/$a"
done
CRAM=$CAM/ram
mkdir -p "$CRAM"
CRAMP=$(cd "$CRAM" && pwd -P)
RD=$CRAMP/akam_reflash
printf '/dev/mtdblock7 /usr squashfs ro 0 0\ntmpfs %s tmpfs rw 0 0\n' "$CRAMP" >"$CAM/mounts"
printf 'dev: size erasesize name\nmtd6: 00200000 00010000 "x"\nmtd7: 00500000 00010000 "APP"\n' >"$CAM/mtd"
printf 'dev: size erasesize name\nmtd7: 00100000 00010000 "OTHER"\n' >"$CAM/mtd_bad"
cat >"$PB/df" <<EOF
#!/bin/sh
echo "Filesystem           1K-blocks      Used Available Use% Mounted on"
echo "tmpfs                    30000       100 \${FAKE_DF_FREE:-21900}   1% /tmp"
EOF
chmod +x "$PB/df"

# Fake dropbear (a launcher script that "daemonizes": leaves a sleeper whose pid is
# in the -P file) and fake flash_tool.
cat >"$CAM/usr/bin/dropbear" <<EOF
#!/bin/sh
echo "argv0=\$0 \$*" >>"$CAM/db_args"
pf=
while [ \$# -gt 0 ]; do
    if [ "\$1" = -P ]; then pf=\$2; fi
    shift
done
sleep 600 </dev/null >/dev/null 2>&1 &
echo \$! >"\$pf"
echo \$! >>"$WORK/pids"
EOF
chmod +x "$CAM/usr/bin/dropbear"
cat >"$WORK/flash_tool" <<EOF
#!/bin/sh
# fake flash_tool IMAGE DEV; behaviour from $CAM/tool_mode (ok|corrupt|failonce|slow)
img=\$1; dev=\$2
mode=\$(cat "$CAM/tool_mode" 2>/dev/null)
echo "SSH env: [\${SSH_CONNECTION:-}\${SSH_CLIENT:-}\${SSH_TTY:-}]" >>"$CAM/tool_info"
st=\$(cat /proc/\$PPID/stat)
st=\${st##*) }
set -- \$st
echo "parent=\$PPID parent_sid=\$4 argv=\$*" >>"$CAM/tool_info"
echo "stdin=\$(readlink /proc/\$\$/fd/0)" >>"$CAM/tool_info"
case "\$mode" in
    slow) sleep 3 ;;
esac
if [ "\$mode" = failonce ] && [ ! -e "$CAM/failed_once" ]; then
    : >"$CAM/failed_once"
    echo "fake flash_tool: simulated failure"
    exit 1
fi
cat "\$img" >"\$dev"
if [ "\$mode" = corrupt ]; then printf 'XXXXXXXX' | dd of="\$dev" bs=1 seek=100 conv=notrunc 2>/dev/null; fi
echo "fake flash_tool: wrote"
exit 0
EOF
chmod +x "$WORK/flash_tool"
head -c 5242880 /dev/urandom >"$WORK/image.squashfs"
IMD5=$(md5sum "$WORK/image.squashfs" | cut -d' ' -f1)
head -c 1000 /dev/urandom >"$WORK/small.img"

# cam_env: variables that make the camera scripts use the fakes.
cam_env() {
    env -u SSH_CONNECTION -u SSH_CLIENT -u SSH_TTY \
        PATH="$PB:/usr/bin:/bin" \
        AKAM_REFLASH_DIR="$RD" AKAM_FLASH_TMPDIR="$CRAMP" AKAM_REFLASH_DEV="$CAM/dev_mtd7" \
        AKAM_PROC_MTD="${MTD_FILE:-$CAM/mtd}" AKAM_PROC_MOUNTS="${MOUNTS_FILE:-$CAM/mounts}" \
        AKAM_BIN_DIR="$CAM/usr/bin" AKAM_TMP_DIR="$CAM/tmp" AKAM_CONFIG_DIR="$CAM/etc/config" \
        AKAM_REFLASH_DETACH_WAIT=3 "$@"
}
camrun() { # camrun OUTNAME args... : run the camera script under BusyBox sh
    local n=$1; shift
    cam_env "${SHCMD[@]}" "$RD/akam_reflash_ssh.sh" "$@" >"$WORK/$n.out" 2>&1
    RC=$?
}
reset_cam() {
    rm -rf "$RD" "$CAM/tmp" "$CAM/db_args" "$CAM/tool_info" "$CAM/failed_once"
    mkdir -p "$RD" "$CAM/tmp/akam_home"
    chmod 700 "$CAM/tmp/akam_home"
    echo "ssh-ed25519 AAAAFAKE test" >"$CAM/tmp/akam_home/authorized_keys"
    echo hostkey >"$CAM/etc/config/dropbear/dropbear_ed25519_host_key"
    head -c 5242880 /dev/zero >"$CAM/dev_mtd7"
    cp "$FS/akam_reflash_ssh.sh" "$FS/flash_guard.sh" "$RD/"
    cp "$WORK/image.squashfs" "$RD/image.squashfs"
    cp "$WORK/flash_tool" "$RD/flash_tool"
    echo ok >"$CAM/tool_mode"
    unset MTD_FILE MOUNTS_FILE
}
wait_result() { # wait up to 30 s for a RESULT line in the camera log
    local i=0
    while [ $i -lt 60 ]; do
        grep -q 'RESULT: ' "$RD/flash.log" 2>/dev/null && return 0
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}
# A fake "running dropbear" (pid file + cmdline with -r/-D) like the boot one.
start_fake_boot_dropbear() {
    cat >"$CAM/fakedb" <<'EOF'
#!/bin/sh
trap 'kill $!; exit 0' TERM
sleep 600 &
wait
EOF
    "${SHCMD[0]}" sh "$CAM/fakedb" -r "$CAM/etc/config/dropbear/dropbear_ed25519_host_key" -D "$CAM/tmp/akam_home" -p 22 </dev/null >/dev/null 2>&1 &
    echo $! >"$CAM/tmp/dropbear.pid"
    echo $! >>"$WORK/pids"
}

# --- B1. happy path ----------------------------------------------------------
reset_cam
start_fake_boot_dropbear
camrun b_pre precheck
rc_is "camera: precheck passes" "$RC" 0
has "camera: precheck sees APP" 'mtd7 is APP, 00500000' "$WORK/b_pre.out"
camrun b_prep prepare --md5 "$IMD5"
rc_is "camera: prepare passes" "$RC" 0
[ -x "$RD/bin/busybox" ] && [ -L "$RD/bin/md5sum" ] && ok "camera: busybox + applet links in RAM" || ko "camera: busybox + applet links in RAM"
[ -e "$RD/dropbear" ] && [ -f "$RD/dropbearmulti" ] && ok "camera: dropbear copied to RAM" || ko "camera: dropbear copied to RAM"
has "camera: second dropbear runs from RAM" "argv0=$RD/dropbear " "$CAM/db_args"
has "camera: second dropbear on 2222" ' -p 2222( |$)' "$CAM/db_args"
has "camera: same host key as the running one" " -r $CAM/etc/config/dropbear/dropbear_ed25519_host_key " "$CAM/db_args"
has "camera: same key directory as the running one" " -D $CAM/tmp/akam_home " "$CAM/db_args"
has "camera: own pid file in RAM" " -P $RD/dropbear2.pid " "$CAM/db_args"
camrun b_nocf start --md5 "$IMD5"
rc_not0 "camera: start without the confirmation refuses" "$RC"
has "camera: refusal names the flag" 'i-understand-no-uart' "$WORK/b_nocf.out"
cmp -s "$CAM/dev_mtd7" <(head -c 5242880 /dev/zero) && ok "camera: nothing written without confirmation" || ko "camera: nothing written without confirmation"
# Start from a shell that looks like an SSH session.
cam_env \
    SSH_CONNECTION="1.2.3.4 5 6.7.8.9 22" SSH_CLIENT="1.2.3.4 5 22" SSH_TTY=/dev/pts/9 \
    "${SHCMD[@]}" "$RD/akam_reflash_ssh.sh" start --md5 "$IMD5" --i-understand-no-uart >"$WORK/b_start.out" 2>&1
rc_is "camera: start returns at once" "$?" 0
has "camera: start reports a detached worker" 'detached worker started \(pid [0-9]+\)' "$WORK/b_start.out"
if wait_result; then ok "camera: worker finished"; else ko "camera: worker finished"; fi
has "camera: log says OK" 'RESULT: OK read-back md5 '"$IMD5"' matches' "$RD/flash.log"
has "camera: log says power cycle by hand" 'Power cycle BY HAND' "$RD/flash.log"
has "camera: guard passed in the worker" 'guard OK' "$RD/flash.log"
[ "$(md5sum <"$CAM/dev_mtd7" | cut -d' ' -f1)" = "$IMD5" ] && ok "camera: device holds the image" || ko "camera: device holds the image"
has "camera: SSH variables scrubbed in flash_tool" 'SSH env: \[\]' "$CAM/tool_info"
ppid=$(sed -n 's/^parent=\([0-9]*\) .*/\1/p' "$CAM/tool_info" | head -n 1)
psid=$(sed -n 's/^parent=[0-9]* parent_sid=\([0-9]*\) .*/\1/p' "$CAM/tool_info" | head -n 1)
[ -n "$ppid" ] && [ "$ppid" = "$psid" ] && ok "camera: flash worker is its own session leader (setsid)" || ko "camera: flash worker session ($ppid vs $psid)"
has "camera: flash_tool stdin is not a pipe/socket" 'stdin=/dev/null' "$CAM/tool_info"
camrun b_ver verify --md5 "$IMD5"
rc_is "camera: verify passes after the write" "$RC" 0
has "camera: verify reports the md5" "READBACK_MD5=$IMD5" "$WORK/b_ver.out"
camrun b_ver2 verify --md5 00000000000000000000000000000000
rc_not0 "camera: verify fails on a wrong md5" "$RC"
has "camera: verify says MISMATCH" 'VERIFY=MISMATCH' "$WORK/b_ver2.out"
camrun b_st status
has "camera: status reports the second dropbear" 'dropbear2=alive' "$WORK/b_st.out"
has "camera: status shows the log" 'RESULT: OK' "$WORK/b_st.out"
[ ! -e "$CAM/rebooted" ] && ok "camera: nothing ever called reboot/poweroff/halt" || ko "camera: reboot/poweroff/halt was called"

# --- B2. pre-check failures ----------------------------------------------------
reset_cam
MTD_FILE=$CAM/mtd_bad camrun b_badmtd precheck
rc_not0 "camera: mtd7 not APP/5 MiB refused" "$RC"
has "camera: says why" 'mtd7 is not the 5 MiB APP' "$WORK/b_badmtd.out"
FAKE_DF_FREE=1000 camrun b_space precheck
rc_not0 "camera: low free RAM refused" "$RC"
has "camera: says how much" 'only 1000 kB free' "$WORK/b_space.out"
camrun b_badmd5 prepare --md5 00000000000000000000000000000000
rc_not0 "camera: image md5 mismatch refused" "$RC"
has "camera: names the md5 mismatch" 'does not match the expected' "$WORK/b_badmd5.out"
camrun b_notmd5 prepare --md5 xyz
rc_not0 "camera: malformed md5 refused" "$RC"
cp "$WORK/small.img" "$RD/image.squashfs"
camrun b_size prepare --md5 "$(md5sum "$WORK/small.img" | cut -d' ' -f1)"
rc_not0 "camera: wrong image size refused" "$RC"
has "camera: names the size" 'expected exactly 5242880' "$WORK/b_size.out"
reset_cam
rm "$CAM/tmp/akam_home/authorized_keys"
camrun b_nokeys prepare --md5 "$IMD5"
rc_not0 "camera: no authorized_keys: second dropbear not started" "$RC"
has "camera: says nobody could log in" 'nobody could log in' "$WORK/b_nokeys.out"
cmp -s "$CAM/dev_mtd7" <(head -c 5242880 /dev/zero) && ok "camera: nothing written after failed prepare" || ko "camera: nothing written after failed prepare"

# --- B3. corrupted write: 3 attempts, then FAILED, never OK -------------------------
reset_cam
echo corrupt >"$CAM/tool_mode"
camrun b_c1 prepare --md5 "$IMD5"
camrun b_c2 start --md5 "$IMD5" --i-understand-no-uart
wait_result
has "camera: corrupt write ends FAILED" 'RESULT: FAILED after 3 attempts. DO NOT power cycle' "$RD/flash.log"
hasnt "camera: corrupt write is never OK" 'RESULT: OK' "$RD/flash.log"
[ "$(grep -c 'attempt [0-9]/3' "$RD/flash.log")" -eq 3 ] && ok "camera: three attempts logged" || ko "camera: three attempts logged"

# --- B4. transient failure is retried -----------------------------------------------
reset_cam
echo failonce >"$CAM/tool_mode"
camrun b_r1 prepare --md5 "$IMD5"
camrun b_r2 start --md5 "$IMD5" --i-understand-no-uart
wait_result
has "camera: retry succeeds on attempt 2" 'RESULT: OK' "$RD/flash.log"
has "camera: first attempt failure logged" 'flash_tool exit status 1' "$RD/flash.log"

# --- B5. the worker's own guard: no tmpfs, no write -----------------------------------
reset_cam
camrun b_g1 prepare --md5 "$IMD5"
printf '/dev/mtdblock7 /usr squashfs ro 0 0\n' >"$CAM/mounts_none"
MOUNTS_FILE=$CAM/mounts_none camrun b_g2 start --md5 "$IMD5" --i-understand-no-uart
MOUNTS_FILE=$CAM/mounts_none wait_result
has "camera: worker refuses when /tmp is not tmpfs" 'RESULT: REFUSED by flash_guard' "$RD/flash.log"
cmp -s "$CAM/dev_mtd7" <(head -c 5242880 /dev/zero) && ok "camera: refused worker wrote nothing" || ko "camera: refused worker wrote nothing"

# --- B6. one worker at a time (atomic lock) ------------------------------------------
live_sleeper() { sleep 600 >/dev/null 2>&1 & LIVE_PID=$!; echo "$LIVE_PID" >>"$WORK/pids"; }
zeros_ok() { cmp -s "$CAM/dev_mtd7" <(head -c 5242880 /dev/zero); }
reset_cam
camrun b_l1 prepare --md5 "$IMD5"
live_sleeper
mkdir "$RD/worker.lock"; echo "$LIVE_PID" >"$RD/worker.lock/pid"
camrun b_l2 start --md5 "$IMD5" --i-understand-no-uart
rc_not0 "camera: start refused while a live worker holds the lock" "$RC"
has "camera: says a worker is already running" 'already running' "$WORK/b_l2.out"
camrun b_l2p precheck
rc_not0 "camera: precheck refused while a worker holds the lock" "$RC"
# the worker itself refuses too (two racing starts) and leaves the owner's state alone
echo "$LIVE_PID" >"$RD/worker.pid"
cam_env setsid -w "${SHCMD[@]}" "$RD/akam_reflash_ssh.sh" __worker "$IMD5" --i-understand-no-uart >"$WORK/b_l3.out" 2>&1
has "camera: a second worker refuses" 'RESULT: REFUSED another worker holds' "$WORK/b_l3.out"
[ "$(cat "$RD/worker.pid")" = "$LIVE_PID" ] && [ "$(cat "$RD/worker.lock/pid")" = "$LIVE_PID" ] && ok "camera: a refused worker leaves the owner's pid/lock alone" || ko "camera: a refused worker leaves the owner's pid/lock alone"
zeros_ok && ok "camera: nothing written by the refused worker" || ko "camera: nothing written by the refused worker"
# a lock without a pid yet: do not take it over
rm -rf "$RD/worker.lock" "$RD/worker.pid"; mkdir "$RD/worker.lock"
camrun b_l4 start --md5 "$IMD5" --i-understand-no-uart
rc_not0 "camera: start refused on a lock with no pid" "$RC"
has "camera: tells to check by hand" 'without a live owner pid' "$WORK/b_l4.out"
# a stale lock (dead owner) is taken over and the flash goes through
rm -rf "$RD/worker.lock"; mkdir "$RD/worker.lock"; echo 999999 >"$RD/worker.lock/pid"
camrun b_l5 start --md5 "$IMD5" --i-understand-no-uart
rc_is "camera: start takes over a stale lock" "$RC" 0
wait_result
has "camera: flash done after the stale lock" 'RESULT: OK' "$RD/flash.log"
sleep 1
[ ! -d "$RD/worker.lock" ] && ok "camera: the lock is released when the worker exits" || ko "camera: the lock is released when the worker exits"

# --- B7. mtd7 is re-checked by start and by the worker -----------------------------------
reset_cam
camrun b_m1 prepare --md5 "$IMD5"
MTD_FILE=$CAM/mtd_bad camrun b_m2 start --md5 "$IMD5" --i-understand-no-uart
rc_not0 "camera: start refused when mtd7 is not the APP" "$RC"
has "camera: start names the mtd7 problem" 'mtd7 is not the 5 MiB APP' "$WORK/b_m2.out"
zeros_ok && ok "camera: nothing written when mtd7 is wrong" || ko "camera: nothing written when mtd7 is wrong"
MTD_FILE=$CAM/mtd_bad cam_env setsid -w "${SHCMD[@]}" "$RD/akam_reflash_ssh.sh" __worker "$IMD5" --i-understand-no-uart >"$WORK/b_m3.out" 2>&1
has "camera: the worker re-checks mtd7 itself" 'RESULT: REFUSED mtd7 is not the 5 MiB APP' "$WORK/b_m3.out"
zeros_ok && ok "camera: worker wrote nothing when mtd7 is wrong" || ko "camera: worker wrote nothing when mtd7 is wrong"
[ ! -d "$RD/worker.lock" ] && ok "camera: refused worker released its lock" || ko "camera: refused worker released its lock"
# wrong size reaching the worker directly
cp "$WORK/small.img" "$RD/image.squashfs"
cam_env setsid -w "${SHCMD[@]}" "$RD/akam_reflash_ssh.sh" __worker "$(md5sum "$WORK/small.img" | cut -d' ' -f1)" --i-understand-no-uart >"$WORK/b_m4.out" 2>&1
has "camera: the worker refuses a wrong-size image" 'RESULT: REFUSED image is 1000 bytes' "$WORK/b_m4.out"
zeros_ok && ok "camera: worker wrote nothing for a wrong-size image" || ko "camera: worker wrote nothing for a wrong-size image"

# =============================================================== C. host helper
cat >"$WORK/fakessh" <<EOF
#!/bin/bash
# fake ssh: skip options, log "PORT: command", run the command with the camera sh.
port=22
while [ \$# -gt 0 ]; do
    case "\$1" in
        -o|-i) shift 2 ;;
        -p) port=\$2; shift 2 ;;
        -*) shift ;;
        *) break ;;
    esac
done
shift   # user@host
cmd="\$*"
echo "\$port: \$cmd" >>"$WORK/ssh.log"
if [ "\$port" = 2222 ] && [ -e "$WORK/no_second" ]; then exit 255; fi
case "\$cmd" in
    *"tail -n +"*)
        n=\$(cat "$WORK/polls" 2>/dev/null || echo 0); n=\$((n + 1)); echo \$n >"$WORK/polls"
        if [ -e "$WORK/flaky" ] && [ \$n -le 2 ]; then exit 255; fi ;;
    *" verify "*)
        if [ -e "$WORK/tamper" ]; then printf 'Z' | dd of="$CAM/dev_mtd7" bs=1 seek=4000000 conv=notrunc 2>/dev/null; fi ;;
esac
"$BB" sh -c "\$cmd"
rc=\$?
case "\$cmd" in
    *"cat > '"*image.squashfs*) if [ -e "$WORK/corrupt_upload" ]; then printf 'Z' >>"$RD/image.squashfs"; fi ;;
esac
exit \$rc
EOF
chmod +x "$WORK/fakessh"

hrun() { # hrun OUTNAME args... (all the camera env is exported by cam_env)
    local n=$1; shift
    : >"$WORK/ssh.log"
    rm -f "$WORK/polls"
    cam_env REFLASH_SSH="$WORK/fakessh" REFLASH_REMOTE_TMP="$CRAMP" \
        "$FS/reflash_over_ssh.sh" --host camera.invalid --image "$WORK/image.squashfs" --md5 "$IMD5" \
        --flash-tool "$WORK/flash_tool" --remote-dir "$RD" --poll-interval 1 --timeout 60 "$@" \
        >"$WORK/$n.out" 2>&1
    RC=$?
}
host_clear() { rm -f "$WORK/no_second" "$WORK/flaky" "$WORK/tamper" "$WORK/corrupt_upload"; }

# --- C1. refuses without the confirmation, before any ssh -------------------------------
reset_cam; host_clear
hrun h_nocf
rc_is "host: refuses without --i-understand-no-uart" "$RC" 2
has "host: explains" 'refusing to flash without --i-understand-no-uart' "$WORK/h_nocf.out"
[ ! -s "$WORK/ssh.log" ] && ok "host: no ssh call without the confirmation" || ko "host: no ssh call without the confirmation"
# --- C2. local pre-checks -------------------------------------------------------------
hrun h_md5 --i-understand-no-uart --md5 00000000000000000000000000000000
rc_is "host: wrong md5 refused" "$RC" 1
has "host: says so" 'image md5 is .* you said' "$WORK/h_md5.out"
[ ! -s "$WORK/ssh.log" ] && ok "host: wrong md5 before any ssh" || ko "host: wrong md5 before any ssh"
cam_env REFLASH_SSH="$WORK/fakessh" "$FS/reflash_over_ssh.sh" --host h --image "$WORK/small.img" \
    --md5 "$(md5sum "$WORK/small.img" | cut -d' ' -f1)" --flash-tool "$WORK/flash_tool" --i-understand-no-uart >"$WORK/h_size.out" 2>&1
rc_is "host: wrong image size refused" "$?" 1
has "host: names the size" 'expected exactly 5242880' "$WORK/h_size.out"
hrun h_dir --i-understand-no-uart --remote-dir /etc/akam
rc_is "host: --remote-dir outside /tmp refused" "$RC" 1
# --- C3. precheck-only ---------------------------------------------------------------------
reset_cam; host_clear
hrun h_pre --precheck-only
rc_is "host: --precheck-only succeeds" "$RC" 0
has "host: precheck-only wrote nothing" 'nothing written' "$WORK/h_pre.out"
[ ! -e "$RD/flash_tool" ] && ok "host: precheck-only does not upload flash_tool" || ko "host: precheck-only does not upload flash_tool"
zeros_ok && ok "host: precheck-only leaves the device alone" || ko "host: precheck-only leaves the device alone"
# --- C4. full run -------------------------------------------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
echo slow >"$CAM/tool_mode"
hrun h_ok --i-understand-no-uart
rc_is "host: full run succeeds" "$RC" 0
has "host: sha256 verified for the image" 'sha256 OK for image\.squashfs' "$WORK/h_ok.out"
has "host: sha256 verified for flash_tool" 'sha256 OK for flash_tool' "$WORK/h_ok.out"
has "host: second session checked" 'port 2222 session works' "$WORK/h_ok.out"
has "host: log lines relayed while the flash runs" 'camera: .*attempt 1/3' "$WORK/h_ok.out"
has "host: result relayed" 'camera: .*RESULT: OK' "$WORK/h_ok.out"
has "host: independent read-back" 'read-back md5 of /dev/mtd7 = '"$IMD5" "$WORK/h_ok.out"
has "host: tells the user to power cycle by hand" 'power-cycle BY HAND' "$WORK/h_ok.out"
[ "$(md5sum <"$CAM/dev_mtd7" | cut -d' ' -f1)" = "$IMD5" ] && ok "host: device holds the image" || ko "host: device holds the image"
up_ssh=$(grep -c '^22: .*cat >' "$WORK/ssh.log")
[ "$up_ssh" -ge 4 ] && ok "host: uploads go through port 22 ($up_ssh)" || ko "host: uploads through port 22 ($up_ssh)"
grep -q '^2222: .* start --md5' "$WORK/ssh.log" && ok "host: start is issued on port 2222" || ko "host: start is issued on port 2222"
grep -q '^22: .* start --md5' "$WORK/ssh.log" && ko "host: start must not use port 22" || ok "host: start does not use port 22"
grep -q '^2222: .*tail -n' "$WORK/ssh.log" && ok "host: log polled on port 2222" || ko "host: log polled on port 2222"
grep -Eq 'reboot|poweroff|halt|shutdown' "$WORK/ssh.log" && ko "host: never reboots the camera" || ok "host: never reboots the camera"
has "host: flash ran detached from RAM tools" "argv=" "$CAM/tool_info"
# --- C5. flaky polling is tolerated ----------------------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
: >"$WORK/flaky"
hrun h_flaky --i-understand-no-uart
rc_is "host: survives failed polls" "$RC" 0
has "host: reports the failed polls" 'poll failed \(2 in a row\)' "$WORK/h_flaky.out"
# --- C6. no second session: abort BEFORE the flash ----------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
: >"$WORK/no_second"
hrun h_nosec --i-understand-no-uart
rc_is "host: aborts when port 2222 is unreachable" "$RC" 1
has "host: says nothing was written" 'nothing was written, aborting before the flash' "$WORK/h_nosec.out"
zeros_ok && ok "host: no write when the second session is missing" || ko "host: no write when the second session is missing"
# --- C7. upload corrupted in transit -----------------------------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
: >"$WORK/corrupt_upload"
hrun h_sha --i-understand-no-uart
rc_is "host: sha256 mismatch aborts" "$RC" 1
has "host: says sha256 mismatch" 'sha256 mismatch for image\.squashfs' "$WORK/h_sha.out"
zeros_ok && ok "host: no write after a bad upload" || ko "host: no write after a bad upload"
# --- C8. flash_tool corrupts the write -------------------------------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
echo corrupt >"$CAM/tool_mode"
hrun h_bad --i-understand-no-uart
rc_is "host: failed flash exits 1" "$RC" 1
has "host: tells not to power cycle" 'DO NOT power-cycle' "$WORK/h_bad.out"
hasnt "host: no VERIFIED line" 'VERIFIED' "$WORK/h_bad.out"
# --- C9. worker says OK but the host's own read-back disagrees --------------------------------------------
reset_cam; host_clear
start_fake_boot_dropbear
: >"$WORK/tamper"
hrun h_tamper --i-understand-no-uart
rc_is "host: independent read-back mismatch exits 1" "$RC" 1
has "host: says do not power cycle" 'independent read-back md5 .* expected .*DO NOT power-cycle' "$WORK/h_tamper.out"
hasnt "host: no VERIFIED on mismatch" 'VERIFIED' "$WORK/h_tamper.out"
# --- C10. a worker is running: the helper must not wipe its directory -----------------------
reset_cam; host_clear
start_fake_boot_dropbear
live_sleeper
echo "$LIVE_PID" >"$RD/worker.pid"
echo "[up 1] precious log" >"$RD/flash.log"
hrun h_busy --i-understand-no-uart
rc_is "host: refuses while a worker is running" "$RC" 1
has "host: says the worker is still running" 'worker is still running' "$WORK/h_busy.out"
has "host: the running worker's log is intact" 'precious log' "$RD/flash.log"
[ -e "$RD/worker.pid" ] && ok "host: the running worker's pid file is intact" || ko "host: the running worker's pid file is intact"
zeros_ok && ok "host: nothing written while a worker runs" || ko "host: nothing written while a worker runs"
# --- C11. a stale log from an earlier run must not be taken for this run's result ------------
reset_cam; host_clear
start_fake_boot_dropbear
printf '[up 1] RESULT: FAILED after 3 attempts. DO NOT power cycle.\n' >"$RD/flash.log"
echo 999999 >"$RD/worker.pid"
hrun h_stale --i-understand-no-uart
rc_is "host: a stale FAILED log does not block a new run" "$RC" 0
hasnt "host: the stale result is not relayed" 'FAILED after 3' "$WORK/h_stale.out"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
