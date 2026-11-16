#!/usr/bin/env bash
# flash_appfs.sh -- write the card-less Akam appfs image to mtd3 of the LSC Rotatable 2K, with verification and a
# rollback to the stock image. HOST side (this script) + CAMERA side (over ssh). NOT EXECUTED during development.
#
#   DEFAULT = DRY RUN: only local read-only checks of the files, then every camera/flash step is PRINTED, not run.
#   LIVE    = both of:   AKAM_I_HAVE_RECOVERY=yes   AND   the argument --execute
#             (--execute without the variable is refused; the variable without --execute is still a dry run)
#
# THIS WRITES THE CAMERA FLASH (mtd3). The only way it can brick the camera is a power loss (or a crash) between
# the start of the erase and the end of the write (~1-2 minutes): then mtd3 is half written and the camera does not
# boot, because init.sh (the SD recovery hook) lives inside mtd3. That needs a UART (U-Boot `sf write`) or a
# CH341A + SOIC8 clip. Do not run this without one of them at hand. See docs/porting/lsc-rotatable-2k/cardless-appfs.md.
#
# Procedure (a)-(e). The script NEVER reboots (step (f) is manual, see the end of the output):
#   (a) full re-dump of mtd0..mtd5 to the PC, byte-compare with the backup (mtd4, the jffs2 config, may differ
#       legitimately: warning only; anything else different = abort before any write)
#   (b) camera preflight: mtd layout (mtd3 = appfs, 0x490000), watchdog feeder alive, SD card is a real mmcblk vfat mount with >20 MB free; stop the
#       supervisor + app (the feeder is NOT touched); nothing may map /appfs/lib any more
#   (c) upload the new image AND the stock backup to the SD card (/mnt/sdcard/akam_flash) on the camera (md5 checked), then run a DETACHED
#       worker there (survives a lost Wi-Fi/SSH link): flashcp -v new -> /dev/mtd3, read back, md5 compare; on its own
#       mismatch it re-flashes the stock backup by itself (3 tries, flashcp then dd to /dev/mtdblock3)
#   (d) read mtd3 back to the PC and byte-compare (cmp) with the intended image BEFORE any reboot. NON-DESTRUCTIVE:
#       this step never writes flash (a rollback over a foreground ssh that drops = half-written mtd3 = brick). On a
#       mismatch or a failed read it aborts and tells the operator to launch the DETACHED rollback worker by hand
#   (e) ONLY after a verified match, and only with AKAM_NVRAM_WRITE=yes: nvram set appfs_sign <value>; nvram commit.
#       This is the FIRST nvram write ever done on this camera. The boot path does not check the sign (CRC-only
#       bootm), so skipping (e) is safe; the previous value is saved first. Without the variable (e) is only printed.
#
# UPDATE MODE (--update CURRENT_IMAGE): re-flash an Akam appfs that is ALREADY on mtd3. CURRENT_IMAGE is the image
# believed to be on mtd3 now (previous dist build or a saved copy, 0x490000 bytes). Differences from the default:
#   (a) mtd0/1/2/5 must still equal the stock backup; mtd3 must equal CURRENT_IMAGE byte for byte (else abort, nothing
#       written). The fresh mtd3 dump (== CURRENT_IMAGE) becomes the ROLLBACK source, staged on the SD card as
#       appfs-backup.bin; the stock backup is never the rollback source in this mode.
#   (b) the card must be the REAL one: boot with the Akam SD card INSERTED first (recovery mode). In a card-less boot
#       /mnt/sdcard is a tmpfs bind and the script refuses. The feeder must run from the card, not from /appfs.
# Without --update nothing changes: mtd3 must equal the stock backup, rollback = the stock backup.
#
# HOT UPDATE (--update CURRENT_IMAGE --hot): same update, but from a CARD-LESS boot (no SD card; /mnt/sdcard is a
# tmpfs bind; the feeder busybox, dropbear, wpa_supplicant and the scripts all run FROM /appfs, the squashfs that is
# about to be rewritten). It writes ONLY when every one of these is proven, otherwise it refuses with nothing written:
#   H1 card-less boot detected; every process that touches /appfs (exe, cwd, an open file or a mapping) is listed and
#      must be one of the known roles (stub, init.sh, supervisor, app, ntp helper, decloud keeper, feeder, dropbear,
#      wpa_supplicant); anything else = abort
#   H2 RELOCATION (reversible, no flash write): copies of busybox, dropbearmulti, wpa_supplicant (md5-checked, no
#      /appfs library mapped) in /tmp/akam_hot; a new feeder from /tmp is proven alive, then the old one is SIGKILLed
#      (no magic close: the watchdog stays armed); wpa_supplicant is restarted from /tmp (dead-man: rolled back unless
#      this script confirms through an SSH round trip); a second dropbear from /tmp (port 2222) takes over, the
#      /appfs ones are stopped, a /tmp one serves port 22 again; app, supervisor, ntp helper, keeper and the stub are
#      stopped. A rescan must then show NO process touching /appfs
#   H3 memory gate: MemFree > 2 x image + 3 MiB (staging in /tmp = RAM, there is no card), /tmp space likewise
#   then the normal update path (fresh mtd3 dump == CURRENT_IMAGE, detached worker, rollback worker, step (d)), staging
#   in /tmp/akam_flash. After success: the operator reboots; this script never does. Any failure before the write
#   restarts what was stopped (supervisor, ntp helper, keeper from their captured command lines).
#   The camera side is ONE detached on-camera runner (nohup, setsid, oom_score_adj -1000): the PC uploads, starts it, polls
#   /tmp/akam_hot/status and answers the checkpoints (wpa, dbalt, db22, upload, go); a missing answer within 120 s = the runner
#   restores by itself, nothing written. After the write starts it never waits for the PC.
#   --hot-preflight = H1..H3, then RESTORE: a reversible rehearsal that never writes flash (needs --update too).
#   Same gates as always: dry run by default, live = --execute + AKAM_I_HAVE_RECOVERY=yes (also for the rehearsal).
#
# Environment:
#   CAM                    ssh target, e.g. root@192.0.2.10 (required)
#   BACKUP_DIR             the full dump taken before anything (mtd0..mtd5.bin + SHA256SUMS) (required)
#   IMAGE_DIR              output of appfs/build_appfs.sh (default: <repo>/dist/rot2k-appfs)
#   WORK_DIR               where the fresh dumps and read-backs go (default: <repo>/dist/rot2k-flash-<UTC time>)
#   SSH_OPTS               extra ssh options (the defaults add timeouts and keepalives)
#   AKAM_HOT_SSH_PORT      --hot only: the temporary second dropbear port (default 2222)
#   AKAM_I_HAVE_RECOVERY   must be exactly "yes" (UART or CH341A available, backup verified)
#   AKAM_NVRAM_WRITE       "yes" to do step (e)
# shellcheck disable=SC2016  # the remote commands are single-quoted on purpose: they expand on the camera
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FW=$(cd "$HERE/.." && pwd)
REPO=$(cd "$FW/../.." && pwd)
PART=$((0x490000))
LIVE=0
EXEC=0

die() {
    echo "flash_appfs: ERROR: $*" >&2
    exit 1
}
say() { echo "flash_appfs: $*"; }
sha_of() { sha256sum "$1" | cut -d' ' -f1; }

UPDATE_IMG=""
UPDATE=0
HOT=0
HOTPRE=0
while [ $# -gt 0 ]; do
    a=$1
    shift
    case $a in
        --execute) EXEC=1 ;;
        --update)
            [ $# -gt 0 ] || die "--update needs CURRENT_IMAGE (the image believed to be on mtd3 now)"
            UPDATE=1
            UPDATE_IMG=$1
            shift ;;
        --hot) HOT=1 ;;
        --hot-preflight) HOT=1; HOTPRE=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument $a" ;;
    esac
done
[ "$HOT" = 0 ] || [ "$UPDATE" = 1 ] || die "--hot / --hot-preflight need --update CURRENT_IMAGE (a hot update only exists for an already flashed appfs)"
if [ "$EXEC" = 1 ]; then
    [ "${AKAM_I_HAVE_RECOVERY:-}" = yes ] || die "refusing to run live: AKAM_I_HAVE_RECOVERY=yes is not set (needs a UART or a CH341A at hand; see cardless-appfs.md)"
    LIVE=1
fi

CAM=${CAM:-}
BACKUP_DIR=${BACKUP_DIR:-}
IMAGE_DIR=${IMAGE_DIR:-$REPO/dist/rot2k-appfs}
WORK_DIR=${WORK_DIR:-$REPO/dist/rot2k-flash-$(date -u +%Y%m%dT%H%M%SZ)}
SSH_OPTS=${SSH_OPTS:-}
[ -n "$CAM" ] || die "CAM (e.g. root@192.0.2.10) is required"
[ -d "$BACKUP_DIR" ] || die "BACKUP_DIR is required (the verified full dump)"
# Aliasing guards: the fresh dump must never overwrite or be compared with itself, the backup or CURRENT_IMAGE.
if [ -e "$WORK_DIR" ]; then
    [ "$WORK_DIR" -ef "$BACKUP_DIR" ] && die "WORK_DIR is BACKUP_DIR: refused (the backup would be overwritten)"
    ls "$WORK_DIR"/mtd*.bin >/dev/null 2>&1 && die "WORK_DIR $WORK_DIR already holds mtd*.bin: use a fresh directory"
fi
if [ -n "$UPDATE_IMG" ] && [ -e "$WORK_DIR/mtd3.bin" ] && [ "$UPDATE_IMG" -ef "$WORK_DIR/mtd3.bin" ]; then
    die "--update image is the work dump itself: refused"
fi
IMG=$IMAGE_DIR/appfs-akam.squashfs
SIGN_FILE=$IMAGE_DIR/appfs_sign.txt

