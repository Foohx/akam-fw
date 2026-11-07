#!/usr/bin/env bash
# prepare_first_boot_sd.sh -- HOST tool: build the "first-boot" microSD for Camyn.
#
# The card is left in the camera for the FIRST Camyn boot and contains:
#   - the pinned lsc-tuya-toolkit payload configured as for the backup
#     (telnet on 24, OFFLINE=1, wifi.conf, entrypoint lock, hack.sh hook, dump.done),
#   - an empty CAMYN_MAINTENANCE at the card root (keeps that telnet alive: the
#     patched service.sh records it in /tmp/camyn_maintenance),
#   - camyn/authorized_keys with your ed25519 public key (SSH on port 22),
#   - NO *.bin at the card root (stock tf_update.sh would feed it to system_upgrade).
# After SSH is validated, remove the card and reboot: no telnet.
#
# Usage: prepare_first_boot_sd.sh --device /dev/sdX --pubkey ~/.ssh/camyn.pub
#                                 [--toolkit DIR] [--dry-run --stage-dir DIR]
#   --toolkit    clone of lsc-tuya-toolkit (default ~/camyn-sdprep/toolkit; cloned
#                if absent). Commit and sd_card tree hash are verified.
#   --dry-run    touches NO device: verifies the toolkit, builds the card tree in
#                --stage-dir and lists it. SSID/password may come from
#                CAMYN_SD_SSID / CAMYN_SD_PASS (otherwise asked, echoed visibly).
#
# The real run refuses anything that is not a whole, removable, USB-attached disk
# and needs a typed YES before wiping. The Wi-Fi password ends up in clear text on
# the card (and in the staging dir, removed on exit): treat the card as secret.

set -euo pipefail

PIN_COMMIT=396db8a835d9787b2997de038b48f645c837e754
PIN_TREE_SHA=0436e3f89f51ee1ff6b2085e007a7b51e2f155bdea5de7213ba1e05f1e341d91
TOOLKIT_URL=https://github.com/tasarren/lsc-tuya-toolkit.git
# Test hook, honoured ONLY with --dry-run: point the pins at a synthetic toolkit.
TESTPIN=0
if [ "${CAMYN_TEST_DRYRUN_ONLY:-}" = 1 ]; then
    TESTPIN=1
    PIN_TREE_SHA=${CAMYN_TEST_PIN_TREE_SHA:-}
fi

die() { echo "prepare_first_boot_sd: error: $*" >&2; exit 1; }
log() { echo "prepare_first_boot_sd: $*"; }

DEV=
PUBKEY=
TOOLKIT=$HOME/camyn-sdprep/toolkit
DRY=0
STAGE=
while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEV=${2:-}; shift 2 ;;
        --pubkey) PUBKEY=${2:-}; shift 2 ;;
        --toolkit) TOOLKIT=${2:-}; shift 2 ;;
        --stage-dir) STAGE=${2:-}; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -h | --help) sed -n 2,22p "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
if [ "$TESTPIN" = 1 ] && [ "$DRY" = 0 ]; then die "CAMYN_TEST_DRYRUN_ONLY is for --dry-run only"; fi
[ -n "$PUBKEY" ] || die "--pubkey is required"
if [ "$DRY" = 1 ]; then
    [ -n "$STAGE" ] || die "--dry-run needs --stage-dir DIR"
else
    [ -n "$DEV" ] || die "--device is required (e.g. /dev/sdb)"
fi

# --- public key ------------------------------------------------------------------
[ -f "$PUBKEY" ] || die "public key not found: $PUBKEY"
[ "$(wc -l <"$PUBKEY")" -le 1 ] || die "$PUBKEY must hold exactly one key line"
KT=
KB=
read -r KT KB _ <"$PUBKEY" || true
[ "$KT" = ssh-ed25519 ] || die "$PUBKEY is not an ssh-ed25519 public key"
case "$KB" in AAAAC3NzaC1lZDI1NTE5*) ;; *) die "$PUBKEY does not look like an ed25519 public key" ;; esac

