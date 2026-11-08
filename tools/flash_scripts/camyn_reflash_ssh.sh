#!/bin/sh
# camyn_reflash_ssh.sh -- reflash the APP partition (mtd7) of a RUNNING Camyn
# camera from an SSH session, WITHOUT UART and WITHOUT the SD card.
# Runs ON the camera (POSIX sh, BusyBox 1.30.1: no tr, mktemp, nohup, flock).
# Normally driven from the PC by reflash_over_ssh.sh, which uploads the files
# to /tmp/camyn_reflash/ first. Read docs/camyn_v1_flash_and_restore.md
# ("Reflashing over SSH, without UART") before using it: a write that is
# interrupted half way leaves an APP that only UART can recover.
#
# Why it is "safe enough": on a Camyn image /usr is the APP squashfs, so the
# running dropbear, watchdog, scripts are paged in from the partition being
# written. This script keeps everything that matters OFF the partition:
#   prepare  copies busybox, dropbear and (already uploaded) flash_tool and the
#            image into RAM (/tmp, tmpfs) and starts a SECOND dropbear from
#            /tmp on port 2222 (same host key, same key directory), so the
#            operator keeps a shell that does not depend on /usr;
#   start    launches the flash with setsid, from the RAM copies, with its own
#            session, no SSH environment and stdio on a log file, so a dying
#            SSH session cannot kill it. The worker re-checks everything with
#            camyn_flash_guard_detached (flash_guard.sh) before the first write.
# Only one worker can run (mkdir lock in $DIR/worker.lock); the worker also
# re-checks mtd7 and the image size just before the erase.
# Nothing here ever reboots the camera: power cycle by hand once the log says
# "RESULT: OK" and the read-back md5 matches.
#
# Usage (all from the work directory, default /tmp/camyn_reflash):
#   sh camyn_reflash_ssh.sh precheck
#       mtd7 is APP 00500000, enough free RAM in /tmp. Read-only.
#   sh camyn_reflash_ssh.sh prepare --md5 HEX
#       image is 5242880 bytes with md5 HEX, tools are in RAM, 2nd dropbear up.
#   sh camyn_reflash_ssh.sh start --md5 HEX --i-understand-no-uart
#       start the detached flash worker and return at once.
#   sh camyn_reflash_ssh.sh status      worker state + log tail
#   sh camyn_reflash_ssh.sh verify --md5 HEX
#       read /dev/mtd7 back and compare (exit 0 only on a match).
#
# Environment (test hooks; the defaults are right on the camera):
#   CAMYN_REFLASH_DIR  work directory           (/tmp/camyn_reflash)
#   CAMYN_FLASH_TMPDIR the RAM directory        (/tmp)
#   CAMYN_REFLASH_DEV  device written           (/dev/mtd7)
#   CAMYN_PROC_MTD     partition table          (/proc/mtd)
#   CAMYN_PROC_DIR     proc root                (/proc)
#   CAMYN_REFLASH_PORT second dropbear port     (2222)
#   CAMYN_BIN_DIR      where dropbear lives     (/usr/bin)
#   CAMYN_TMP_DIR      dropbear.pid, camyn_home (/tmp)
#   CAMYN_CONFIG_DIR   persistent dropbear dir  (/etc/config)
#   CAMYN_REFLASH_ATTEMPTS  write+verify attempts (3)
#   CAMYN_REFLASH_MIN_FREE_KB free /tmp needed  (10240)
#   CAMYN_REFLASH_DETACH_WAIT seconds the worker waits to be detached (20)