# ---- local, read-only checks: always run (also in a dry run) ----------------------------------------------------------
[ -f "$IMG" ] || die "$IMG not found (run appfs/build_appfs.sh)"
[ "$(wc -c <"$IMG")" = "$PART" ] || die "$IMG is not $PART bytes"
[ "$(head -c 4 "$IMG")" = hsqs ] || die "$IMG is not a squashfs"
want=$(cut -d' ' -f1 "$IMAGE_DIR/appfs-akam.squashfs.sha256")
[ "$(sha_of "$IMG")" = "$want" ] || die "$IMG does not match its .sha256 (rebuild it)"
want_md5=$(md5sum "$IMG" | cut -d' ' -f1)   # the camera has md5sum, not sha256sum: the operator read-back uses this
SIGN=$(tr -d '[:space:]' <"$SIGN_FILE")
case $SIGN in *[!0-9a-f]* | '') die "$SIGN_FILE does not hold a hex sign" ;; esac
[ "$(python3 -I "$HERE/calc_partition_sign.py" appfs "$IMG" | sed -n 's/.*sign=//p')" = "$SIGN" ] || die "appfs_sign does not match the image"
unsquashfs -s "$IMG" >/dev/null 2>&1 || die "$IMG does not parse as a squashfs"
for n in 0 1 2 3 4 5; do [ -f "$BACKUP_DIR/mtd$n.bin" ] || die "$BACKUP_DIR/mtd$n.bin missing"; done
(cd "$BACKUP_DIR" && sha256sum -c SHA256SUMS >/dev/null 2>&1) || die "the backup does not match $BACKUP_DIR/SHA256SUMS"
[ "$(wc -c <"$BACKUP_DIR/mtd3.bin")" = "$PART" ] && [ "$(head -c 4 "$BACKUP_DIR/mtd3.bin")" = hsqs ] || die "backup mtd3.bin is not a full appfs dump"
BACKUP3=$BACKUP_DIR/mtd3.bin
if [ "$UPDATE" = 1 ]; then
    [ -f "$UPDATE_IMG" ] || die "--update: $UPDATE_IMG not found"
    [ "$(wc -c <"$UPDATE_IMG")" = "$PART" ] && [ "$(head -c 4 "$UPDATE_IMG")" = hsqs ] || die "--update: $UPDATE_IMG is not a full $PART byte appfs squashfs"
    ! cmp -s "$UPDATE_IMG" "$IMG" || die "--update: CURRENT_IMAGE is identical to the new image: nothing to update"
    ROLLNAME="the previous image (CURRENT_IMAGE, fresh mtd3 dump)"
else
    ROLLNAME="the stock backup"
fi
say "image   $IMG"
say "  sha256 $(sha_of "$IMG")  appfs_sign $SIGN"
say "backup  $BACKUP3 (sha256 $(sha_of "$BACKUP3"))"
if [ "$UPDATE" = 1 ]; then
    say "UPDATE MODE: mtd3 must equal $UPDATE_IMG (sha256 $(sha_of "$UPDATE_IMG")); rollback source = that image, NOT the stock backup"
    if [ "$HOT" = 1 ]; then
        say "HOT MODE$( [ "$HOTPRE" = 1 ] && echo " (rehearsal: relocate, check, RESTORE, never writes flash)" ): card-less boot required (no SD card); staging in RAM (/tmp/akam_flash)"
    else
        say "UPDATE MODE needs the Akam SD card inserted at boot (recovery): insert the Akam card and reboot first"
    fi
fi
STAGE=/mnt/sdcard/akam_flash
[ "$HOT" = 0 ] || STAGE=/tmp/akam_flash
# rollback source staged on the SD: the stock backup, or (update mode) the fresh mtd3 dump verified == CURRENT_IMAGE
ROLLSRC_FILE=$BACKUP3
[ "$UPDATE" = 0 ] || ROLLSRC_FILE=$WORK_DIR/mtd3.bin

# ---- run helpers ---------------------------------------------------------------------------------------------------------
CAM_HOST=${CAM#*@}
mk_ssh() { # mk_ssh [port]: (re)build the ssh command; a port selects the temporary --hot dropbear (same host key: alias)
    # shellcheck disable=SC2206  # SSH_OPTS is a deliberate word list
    SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3 $SSH_OPTS ${1:+-p "$1" -o "HostKeyAlias=$CAM_HOST"} "$CAM")
}
mk_ssh
step() { # step <text>: header
    echo
    say "== $* =="
}
cam() { # cam <remote command>: run on the camera (live) or print (dry run)
    if [ "$LIVE" = 1 ]; then "${SSH[@]}" "$1"; else say "[dry-run] ssh $CAM: $1"; fi
}
camq() { "${SSH[@]}" "$1"; } # live only
if [ "$LIVE" = 1 ]; then
    mkdir -p "$WORK_DIR"
    say "LIVE run. Work dir $WORK_DIR"
    if [ "$HOT" = 1 ]; then
        say "Last chance: Ctrl-C within 15 s. Needs: camera on stable power, CARD-LESS boot. This stops the app, the supervisor, the stub and moves the feeder, Wi-Fi and SSH to /tmp. The whole sequence then runs as ONE detached job on the camera: a Ctrl-C here is safe, the camera restores by itself within 120 s when a confirmation does not arrive (or by hand: ssh $CAM 'sh /tmp/akam_hot/akam_hot.sh restore'; or power-cycle: nothing was written)."
    else
        say "Last chance: Ctrl-C within 15 s. Needs: camera on stable power, Akam SD card INSERTED (do not remove it until this script finishes)."
    fi
    say "Recovery net: the SD card only recovers an appfs that MOUNTS. A power cut or crash during the 1-2 min write leaves mtd3 half written: then only UART (sf write) or a CH341A can recover it. AKAM_I_HAVE_RECOVERY=yes means you accept that."
    sleep 15
else
    say "DRY RUN: nothing below is executed. Live = AKAM_I_HAVE_RECOVERY=yes $0 --execute"
fi

dump() { # dump <n> <outfile>
    camq "dd if=/dev/mtd$1 bs=65536 2>/dev/null" >"$2"
}

# ---- --hot: camera-side helper (uploaded to /tmp/akam_hot) and the host-side steps ------------------------------------
HOTD=/tmp/akam_hot
HOTSH=$HOTD/akam_hot.sh
HOT_PORT=${AKAM_HOT_SSH_PORT:-2222}
MEM_MARGIN_KB=3072
HOT_NEED_KB=$((2 * PART / 1024 + MEM_MARGIN_KB))   # new image + rollback dump + margin, all in RAM
HOT_TMP_KB=$((2 * PART / 1024 + 1024))             # the tmpfs itself must be able to hold both
read -r -d '' HOT_REMOTE <<'AKAM_HOT_EOF' || true
#!/bin/sh
# shellcheck disable=SC2010,SC2012,SC2015,SC2086,SC2046  # ls -l is the only portable way to read a link target on this BusyBox
# akam_hot.sh -- CAMERA side of flash_appfs.sh --hot / --hot-preflight (uploaded to /tmp/akam_hot/). POSIX sh (BusyBox ash).
# Subcommands: run MODE NEED_KB TMP_KB FINAL_KB (the detached runner: MODE hot|rehearsal) and the single steps it calls:
#              cardless tools scan [free] feedercheck capture stage feeder wpaswap hasdb dbstart PORT dbstop_old
#              dbkill PORT stopapp memgate NEED_KB TMP_KB final NEED_KB restore
# The runner is the ONE detached on-camera job: the PC only uploads, starts it, polls $D/status and answers the
# confirmation checkpoints (wpa, dbalt, db22, upload, go) by touching $D/confirm.<name>. No answer within
# AKAM_CONFIRM_S (120 s) = the runner restores by itself and exits, nothing written.
# NEVER writes flash, nvram or /config. Writes only under $D (/tmp/akam_hot). Overridable for the host test:
# AKAM_PROC (/proc), AKAM_HOT_DIR, AKAM_MOUNTS (/proc/mounts), AKAM_MEMINFO, AKAM_SD (/mnt/sdcard), AKAM_FLASHDIR,
# AKAM_DROP (drop_caches), AKAM_PANIC (panic_on_oom), AKAM_OOMFILE, AKAM_SOAK, AKAM_CONFIRM_S, AKAM_WORKER,
# AKAM_FLASH_RES, AKAM_FLASH_LOG.
P=${AKAM_PROC:-/proc}
D=${AKAM_HOT_DIR:-/tmp/akam_hot}
UNSAFE='[^A-Za-z0-9_./:=@%+,-]'
FD=${AKAM_FLASHDIR:-/tmp/akam_flash}
ST=$D/status
CONFIRM_S=${AKAM_CONFIRM_S:-120}
OWNLOCK=0
SOAK=${AKAM_SOAK:-25}   # feeder soak after the old feeder is killed: longer than the watchdog timeout (-T 20)
RESTORE=0

# oom_protect: this process (and what it spawns: the feeder, flashcp) is never the OOM victim. Only when
# panic_on_oom is 0: with panic_on_oom set, an unkillable victim would panic the kernel instead.
oom_protect() {
    pv=$(cat "${AKAM_PANIC:-/proc/sys/vm/panic_on_oom}" 2>/dev/null)
    if [ "$pv" = 0 ]; then
        if echo -1000 >"${AKAM_OOMFILE:-/proc/$$/oom_score_adj}" 2>/dev/null; then echo "oom_score_adj -1000 set for pid $$"; else echo "oom_score_adj not writable: not protected"; fi
    else
        echo "panic_on_oom is '${pv:-unreadable}', not 0: oom_score_adj left alone"
    fi
}
dropcaches() { sync; echo 3 >"${AKAM_DROP:-/proc/sys/vm/drop_caches}" 2>/dev/null || echo "drop_caches not writable"; }
memlog() { # memlog <label>: MemFree and MemAvailable at a gate
    mi=${AKAM_MEMINFO:-/proc/meminfo}
    m1=$(sed -n 's/^MemFree:[[:space:]]*\([0-9]*\) kB.*/\1/p' "$mi")
    m2=$(sed -n 's/^MemAvailable:[[:space:]]*\([0-9]*\) kB.*/\1/p' "$mi")
    echo "mem[$1]: MemFree ${m1:-?} kB, MemAvailable ${m2:-n/a} kB"
}
st() { # st <PHASE> <message>: the status file the PC polls (atomic) and the log
    echo "$1|$2" >"$ST.tmp" && mv -f "$ST.tmp" "$ST"
    echo "[$1] $2"
}
wait_confirm() { # wait_confirm <name> <message>: 0 when the PC touches $D/confirm.<name> within CONFIRM_S
    rm -f "$D/confirm.$1"
    st "WAIT_$1" "$2"
    t=0
    while [ "$t" -lt "$CONFIRM_S" ]; do
        [ ! -e "$D/confirm.$1" ] || { echo "confirmed by the PC: $1"; return 0; }
        sleep 1
        t=$((t + 1))
    done
    echo "no confirmation '$1' from the PC within $CONFIRM_S s"
    return 1
}

