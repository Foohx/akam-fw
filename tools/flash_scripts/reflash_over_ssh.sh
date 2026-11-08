#!/usr/bin/env bash
# reflash_over_ssh.sh -- PC-side driver of the no-UART reflash of a running
# Akam camera. Uploads the image, flash_tool and the camera-side scripts to
# RAM (/tmp/akam_reflash on the camera) over the existing SSH access, has the
# camera start a SECOND dropbear on port 2222 from RAM, starts the flash fully
# detached from RAM, then polls the camera's log over port 2222, verifies the
# read-back md5 of /dev/mtd7 itself and only then tells you to power-cycle BY
# HAND. It never reboots the camera.
#
# RISK: if the write is interrupted (power loss, crash) the APP partition is
# half written and only UART can recover it. Read docs/akam_v1_flash_and_restore.md
# ("Reflashing over SSH, without UART"). Without --i-understand-no-uart the
# script refuses to flash (--precheck-only is allowed).
#
# Usage:
#   reflash_over_ssh.sh --host CAM --image IMG.squashfs --md5 HEX \
#       --flash-tool PATH/flash_tool --i-understand-no-uart [options]
#   reflash_over_ssh.sh --host CAM --image IMG --md5 HEX --flash-tool T --precheck-only
#
# Options:
#   --host H             camera address (required)
#   --user U             SSH user (default root)
#   --port P             SSH port of the normal Dropbear (default 22)
#   --second-port P      port of the RAM Dropbear (default 2222)
#   --identity FILE      ssh -i FILE
#   --image FILE         exactly 5242880 bytes
#   --md5 HEX            md5 of the image you expect (checked locally and on the camera)
#   --flash-tool FILE    the flash_tool binary (ARM, from the flasher build)
#   --remote-dir DIR     RAM work dir on the camera (default /tmp/akam_reflash)
#   --poll-interval S    seconds between log polls (default 5)
#   --timeout S          give up polling after S seconds (default 900)
#   --precheck-only      upload the scripts and run the camera pre-checks only
#   --i-understand-no-uart   required to flash
# Environment: REFLASH_SSH (ssh binary, default ssh); REFLASH_REMOTE_TMP
# (default /tmp, the only tree --remote-dir may live in; test hook).
#
# Host keys: both ports use the same host key, so the script passes
# -o HostKeyAlias=<host> and your known_hosts entry for the camera applies to
# port 2222 too.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSH_BIN="${REFLASH_SSH:-ssh}"
RTMP="${REFLASH_REMOTE_TMP:-/tmp}"
EXPECTSIZE=5242880

host="" user=root port=22 port2=2222 ident="" image="" md5="" tool=""
rdir=/tmp/akam_reflash poll=5 tmo=900 precheck_only=0 confirm=0

die() { echo "reflash_over_ssh: ERROR: $*" >&2; exit 1; }
say() { echo "reflash_over_ssh: $*"; }
usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//' >&2; }

while [ $# -gt 0 ]; do
    case "$1" in
        --host) host="${2:?}"; shift 2 ;;
        --user) user="${2:?}"; shift 2 ;;
        --port) port="${2:?}"; shift 2 ;;
        --second-port) port2="${2:?}"; shift 2 ;;
        --identity) ident="${2:?}"; shift 2 ;;
        --image) image="${2:?}"; shift 2 ;;
        --md5) md5="${2:?}"; shift 2 ;;
        --flash-tool) tool="${2:?}"; shift 2 ;;
        --remote-dir) rdir="${2:?}"; shift 2 ;;
        --poll-interval) poll="${2:?}"; shift 2 ;;
        --timeout) tmo="${2:?}"; shift 2 ;;
        --precheck-only) precheck_only=1; shift ;;
        --i-understand-no-uart) confirm=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) usage; die "unknown argument: $1" ;;
    esac
done

# ---------------------------------------------------------------- local checks
[ -n "$host" ] || { usage; die "--host is required"; }
[ -n "$image" ] && [ -n "$md5" ] && [ -n "$tool" ] || { usage; die "--image, --md5 and --flash-tool are required"; }
[[ "$md5" =~ ^[0-9a-f]{32}$ ]] || die "--md5 must be 32 lowercase hex digits"
[[ "$rdir" =~ ^/[A-Za-z0-9_./-]+$ ]] && [[ "$rdir" != *..* ]] || die "--remote-dir must be a plain absolute path"
case "$rdir" in
    "$RTMP"/?*) ;;
    *) die "--remote-dir must be below $RTMP (the camera's RAM disk)" ;;