DIR=${CAMYN_REFLASH_DIR:-/tmp/camyn_reflash}
RAMDIR=${CAMYN_FLASH_TMPDIR:-/tmp}
DEV=${CAMYN_REFLASH_DEV:-/dev/mtd7}
MTD=${CAMYN_PROC_MTD:-/proc/mtd}
PROC=${CAMYN_PROC_DIR:-/proc}
PORT=${CAMYN_REFLASH_PORT:-2222}
BINSRC=${CAMYN_BIN_DIR:-/usr/bin}
TMPD=${CAMYN_TMP_DIR:-/tmp}
CONFD=${CAMYN_CONFIG_DIR:-/etc/config}
ATTEMPTS=${CAMYN_REFLASH_ATTEMPTS:-3}
MIN_FREE_KB=${CAMYN_REFLASH_MIN_FREE_KB:-10240}
DETACH_WAIT=${CAMYN_REFLASH_DETACH_WAIT:-20}
EXPECTSIZE=5242880
IMAGE=$DIR/image.squashfs
TOOL=$DIR/flash_tool
GUARD=$DIR/flash_guard.sh
SELF=$DIR/camyn_reflash_ssh.sh
BIN=$DIR/bin
LOG=$DIR/flash.log
WPID=$DIR/worker.pid
LOCK=$DIR/worker.lock
DBPID=$DIR/dropbear2.pid
DBLOG=$DIR/dropbear2.log
CONFIRM_FLAG=--i-understand-no-uart
# Applets the flash path needs from the RAM copy of busybox.
APPLETS="sh cat dd md5sum sha256sum sleep sync tail grep sed stat ls kill rm df setsid mkdir cp ln mknod chmod"

say() { echo "camyn_reflash: $*"; }
die() { echo "camyn_reflash: ERROR: $*" >&2; exit 1; }

uptime_s() {
    _up='?'
    [ -r "$PROC/uptime" ] && read -r _up _ <"$PROC/uptime"
    echo "$_up"
}
log() { echo "[up $(uptime_s)] $*"; }

# First field of "md5sum"/"sha256sum" output for file/stdin.
md5_of() { _m=$("$1" "$2" 2>/dev/null) || return 1; echo "${_m%% *}"; }

alive() { # alive PIDFILE : pid in the file is a live process
    [ -s "$1" ] || return 1
    read -r _pid <"$1" 2>/dev/null || return 1
    case "$_pid" in '' | *[!0-9]*) return 1 ;; esac
    kill -0 "$_pid" 2>/dev/null
}

check_hex_md5() {
    case "$1" in
        '' | *[!0-9a-f]*) die "--md5 must be 32 lowercase hex digits" ;;
    esac
    [ "${#1}" -eq 32 ] || die "--md5 must be 32 lowercase hex digits"
}

check_mtd() {
    [ -r "$MTD" ] || die "$MTD not readable"
    _line=$(grep '^mtd7:' "$MTD") || die "no mtd7 in $MTD"
    case "$_line" in
        'mtd7: 00500000 '*'"APP"'*) say "mtd7 is APP, 00500000 bytes" ;;
        *) die "mtd7 is not the 5 MiB APP partition: $_line" ;;
    esac
}

# Same test without exiting, for the worker (it must log a RESULT line).
mtd_is_app() {
    [ -r "$MTD" ] || return 1
    _line=$(grep '^mtd7:' "$MTD") || return 1
    case "$_line" in
        'mtd7: 00500000 '*'"APP"'*) return 0 ;;
    esac
    return 1
}

# Worker lock: $LOCK is created with mkdir (atomic) by the worker and holds its
# pid. Returns 0 if a live worker owns it, 1 if there is none or it is stale
# (then it is removed), 2 if it exists without a pid yet (a worker is starting,
# or died between mkdir and the pid write: leave it, the operator decides).
lock_state() {
    [ -d "$LOCK" ] || return 1
    _lp=
    [ -s "$LOCK/pid" ] && read -r _lp <"$LOCK/pid" 2>/dev/null
    case "$_lp" in
        '' | *[!0-9]*) return 2 ;;
    esac
    if kill -0 "$_lp" 2>/dev/null; then return 0; fi
    rm -rf "$LOCK"
    return 1
}

check_space() {
    [ -d "$RAMDIR" ] || die "$RAMDIR does not exist"
    _l=$(df -k "$RAMDIR" 2>/dev/null | tail -n 1)
    # shellcheck disable=SC2086
    set -- $_l
    # BusyBox df: "tmpfs 22000 100 21900 1% /tmp" (a long device name wraps
    # onto the previous line, leaving 5 fields).
    if [ "$#" -ge 6 ]; then _free=$4; else _free=${3:-}; fi
    case "$_free" in '' | *[!0-9]*) die "cannot read free space of $RAMDIR (df said: $_l)" ;; esac
    [ "$_free" -ge "$MIN_FREE_KB" ] || die "only $_free kB free in $RAMDIR, need $MIN_FREE_KB kB (image + tools in RAM)"
    say "$_free kB free in $RAMDIR (need $MIN_FREE_KB)"
}