lnk() { ls -l "$1" 2>/dev/null | sed -n 's/^.* -> //p'; }
cmdl() { tr '\0' ' ' <"$P/$1/cmdline" 2>/dev/null | sed 's/ *$//'; }
pids() { for d in "$P"/[0-9]*; do [ -d "$d" ] && echo "${d##*/}"; done; }

# refs <pid>: why this process depends on the appfs squashfs (empty = it does not). Own cmdline is NOT looked at.
refs() {
    r=""
    ls -l "$P/$1/exe" 2>/dev/null | grep -q -- '-> /appfs/' && r="$r exe"
    ls -l "$P/$1/cwd" 2>/dev/null | grep -q -- '-> /appfs' && r="$r cwd"
    ls -l "$P/$1/fd/" 2>/dev/null | grep -q -- '-> /appfs/' && r="$r fd"
    grep -q '/appfs/' "$P/$1/maps" 2>/dev/null && r="$r maps"
    echo "$r"
}

role() {
    c=$(cmdl "$1")
    e=$(lnk "$P/$1/exe")
    case $c in
        *own_app_stub.sh*) echo stub ;;
        */appfs/script/init.sh*) echo init ;; # the stock init loop (resident): polls yy_debug.ini and an OTA trigger
        *own_app_run.sh*) echo supervisor ;;
        *akam_ntp.sh*) echo ntp ;;
        *decloud.sh*) echo keeper ;;
        *akam2k_rtsp*) echo app ;;
        *wpa_supplicant*) echo wpa ;;
        *)
            case $e in
                */dropbearmulti) echo dropbear ;;
                */busybox) case $c in *" watchdog "*) echo feeder ;; *) echo UNEXPECTED ;; esac ;;
                *) echo UNEXPECTED ;;
            esac ;;
    esac
}

pids_of() { # pids_of <role> [norefs]: live processes of a role (norefs: only those that do not touch /appfs)
    for p in $(pids); do
        [ "$p" = "$$" ] && continue
        [ "$(role "$p")" = "$1" ] || continue
        [ "$2" != norefs ] || [ -z "$(refs "$p")" ] || continue
        echo "$p"
    done
}

gone() { # gone <seconds> <pid...>: wait until none of the pids lives
    t=$1
    shift
    while [ "$t" -ge 0 ]; do
        l=""
        for p in "$@"; do kill -0 "$p" 2>/dev/null && l="$l $p"; done
        [ -n "$l" ] || return 0
        t=$((t - 1))
        sleep 1
    done
    return 1
}

stop_pids() { # stop_pids <wait s> <pid...>: SIGTERM, wait, SIGKILL the rest
    w=$1
    shift
    [ $# -gt 0 ] || return 0
    for p in "$@"; do kill -TERM "$p" 2>/dev/null; done
    gone "$w" "$@" && return 0
    for p in "$@"; do kill -KILL "$p" 2>/dev/null; done
    gone 5 "$@"
}

cap_file() { ls "$D"/cap."$1".*.argv 2>/dev/null | head -n 1; }

# spawn <argvfile> <argv0> <fg|bg>: run a captured command line with argv[0] replaced
spawn() {
    af=$1
    a0=$2
    md=$3
    set -- "$a0"
    first=1
    while IFS= read -r a; do
        if [ "$first" = 1 ]; then first=0; continue; fi
        set -- "$@" "$a"
    done <"$af"
    if [ "$md" = fg ]; then
        "$@" >"$D/spawn.out" 2>&1 </dev/null
    else
        "$@" >/dev/null 2>&1 </dev/null &
    fi
}

# replay <prefix>: restart a captured process exactly (cmdline + its AKAM_* environment), detached
replay() {
    pre=$1
    set --
    while IFS= read -r a; do set -- "$@" "$a"; done <"$pre.argv"
    (
        cd / || exit 1
        if [ -s "$pre.env" ]; then
            set -a
            # shellcheck disable=SC1090  # validated KEY=safe-value lines written by capture
            . "$pre.env"
            set +a
        fi
        exec "$@"
    ) >/dev/null 2>&1 </dev/null &
}

# netok: wlan0 up with an IPv4 address and a LAN route. NOT a default route: the decloud keeper replaces it with the
# blackhole pair 0.0.0.0/1 + 128.0.0.0/1, so a default-route check can never pass on an Akam camera.
netok() {
    [ "$(cat /sys/class/net/wlan0/operstate 2>/dev/null)" = up ] || return 1
    ip addr show wlan0 2>/dev/null | grep -q 'inet ' || return 1
    ip route show dev wlan0 2>/dev/null | grep -q 'scope link'
}
waitnet() { i=0; while [ "$i" -lt "$1" ]; do netok && return 0; sleep 1; i=$((i + 1)); done; return 1; }

cmd_cardless() {
    m=${AKAM_MOUNTS:-/proc/mounts}
    if grep -q '^/dev/mmcblk' "$m" 2>/dev/null; then echo "an mmcblk device is mounted: this is not a card-less boot"; exit 1; fi
    l=$(awk '$2=="/mnt/sdcard"{print $1" "$3}' "$m" 2>/dev/null | tail -n 1)
    [ "$l" = "tmpfs tmpfs" ] || { echo "/mnt/sdcard is '$l', not the tmpfs bind of a card-less boot"; exit 1; }
    ls -l "${AKAM_SD:-/mnt/sdcard}/busybox" 2>/dev/null | grep -q -- '-> /appfs/akam/busybox' || { echo "no Akam flash tree on /mnt/sdcard"; exit 1; }
    echo "card-less: /mnt/sdcard = tmpfs bind of the Akam flash tree, no mmcblk mounted"
}

cmd_tools() { # the tools the flash path runs must not come from /appfs
    for t in flashcp md5sum nohup dd sh cat awk sed ip cut sync mkdir rmdir sleep; do
        x=$(command -v "$t") || { echo "missing on the camera: $t"; exit 1; }
        echo "$t $x"
        case $x in /appfs/*) echo "$t resolves into /appfs"; exit 1 ;; esac
        if ls -l "$x" 2>/dev/null | grep -q -- '-> .*/appfs/'; then echo "$t is a link into /appfs"; exit 1; fi
    done
}

cmd_scan() { # scan [free]: list every process touching /appfs; exit 2 when one has no known role; free: exit 4 if any
    bad=0
    n=0
    for p in $(pids); do
        [ "$p" = "$$" ] && continue
        w=$(refs "$p")
        [ -n "$w" ] || continue
        ro=$(role "$p")
        n=$((n + 1))
        echo "$p $ro [$w ] $(cmdl "$p")"
        [ "$ro" != UNEXPECTED ] || bad=1
    done
    if [ "$1" = free ]; then
        [ "$n" -eq 0 ] || exit 4
        echo "no process touches /appfs"
        return 0
    fi
    [ "$bad" = 0 ] || exit 2
}

cmd_feedercheck() {
    n=$(pids_of feeder norefs | wc -l)
    [ "$n" -ge 1 ] || { echo "no watchdog feeder runs from outside /appfs"; exit 1; }
    echo "feeder from outside /appfs alive: $(pids_of feeder norefs | tr '\n' ' ')"
}

cmd_capture() {
    mkdir -p "$D" || exit 1
    rm -f "$D"/cap.*
    for p in $(pids); do
        [ "$p" = "$$" ] && continue
        [ -n "$(refs "$p")" ] || continue
        ro=$(role "$p")
        case $ro in supervisor | ntp | keeper | feeder | wpa | dropbear) ;; *) continue ;; esac
        f=$D/cap.$ro.$p
        tr '\0' '\n' <"$P/$p/cmdline" >"$f.argv" 2>/dev/null
        if grep -q "$UNSAFE" "$f.argv"; then echo "pid $p ($ro): an argument has characters that cannot be replayed safely: refused"; exit 3; fi
        lnk "$P/$p/exe" >"$f.exe"
        : >"$f.env"
        case $ro in supervisor | ntp | keeper)
            tr '\0' '\n' <"$P/$p/environ" 2>/dev/null | grep '^AKAM_' >"$f.env"
            if grep -q -v '^AKAM_[A-Za-z0-9_]*=[A-Za-z0-9_./:=@%+,-]*$' "$f.env"; then echo "pid $p ($ro): an AKAM_ variable cannot be replayed safely: refused"; exit 3; fi ;;
        esac
        echo "captured $ro pid $p"
    done
}

cmd_hasdb() { [ -n "$(cap_file dropbear)" ]; }