# --- device checks (real run only) ----------------------------------------------
SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo
if [ "$DRY" = 0 ]; then
    case "$DEV" in
        /dev/sd[a-z] | /dev/mmcblk[0-9]) ;;
        *) die "$DEV is not a whole-disk node like /dev/sdb or /dev/mmcblk0" ;;
    esac
    [ -b "$DEV" ] || die "$DEV is not a block device"
    for t in lsblk wipefs sfdisk mkfs.vfat partprobe blockdev; do
        command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
    done
    info=$(lsblk -dnro TYPE,RM,TRAN "$DEV") || die "lsblk failed on $DEV"
    d_type=
    d_rm=
    d_tran=
    read -r d_type d_rm d_tran <<<"$info"
    [ "$d_type" = disk ] || die "$DEV is not a disk (type $d_type)"
    [ "$d_rm" = 1 ] || die "$DEV is not removable (RM=$d_rm): refusing"
    [ "$d_tran" = usb ] || die "$DEV is not USB-attached (TRAN=${d_tran:-none}): refusing"
    size=$(blockdev --getsize64 "$DEV" 2>/dev/null || echo 0)
    [ "$size" -le 137438953472 ] || die "$DEV is larger than 128 GiB: use a small card"
fi

# --- toolkit: pinned commit + tree hash ------------------------------------------
if [ ! -d "$TOOLKIT" ]; then
    [ "$TESTPIN" = 0 ] || die "toolkit dir missing: $TOOLKIT"
    log "cloning the toolkit into $TOOLKIT"
    git clone -q "$TOOLKIT_URL" "$TOOLKIT" || die "clone failed"
fi
if [ "$TESTPIN" = 0 ]; then
    git -C "$TOOLKIT" checkout -q --detach "$PIN_COMMIT" || die "cannot check out $PIN_COMMIT"
    [ "$(git -C "$TOOLKIT" rev-parse HEAD)" = "$PIN_COMMIT" ] || die "toolkit HEAD is not $PIN_COMMIT"
    [ -z "$(git -C "$TOOLKIT" status --porcelain)" ] || die "toolkit checkout is modified"