cmd_precheck() {
    check_mtd
    check_space
    [ -d "$DIR" ] || die "$DIR missing (upload the scripts first)"
    if alive "$WPID"; then die "a reflash worker is already running (see $LOG)"; fi
    lock_state
    [ $? -eq 1 ] || die "a reflash worker holds $LOCK (see $LOG)"
    if grep -q ' /usr .*mtdblock7' "$PROC/mounts" 2>/dev/null; then
        say "note: /usr is the APP squashfs (mtdblock7): that is why everything runs from RAM"
    fi
    say "precheck OK"
}

# check_files MD5: image size + md5, tool, guard.
check_files() {
    [ -f "$IMAGE" ] || die "$IMAGE missing"
    [ -f "$TOOL" ] || die "$TOOL missing"
    [ -f "$GUARD" ] || die "$GUARD missing"
    _sz=$(stat -c%s "$IMAGE" 2>/dev/null) || die "cannot stat $IMAGE"
    [ "$_sz" -eq "$EXPECTSIZE" ] || die "image is $_sz bytes, expected exactly $EXPECTSIZE (full partition)"
    _got=$(md5_of "${BIN_MD5:-md5sum}" "$IMAGE") || die "md5sum failed"
    [ "$_got" = "$1" ] || die "image md5 $_got does not match the expected $1"
    chmod 755 "$TOOL" 2>/dev/null
    [ -x "$TOOL" ] || die "$TOOL is not executable"
    say "image: $EXPECTSIZE bytes, md5 $_got; flash_tool in RAM"
}

# Copy a busybox into RAM and link the applets we need. It must come from RAM
# because /usr/bin/<applet> may be gone, or fault, while the APP is written.
copy_busybox() {
    mkdir -p "$BIN" || die "cannot create $BIN"
    _src=$(command -v busybox 2>/dev/null)
    [ -n "$_src" ] || _src=$PROC/$$/exe
    [ -r "$_src" ] || die "no busybox binary found to copy"
    # Unlink first: overwriting a busybox that a live session executes fails
    # with "Text file busy" (a re-run while the second Dropbear session is up).
    rm -f "$BIN/busybox"
    cp "$_src" "$BIN/busybox" || die "cannot copy busybox to $BIN"
    chmod 755 "$BIN/busybox"
    _list=$("$BIN/busybox" --list 2>/dev/null) || die "copied busybox does not run ($_src)"
    for _a in $APPLETS; do
        echo "$_list" | grep -qx "$_a" || die "busybox lacks the applet $_a"
        ln -sf busybox "$BIN/$_a" || die "cannot link applet $_a"
    done
    # readlink only helps flash_guard.sh resolve symlinks; not fatal if absent.
    if echo "$_list" | grep -qx readlink; then ln -sf busybox "$BIN/readlink"; fi
    say "busybox copied to $BIN ($_src)"
}

# Arguments of the running dropbear ($TMPD/dropbear.pid): value of option $1.
running_dropbear_opt() {
    [ -s "$TMPD/dropbear.pid" ] || return 1
    read -r _p <"$TMPD/dropbear.pid" || return 1
    [ -r "$PROC/$_p/cmdline" ] || return 1
    _prev=; _val=
    strings -n 1 <"$PROC/$_p/cmdline" 2>/dev/null >"$DIR/.cmdline" || return 1
    while IFS= read -r _w; do
        if [ "$_prev" = "$1" ]; then _val=$_w; break; fi
        _prev=$_w
    done <"$DIR/.cmdline"
    rm -f "$DIR/.cmdline"
    [ -n "$_val" ] || return 1
    echo "$_val"
    return 0
}

