#!/usr/bin/env bash
# fake_camera.sh: a fake camera for the wizard tests. NOT a product tool, never part of a kit.
#
# It is a telnet-like server on 127.0.0.1 (any port) that gives a BusyBox shell over a private
# directory tree. The "flash" is a set of regular files; nothing outside the tree is touched and
# no real device is ever opened. It needs the pinned BusyBox (tools/build/bootstrap.sh): its `nc
# -lk -e` is the TCP server and its applets are the only commands the "camera" knows (no `wc`,
# like the real rootfs).
#
# Usage:
#   fake_camera.sh init ROOT [--flash-card IMAGE FLASH_TOOL] [--no-card]
#   fake_camera.sh start ROOT PORT       start the server (pid in ROOT/ctl/pid)
#   fake_camera.sh stop ROOT
# Environment: AKAM_BUSYBOX (default: the pinned one), FAKE_STOCK_VERSION to change /proc/version,
# FAKE_WIFI_SSID / FAKE_WIFI_PASS (the card's custom/configs/wifi.conf with --flash-card; the camera
# starts WITHOUT /etc/config/wpa_supplicant.conf, like one never paired with the vendor app).
#
# What the shell looks like (reconstructed from docs/akam_v1_backup_sd.md, NOT recorded on a camera):
#   - IAC negotiation bytes at connect, CRLF line endings on the echo of the typed line, "# " prompt;
#   - paths /dev/mtd*, /proc/*, /tmp/*, /mnt, /etc/* are mapped into ROOT; the card is /tmp/sd;
#   - `mount` lists the card, `mount --bind A B` links B to A, `uname -r/-v` answer, reboot and
#     poweroff only write a line to ROOT/ctl/events (the tests assert it stays empty);
#   - sh -c per command, with stdin shared with the session, so a script that does `read` consumes
#     the next line sent by the client (this is how the "Type YES to continue" prompt behaves).
# Fault injection, by files in ROOT/ctl (the content is a count, decremented at each use):
#   drop      ignore the next N command lines completely (the client sees a timeout)
#   close     close the connection at the next command line
#   corrupt_rx  after `nc -l -p P > FILE` flip one byte of FILE (a corrupted PC->camera transfer)
#   corrupt_tx  serve `nc -l -p P < FILE` from a copy with one flipped byte (camera->PC)
# Logs: ctl/commands.log (every received line), ctl/events, ctl/confirm.log (written by the fake
# flash script: what it read at its "Type YES" prompt).
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BB=${AKAM_BUSYBOX:-$REPO/tools/.toolchain/busybox-1.35.0-x86_64/busybox}

MTD_SIZES=(204800 4096 4096 65536 1572864 1032192 262144 5242880)
MTD_NAMES=(UBOOT ENV ENVBK DTB KERNEL ROOTFS CONFIG APP)
MTD_HEX=(00032000 00001000 00001000 00010000 00180000 000fc000 00040000 00500000)

die() { echo "fake_camera: $*" >&2; exit 1; }

cmd_init() {
    local R=$1 i n card=1 img="" ft=""
    shift
    while [ $# -gt 0 ]; do
        case $1 in
            --flash-card) img=$2; ft=$3; shift 3 ;;
            --no-card) card=0; shift ;;
            *) die "init: unknown option $1" ;;
        esac
    done
    [ -x "$BB" ] || die "BusyBox not found: $BB (run tools/build/bootstrap.sh)"
    mkdir -p "$R"/{dev,proc,tmp,mnt,etc,ctl,.bin}
    : >"$R/ctl/commands.log"
    : >"$R/ctl/events"
    {
        echo 'dev:    size   erasesize  name'
        i=0
        for n in "${MTD_NAMES[@]}"; do
            printf 'mtd%d: %s 00001000 "%s"\n' "$i" "${MTD_HEX[$i]}" "$n"
            i=$((i + 1))
        done
    } >"$R/proc/mtd"
    echo "Linux version ${FAKE_STOCK_VERSION:-4.4.302-cip94} (fake@build) (gcc) #1 Fri Nov 14 19:06:55 CST 2025" >"$R/proc/version"
    mkdir -p "$R/etc/config"
    echo "fake config" >"$R/etc/config/fake"
    for i in 0 1 2 3 4 5 6; do
        # Distinct, deterministic content, exact size.
        { yes "fake mtd$i content" | head -c "${MTD_SIZES[$i]}"; } >"$R/dev/mtdblock$i" 2>/dev/null
    done
    { yes "fake APP content" | head -c "${MTD_SIZES[7]}"; } >"$R/dev/mtd7" 2>/dev/null
    ln -sfn mtd7 "$R/dev/mtdblock7"
    # Command subset: BusyBox applets only (wc is deliberately absent, as on the camera).
    for n in cat dd stat md5sum sha256sum ls df tail head grep sed sh sleep mkdir rm cp chmod nc \
        sync ps awk true false expr tee mv ln rmdir cmp date id seq tr cut sort printf echo test '['; do
        ln -sf "$BB" "$R/.bin/$n"
    done
    cat >"$R/.bin/mount" <<'EOF'