fi
[ -d "$TOOLKIT/sd_card" ] || die "$TOOLKIT/sd_card missing"
got=$( (cd "$TOOLKIT/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1)
[ "$got" = "$PIN_TREE_SHA" ] || die "sd_card tree hash mismatch (got $got): refusing"
log "toolkit verified: sd_card tree $got"

# --- Wi-Fi (visible prompts, by user preference) ---------------------------------
SSID=${CAMYN_SD_SSID:-}
PASS=${CAMYN_SD_PASS:-}
if [ -z "$SSID" ]; then read -r -p 'Wi-Fi SSID: ' SSID; fi
if [ -z "$PASS" ]; then read -r -p 'Wi-Fi password (shown): ' PASS; fi
[ -n "$SSID" ] && [ -n "$PASS" ] || die "SSID and password must not be empty"
case "$SSID$PASS" in *$'\n'* | *$'\r'*) die "SSID/password must not contain newlines" ;; esac
q() { printf "'%s'" "${1//\'/\'\\\'\'}"; }

# --- build the card tree in a private staging dir --------------------------------
if [ "$DRY" = 1 ]; then
    mkdir -p "$STAGE"
    [ -z "$(ls -A "$STAGE")" ] || die "--stage-dir must be empty"
    S=$STAGE
else
    S=$(mktemp -d) || die "mktemp failed"
fi
MNT=
cleanup() {
    if [ -n "$MNT" ]; then
        if mountpoint -q "$MNT" 2>/dev/null; then $SUDO umount "$MNT" 2>/dev/null || true; fi
        rmdir "$MNT" 2>/dev/null || true
    fi
    if [ "$DRY" = 0 ]; then rm -rf "$S"; fi
    return 0
}
trap cleanup EXIT

cp -a "$TOOLKIT/sd_card/." "$S/"
mkdir -p "$S/custom/state" "$S/logs" "$S/camyn"
: >"$S/custom/state/dump.done"
rm -f "$S/custom/state/shadow.done"
CONF=$S/custom/configs/hack.conf
[ -f "$CONF" ] || die "hack.conf missing in the toolkit tree"
sed -i -E 's/^TELNET=.*/TELNET=1/; s/^TELNET_PORT=.*/TELNET_PORT=24/; s/^OFFLINE=.*/OFFLINE=1/' "$CONF"
[ "$(grep -cE '^(TELNET=1|TELNET_PORT=24|OFFLINE=1)$' "$CONF")" = 3 ] || die "hack.conf edits did not apply"
printf 'MODE=sta\nSSID=%s\nPASS=%s\nSECURITY=3\nDHCP=1\n' "$(q "$SSID")" "$(q "$PASS")" >"$S/custom/configs/wifi.conf"
sed -i '2i\# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0' "$S/custom/scripts/entrypoint.sh"
cat >>"$S/hack.sh" <<'EOF_HOOK'
# DECLOUD_WIZARD_HOOK
/bin/sh "${SCRIPTS_DIR}/entrypoint.sh" >/dev/null 2>&1 &
EOF_HOOK
: >"$S/CAMYN_MAINTENANCE"
cp "$PUBKEY" "$S/camyn/authorized_keys"

# Safety: nothing the stock tf_update.sh could treat as an update (top level *.bin).
if find "$S" -maxdepth 1 -iname '*.bin' | grep -q .; then
    die "a *.bin sits at the card root: refusing (stock tf_update.sh would run system_upgrade on it)"
fi

if [ "$DRY" = 1 ]; then
    log "dry run: card tree built in $S (no device touched). Top level:"
    ls -A "$S"
    exit 0
fi

# --- confirm, wipe, format, copy --------------------------------------------------
echo
lsblk -o NAME,SIZE,MODEL,TRAN,RM,FSTYPE,MOUNTPOINTS "$DEV"
echo
echo "ALL DATA ON $DEV WILL BE ERASED."
read -r -p "Type YES (capitals) to continue: " ans
[ "$ans" = YES ] || die "aborted, nothing was written"

for p in $(lsblk -nrpo NAME,MOUNTPOINT "$DEV" | awk 'NF==2{print $1}'); do
    $SUDO umount "$p" || die "cannot unmount $p"
done
$SUDO wipefs -a "$DEV" >/dev/null || die "wipefs failed"

wait_disk() { # some readers drop the medium after a wipe: wait until it is back
    local i=0 s
    while [ "$i" -lt 60 ]; do
        s=$($SUDO blockdev --getsize64 "$DEV" 2>/dev/null || echo 0)
        if [ "$s" -gt 0 ]; then return 0; fi
        i=$((i + 1))
        sleep 1
    done
    die "$DEV did not come back (\"No medium found\"): replug the reader and rerun"
}
wait_disk
printf 'label: dos\n, , c, *\n' | $SUDO sfdisk -q "$DEV" || die "sfdisk failed"
$SUDO partprobe "$DEV" 2>/dev/null || true
case "$DEV" in *mmcblk*) PART=${DEV}p1 ;; *) PART=${DEV}1 ;; esac
i=0
while [ ! -b "$PART" ] && [ "$i" -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done
[ -b "$PART" ] || die "$PART did not appear"
wait_disk
$SUDO mkfs.vfat -F 32 -n CAMYNSD "$PART" >/dev/null || die "mkfs.vfat failed"
MNT=$(mktemp -d) || die "mktemp failed"
$SUDO mount -o "uid=$(id -u),gid=$(id -g),umask=022" "$PART" "$MNT" || die "mount failed"
cp -a "$S/." "$MNT/" || die "copy to the card failed"
sync
# Re-check on the card itself.
{ [ -f "$MNT/CAMYN_MAINTENANCE" ] && [ -s "$MNT/camyn/authorized_keys" ]; } || die "card verification failed"
if find "$MNT" -maxdepth 1 -iname '*.bin' | grep -q .; then die "a *.bin sits at the card root"; fi
$SUDO umount "$MNT" || die "umount failed"
rmdir "$MNT"
MNT=
log "done. Card ready: toolkit telnet 24 + SSH 22 on the first boot (CAMYN_MAINTENANCE present)."
log "After SSH works: remove the card and reboot; the telnet must be gone."