start_second_dropbear() {
    [ -x "$BINSRC/dropbear" ] || die "$BINSRC/dropbear not found"
    if alive "$DBPID"; then say "second dropbear already running (port $PORT)"; return 0; fi
    cp "$BINSRC/dropbear" "$DIR/dropbearmulti" || die "cannot copy dropbear to $DIR"
    chmod 755 "$DIR/dropbearmulti"
    ln -sf dropbearmulti "$DIR/dropbear"
    _hk=$(running_dropbear_opt -r) || _hk=$CONFD/dropbear/dropbear_ed25519_host_key
    _kd=$(running_dropbear_opt -D) || _kd=$TMPD/camyn_home
    [ -s "$_hk" ] || die "host key $_hk missing"
    [ -s "$_kd/authorized_keys" ] || die "$_kd/authorized_keys missing: nobody could log in on port $PORT"
    # No -F: dropbear daemonizes. -T 3: three auth tries. Public-key only
    # (password auth is compiled out). -E: log to stderr -> $DBLOG.
    "$DIR/dropbear" -r "$_hk" -D "$_kd" -p "$PORT" -P "$DBPID" -T 3 -E </dev/null >/dev/null 2>>"$DBLOG"
    _i=0
    while [ "$_i" -lt 10 ]; do
        alive "$DBPID" && { say "second dropbear up on port $PORT (keys $_kd, host key $_hk)"; return 0; }
        sleep 1
        _i=$((_i + 1))
    done
    die "second dropbear did not start (see $DBLOG)"
}

cmd_prepare() {
    check_hex_md5 "$MD5"
    check_mtd
    # The RAM busybox does the checksumming from here on.
    copy_busybox
    BIN_MD5=$BIN/md5sum
    check_files "$MD5"
    if [ ! -c "$DEV" ] && [ "$DEV" = /dev/mtd7 ]; then
        "$BIN/mknod" "$DEV" c 90 14 || die "cannot create $DEV"
    fi
    [ -e "$DEV" ] || die "$DEV does not exist"
    start_second_dropbear
    say "prepare OK: reconnect on port $PORT, then run: start --md5 $MD5 $CONFIRM_FLAG"
}

cmd_start() {
    [ "$CONFIRM" = "$CONFIRM_FLAG" ] || die "refusing to start without $CONFIRM_FLAG (a half-written APP needs UART to recover)"
    check_hex_md5 "$MD5"
    [ -x "$BIN/busybox" ] || die "run prepare first ($BIN/busybox missing)"
    check_mtd
    alive "$WPID" && die "a reflash worker is already running"
    lock_state
    case $? in
        0) die "a reflash worker is already running (lock $LOCK)" ;;
        2) die "$LOCK exists without a live owner pid: a worker is starting or died; check $LOG, remove $LOCK by hand only if no flash is running" ;;
    esac
    alive "$DBPID" || die "the second dropbear (port $PORT) is not running: run prepare first"
    BIN_MD5=$BIN/md5sum
    check_files "$MD5"
    rm -f "$WPID"
    {
        echo "[up $(uptime_s)] start requested (md5 $MD5)"
    } >>"$LOG" || die "cannot write $LOG"
    # Detach: new session (setsid), SSH variables dropped, stdio on the log.
    # The subshell is replaced by setsid, so the worker's only link to this
    # shell is its parent pid, and it is re-parented to init when we exit.
    (
        unset SSH_CONNECTION SSH_CLIENT SSH_TTY SSH_AUTH_SOCK
        PATH=$BIN
        export PATH
        exec "$BIN/setsid" "$BIN/busybox" sh "$SELF" __worker "$MD5" "$CONFIRM"
    ) </dev/null >>"$LOG" 2>&1 &
    _i=0
    while [ "$_i" -lt 10 ]; do
        [ -s "$WPID" ] && break
        sleep 1
        _i=$((_i + 1))
    done
    [ -s "$WPID" ] || die "the worker did not report in (see $LOG)"
    read -r _w <"$WPID"
    say "detached worker started (pid $_w); follow $LOG"
}

