#!/bin/sh
# akam_flash_boot.sh -- card-less Akam boot for the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
# Baked into the appfs image as /appfs/akam/akam_flash_boot.sh and run by the image's /appfs/script/init.sh
# (mode "normal" = no override card). It does what the SD overlay card does, but from the read-only image
# and with /config (jffs2, persistent) instead of the card:
#
#   1  builds a small tmpfs tree /tmp/akamsd that LOOKS like the overlay card (akam/*.sh, akam/bin, busybox, and
#      symlinks to the user's files in /config/akam) and bind-mounts it over /mnt/sdcard. The shared overlay
#      scripts and the app (which hard-codes /mnt/sdcard/akam/{zones,mqtt,rtsp.conf}) run UNCHANGED.
#      Logs and the app log therefore live in RAM (/tmp), size-capped by the scripts themselves.
#   2  Dropbear host key generated ONCE into /config/akam (the only /config write this script ever does), then
#      start_ssh.sh in the background (key-only; needs /config/akam/authorized_keys).
#   3  hands off to /appfs/script/start.sh (drivers; then its /appfs/bin/yy_cam = our stub launcher).
#   4  background: decloud.sh on (a no-op unless /config/akam/decloud exists).
#
# User files, all in /config/akam (seed them over SSH, see docs/porting/lsc-rotatable-2k/cardless-appfs.md):
#   wpa.conf  authorized_keys  dropbear_ed25519_host_key (generated)  akam2k.conf  zones  mqtt  rtsp.conf  ntp
#   no_app  decloud   (the last two are flag files: empty)
# Never written here: nvram, mtd, /config/param.conf, anything outside /tmp and the host key above.
# Never reads a secret (wpa.conf is only symlinked; the stub copies it to /tmp with mode 600).
#
# ALWAYS reaches the hand-off to start.sh: no "set -e", every step in a subshell, every failure only logs.
# Paths are prefixed by $AKAM_ROOT (empty on the camera) for the host test.

ROOT=${AKAM_ROOT:-}
IMG=$ROOT/appfs/akam
CFG=${AKAM_CONFIG:-$ROOT/config}/akam
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
FAKE=${AKAM_TMP_DIR:-$ROOT/tmp}/akamsd
START=$ROOT/appfs/script/start.sh
SH=${AKAM_SH:-sh}

say() { echo "akam_flash_boot: $*"; }

# 1) the card-shaped tree. Image-owned files first; user files only from a fixed list (a file in /config can
#    never replace a script of the image).
(
    mkdir -p "$FAKE/akam" || exit 1
    ln -sf "$IMG/busybox" "$FAKE/busybox"
    # every file the image ships in its akam dir (no hand list to forget: akam_api_key.sh was once missed)
    for p in "$IMG"/*; do
        f=${p##*/}
        case $f in busybox | '*') continue ;; esac
        ln -sfn "$p" "$FAKE/akam/$f"
    done
) || say "WARNING: could not build $FAKE"

# 2) the host key must exist (in /config) BEFORE start_ssh.sh looks, or it would make an ephemeral one in RAM.
(
    if [ -s "$CFG/authorized_keys" ] && [ ! -s "$CFG/dropbear_ed25519_host_key" ] && [ -x "$IMG/dropbearmulti" ]; then
        umask 077
        if "$IMG/dropbearmulti" dropbearkey -t ed25519 -f "$CFG/.hostkey.new" >/dev/null 2>&1 && [ -s "$CFG/.hostkey.new" ]; then
            mv -f "$CFG/.hostkey.new" "$CFG/dropbear_ed25519_host_key"
        else
            rm -f "$CFG/.hostkey.new"
        fi
    fi
)

for f in wpa.conf authorized_keys dropbear_ed25519_host_key akam2k.conf ptz.conf zones mqtt rtsp.conf ntp no_app decloud; do
    [ -e "$CFG/$f" ] && ln -sf "$CFG/$f" "$FAKE/akam/$f"
done

# bind the tree over the (empty, or user-card) /mnt/sdcard; skip when it is already ours
if [ -L "$SD/busybox" ]; then
    say "$SD is already the Akam tree"
elif mount --bind "$FAKE" "$SD" 2>/dev/null; then
    say "Akam tree bound over $SD"
else
    say "WARNING: bind mount over $SD failed: the overlay scripts will not find their files"
fi

# the log shared with the scripts (RAM)
# shellcheck disable=SC1091  # sourced from the tree built above
if ( . "$SD/akam/common.sh" ) >/dev/null 2>&1; then
    # shellcheck disable=SC1091  # sourced from the tree built above
    . "$SD/akam/common.sh"
    log flashboot "akam_flash_boot.sh start (image $IMG, config $CFG)"
    [ -s "$CFG/wpa.conf" ] || log flashboot "NOTE: $CFG/wpa.conf missing: not seeded, Wi-Fi will fail (use the Akam SD card, seed over SSH)"
    [ -s "$CFG/authorized_keys" ] || log flashboot "NOTE: $CFG/authorized_keys missing: no SSH"
fi

# SSH in the background
( $SH "$SD/akam/start_ssh.sh" >/dev/null 2>&1 </dev/null & ) || say "WARNING: could not launch start_ssh.sh"

# 3) hand off to the stock boot: drivers, then yy_cam (= the stub launcher of this image)
"$START" &
say "handed off to $START"

# 4) decloud (flag-gated inside)
( ( $SH "$SD/akam/decloud.sh" on >/dev/null 2>&1 </dev/null & ) ) || say "WARNING: could not launch decloud.sh"
exit 0