cmd_stage() { # copy the three relocatable binaries to $D, md5-checked, no other /appfs mapping allowed
    for f in "$D"/cap.feeder.*.argv "$D"/cap.dropbear.*.argv "$D"/cap.wpa.*.argv; do
        [ -f "$f" ] || continue
        p=${f#"$D"/cap.*.}
        p=${p%.argv}
        src=$(cat "${f%.argv}.exe")
        dst=$D/${src##*/}
        if [ -x "$dst" ]; then
            if [ "$(md5sum <"$src")" = "$(md5sum <"$dst")" ]; then echo "reused $dst (md5 equal)"; continue; fi
            echo "$dst differs from $src: copying again"
            rm -f "$dst"
        fi
        if grep '/appfs/' "$P/$p/maps" 2>/dev/null | grep -q -v -F "$src"; then
            echo "pid $p maps another file from /appfs besides $src (a library): cannot be relocated: refused"
            exit 1
        fi
        cp "$src" "$dst" && chmod 755 "$dst" || { echo "cannot copy $src"; exit 1; }
        [ "$(md5sum <"$src")" = "$(md5sum <"$dst")" ] || { echo "copy of $src differs"; exit 1; }
        echo "staged $dst ($(wc -c <"$dst") bytes, md5 equal)"
    done
}

cmd_feeder() {
    f=$(cap_file feeder)
    [ -n "$f" ] || { echo "feeder: none runs from /appfs, nothing to do"; return 0; }
    oom_protect
    olds=$(pids_of feeder)
    olds_app=""
    for p in $olds; do [ -z "$(refs "$p")" ] || olds_app="$olds_app $p"; done
    spawn "$f" "$D/busybox" bg
    new=$!
    sleep 3
    if kill -0 "$new" 2>/dev/null; then
        echo "feeder: the new one (pid $new) opened the watchdog while the old one runs (shared open allowed)"
        sleep 4
    else
        echo "feeder: the new one exited (watchdog is exclusive): handing over"
        # SIGKILL, never SIGTERM: no magic close, so the timer keeps running during the 1 s gap
        for p in $olds_app; do kill -KILL "$p" 2>/dev/null; done
        gone 5 $olds_app
        spawn "$f" "$D/busybox" bg
        new=$!
        sleep 3
        if ! kill -0 "$new" 2>/dev/null; then
            spawn "$f" "$(cat "${f%.argv}.exe")" bg
            fb=$!
            sleep 2
            if kill -0 "$fb" 2>/dev/null; then
                echo "feeder: FAILED to start a feeder from outside /appfs; the old command was restarted (pid $fb, alive)"
            else
                echo "feeder: CRITICAL: FAILED to start a feeder from /tmp AND the fallback feeder (the old command) is NOT alive: no watchdog feeder runs, the camera may reset soon"
            fi
            exit 1
        fi
        echo "feeder: handed over, new pid $new"
    fi
    kill -0 "$new" 2>/dev/null || { echo "feeder: the new feeder died"; exit 1; }
    for p in $olds_app; do kill -KILL "$p" 2>/dev/null; done
    gone 5 $olds_app || { echo "feeder: an old feeder did not die"; exit 1; }
    # soak: the new feeder alone must keep running longer than the watchdog timeout (-T 20) after the old one is gone
    i=0
    while [ "$i" -lt "$SOAK" ]; do
        kill -0 "$new" 2>/dev/null || { echo "feeder: the new feeder died ${i} s after the old one was stopped"; exit 1; }
        sleep 1
        i=$((i + 1))
    done
    echo "$new" >"$D/feeder.pid"
    echo "feeder: running from /tmp (pid $new, soaked ${SOAK} s alone), old one(s) stopped:$olds_app"
}

# wpaswap: stop the /appfs wpa_supplicant, start the /tmp copy with the same arguments, wait for the PC's confirmation
# (a fresh ssh round trip proves Wi-Fi + SSH work). No confirmation within CONFIRM_S, or no network = the old one
# (still on the untouched /appfs) is restarted and 1 is returned. Nothing to do (none runs from /appfs: already
# relocated, e.g. after a rehearsal) = 0 at once, no checkpoint.
cmd_wpaswap() {
    f=$(cap_file wpa)
    if [ -z "$f" ]; then echo "wpa: none runs from /appfs (already on /tmp): skipped"; return 0; fi
    old=""
    for p in $(pids_of wpa); do [ -z "$(refs "$p")" ] || old="$old $p"; done
    exe0=$(cat "${f%.argv}.exe")
    # shellcheck disable=SC2086  # pid list
    stop_pids 10 $old
    sleep 2 # let the Wi-Fi driver release the interface before the new instance claims it
    a=0
    ok=0
    while [ "$a" -lt 3 ]; do
        a=$((a + 1))
        if spawn "$f" "$D/wpa_supplicant" fg; then ok=1; break; fi
        echo "wpa: attempt $a failed, its output:"
        cat "$D/spawn.out" 2>/dev/null
        sleep 3
    done
    if [ "$ok" = 1 ] && [ -z "$(pids_of wpa norefs)" ]; then
        echo "wpa: spawned but not seen as a /tmp process; ps:"
        # shellcheck disable=SC2009  # BusyBox ps output, diagnostic only
        ps | grep "[w]pa_supplicant"
    fi
    sleep 3
    if [ "$ok" = 1 ] && [ -n "$(pids_of wpa norefs)" ] && waitnet 60; then
        echo "wpa: running from /tmp, network up; waiting for the PC's confirmation"
        wait_confirm wpa "wpa_supplicant runs from /tmp, network up: confirm with a fresh ssh round trip" && return 0
        echo "wpa: NOT confirmed within $CONFIRM_S s: rolling back"
    else
        echo "wpa: the new wpa_supplicant did not come up: rolling back"
    fi
    # shellcheck disable=SC2046  # pid list
    stop_pids 10 $(pids_of wpa norefs)
    spawn "$f" "$exe0" fg
    waitnet 60
    echo "wpa: rolled back to the /appfs copy"
    return 1
}

dbspawn() { # dbspawn <port>: a dropbear from the copy in /tmp with the captured arguments, port and pid file replaced
    f=$(cap_file dropbear)
    [ -n "$f" ] || return 1
    port=$1
    bin=$D/dropbearmulti
    [ -x "$bin" ] || bin=$(cat "${f%.argv}.exe")
    set -- "$bin"
    first=1
    skip=""
    sp=0
    sP=0
    while IFS= read -r a; do
        if [ "$first" = 1 ]; then first=0; continue; fi
        if [ -n "$skip" ]; then
            if [ "$skip" = p ]; then set -- "$@" "$port"; else set -- "$@" "$D/db$port.pid"; fi
            skip=""
            continue
        fi
        case $a in -p) skip=p; sp=1 ;; -P) skip=P; sP=1 ;; esac
        set -- "$@" "$a"
    done <"$f"
    [ "$sp" = 1 ] || set -- "$@" -p "$port"
    [ "$sP" = 1 ] || set -- "$@" -P "$D/db$port.pid"
    rm -f "$D/db$port.pid"
    "$@" >"$D/db$port.log" 2>&1 </dev/null
}

cmd_dbstart() {
    i=0
    while [ "$i" -lt 4 ]; do
        i=$((i + 1))
        dbspawn "$1" || { echo "dropbear: no captured command line"; exit 1; }
        sleep 2
        if [ -s "$D/db$1.pid" ] && kill -0 "$(cat "$D/db$1.pid")" 2>/dev/null; then echo "dropbear from /tmp on port $1, pid $(cat "$D/db$1.pid")"; return 0; fi
    done
    echo "dropbear on port $1 did not start: $(tail -n 3 "$D/db$1.log" 2>/dev/null)"
    exit 1
}

cmd_dbkill() { # only the listener of this port (its sessions live on)
    [ -s "$D/db$1.pid" ] && kill "$(cat "$D/db$1.pid")" 2>/dev/null
    echo "dropbear listener on port $1 stopped"
}

anc_refs() { # does this shell's own ancestry touch /appfs (= it would kill itself)?
    q=$$
    while [ -n "$q" ] && [ "$q" -gt 1 ]; do
        [ -z "$(refs "$q")" ] || return 0
        q=$(sed -n 's/^PPid:[[:space:]]*//p' "$P/$q/status" 2>/dev/null)
    done
    return 1
}

cmd_dbstop_old() {
    if anc_refs; then echo "refusing: this session itself runs from /appfs (use the second dropbear)"; exit 3; fi
    l=""
    for p in $(pids_of dropbear); do [ -z "$(refs "$p")" ] || l="$l $p"; done
    # shellcheck disable=SC2086  # pid list
    stop_pids 8 $l
    echo "old dropbear processes stopped:$l"
}

cmd_stopapp() {
    cmd_feedercheck || exit 1
    # shellcheck disable=SC2046  # pid lists
    stop_pids 25 $(pids_of supervisor) && stop_pids 12 $(pids_of app) && stop_pids 6 $(pids_of ntp) &&
        stop_pids 6 $(pids_of keeper) || { echo "a process did not stop"; exit 1; }
    # KILL: the stub's TERM trap would signal the feeder; init.sh only polls yy_debug.ini and an OTA trigger
    for p in $(pids_of stub) $(pids_of init); do kill -KILL "$p" 2>/dev/null; done
    # shellcheck disable=SC2046  # pid list
    gone 5 $(pids_of stub) $(pids_of init) || { echo "the stub or init.sh did not stop"; exit 1; }
    echo "supervisor, app, ntp helper, keeper, stub and init.sh stopped"
}

cmd_memgate() { # memgate <need kB> <tmp kB>
    dropcaches
    mi=${AKAM_MEMINFO:-/proc/meminfo}
    memlog "memory gate"
    mf=$(sed -n 's/^MemFree:[[:space:]]*\([0-9]*\) kB.*/\1/p' "$mi")
    tf=$(df -k /tmp 2>/dev/null | awk 'END{print $4}')
    echo "MemFree ${mf:-?} kB (need > $1), /tmp free ${tf:-?} kB (need > $2)"
    [ "${mf:-0}" -gt "$1" ] || { echo "not enough free memory"; exit 1; }
    [ "${tf:-0}" -gt "$2" ] || { echo "not enough space in /tmp"; exit 1; }
    mkdir -p "$FD" || exit 1
}

cmd_final() { # last gate before the write: nothing touches /appfs, a feeder runs, memory is left
    cmd_scan free || exit 1
    cmd_feedercheck || exit 1
    [ -s "${AKAM_WORKER:-/tmp/akam_flash_worker.sh}" ] && [ -s "${AKAM_RBWORKER:-/tmp/akam_rollback_worker.sh}" ] || { echo "the flash worker or the rollback worker script is missing"; exit 1; }
    dropcaches
    mi=${AKAM_MEMINFO:-/proc/meminfo}
    memlog "final gate"
    mf=$(sed -n 's/^MemFree:[[:space:]]*\([0-9]*\) kB.*/\1/p' "$mi")
    echo "MemFree after staging: ${mf:-?} kB (need > $1)"
    [ "${mf:-0}" -gt "$1" ] || exit 1
}

cmd_restore() { # restart what the relocation stopped; feeder, wpa_supplicant and dropbear stay on their /tmp copies
    [ -d "$D" ] || { echo "nothing captured"; exit 0; }
    rm -rf "$FD"   # frees the staged images (~9.3 MB of RAM)
    for ro in supervisor ntp keeper; do
        for pre in "$D"/cap."$ro".*.argv; do
            [ -f "$pre" ] || continue
            if [ -z "$(pids_of "$ro")" ]; then replay "${pre%.argv}"; echo "restarted $ro"; fi
            break
        done
    done
    if [ -z "$(pids_of feeder)" ]; then
        f=$(cap_file feeder)
        [ -z "$f" ] || { oom_protect; spawn "$f" "$D/busybox" bg; echo "restarted a feeder"; }
    fi
    # a port-22 listener: by the pid file of the one this helper started, else by an /appfs one still on port 22
    p22=0
    if [ -s "$D/db22.pid" ] && kill -0 "$(cat "$D/db22.pid")" 2>/dev/null; then p22=1; fi
    for p in $(pids_of dropbear); do
        case "$(cmdl "$p") " in *" -p 22 "*) p22=1 ;; esac
    done
    if [ "$p22" = 0 ] && [ -n "$(cap_file dropbear)" ]; then cmd_dbstart 22 || exit 1; fi
    cmd_dbkill "${AKAM_HOT_PORT:-2222}" >/dev/null   # the temporary listener of a failed swap
    if [ -z "$(pids_of wpa)" ]; then echo "restore: NO wpa_supplicant is running: the camera has no Wi-Fi"; exit 5; fi
    echo "restore done (the stub is not restarted: its feeder/Wi-Fi/health job is done; a reboot returns to the normal state)"
}