#!/bin/sh
# fake mount: no argument lists the card; --bind SRC DST makes DST a link to SRC
if [ "$1" = "--bind" ]; then
    rmdir "$3" 2>/dev/null
    ln -sfn "$2" "$3"
    exit 0
fi
echo "/dev/root on / type squashfs (ro,relatime)"
echo "/dev/mmcblk0p1 on /tmp/sd type vfat (rw,relatime)"
EOF
    cat >"$R/.bin/uname" <<'EOF'
#!/bin/sh
case "$1" in
    -r) echo "${FAKE_UNAME_R:-4.4.302-cip94}" ;;
    -v) echo "${FAKE_UNAME_V:-#1 Fri Nov 14 19:06:55 CST 2025}" ;;
    *) echo "Linux camera ${FAKE_UNAME_R:-4.4.302-cip94} armv5tejl" ;;
esac
EOF
    for n in reboot poweroff halt; do
        cat >"$R/.bin/$n" <<EOF
#!/bin/sh
echo "$n called" >>"\$FAKE_ROOT/ctl/events"
EOF
    done
    chmod +x "$R"/.bin/mount "$R"/.bin/uname "$R"/.bin/reboot "$R"/.bin/poweroff "$R"/.bin/halt
    if [ "$card" = 1 ]; then
        mkdir -p "$R/tmp/sd/logs"
        echo "toolkit card" >"$R/tmp/sd/hack.bin"
    fi
    if [ -n "$img" ]; then
        mkdir -p "$R/tmp/sd/custom_firmware" "$R/tmp/sd/original_firmware"
        cp "$img" "$R/tmp/sd/custom_firmware/$(basename "$img")"
        cp "$ft" "$R/tmp/sd/flash_tool"
        echo "#!/bin/sh" >"$R/tmp/sd/flash_guard.sh"
        # What the toolkit card carries: the real preflight_check.sh and the Wi-Fi settings (the card is
        # the only place that holds them on a camera never paired with the app: no wpa_supplicant.conf).
        cp "$REPO/tools/flash_scripts/preflight_check.sh" "$R/tmp/sd/preflight_check.sh"
        mkdir -p "$R/tmp/sd/custom/configs"
        printf "MODE=sta\nSSID='%s'\nPASS='%s'\nSECURITY=3\nDHCP=1\n" "${FAKE_WIFI_SSID:-Fake Net}" "${FAKE_WIFI_PASS:-fake-wifi-pass}" \
            >"$R/tmp/sd/custom/configs/wifi.conf"
        cat >"$R/tmp/sd/install_with_flash_tool.sh" <<'EOF'
#!/bin/sh
# fake of tools/flash_scripts/install_with_flash_tool.sh (same prompts and messages)
M=$FAKE_ROOT/mnt
DEV=$FAKE_ROOT/dev/mtd7
n=0
for f in "$M"/custom_firmware/ak_rtsp_firmware_*.squashfs; do IMG=$f; n=$((n + 1)); done
if [ "$n" -lt 1 ] || [ ! -f "$IMG" ]; then
    echo "ERROR: no ak_rtsp_firmware_*.squashfs found in /mnt/custom_firmware/"
    exit 1
fi
SIZE=$(stat -c%s "$IMG")
if [ "$SIZE" -ne 5242880 ]; then
    echo "ERROR: expected exactly 5242880 bytes (full partition), got $SIZE - refusing to flash"
    exit 1
fi
echo "md5sum of $IMG (record this before flashing):"
md5sum "$IMG"
echo "About to overwrite /dev/mtd7 (APP partition) with $IMG using /mnt/flash_tool."
printf "Type YES to continue: "
read CONFIRM
echo "CONFIRM=$CONFIRM" >>"$FAKE_ROOT/ctl/confirm.log"
if [ "$CONFIRM" != "YES" ]; then
    echo "Aborted."
    exit 1