esac
[[ "$poll" =~ ^[0-9]+$ ]] && [[ "$tmo" =~ ^[0-9]+$ ]] || die "--poll-interval and --timeout must be integers"
[ -f "$image" ] || die "image not found: $image"
[ -f "$tool" ] || die "flash tool not found: $tool"
for f in akam_reflash_ssh.sh flash_guard.sh; do
    [ -f "$HERE/$f" ] || die "$HERE/$f missing"
done
size="$(stat -c%s "$image")"
[ "$size" -eq "$EXPECTSIZE" ] || die "image is $size bytes, expected exactly $EXPECTSIZE (full APP partition)"
got_md5="$(md5sum "$image" | cut -d' ' -f1)"
[ "$got_md5" = "$md5" ] || die "image md5 is $got_md5, you said $md5: refusing"
if [ "$confirm" -ne 1 ] && [ "$precheck_only" -ne 1 ]; then
    cat >&2 <<'EOF'
reflash_over_ssh: refusing to flash without --i-understand-no-uart.
Flashing a running camera over SSH has no UART safety net: an interrupted
write leaves a half-written APP that only UART can recover. Read
docs/akam_v1_flash_and_restore.md first. Use --precheck-only to test the setup.
EOF
    exit 2
fi

# ---------------------------------------------------------------- ssh plumbing
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3
    -o "HostKeyAlias=$host")
[ -n "$ident" ] && ssh_opts+=(-i "$ident")
# rssh PORT COMMAND: run COMMAND (one string, interpreted by the camera's /bin/sh)
rssh() {
    local p="$1"; shift
    "$SSH_BIN" -p "$p" "${ssh_opts[@]}" "$user@$host" "$@"
}
# upload LOCAL REMOTENAME
upload() {
    rssh "$port" "umask 077; cat > '$rdir/$2'" < "$1" || die "upload of $2 failed"
}
cam_sh() { # run the camera script on the normal port (before the flash)
    rssh "$port" "sh '$rdir/akam_reflash_ssh.sh' $*"
}
RBB="$rdir/bin/busybox"

# ------------------------------------------------------------ stage 1: scripts
say "uploading the camera-side scripts to $rdir"
# Never wipe the work directory under a running worker: that would delete its
# log and pid file, and a second start could then write at the same time. Refuse
# (exit 3) while a worker or its lock is alive; otherwise remove only the files
# this script uploads (the RAM busybox/Dropbear copies and the second Dropbear
# of an earlier run stay, and are reused).
# shellcheck disable=SC2016  # the snippet is expanded on the camera, not here
busy_snippet='busy=0
for f in RDIR/worker.pid RDIR/worker.lock/pid; do
    p=; [ -s "$f" ] && read -r p <"$f"
    case "$p" in "" | *[!0-9]*) ;; *) kill -0 "$p" 2>/dev/null && busy=1 ;; esac
done
[ -d RDIR/worker.lock ] && [ ! -s RDIR/worker.lock/pid ] && busy=1
[ "$busy" = 0 ] || exit 3
umask 077
mkdir -p RDIR && cd RDIR || exit 1
rm -f akam_reflash_ssh.sh flash_guard.sh flash_tool image.squashfs flash.log worker.pid
rm -rf worker.lock'
rc=0
# $rdir was validated above (plain absolute path): no quoting needed.
rssh "$port" "${busy_snippet//RDIR/$rdir}" || rc=$?
[ "$rc" -ne 3 ] || die "a flash worker is still running or starting in $rdir on the camera: nothing touched. Follow $rdir/flash.log over port $port2, do not power-cycle."
[ "$rc" -eq 0 ] || die "cannot prepare $rdir on the camera"
upload "$HERE/akam_reflash_ssh.sh" akam_reflash_ssh.sh
upload "$HERE/flash_guard.sh" flash_guard.sh
say "camera pre-checks (mtd7 = APP 00500000, free RAM)"
cam_sh precheck || die "camera pre-check failed, nothing was written"
if [ "$precheck_only" -eq 1 ]; then
    say "precheck-only: done, nothing written. Re-run with --i-understand-no-uart to flash."
    exit 0