# ---- the runner: ONE detached job, the PC only uploads, polls $ST and answers the checkpoints ----------------------------
do_restore() { # status word for the outcome of a restore: RESTORED | RESTORED_NO_WIFI | RESTORE_FAILED
    (cmd_restore) >&2
    case $? in 0) echo RESTORED ;; 5) echo RESTORED_NO_WIFI ;; *) echo RESTORE_FAILED ;; esac
}
runner_exit() { # EXIT trap: killed or failed with the relocation in place and no write started = restore
    if [ "$RESTORE" = 1 ]; then
        RESTORE=0
        echo "runner leaving with the relocation in place and no write: restoring"
        rs=$(do_restore)
        st "ABORTED_$rs" "runner interrupted, $rs"
    fi
    [ "$OWNLOCK" != 1 ] || rm -rf "$D/run.lock"
}
abort() { # abort <message>: a failure before the write
    echo "ABORT: $1"
    if [ "$RESTORE" = 1 ]; then
        RESTORE=0
        rs=$(do_restore)
        st "ABORTED_$rs" "$1 ($rs, nothing written)"
    else
        st ABORTED_UNCHANGED "$1 (nothing changed, nothing written)"
    fi
    exit 1
}

cmd_run() { # run <hot|rehearsal> <need kB> <tmp kB> <final kB>
    mode=$1
    need=$2
    tmpk=$3
    fin=$4
    port=${AKAM_HOT_PORT:-2222}
    case $mode in hot | rehearsal) ;; *) echo "run: mode must be hot or rehearsal"; exit 64 ;; esac
    cd / || exit 1
    mkdir -p "$D" || exit 1
    # single instance: the lock holds the runner's pid; a lock whose pid is dead is taken over (an empty pid = alive)
    if ! mkdir "$D/run.lock" 2>/dev/null; then
        lp=$(cat "$D/run.lock/pid" 2>/dev/null)
        if [ -z "$lp" ] || grep -q akam_hot "/proc/$lp/cmdline" 2>/dev/null; then
            echo "another runner (pid ${lp:-unknown}) holds $D/run.lock: refusing to start a second one"
            exit 1
        fi
        echo "stale run.lock (pid $lp is dead): taking it over"
        rm -rf "$D/run.lock"
        mkdir "$D/run.lock" 2>/dev/null || { echo "cannot take run.lock"; exit 1; }
    fi
    echo $$ >"$D/run.lock/pid"
    OWNLOCK=1
    rm -f "$D"/confirm.*
    trap runner_exit EXIT
    trap 'exit 143' INT TERM HUP
    oom_protect
    st H1 "card-less boot, tools, feeder, process scan"
    memlog H1
    (cmd_cardless) || abort "not a card-less boot (an SD card is mounted or the Akam flash tree is not on /mnt/sdcard)"
    (cmd_tools) || abort "a tool the flash path needs is missing or lives in /appfs"
    [ -n "$(pids_of feeder)" ] || abort "no watchdog feeder running: a write of 1-2 minutes without one = reset mid-write"
    rc=0
    (cmd_scan) || rc=$?
    case $rc in
        0) ;;
        2) abort "a process with no known role touches /appfs (listed above): cannot prove the update safe" ;;
        *) abort "the process scan failed (status $rc)" ;;
    esac
    (cmd_capture) || abort "cannot capture the command lines for a restore"
    RESTORE=1
    st H2 "stage the /tmp copies"
    (cmd_stage) || abort "cannot stage the binaries in /tmp"
    st H2 "feeder relocation"
    (cmd_feeder) || abort "the feeder relocation failed"
    memlog "after feeder"
    st H2 "wpa_supplicant swap"
    (cmd_wpaswap) || abort "the wpa_supplicant swap failed or was not confirmed by the PC (the old one is back)"
    if cmd_hasdb; then
        st H2 "dropbear from /tmp on port $port"
        (cmd_dbstart "$port") || abort "no second dropbear from /tmp"
        wait_confirm dbalt "dropbear from /tmp answers on port $port: confirm through it" || abort "the PC did not confirm the dropbear on port $port"
        (cmd_dbstop_old) || abort "cannot stop the dropbear that runs from /appfs"
        (cmd_dbstart 22) || abort "no dropbear from /tmp on port 22"
        wait_confirm db22 "dropbear from /tmp serves port 22: confirm through it" || abort "the PC did not confirm port 22 after the dropbear swap"
        (cmd_dbkill "$port") || true
    else
        echo "no dropbear runs from /appfs: nothing to relocate"
    fi
    st H2 "stop app, supervisor, ntp helper, keeper, stub"
    (cmd_stopapp) || abort "could not stop the app, supervisor, ntp helper, keeper or stub"
    (cmd_scan free) || abort "something still touches /appfs (listed above): refused"
    st H3 "memory gate"
    (cmd_memgate "$need" "$tmpk") || abort "memory gate failed even after the relocation"
    if [ "$mode" = rehearsal ]; then
        st H3 "rehearsal: gates passed, restoring (nothing is written)"
        RESTORE=0
        rs=$(do_restore)
        case $rs in
            RESTORED) st DONE_RESTORED "rehearsal passed; everything restored"; exit 0 ;;
            RESTORED_NO_WIFI) st DONE_RESTORED_NO_WIFI "rehearsal passed, restored, but NO wpa_supplicant runs"; exit 1 ;;
        esac
        st DONE_RESTORE_FAILED "rehearsal passed but the restore failed"
        exit 1
    fi
    wait_confirm upload "relocation done, memory gate passed: upload the two images to $FD" || abort "the PC did not upload the images in time"
    [ -s "$FD/appfs-new.bin" ] && [ -s "$FD/appfs-backup.bin" ] || abort "the staged images are missing"
    wait_confirm go "images staged and md5-checked by the PC: final go" || abort "the PC gave no final go in time"
    st FINAL "final gate"
    (cmd_final "$fin") || abort "the final gate failed (something touches /appfs again, no feeder, or the staged images ate the memory)"
    RESTORE=0   # from here NOTHING is restarted and NOTHING waits for the PC: a write may be running
    trap '' TERM INT HUP   # shield the write: ignored dispositions are inherited by the worker and flashcp
    st WRITING "mtd3 write started: no PC confirmation is waited for from here on"
    wk=${AKAM_WORKER:-/tmp/akam_flash_worker.sh}
    rf=${AKAM_FLASH_RES:-/tmp/akam_flash.result}
    rm -f "$rf"
    sh "$wk" >"${AKAM_FLASH_LOG:-/tmp/akam_flash.log}" 2>&1 </dev/null
    res=$(cat "$rf" 2>/dev/null)
    st WRITTEN "${res:-NONE}"
}

c=$1
shift
case $c in
    cardless) cmd_cardless ;;
    tools) cmd_tools ;;
    scan) cmd_scan "$@" ;;
    feedercheck) cmd_feedercheck ;;
    capture) cmd_capture ;;
    hasdb) cmd_hasdb ;;
    stage) cmd_stage ;;
    feeder) cmd_feeder ;;
    dbstart) cmd_dbstart "$@" ;;
    dbstop_old) cmd_dbstop_old ;;
    dbkill) cmd_dbkill "$@" ;;
    stopapp) cmd_stopapp ;;
    memgate) cmd_memgate "$@" ;;
    final) cmd_final "$@" ;;
    restore) cmd_restore ;;
    run) cmd_run "$@" ;;
    *) echo "akam_hot: unknown subcommand '$c'"; exit 64 ;;
esac
AKAM_HOT_EOF

# shellcheck disable=SC2016  # the worker script is literal
WORKER='#!/bin/sh
# runs on the camera, detached. Result: /tmp/akam_flash.result = OK | ROLLED_BACK | ROLLBACK_FAILED | WORKER_BAD_INPUT
# never the OOM victim (only when panic_on_oom is 0); flashcp and the rollback worker inherit it
if [ "$(cat /proc/sys/vm/panic_on_oom 2>/dev/null)" = 0 ]; then echo -1000 > /proc/$$/oom_score_adj 2>/dev/null; fi
NEW=@STAGE@/appfs-new.bin
OLD=@STAGE@/appfs-backup.bin
sum() { dd if=/dev/mtd3 bs=65536 2>/dev/null | md5sum | cut -d" " -f1; }
want=$(md5sum $NEW | cut -d" " -f1)
oldsum=$(md5sum $OLD | cut -d" " -f1)
if [ -z "$want" ] || [ -z "$oldsum" ]; then
    echo "md5sum produced nothing (want=$want oldsum=$oldsum): NOT flashing"
    echo WORKER_BAD_INPUT > /tmp/akam_flash.result
    exit 1
fi
echo "flashing new image"
if flashcp -v $NEW /dev/mtd3 && [ "$(sum)" = "$want" ]; then
    echo OK > /tmp/akam_flash.result
    exit 0
fi
echo "new image FAILED verification: re-flashing the rollback image (up to 3 tries)"
sh /tmp/akam_rollback_worker.sh
if [ "$(cat /tmp/akam_rollback.result 2>/dev/null)" = ROLLED_BACK ]; then
    echo ROLLED_BACK > /tmp/akam_flash.result
else
    echo ROLLBACK_FAILED > /tmp/akam_flash.result
fi
'
# the reusable DETACHED rollback worker: also what the operator launches by hand (see ROLLBACK_CMD)
ROLLBACK_WORKER='#!/bin/sh
# runs on the camera, detached. Result: /tmp/akam_rollback.result = ROLLED_BACK | ROLLBACK_FAILED
# never the OOM victim (only when panic_on_oom is 0); flashcp and the rollback worker inherit it
if [ "$(cat /proc/sys/vm/panic_on_oom 2>/dev/null)" = 0 ]; then echo -1000 > /proc/$$/oom_score_adj 2>/dev/null; fi
OLD=@STAGE@/appfs-backup.bin
RES=/tmp/akam_rollback.result
LOCK=/tmp/akam_rollback.lock
if ! mkdir $LOCK 2>/dev/null; then
    lp=$(cat $LOCK/pid 2>/dev/null)
    if [ -z "$lp" ] || grep -q akam_rollback /proc/$lp/cmdline 2>/dev/null; then
        echo "another rollback worker (pid ${lp:-unknown}) holds $LOCK: not starting a second writer on mtd3 (no worker running? rm -rf $LOCK)"
        exit 1
    fi
    echo "stale $LOCK (pid $lp is dead, e.g. SIGKILL): taking it over"
    rm -rf $LOCK
    mkdir $LOCK 2>/dev/null || { echo "cannot take $LOCK"; exit 1; }