fi
MODE=ok
[ -f "$FAKE_ROOT/ctl/flash_mode" ] && MODE=$(cat "$FAKE_ROOT/ctl/flash_mode")
# flash_tool draws its bars with a carriage return and no newline: about 200 KB on the real camera
i=0
while [ $i -le 3000 ]; do
    printf '\rWriting: [====>                                   ] %3d%% (%d/5242880)' $((i / 30)) $((i * 1747))
    i=$((i + 1))
done
printf '\n'
if [ "$MODE" = fail ]; then
    echo "ERROR: Flashing/Verification failed with code 1!"
    exit 1
fi
cp "$IMG" "$DEV"
# badwrite: the script says success but the partition is not the image (the read-back must catch it)
[ "$MODE" = badwrite ] && printf 'Z' | dd of="$DEV" bs=1 count=1 conv=notrunc 2>/dev/null
echo "FAULT SUMMARY: 0 retries"
# noline: the write is good, the success line is missing (the read-back must still pass)
[ "$MODE" = noline ] || echo "Flashing and verification completed successfully!"
echo "Disconnect and reconnect USB-C power to power cycle."
EOF
        chmod +x "$R/tmp/sd/flash_tool"
    fi
    echo "$R"
}

cmd_start() {
    local R=$1 port=$2
    [ -f "$R/proc/mtd" ] || die "start: run init first"
    # shellcheck disable=SC2016
    "$BB" nc -lk -p "$port" -e bash "${BASH_SOURCE[0]}" session "$R" >/dev/null 2>&1 &
    echo $! >"$R/ctl/pid"
    sleep 0.3
}

cmd_stop() {
    local pid
    pid=$(cat "$1/ctl/pid" 2>/dev/null || true)
    if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
    return 0
}

# ctl_take NAME: true (and decrement) when ROOT/ctl/NAME holds a count > 0.
ctl_take() {
    local f="$R/ctl/$1" n
    [ -f "$f" ] || return 1
    n=$(cat "$f")
    case $n in '' | *[!0-9]*) return 1 ;; esac
    [ "$n" -gt 0 ] || return 1
    echo $((n - 1)) >"$f"
}

flip_byte() { # file: change the first byte
    printf 'Z' | dd of="$1" bs=1 count=1 conv=notrunc 2>/dev/null
}

cmd_session() {
    R=$1
    local line cmd target tmp
    export PATH="$R/.bin" FAKE_ROOT="$R" AKAM_ROOT="$R"
    printf '\377\375\042\377\373\001'
    printf '# '
    while IFS= read -r line; do
        line=${line%$'\r'}
        echo "$line" >>"$R/ctl/commands.log"
        if ctl_take drop; then continue; fi
        if ctl_take close; then exit 0; fi
        printf '%s\r\n' "$line"
        case $line in exit | exit\ *) exit 0 ;; esac
        cmd=$(printf '%s\n' "$line" | sed \
            -e 's#/dev/mtd#@@R@@/dev/mtd#g' -e 's#/proc/#@@R@@/proc/#g' \
            -e 's#/tmp/#@@R@@/tmp/#g' -e 's#/mnt#@@R@@/mnt#g' -e 's#/etc/#@@R@@/etc/#g' \
            -e "s#@@R@@#$R#g")
        if [[ $cmd =~ nc\ -l\ -p\ [0-9]+\ \<\ \'?([^\ \';]+) ]] && ctl_take corrupt_tx; then
            target=${BASH_REMATCH[1]}
            tmp="$R/ctl/tx.tmp"
            cp "$target" "$tmp"
            flip_byte "$tmp"
            cmd=${cmd//"$target"/$tmp}
        fi
        "$BB" sh -c "$cmd" || true
        if [[ $cmd =~ nc\ -l\ -p\ [0-9]+\ \>\ \'?([^\ \';]+) ]] && ctl_take corrupt_rx; then
            flip_byte "${BASH_REMATCH[1]}"
        fi
        printf '# '
    done
}

case ${1:-} in
    init) shift; cmd_init "$@" ;;
    start) cmd_start "$2" "$3" ;;
    stop) cmd_stop "$2" ;;
    session) cmd_session "$2" ;;
    *) die "usage: fake_camera.sh init|start|stop ROOT ..." ;;
esac