fi

# ------------------------------------------------------- stage 2: image to RAM
say "uploading flash_tool and the image to RAM"
upload "$tool" flash_tool
upload "$image" image.squashfs
for pair in "$image:image.squashfs" "$tool:flash_tool"; do
    lf="${pair%%:*}" rf="${pair##*:}"
    l="$(sha256sum "$lf" | cut -d' ' -f1)"
    r="$(rssh "$port" "sha256sum '$rdir/$rf'" | cut -d' ' -f1)"
    [ "$l" = "$r" ] || die "sha256 mismatch for $rf (local $l, camera ${r:-none}): nothing was written"
    say "sha256 OK for $rf ($l)"
done

say "preparing: busybox + dropbear copied to RAM, second Dropbear on port $port2"
cam_sh prepare --md5 "$md5" || die "prepare failed on the camera, nothing was written"

# ----------------------------------------- stage 3: the independent session
ok2=0
for _ in 1 2 3 4 5; do
    if probe="$(rssh "$port2" "$RBB echo second-session-ok" 2>/dev/null)" && [[ "$probe" == *second-session-ok* ]]; then
        ok2=1
        break
    fi
    sleep 2
done
[ "$ok2" -eq 1 ] || die "cannot log in on port $port2 (the RAM Dropbear): nothing was written, aborting before the flash"
say "port $port2 session works and does not depend on /usr"

# ------------------------------------------------------------- stage 4: flash
say "starting the detached flash (from now on an interruption can leave APP half written)"
rssh "$port2" "$RBB sh '$rdir/akam_reflash_ssh.sh' start --md5 '$md5' --i-understand-no-uart" \
    || die "start failed (see the camera log $rdir/flash.log); check the flash state before anything else"

# ------------------------------------------------------------- stage 5: poll
printed=0 result="" fails=0 waited=0 dead=0
while [ "$waited" -le "$tmo" ]; do
    if out="$(rssh "$port2" "$RBB tail -n +$((printed + 1)) '$rdir/flash.log'; $RBB sh '$rdir/akam_reflash_ssh.sh' status | $RBB grep '^worker='" 2>/dev/null)"; then
        fails=0
        wline="$(printf '%s\n' "$out" | grep '^worker=' | tail -n 1 || true)"
        new="$(printf '%s\n' "$out" | grep -v '^worker=' || true)"
        if [ -n "$new" ]; then
            printf '%s\n' "$new" | sed 's/^/camera: /'
            printed=$((printed + $(printf '%s\n' "$new" | wc -l)))
        fi
        if grep -q 'RESULT: ' <<<"$new"; then
            result="$(grep 'RESULT: ' <<<"$new" | tail -n 1)"
            break
        fi
        if [ "$wline" = "worker=dead" ]; then
            dead=$((dead + 1))
            if [ "$dead" -ge 2 ]; then break; fi
        else
            dead=0
        fi
    else
        fails=$((fails + 1))
        say "poll failed ($fails in a row); the camera may be busy or the session dropped, retrying"
    fi
    sleep "$poll"
    waited=$((waited + poll))
done

case "$result" in
    *"RESULT: OK"*) ;;
    "")
        die "no result from the flash worker (worker dead: $dead, waited ${waited}s). DO NOT power-cycle. Inspect $rdir/flash.log over port $port2."
        ;;
    *) die "the flash worker reported: $result. DO NOT power-cycle; re-run, or recover with UART." ;;
esac

# --------------------------------------------- stage 6: independent read-back
v=""
for _ in 1 2 3; do
    if v="$(rssh "$port2" "$RBB sh '$rdir/akam_reflash_ssh.sh' verify --md5 '$md5'" 2>/dev/null)"; then break; fi
    sleep 2
done
rb="$(grep '^READBACK_MD5=' <<<"$v" | tail -n 1 | cut -d= -f2 || true)"
if [ "$rb" = "$md5" ] && grep -q '^VERIFY=MATCH' <<<"$v"; then
    say "read-back md5 of /dev/mtd7 = $rb, equal to the image."
    say "VERIFIED. Now power-cycle BY HAND: unplug and replug USB-C (never use 'reboot')."
    exit 0
fi
die "independent read-back md5 is '${rb:-unavailable}', expected $md5. DO NOT power-cycle."