fi
echo $$ > $LOCK/pid
trap "rm -rf $LOCK 2>/dev/null" EXIT
rm -f $RES
sum() { dd if=/dev/mtd3 bs=65536 2>/dev/null | md5sum | cut -d" " -f1; }
oldsum=$(md5sum $OLD | cut -d" " -f1)
if [ -z "$oldsum" ]; then
    echo "no md5 of $OLD: backup missing?"
    echo ROLLBACK_FAILED > $RES
    exit 1
fi
i=0
while [ $i -lt 3 ]; do
    i=$((i + 1))
    echo "rollback try $i: flashcp"
    if flashcp -v $OLD /dev/mtd3 && [ "$(sum)" = "$oldsum" ]; then
        echo ROLLED_BACK > $RES
        exit 0
    fi
    echo "rollback try $i: dd to /dev/mtdblock3"
    dd if=$OLD of=/dev/mtdblock3 bs=65536 2>/dev/null
    sync
    if [ "$(sum)" = "$oldsum" ]; then
        echo ROLLED_BACK > $RES
        exit 0
    fi
done
echo ROLLBACK_FAILED > $RES
'
WORKER=${WORKER//@STAGE@/$STAGE}
ROLLBACK_WORKER=${ROLLBACK_WORKER//@STAGE@/$STAGE}
# printed for the operator as an emergency copy-paste: include the same ssh options this script uses (key, known-hosts)
SSH_DISP="ssh -o BatchMode=yes -o ConnectTimeout=10 $SSH_OPTS $CAM"
ROLLBACK_CMD="$SSH_DISP 'nohup sh /tmp/akam_rollback_worker.sh >/tmp/akam_rollback.log 2>&1 </dev/null &'"
ROLLBACK_POLL="$SSH_DISP 'cat /tmp/akam_rollback.result /tmp/akam_rollback.log'"
upload() { # upload <local> <remote>
    "${SSH[@]}" "cat > $2" <"$1"
    l=$(md5sum "$1" | cut -d' ' -f1)
    r=$(camq "md5sum $2" | cut -d' ' -f1)
    [ "$l" = "$r" ] || die "upload of $1 corrupted (md5 $l vs $r): abort, nothing written"
}
put_workers() { # the two on-camera workers (tiny, in /tmp)
    printf '%s' "$ROLLBACK_WORKER" | "${SSH[@]}" "cat > /tmp/akam_rollback_worker.sh && chmod 755 /tmp/akam_rollback_worker.sh && rm -f /tmp/akam_rollback.result"
    printf '%s' "$WORKER" | "${SSH[@]}" "cat > /tmp/akam_flash_worker.sh && chmod 755 /tmp/akam_flash_worker.sh && rm -f /tmp/akam_flash.result"
}

hot() { camq "sh $HOTSH $*"; }

hot_wait_ssh() { # hot_wait_ssh <tries>: a plain ssh round trip
    local i=0
    while [ "$i" -lt "$1" ]; do
        camq true >/dev/null 2>&1 && return 0
        i=$((i + 1))
        sleep 3
    done
    return 1
}

hot_status() { camq "cat $HOTD/status 2>/dev/null" 2>/dev/null || true; }
hot_confirm() { camq "touch $HOTD/confirm.$1"; }

hot_images() { # upload the two images + the workers into /tmp/akam_flash (md5-checked), at the runner's upload checkpoint
    upload "$IMG" $STAGE/appfs-new.bin
    upload "$ROLLSRC_FILE" $STAGE/appfs-backup.bin
    put_workers
    camq "sync" || true
}

# hot_run <hot|rehearsal>: live only. Upload the helper, start the ONE detached on-camera runner, then only poll its status
# and answer its checkpoints. Every failure before the write is restored by the runner itself (also when this script dies:
# no confirmation within the dead-man time = it restores and exits). Sets HOT_RES (the worker result) for mode hot.
HOT_RES=""
hot_run() {
    local mode=$1 stl="" last="" phase msg t=0 nost=0 up=0 iv=2 rv wr alive warned=0
    camq "mkdir -p $HOTD && chmod 700 $HOTD" || die "cannot create $HOTD on the camera: nothing changed"
    # single instance: never start (or overwrite the helper of) a runner while another one is alive
    if camq "p=\$(cat $HOTD/run.lock/pid 2>/dev/null); [ -n \"\$p\" ] && grep -q akam_hot /proc/\$p/cmdline 2>/dev/null" >/dev/null 2>&1; then
        die "a runner is already alive on the camera (pid in $HOTD/run.lock/pid): not starting a second one. ssh $CAM 'cat $HOTD/status $HOTD/run.log'"
    fi
    printf '%s\n' "$HOT_REMOTE" | "${SSH[@]}" "cat > $HOTSH && chmod 755 $HOTSH" || die "cannot upload the helper: nothing changed"
    [ "$(printf '%s\n' "$HOT_REMOTE" | md5sum | cut -d' ' -f1)" = "$(camq "md5sum <$HOTSH" | cut -d' ' -f1)" ] || die "the uploaded helper is corrupted: nothing changed"
    camq "rm -f $HOTD/status $HOTD/confirm.* $HOTD/run.log"
    say "starting the detached on-camera runner ($mode): the whole sequence runs on the camera; this script polls $HOTD/status and answers the checkpoints"
    say "if this script dies the runner restores by itself when a confirmation does not arrive in time; by hand: ssh $CAM 'sh $HOTSH restore'; log: $HOTD/run.log"
    camq "cd / && if command -v setsid >/dev/null 2>&1; then S=setsid; else S=; fi; AKAM_HOT_PORT=$HOT_PORT nohup \$S sh $HOTSH run $mode $HOT_NEED_KB $HOT_TMP_KB $((MEM_MARGIN_KB - 1024)) >$HOTD/run.log 2>&1 </dev/null &" ||
        die "cannot start the runner: nothing was changed unless the log says so (ssh $CAM 'cat $HOTD/run.log $HOTD/status')"
    while [ "$t" -lt 3000 ]; do
        sleep "$iv"
        t=$((t + iv))
        stl=$(hot_status)
        if [ -z "$stl" ]; then
            nost=$((nost + iv))
            [ "$nost" -lt 300 ] || die "no status from the camera for 5 minutes. The runner restores by itself without confirmations (or finishes a write that already started: do NOT power off). Check: ssh $CAM 'cat $HOTD/status $HOTD/run.log'"
            continue
        fi
        nost=0
        if [ "$stl" != "$last" ]; then say "camera: $stl"; last=$stl; fi
        phase=${stl%%|*}
        msg=${stl#*|}
        case $phase in
            WRITING)
                # the write runs: poll rarely (every ssh session forks a dropbear at oom -1000) and also read the worker's
                # own result file and the runner's liveness
                iv=15
                rv=$(camq "cat /tmp/akam_flash.result 2>/dev/null; echo ---; p=\$(cat $HOTD/run.lock/pid 2>/dev/null); if [ -n \"\$p\" ] && grep -q akam_hot /proc/\$p/cmdline 2>/dev/null; then echo alive; else echo dead; fi" 2>/dev/null || true)
                wr=${rv%%---*}
                wr=$(printf '%s' "$wr" | tr -d '[:space:]')
                alive=${rv##*---}
                if [ -n "$wr" ]; then
                    camq "cat /tmp/akam_flash.log" 2>/dev/null || true
                    HOT_RES=$wr
                    return 0
                fi
                case $alive in *dead*) if [ "$warned" = 0 ]; then say "WARNING: the runner is no longer alive but the worker has no result yet: it may still be writing. Do NOT power off; waiting for /tmp/akam_flash.result"; warned=1; fi ;; esac ;;
            WAIT_wpa) hot_wait_ssh 3 && hot_confirm wpa || true ;;
            WAIT_dbalt)
                mk_ssh "$HOT_PORT"
                if hot_wait_ssh 3; then hot_confirm dbalt || true; fi
                mk_ssh ;;
            WAIT_db22) hot_wait_ssh 3 && hot_confirm db22 || true ;;
            WAIT_upload)
                if [ "$up" = 0 ]; then
                    say "uploading the images (md5-checked) into $STAGE"
                    hot_images
                    up=1
                fi
                hot_confirm upload || true ;;
            WAIT_go) hot_wait_ssh 3 && hot_confirm go || true ;;
            DONE_RESTORED)
                camq "cat $HOTD/run.log" 2>/dev/null || true
                return 0 ;;
            DONE_RESTORED_NO_WIFI | ABORTED_RESTORED_NO_WIFI)
                camq "cat $HOTD/run.log" 2>/dev/null || true
                die "$msg. The camera is restored but NO wpa_supplicant runs: it has no Wi-Fi. NOTHING was written. Power-cycle it (a reboot returns to the normal state)" ;;
            DONE_RESTORE_FAILED | ABORTED_RESTORE_FAILED)
                camq "cat $HOTD/run.log" 2>/dev/null || true
                die "$msg. NOTHING was written. Wait 3 minutes (the on-camera dead-man restores Wi-Fi), else power-cycle the camera: a reboot returns to the normal state" ;;
            ABORTED_RESTORED | ABORTED_UNCHANGED)
                camq "cat $HOTD/run.log" 2>/dev/null || true
                die "the camera runner aborted: $msg" ;;
            WRITTEN)
                camq "cat /tmp/akam_flash.log" 2>/dev/null || true
                [ "$msg" = NONE ] && msg=""
                HOT_RES=$msg
                return 0 ;;
        esac
    done
    die "no end state from the runner after 50 minutes (the camera may still be writing: do NOT power it off; ssh $CAM 'cat $HOTD/status $HOTD/run.log /tmp/akam_flash.result'); a re-run is safe, step (a) aborts when mtd3 != the expected image"
}

