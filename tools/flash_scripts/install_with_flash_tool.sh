#!/bin/sh
# install_with_flash_tool.sh — flash using the custom compile flash_tool binary.
# Erases, writes, and verifies the APP partition (/dev/mtd7) directly.
#
# This is the PRIMARY, proven-safe method: unlike a plain `dd` to
# /dev/mtdblock7 (which corrupts writes via DMA contention with the SPI-NOR
# controller when WiFi/camera/ISP drivers are active), flash_tool writes and
# immediately reads back each 4KB erase-block via the raw /dev/mtd7 char
# device (not page-cached) and retries any mismatched chunk. This has been
# confirmed safe running live over Telnet with WiFi/camera/ISP all active —
# on STOCK firmware only. On a Camyn image the APP partition is mounted on /usr
# and holds dropbear/watchdog, so flash from the UART init=/bin/sh shell with
# this script and flash_tool on the SD card, never over SSH (flash_guard.sh
# refuses; override CAMYN_FLASH_I_KNOW=1). See docs/camyn_v1_flash_and_restore.md.
#
# Run: sh /mnt/install_with_flash_tool.sh

DEV=/dev/mtd7
TOOL=/mnt/flash_tool

# Refuse to write APP from a context that depends on it (SSH, running from /usr).
# No dirname: the bare UART init=/bin/sh rootfs has no dirname (it lives in the APP).
case "$0" in */*) _g="${0%/*}/flash_guard.sh" ;; *) _g="./flash_guard.sh" ;; esac
if [ -r "$_g" ]; then
    # shellcheck source=flash_guard.sh
    . "$_g"
    camyn_flash_guard "$0" "$TOOL" || exit 1
elif [ "${CAMYN_FLASH_I_KNOW:-}" != "1" ]; then
    echo "ERROR: flash_guard.sh not found next to $0 (copy it to the SD card too)." >&2
    exit 1
fi
EXPECTSIZE=5242880

if [ ! -f "$TOOL" ]; then
    echo "ERROR: Flash tool $TOOL not found. Please compile it and copy to SD card root."
    exit 1
fi

# Pick the newest ak_rtsp_firmware_*.squashfs by filename (lexicographic == chronological
# for YYYYMMDD_HHMMSS). This camera has no working RTC, so file mtimes are meaningless —
# the build timestamp is baked into the filename itself instead. Shell glob results are
# sorted, so the last match in the loop wins.
IMG=""
for f in /mnt/custom_firmware/ak_rtsp_firmware_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].squashfs; do
    IMG="$f"
done
if [ -z "$IMG" ] || [ ! -f "$IMG" ]; then
    echo "ERROR: no ak_rtsp_firmware_*.squashfs found in /mnt/custom_firmware/"
    exit 1
fi

SIZE=$(stat -c%s "$IMG" 2>/dev/null)
if [ -z "$SIZE" ]; then
    echo "ERROR: Could not get size of $IMG"
    exit 1
fi

if [ "$SIZE" -ne "$EXPECTSIZE" ]; then
    echo "ERROR: expected exactly $EXPECTSIZE bytes (full partition), got $SIZE — refusing to flash"
    exit 1
fi

# Ensure device node exists (major 90, minor 14 for partition 7 RW)
if [ ! -c "$DEV" ]; then
    echo "Device node $DEV not found, creating it..."
    mknod "$DEV" c 90 14 || { echo "ERROR: failed to create device node $DEV"; exit 1; }
fi

echo "md5sum of $IMG (record this before flashing):"
md5sum "$IMG"

echo "About to overwrite $DEV (APP partition) with $IMG using $TOOL."
echo "This will erase, write, and verify directly against the physical SPI NOR."
printf "Type YES to continue: "
read CONFIRM
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

# Run the direct flash tool
"$TOOL" "$IMG" "$DEV"
RET=$?

if [ $RET -eq 0 ]; then
    echo "Flashing and verification completed successfully!"
    echo "Disconnect and reconnect USB-C power to power cycle."
else
    echo "ERROR: Flashing/Verification failed with code $RET!"
    echo "DO NOT power cycle yet — check the error messages above."
    echo "Try to re-run the flash tool, or restore your stock backup (/mnt/restore.sh) before power cycling!"
    exit $RET
fi