# The worker. Everything runs from RAM; it never reboots anything.
cmd_worker() {
    PATH=$BIN
    export PATH
    # One worker at a time: mkdir is atomic, so two racing starts cannot both
    # write. A refused duplicate must not touch $WPID or the lock of the owner.
    if ! mkdir "$LOCK" 2>/dev/null; then
        log "RESULT: REFUSED another worker holds $LOCK (nothing was written by this one)"
        exit 1
    fi
    echo $$ >"$LOCK/pid"
    trap 'rm -rf "$LOCK"' EXIT
    echo $$ >"$WPID"
    log "worker pid $$ up, waiting to be detached from the launching shell"
    # shellcheck disable=SC1090,SC1091 # flash_guard.sh is uploaded next to this script
    . "$GUARD" || { log "RESULT: REFUSED cannot load $GUARD"; exit 1; }
    _t=0
    while [ "$_t" -lt "$DETACH_WAIT" ]; do
        camyn_flash_guard_detached "$SELF" "$TOOL" "$IMAGE" "$CONFIRM" 2>/dev/null && break
        sleep 1
        _t=$((_t + 1))
    done
    if ! camyn_flash_guard_detached "$SELF" "$TOOL" "$IMAGE" "$CONFIRM"; then
        log "RESULT: REFUSED by flash_guard (nothing was written)"
        exit 1
    fi
    log "guard OK (RAM-only, detached, session leader)"
    _got=$(md5_of md5sum "$IMAGE")
    if [ "$_got" != "$MD5" ]; then
        log "RESULT: REFUSED image md5 $_got != $MD5 (nothing was written)"
        exit 1
    fi
    log "image in RAM md5 $_got"
    # Re-checked here, not only in start: this is the last gate before the erase.
    _sz=$(stat -c%s "$IMAGE" 2>/dev/null)
    if [ "$_sz" != "$EXPECTSIZE" ]; then
        log "RESULT: REFUSED image is ${_sz:-?} bytes, expected exactly $EXPECTSIZE (nothing was written)"
        exit 1
    fi
    if ! mtd_is_app; then
        log "RESULT: REFUSED mtd7 is not the 5 MiB APP partition in $MTD (nothing was written)"
        exit 1
    fi
    if [ ! -e "$DEV" ]; then
        log "RESULT: REFUSED $DEV does not exist (nothing was written)"
        exit 1
    fi
    _n=0
    while [ "$_n" -lt "$ATTEMPTS" ]; do
        _n=$((_n + 1))
        log "attempt $_n/$ATTEMPTS: $TOOL $IMAGE $DEV"
        "$TOOL" "$IMAGE" "$DEV"
        _rc=$?
        log "flash_tool exit status $_rc"
        sync
        _rb=$(readback_md5)
        log "read-back md5 of $DEV: ${_rb:-?} (expected $MD5)"
        if [ "$_rc" -eq 0 ] && [ "$_rb" = "$MD5" ]; then
            log "RESULT: OK read-back md5 $_rb matches. Power cycle BY HAND (unplug and replug USB-C)."
            exit 0
        fi
    done
    log "RESULT: FAILED after $ATTEMPTS attempts. DO NOT power cycle. Reconnect on port $PORT and run start again, or use UART."
    exit 1
}

readback_md5() {
    _r=$(dd if="$DEV" bs=64k count=$((EXPECTSIZE / 65536)) 2>/dev/null | md5sum) || return 1
    echo "${_r%% *}"
}

cmd_status() {
    if alive "$WPID"; then echo "worker=alive"; else echo "worker=dead"; fi
    if alive "$DBPID"; then echo "dropbear2=alive"; else echo "dropbear2=dead"; fi
    [ -r "$LOG" ] && tail -n 20 "$LOG"
    return 0
}

cmd_verify() {
    check_hex_md5 "$MD5"
    _rb=$(readback_md5) || die "read-back failed"
    echo "READBACK_MD5=$_rb"
    if [ "$_rb" = "$MD5" ]; then echo "VERIFY=MATCH"; return 0; fi
    echo "VERIFY=MISMATCH"
    return 1
}

# ----------------------------------------------------------------- main
CMD=${1:-}
[ -n "$CMD" ] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
shift
MD5=
CONFIRM=
if [ "$CMD" = __worker ]; then
    MD5=${1:-}
    CONFIRM=${2:-}
else
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --md5) MD5=${2:-}; shift 2 || die "--md5 needs a value" ;;
            "$CONFIRM_FLAG") CONFIRM=$1; shift ;;
            *) die "unknown argument: $1" ;;
        esac
    done
fi

# status and verify must not depend on /usr either.
case "$CMD" in
    status | verify)
        if [ -x "$BIN/busybox" ]; then
            PATH=$BIN
            export PATH
        fi
        ;;
esac

case "$CMD" in
    precheck) cmd_precheck ;;
    prepare) cmd_prepare ;;
    start) cmd_start ;;
    __worker) cmd_worker ;;
    status) cmd_status ;;
    verify) cmd_verify ;;
    *) die "unknown command: $CMD" ;;
esac