hot_dry() {
    say "[dry-run] the whole camera-side sequence runs as ONE detached on-camera runner (nohup, setsid if present, fds redirected, oom_score_adj -1000 when panic_on_oom is 0): this script uploads the helper, starts it, polls $HOTD/status and answers the checkpoints (wpa, dbalt, db22, upload, go) with a touch over a fresh ssh; no answer within 120 s = the runner restores by itself and exits, nothing written. After the write starts it never waits for the PC"
    say "[dry-run] H1 ssh $CAM: upload $HOTSH; card-less proof (no mmcblk mounted, /mnt/sdcard = tmpfs bind of the Akam flash tree); tools (flashcp md5sum nohup dd sh cat awk sed ip cut sync mkdir rmdir sleep) not in /appfs"
    say "[dry-run] H1 list every process with an /appfs exe, cwd, open file or mapping; each must be stub|init|supervisor|app|ntp|keeper|feeder|dropbear|wpa_supplicant, anything else = abort, nothing stopped"
    say "[dry-run] H1 capture the command lines (and AKAM_* environment) of supervisor, ntp, keeper, feeder, wpa_supplicant, dropbear for a restore"
    say "[dry-run] H2 copy busybox, dropbearmulti, wpa_supplicant to /tmp/akam_hot (md5-checked, a reused copy too; refuse if one maps another /appfs file)"
    say "[dry-run] H2 start a new feeder from /tmp, prove it alive (oom_score_adj -1000; watchdog exclusive: SIGKILL old, start new at once; no magic close), then soak it alone >= 25 s; the fallback feeder is verified alive"
    say "[dry-run] H2 detached swap of wpa_supplicant to the /tmp copy (same arguments), checkpoint wpa: rolled back unless a fresh ssh round trip confirms within 120 s; already on /tmp (after a rehearsal) = skipped"
    say "[dry-run] H2 dropbear from /tmp on port $HOT_PORT; reconnect through it (HostKeyAlias $CAM_HOST); stop the /appfs dropbears; dropbear from /tmp on port 22; stop the $HOT_PORT listener"
    say "[dry-run] H2 stop supervisor (waits for the app), app, ntp helper, keeper, then SIGKILL the stub and the resident init.sh; rescan: NO process may touch /appfs (else abort + restore)"
    say "[dry-run] H3 drop_caches, MemFree and MemAvailable logged at every gate, MemFree > $HOT_NEED_KB kB (2 x $((PART / 1024)) kB + $MEM_MARGIN_KB kB), /tmp free > $HOT_TMP_KB kB; checkpoint upload: the PC stages the images in /tmp/akam_flash; checkpoint go; final gate; write by the existing worker"
    say "[dry-run] any failure above, or a missing confirmation: the runner restarts supervisor/ntp/keeper from the captured command lines, deletes /tmp/akam_flash, nothing written"
}

# ---- (a) full re-dump and compare ---------------------------------------------------------------------------------------
step "(a) re-dump mtd0..mtd5 and compare with the backup"
if [ "$HOTPRE" = 1 ]; then
    say "skipped in the rehearsal (--hot-preflight never writes; the real --hot run does this step first)"
elif [ "$LIVE" = 1 ]; then
    camq "cat /proc/mtd" | tee "$WORK_DIR/proc_mtd.txt"
    grep -q '^mtd3: 00490000 .* "appfs"' "$WORK_DIR/proc_mtd.txt" || die "mtd3 is not the 0x490000 appfs partition: abort, nothing written"
    for n in 0 1 2 3 4 5; do
        dump "$n" "$WORK_DIR/mtd$n.bin"
        if [ "$n" = 3 ] && [ "$UPDATE" = 1 ]; then
            if cmp -s "$WORK_DIR/mtd3.bin" "$UPDATE_IMG"; then
                say "mtd3 identical to CURRENT_IMAGE $UPDATE_IMG (rollback source for this run)"
                continue
            fi
            die "mtd3 differs from CURRENT_IMAGE $UPDATE_IMG: abort, nothing written (wrong image given, or a flaky read; the dump is $WORK_DIR/mtd3.bin)"
        fi
        if cmp -s "$WORK_DIR/mtd$n.bin" "$BACKUP_DIR/mtd$n.bin"; then
            say "mtd$n identical to the backup"
        elif [ "$n" = 4 ]; then
            say "WARNING: mtd4 (jffs2 config) differs from the backup: normal if the config changed since; the fresh dump is kept in $WORK_DIR"
        else
            die "mtd$n differs from the backup: abort, nothing written (a camera that is not the one that was backed up, or a flaky read)"
        fi
    done
else
    say "[dry-run] ssh $CAM: cat /proc/mtd   (mtd3 must be 00490000 \"appfs\")"
    say "[dry-run] for n in 0..5: ssh $CAM 'dd if=/dev/mtd\$n bs=65536' > \$WORK_DIR/mtd\$n.bin ; cmp with \$BACKUP_DIR/mtd\$n.bin (mtd4: warning only)"
    [ "$UPDATE" = 0 ] || say "[dry-run] update mode: mtd3 is NOT compared with the stock backup but with $UPDATE_IMG (abort on any difference); mtd0/1/2/5 still vs the stock backup"
fi

# ---- (b) preflight + stop the app ----------------------------------------------------------------------------------------
step "(b) camera preflight, stop supervisor + app (the watchdog feeder stays)"
if [ "$HOT" = 1 ]; then
    if [ "$LIVE" = 1 ]; then hot_run "$( [ "$HOTPRE" = 1 ] && echo rehearsal || echo hot )"; else hot_dry; fi
elif [ "$LIVE" = 1 ]; then
    camq "ps | grep '[b]usybox watchdog'" || die "no watchdog feeder running: abort (a flash of 1-2 minutes without a feeder = reset mid-write)"
    camq 'for t in flashcp md5sum nohup dd; do command -v $t >/dev/null || { echo "missing on the camera: $t"; exit 1; }; done' ||
        die "a tool the on-camera worker needs is missing: abort, nothing written"
    # the feeder must not execute from the appfs squashfs that is about to be erased (a page fault into the new
    # content would kill it = reset in the middle of the write). The SD overlay's feeder runs from the card.
    camq 'for p in $(ps | grep "[b]usybox watchdog" | while read a b; do echo $a; done); do grep -q "/appfs/" /proc/$p/maps 2>/dev/null && exit 1; done; exit 0' ||
        die "the watchdog feeder runs from /appfs (it would die when mtd3 is erased): boot with the SD card overlay (Akam card inserted; its stub must start the feeder from the card, even over an already flashed appfs) and retry. Nothing written"
    # The two 4.7 MB images are staged on the SD card (mounted, persistent, survives a reboot), NOT in /tmp RAM:
    # this camera has little free RAM (~10 MB). Worker scripts, results and logs stay in /tmp (tiny).
    # CRITICAL: /mnt/sdcard must be the REAL card, not a RAM bind-mount (card-less mode binds /tmp/akamsd there):
    # staging to RAM would reintroduce the OOM-mid-write brick risk this change avoids.
    # shellcheck disable=SC2016  # remote command, expanded on the camera
    sdmnt=$(camq 'awk "\$2==\"/mnt/sdcard\"{print \$1\" \"\$3}" /proc/mounts')
    case $sdmnt in
        /dev/mmcblk*\ vfat | /dev/mmcblk*\ exfat | /dev/mmcblk*\ msdos) say "SD card: $sdmnt" ;;
        *) die "/mnt/sdcard is not a real SD card mount (got '$sdmnt'): refusing to stage in RAM. $( [ "$UPDATE" = 1 ] && echo "UPDATE MODE: insert the Akam card and reboot first (a card-less boot binds a tmpfs there). " )Boot on the SD overlay. Nothing written." ;;
    esac
    camq 'mkdir -p /mnt/sdcard/akam_flash' || die "cannot create /mnt/sdcard/akam_flash on the card: abort, nothing written"
    sdfree=$(camq "df /mnt/sdcard | awk 'END{print \$4}'")
    [ "${sdfree:-0}" -gt 20000 ] || die "SD free ${sdfree} kB: need > 20 MB on the card for the two images"
    memfree=$(camq "grep MemFree /proc/meminfo | tr -cd '0-9'")
    [ "${memfree:-0}" -gt 4096 ] || die "MemFree ${memfree} kB: too low even for flashcp (need > 4 MB)"
    say "stopping the supervisor (it stops the app); the stub and the feeder keep running"
    # shellcheck disable=SC2016  # remote command, expanded on the camera
    camq 'for p in $(ps | grep "[o]wn_app_run.sh" | while read a b; do echo $a; done); do kill $p; done; i=0; while ps | grep -q "[/]tmp/akam2k_rtsp" && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done; ps | grep -q "[/]tmp/akam2k_rtsp" && exit 1; exit 0' ||
        die "the app did not stop: abort, nothing written"
    # shellcheck disable=SC2016  # remote command, expanded on the camera
    # NB: the remote for-loop ends in a failed grep when nothing maps /appfs/lib (the GOOD case); without the
    # trailing `; true` the remote exits non-zero and `set -e` would kill THIS script silently right when the
    # preflight succeeds. `|| true` on the assignment is a second guard.
    busy=$(camq 'for p in /proc/[0-9]*; do grep -q "/appfs/lib" "$p/maps" 2>/dev/null && echo "${p#/proc/}"; done; true') || true
    [ -z "$busy" ] || die "/appfs/lib is still mapped by pid(s): $busy. Abort, nothing written. (A reboot returns to the normal state.)"
    camq "ps | grep '[b]usybox watchdog'" >/dev/null || die "the feeder is gone"
    say "/appfs/lib is free, feeder alive"
else
    say "[dry-run] ssh $CAM: ps | grep '[b]usybox watchdog'  (abort when absent); SD /mnt/sdcard/akam_flash free > 20 MB, MemFree > 4 MB"
    say "[dry-run] ssh $CAM: kill the own_app_run.sh supervisor (NOT the stub, NOT the feeder), wait for akam2k_rtsp to exit"
    say "[dry-run] ssh $CAM: no process may map /appfs/lib (grep /proc/*/maps) or abort"
fi

if [ "$HOTPRE" = 1 ]; then
    step "rehearsal end: restore everything that was stopped (nothing was written)"
    if [ "$LIVE" = 1 ]; then
        say "the runner already restored everything (nothing was written); informational scan of what still touches /appfs:"
        hot scan || true
    else
        say "[dry-run] restart supervisor/ntp/keeper from the captured command lines; feeder, wpa_supplicant, dropbear stay on their /tmp copies"
    fi
    echo
    say "$( [ "$LIVE" = 1 ] && echo "REHEARSAL DONE (live)." || echo "dry run finished, nothing was changed." ) No flash write was made or attempted. Only the real --hot run writes."
    exit 0
fi

# ---- (c) upload + flash ---------------------------------------------------------------------------------------------------
step "(c) upload to the staging dir ($STAGE: the SD card, or /tmp in --hot) on the camera, flashcp -v (detached worker with its own verify and rollback; --hot: done by the on-camera runner, see (b))"
if [ "$LIVE" = 1 ]; then
    NORES="no result from the worker (the camera may still be writing: do NOT power it off; look at /tmp/akam_flash.result and /tmp/akam_flash.log over ssh; a re-run is safe, step (a) aborts when mtd3 != the expected image: the stock backup, or CURRENT_IMAGE in update mode). Only when mtd3 is really not the intended image and no worker runs, launch the DETACHED rollback worker: $ROLLBACK_CMD  then poll: $ROLLBACK_POLL"
    if [ "$HOT" = 1 ]; then
        # the detached on-camera runner already did upload checkpoint, final gate, write and verify (hot_run, step (b))
        res=$HOT_RES
        [ -n "$res" ] || die "$NORES"
    else
        upload "$IMG" $STAGE/appfs-new.bin
        upload "$ROLLSRC_FILE" $STAGE/appfs-backup.bin
        camq "sync" || true   # flush the two staged images to the SD before flashcp reads them
        put_workers
        say "WRITING mtd3 NOW. Do not power the camera off until this script says so."
        # set -e must not end the script silently here: the worker may already be writing
        if ! camq "nohup sh /tmp/akam_flash_worker.sh >/tmp/akam_flash.log 2>&1 </dev/null &"; then
            say "WARNING: the ssh that launched the worker returned non-zero; the worker MAY be running. Do NOT power off."
            say "  check over ssh: cat /tmp/akam_flash.result /tmp/akam_flash.log   (a re-run of this script is safe: step (a) aborts when mtd3 != the expected image: the stock backup, or CURRENT_IMAGE in update mode)"
        fi
        t=0
        res=""
        while [ "$t" -lt 600 ]; do
            sleep 5
            t=$((t + 5))
            res=$(camq "cat /tmp/akam_flash.result 2>/dev/null" 2>/dev/null || true)
            [ -z "$res" ] || break
        done
        camq "cat /tmp/akam_flash.log" 2>/dev/null || true
        [ -n "$res" ] || die "$NORES"
    fi
    say "worker result: $res"
    case $res in
        OK) ;;
        ROLLED_BACK)
            if dump 3 "$WORK_DIR/mtd3.rollback.bin" && cmp -s "$ROLLSRC_FILE" "$WORK_DIR/mtd3.rollback.bin"; then
                die "the new image failed its on-camera verify; $ROLLNAME was re-flashed and verified on the PC too (cmp). Camera still booted. Aborting."
            fi
            die "the worker says ROLLED_BACK but the PC read-back of mtd3 is NOT $ROLLNAME (maybe only a flaky link). Do NOT reboot, do NOT power off. Check over ssh: dd if=/dev/mtd3 bs=65536 | md5sum  vs  md5sum $STAGE/appfs-backup.bin. Only on a genuine difference launch the DETACHED rollback worker: $ROLLBACK_CMD  then poll: $ROLLBACK_POLL ; else UART/CH341A." ;;
        WORKER_BAD_INPUT) die "the worker found no md5 for the image or the backup and did NOT flash anything (mtd3 untouched). Check /tmp/akam_flash.log; fix the cause and re-run." ;;
        *) die "ROLLBACK FAILED. Do NOT reboot, do NOT power off. Launch the DETACHED rollback worker NOW: $ROLLBACK_CMD  then poll: $ROLLBACK_POLL (result ROLLED_BACK|ROLLBACK_FAILED). Never a foreground flashcp. If the link is gone: UART/CH341A." ;;
    esac
else
    say "[dry-run] ssh $CAM: cat > $STAGE/appfs-new.bin < $IMG ; md5 compare   (4.7 MB, SD card)"
    say "[dry-run] ssh $CAM: cat > $STAGE/appfs-backup.bin < $( [ "$UPDATE" = 1 ] && echo "\$WORK_DIR/mtd3.bin (fresh dump == $UPDATE_IMG)" || echo "$BACKUP3" ) ; md5 compare   (rollback source, persistent on the SD card)"
    say "[dry-run] ssh $CAM: also uploads /tmp/akam_rollback_worker.sh (the reusable detached rollback; by hand: $ROLLBACK_CMD)"
    say "[dry-run] ssh $CAM: detached worker (refuses to flash if an md5 is empty): flashcp -v $STAGE/appfs-new.bin /dev/mtd3 ; md5 of /dev/mtd3 == md5 of the file ; else detached rollback (3 tries: flashcp then dd to /dev/mtdblock3, md5 verified); result in /tmp/akam_flash.result (polled up to 600 s)"
fi

# ---- (d) read back and compare on the PC, before anything else ---------------------------------------------------
step "(d) read mtd3 back and byte-compare with the intended image (before any reboot)"
# No "mount /dev/mtdblock3" test here on purpose (see cardless-appfs.md section 9): mount_bdev would return the
# superblock of the already mounted OLD /appfs (a vacuous pass), and forcing a re-read risks SIGBUS in processes
# that run from /appfs. The mountability of the image is established on the host by build_appfs.sh instead.
if [ "$LIVE" = 1 ]; then
    # NON-DESTRUCTIVE: this step NEVER writes flash. The on-camera worker already md5-verified /dev/mtd3 == the new
    # image (authoritative); this is only a redundant PC-side confirmation. A rollback from here would run over a
    # foreground ssh: if the link dropped mid-write, mtd3 would be half written = brick.
    ok=0
    complete=0
    for try in 1 2 3; do
        rm -f "$WORK_DIR/mtd3.readback.bin"
        if dump 3 "$WORK_DIR/mtd3.readback.bin" && [ "$(stat -c %s "$WORK_DIR/mtd3.readback.bin")" = "$PART" ]; then
            complete=1
            if cmp -s "$IMG" "$WORK_DIR/mtd3.readback.bin"; then ok=1; fi
            break
        fi
        say "read-back attempt $try/3 failed or short (transport error?)"
        [ "$try" = 3 ] || sleep 5
    done
    if [ "$ok" = 1 ]; then
        say "mtd3 == intended image, byte for byte (sha256 $(sha_of "$WORK_DIR/mtd3.readback.bin"))"
    elif [ "$complete" = 1 ]; then
        die "a COMPLETE PC read-back of mtd3 differs from the intended image, although the worker reported \"$res\" (its own md5 verify). A genuine mismatch is unexpected. NOTHING was flashed by this step. Do NOT reboot, do NOT power off. Re-check over ssh: dd if=/dev/mtd3 bs=65536 | md5sum (image md5 $want_md5). To roll back, run the DETACHED rollback worker, never a foreground flashcp: $ROLLBACK_CMD  then poll: $ROLLBACK_POLL (ROLLED_BACK|ROLLBACK_FAILED). Last resort: UART (sf write) or CH341A."
    else
        die "3 PC read-backs of mtd3 failed or were short: a transport problem over the link, NOT a verified mismatch, so NO rollback was started. The on-camera worker already md5-verified mtd3 == the new image (worker result \"$res\"), which is authoritative. Do NOT power off. Over ssh compare: dd if=/dev/mtd3 bs=65536 | md5sum  with the new image md5 $want_md5 . Only on a genuine difference run the DETACHED rollback worker: $ROLLBACK_CMD  then poll: $ROLLBACK_POLL"
    fi
else
    say "[dry-run] ssh $CAM: up to 3x (sleep 5 between): dd if=/dev/mtd3 bs=65536 > \$WORK_DIR/mtd3.readback.bin ; the dump must be the full $PART bytes, then cmp -s $IMG"
    say "[dry-run] complete + equal = success. complete + different = die, NO auto-flash (launch by hand: $ROLLBACK_CMD). 3 failed/short reads = die, NO rollback (the worker md5 is authoritative; compare md5 over ssh first)"
    say "[dry-run] step (d) never writes flash"
fi

# ---- (e) appfs_sign ---------------------------------------------------------------------------------------------------------
step "(e) appfs_sign in nvram (the FIRST nvram write ever done on this camera; optional, the boot path does not check it)"
if [ "$LIVE" = 1 ] && [ "${AKAM_NVRAM_WRITE:-}" = yes ]; then
    before=$(camq "nvram get appfs_sign" | tr -d '[:space:]')
    echo "$before" >"$WORK_DIR/appfs_sign.before"
    say "previous appfs_sign saved to $WORK_DIR/appfs_sign.before (restore with: nvram set appfs_sign <old>; nvram commit)"
    camq "nvram set appfs_sign $SIGN && nvram commit"
    after=$(camq "nvram get appfs_sign" | tr -d '[:space:]')
    [ "$after" = "$SIGN" ] || die "nvram appfs_sign reads back '$after', expected $SIGN"
    say "appfs_sign set and read back"
else
    say "not done (set AKAM_NVRAM_WRITE=yes to do it). The exact commands would be:"
    say "  ssh $CAM 'nvram get appfs_sign'   # save the old value first"
    say "  ssh $CAM 'nvram set appfs_sign $SIGN && nvram commit'"
fi

if [ "$HOT" = 1 ]; then
    cat <<EOF

flash_appfs: $( [ "$LIVE" = 1 ] && echo "DONE (live)." || echo "dry run finished, nothing was written." )
flash_appfs: HOT UPDATE, (f) NOT done by this script:
  The new appfs is on mtd3 but this boot still has the OLD squashfs mounted: do not start anything from /appfs.
  Reboot it yourself now (ssh $CAM reboot): the camera boots card-less into the new image. Keep the Akam SD card at hand:
  inserted at power-up it overrides the flashed boot (recovery). A half-written mtd3 needs UART/CH341A.
EOF
    exit 0
fi
cat <<EOF

flash_appfs: $( [ "$LIVE" = 1 ] && echo "DONE (live)." || echo "dry run finished, nothing was written." )
flash_appfs: (f) NOT done by this script, staged reboot:
  1. Seed /config/akam over SSH BEFORE flashing (wpa.conf, authorized_keys; do it before running this script if not done yet), see docs/porting/lsc-rotatable-2k/cardless-appfs.md.
  2. Reboot ONCE with the Akam SD card INSERTED: confirms the new appfs mounts and the SD override (recovery) works.
  3. Only then power off, remove the card, power on: card-less boot. Keep the card at hand for recovery.
EOF
