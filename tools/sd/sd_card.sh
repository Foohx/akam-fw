#!/usr/bin/env bash
# sd_card.sh -- HOST tool (Linux: Arch, Debian/Ubuntu): prepare, verify and wipe the Akam microSD cards.
#
# Subcommands
#   list-candidates [--human]
#       Eligible whole disks only (removable USB disks, or SD readers on mmcblk), never a disk
#       carrying /, /boot, /home or swap. Output is TSV, one disk per line, no header:
#         device <TAB> size_bytes <TAB> model <TAB> transport <TAB> mounts
#       mounts = comma list of partition:mountpoint (empty when nothing is mounted).
#   prepare --mode backup|first-boot|flash-files --device /dev/sdX [options]
#       backup       toolkit payload (telnet 24, OFFLINE=1, wifi.conf, entrypoint lock, hack.sh hook, dump.done)
#       first-boot   backup payload + AKAM_MAINTENANCE + akam/authorized_keys   (needs --pubkey FILE)
#       flash-files  ADDS the flash files to a card already prepared (no wipe, no ERASE prompt):
#                    custom_firmware/<image>, flash_tool, install_with_flash_tool.sh, flash_guard.sh,
#                    preflight_check.sh, restore.sh, akam_hwcheck.sh, original_firmware/mtdblock7.bin
#                    (needs --image IMG --flash-tool FILE --backup-dir DIR)
#       backup and first-boot WIPE the card: they refuse without a terminal and need the typed
#       confirmation "ERASE <device>".
#       Other options: --toolkit DIR (default ~/akam-sdprep/toolkit, cloned if absent; commit and
#       sd_card tree hash are verified), --hidden (read the Wi-Fi password without echo),
#       --dry-run --stage-dir DIR (touch no device: build the card tree in DIR; with --device the
#       safety guards still run).
#   verify --mode backup|first-boot|flash-files (--device DEV | --mount DIR | --stage-dir DIR) [--toolkit DIR]
#       Read-only check of a prepared card (flags, hooks, hashes against --toolkit when given, sizes,
#       no *.bin at the card root). Prints "ok:" / "FAIL:" lines and a final "VERIFY: OK|FAILED".
#   wipe --device DEV [--dry-run]
#       Zero-fill the whole card with dd (typed "ERASE <device>"), to erase cards holding a Wi-Fi
#       password or a dump.
#
# Wi-Fi: AKAM_WIFI_SSID / AKAM_WIFI_PASS from the environment, otherwise asked (visible input by
# default, --hidden to hide). They are never printed or logged. The password sits in clear text in
# custom/configs/wifi.conf on the card (and in --stage-dir): treat the card as a secret.
# Exit status: 0 success, 1 refusal or failure, 2 usage.

set -euo pipefail

SELF=${BASH_SOURCE[0]}
HERE=$(cd "$(dirname "$SELF")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck source=tools/lib/host.sh
. "$REPO/tools/lib/host.sh"
# shellcheck source=tools/sd/backend_linux.sh
. "$HERE/backend_linux.sh"

PIN_COMMIT=396db8a835d9787b2997de038b48f645c837e754
# Tree hash of sd_card, files listed in C-locale order (locale independent).
PIN_TREE_SHA=3aa3f698fd6d6f228ee785726970f708035799aa5bbcb73ef369bc3757747c2f
TOOLKIT_URL=https://github.com/tasarren/lsc-tuya-toolkit.git
MTD7_SIZE=5242880
# Test hook, honoured ONLY with --dry-run or verify: point the tree pin at a synthetic toolkit.
TESTPIN=0
if [ "${AKAM_TEST_DRYRUN_ONLY:-}" = 1 ]; then
    TESTPIN=1
    PIN_TREE_SHA=${AKAM_TEST_PIN_TREE_SHA:-}
fi

die() { echo "sd_card: error: $*" >&2; exit 1; }
usage_die() { echo "sd_card: error: $*" >&2; echo "see: $SELF --help" >&2; exit 2; }
log() { echo "sd_card: $*"; }

SUDO=
if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; fi

# ------------------------------------------------------------------- helpers
tree_hash() { # DIR: sha256 over the C-locale sorted file list
    (cd "$1" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1
}

check_pubkey() { # FILE: one ed25519 public key line
    local f=$1 kt="" kb=""
    [ -f "$f" ] || die "public key not found: $f"
    [ "$(grep -c '' "$f")" -le 1 ] || die "$f must hold exactly one key line"
    read -r kt kb _ <"$f" || true
    [ "$kt" = ssh-ed25519 ] || die "$f is not an ssh-ed25519 public key"
    case "$kb" in
        *[!A-Za-z0-9+/]*) die "$f: key blob has unexpected characters" ;;
        AAAAC3NzaC1lZDI1NTE5*) ;;
        *) die "$f does not look like an ed25519 public key" ;;
    esac
    [ "${#kb}" -eq 68 ] || die "$f: an ed25519 key blob is 68 characters long (got ${#kb})"
}

ensure_toolkit() { # DIR: pinned commit + tree hash
    local t=$1 got
    if [ ! -d "$t" ]; then
        [ "$TESTPIN" = 0 ] || die "toolkit dir missing: $t"
        log "cloning the toolkit into $t"
        git clone -q "$TOOLKIT_URL" "$t" || die "clone failed"
    fi
    if [ "$TESTPIN" = 0 ]; then
        git -C "$t" checkout -q --detach "$PIN_COMMIT" || die "cannot check out $PIN_COMMIT"
        [ "$(git -C "$t" rev-parse HEAD)" = "$PIN_COMMIT" ] || die "toolkit HEAD is not $PIN_COMMIT"
        [ -z "$(git -C "$t" status --porcelain)" ] || die "toolkit checkout is modified"
    fi
    [ -d "$t/sd_card" ] || die "$t/sd_card missing"
    got=$(tree_hash "$t/sd_card")
    [ "$got" = "$PIN_TREE_SHA" ] || die "sd_card tree hash mismatch (got $got): refusing"
    log "toolkit verified: sd_card tree $got"
}

SSID=
PASS=
get_wifi() { # AKAM_WIFI_SSID / AKAM_WIFI_PASS, else prompt (never echoed back, never logged)
    SSID=${AKAM_WIFI_SSID:-${AKAM_SD_SSID:-}}
    PASS=${AKAM_WIFI_PASS:-${AKAM_SD_PASS:-}}
    if [ -z "$SSID" ]; then
        host_is_tty || die "no terminal and AKAM_WIFI_SSID is not set"
        read -r -p 'Wi-Fi SSID (2.4 GHz): ' SSID
    fi
    if [ -z "$PASS" ]; then
        host_is_tty || die "no terminal and AKAM_WIFI_PASS is not set"
        if [ "$HIDDEN" = 1 ]; then
            read -r -s -p 'Wi-Fi password (hidden): ' PASS
            echo
        else
            read -r -p 'Wi-Fi password (shown): ' PASS
        fi
    fi
    if [ -z "$SSID" ] || [ -z "$PASS" ]; then die "SSID and password must not be empty"; fi
    case "$SSID$PASS" in *$'\n'* | *$'\r'*) die "SSID/password must not contain newlines" ;; esac
    return 0
}

q() { printf "'%s'" "${1//\'/\'\\\'\'}"; }

no_root_bin() { # DIR
    ! find "$1" -maxdepth 1 -iname '*.bin' | grep -q .
}

build_payload() { # STAGE MODE TOOLKIT PUBKEY
    local s=$1 mode=$2 t=$3 pub=$4 conf
    cp -a "$t/sd_card/." "$s/"
    mkdir -p "$s/custom/state" "$s/logs"
    : >"$s/custom/state/dump.done"
    rm -f "$s/custom/state/shadow.done"
    conf=$s/custom/configs/hack.conf
    [ -f "$conf" ] || die "hack.conf missing in the toolkit tree"
    sed -i -E 's/^TELNET=.*/TELNET=1/; s/^TELNET_PORT=.*/TELNET_PORT=24/; s/^OFFLINE=.*/OFFLINE=1/' "$conf"
    [ "$(grep -cE '^(TELNET=1|TELNET_PORT=24|OFFLINE=1)$' "$conf")" = 3 ] || die "hack.conf edits did not apply"
    printf 'MODE=sta\nSSID=%s\nPASS=%s\nSECURITY=3\nDHCP=1\n' "$(q "$SSID")" "$(q "$PASS")" >"$s/custom/configs/wifi.conf"
    sed -i '2i\# DECLOUD_ENTRYPOINT_LOCK\nmkdir /tmp/decloud_entrypoint.lock 2>/dev/null || exit 0' "$s/custom/scripts/entrypoint.sh"
    # The toolkit hack.sh has no trailing newline: keep the marker on its own line.
    if [ -n "$(tail -c1 "$s/hack.sh")" ]; then echo >>"$s/hack.sh"; fi
    cat >>"$s/hack.sh" <<'EOF_HOOK'
# DECLOUD_WIZARD_HOOK
/bin/sh "${SCRIPTS_DIR}/entrypoint.sh" >/dev/null 2>&1 &
EOF_HOOK
    if [ "$mode" = first-boot ]; then
        mkdir -p "$s/akam"
        : >"$s/AKAM_MAINTENANCE"
        cp "$pub" "$s/akam/authorized_keys"
    fi
    no_root_bin "$s" || die "a *.bin sits at the card root: refusing (stock tf_update.sh would run system_upgrade on it)"
}

FLASH_SCRIPTS=(
    "tools/flash_scripts/install_with_flash_tool.sh"
    "tools/flash_scripts/flash_guard.sh"
    "tools/flash_scripts/preflight_check.sh"
    "tools/flash_scripts/restore.sh"
    "tools/hw_tests/akam_hwcheck.sh"
)
IMG_RE='^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs$'

build_flash_overlay() { # STAGE IMAGE FLASHTOOL BACKUPDIR
    local s=$1 img=$2 ft=$3 bd=$4 base f want got
    base=$(basename "$img")
    [[ $base =~ $IMG_RE ]] || die "image name must look like ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs (got $base)"
    [ -s "$img" ] || die "image missing or empty: $img"
    if [ -f "$img.sha256" ]; then
        want=$(cut -d' ' -f1 <"$img.sha256")
        got=$(host_sha256 "$img")
        [ "$want" = "$got" ] || die "$base does not match its .sha256 file"
    fi
    [ -s "$ft" ] || die "flash_tool missing or empty: $ft"
    [ -f "$bd/mtdblock7.bin" ] || die "$bd/mtdblock7.bin missing (stock APP backup)"
    [ "$(wc -c <"$bd/mtdblock7.bin")" -eq "$MTD7_SIZE" ] || die "$bd/mtdblock7.bin is not $MTD7_SIZE bytes"
    mkdir -p "$s/custom_firmware" "$s/original_firmware"
    cp "$img" "$s/custom_firmware/$base"
    cp "$ft" "$s/flash_tool"
    chmod 755 "$s/flash_tool"
    for f in "${FLASH_SCRIPTS[@]}"; do
        [ -f "$REPO/$f" ] || die "repo script missing: $f"
        cp "$REPO/$f" "$s/$(basename "$f")"
    done
    cp "$bd/mtdblock7.bin" "$s/original_firmware/mtdblock7.bin"
    no_root_bin "$s" || die "a *.bin sits at the card root: refusing"
}

# ------------------------------------------------------------------- verify
V_FAIL=0
vok() { echo "ok: $1"; }
vbad() { echo "FAIL: $1"; V_FAIL=$((V_FAIL + 1)); }
vchk() { # DESC CMD...
    local d=$1
    shift
    if "$@" >/dev/null 2>&1; then vok "$d"; else vbad "$d"; fi
}

verify_payload() { # DIR MODE TOOLKIT(optional)
    local d=$1 mode=$2 t=$3 conf=$1/custom/configs/hack.conf f rel
    vchk "hack.conf: TELNET=1, TELNET_PORT=24, OFFLINE=1" bash -c \
        "grep -qx 'TELNET=1' '$conf' && grep -qx 'TELNET_PORT=24' '$conf' && grep -qx 'OFFLINE=1' '$conf'"
    vchk "dump.done present" test -f "$d/custom/state/dump.done"
    vchk "shadow.done absent" test ! -e "$d/custom/state/shadow.done"
    vchk "entrypoint.sh lock marker" grep -qx '# DECLOUD_ENTRYPOINT_LOCK' "$d/custom/scripts/entrypoint.sh"
    vchk "hack.sh hook marker on its own line" grep -qx '# DECLOUD_WIZARD_HOOK' "$d/hack.sh"
    vchk "hack.sh hook command follows the marker" bash -c \
        "grep -A1 -x '# DECLOUD_WIZARD_HOOK' '$d/hack.sh' | tail -n1 | grep -q 'entrypoint.sh'"
    vchk "wifi.conf has MODE=sta, SSID, PASS, SECURITY=3, DHCP=1" bash -c \
        "w='$d/custom/configs/wifi.conf'; grep -qx 'MODE=sta' \"\$w\" && grep -q '^SSID=.' \"\$w\" && grep -q '^PASS=.' \"\$w\" && grep -qx 'SECURITY=3' \"\$w\" && grep -qx 'DHCP=1' \"\$w\""
    if [ "$mode" = first-boot ]; then
        vchk "AKAM_MAINTENANCE present" test -f "$d/AKAM_MAINTENANCE"
        vchk "akam/authorized_keys is one ed25519 key" bash -c \
            "[ \"\$(grep -c '' '$d/akam/authorized_keys')\" -eq 1 ] && grep -q '^ssh-ed25519 ' '$d/akam/authorized_keys'"
    else
        vchk "AKAM_MAINTENANCE absent (backup card)" test ! -e "$d/AKAM_MAINTENANCE"
    fi
    if [ -n "$t" ]; then
        local bad=0
        while IFS= read -r f; do
            rel=${f#./}
            case $rel in
                custom/configs/hack.conf | custom/configs/wifi.conf | custom/scripts/entrypoint.sh | hack.sh | custom/state/shadow.done) continue ;;
            esac
            if [ ! -f "$d/$rel" ] || [ "$(host_sha256 "$d/$rel")" != "$(host_sha256 "$t/sd_card/$rel")" ]; then
                echo "FAIL: differs from the toolkit: $rel"
                bad=$((bad + 1))
            fi
        done < <(cd "$t/sd_card" && find . -type f | LC_ALL=C sort)
        if [ "$bad" -eq 0 ]; then vok "toolkit files unchanged (sha256)"; else V_FAIL=$((V_FAIL + bad)); fi
    else
        echo "skip: toolkit file hashes (no --toolkit given)"
    fi
}

verify_flash() { # DIR
    local d=$1 n f
    n=$(find "$d/custom_firmware" -maxdepth 1 -type f 2>/dev/null | wc -l)
    vchk "custom_firmware holds exactly one file" test "$n" -eq 1
    f=$(find "$d/custom_firmware" -maxdepth 1 -type f -name 'ak_rtsp_firmware_*.squashfs' 2>/dev/null | head -n1)
    vchk "image is ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs" bash -c "[[ \$(basename '${f:-x}') =~ $IMG_RE ]]"
    if [ -n "$f" ]; then
        vchk "image is not empty" test -s "$f"
    fi
    vchk "flash_tool present" test -s "$d/flash_tool"
    for f in install_with_flash_tool.sh flash_guard.sh preflight_check.sh restore.sh akam_hwcheck.sh; do
        vchk "$f present" test -s "$d/$f"
    done
    vchk "original_firmware/mtdblock7.bin is $MTD7_SIZE bytes" bash -c \
        "[ \"\$(wc -c <'$d/original_firmware/mtdblock7.bin')\" -eq $MTD7_SIZE ]"
}

verify_dir() { # DIR MODE TOOLKIT
    V_FAIL=0
    vchk "no *.bin at the card root" bash -c "! find '$1' -maxdepth 1 -iname '*.bin' | grep -q ."
    case $2 in
        flash-files) verify_flash "$1" ;;
        *) verify_payload "$1" "$2" "$3" ;;
    esac
    if [ "$V_FAIL" -eq 0 ]; then echo "VERIFY: OK"; return 0; fi
    echo "VERIFY: FAILED ($V_FAIL problem(s))"
    return 1
}

# ------------------------------------------------------------------- commands
MODE=
DEV=
PUBKEY=
IMAGE=
FLASHTOOL=
BACKUPDIR=
TOOLKIT=$HOME/akam-sdprep/toolkit
TOOLKIT_GIVEN=0
DRY=0
STAGE=
HIDDEN=0
HUMAN=0
MOUNTDIR=

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --mode | --device | --pubkey | --image | --flash-tool | --backup-dir | --toolkit | --stage-dir | --mount)
                [ $# -ge 2 ] || usage_die "$1 needs a value" ;;
        esac
        case "$1" in
            --mode) MODE=$2; shift 2 ;;
            --device) DEV=$2; shift 2 ;;
            --pubkey) PUBKEY=$2; shift 2 ;;
            --image) IMAGE=$2; shift 2 ;;
            --flash-tool) FLASHTOOL=$2; shift 2 ;;
            --backup-dir) BACKUPDIR=$2; shift 2 ;;
            --toolkit) TOOLKIT=$2; TOOLKIT_GIVEN=1; shift 2 ;;
            --stage-dir) STAGE=$2; shift 2 ;;
            --mount) MOUNTDIR=$2; shift 2 ;;
            --dry-run) DRY=1; shift ;;
            --hidden) HIDDEN=1; shift ;;
            --human) HUMAN=1; shift ;;
            -h | --help) sed -n 2,40p "$SELF"; exit 0 ;;
            *) usage_die "unknown argument: $1" ;;
        esac
    done
}

guard_device() { # DEV: refuse anything that is not an eligible card
    if ! be_check_disk "$1"; then die "refusing: $D_WHY"; fi
}

need_tools() {
    local t
    for t in "$@"; do host_have "$t" || die "missing tool: $t ($(host_pkg_install_hint util-linux dosfstools))"; done
}

show_device() { # DEV (after guard_device)
    echo
    echo "  device : $1"
    echo "  model  : ${D_MODEL:-?}"
    echo "  size   : $D_SIZE bytes"
    echo "  mounted: ${D_MOUNTS:-nothing}"
    echo
}

confirm_erase() { # DEV
    local rc=0
    host_confirm_typed "ALL DATA ON $1 WILL BE ERASED." "ERASE $1" || rc=$?
    echo >&2
    case $rc in
        0) ;;
        2) die "refusing to erase without a terminal" ;;
        *) die "aborted, nothing was written" ;;
    esac
}

MNT=
STAGE_TMP=
MOUNTED_BY_US=0
cleanup() {
    if [ -n "$MNT" ] && [ "$MOUNTED_BY_US" = 1 ]; then
        if mountpoint -q "$MNT" 2>/dev/null; then $SUDO umount "$MNT" 2>/dev/null || true; fi
        rmdir "$MNT" 2>/dev/null || true
    fi
    if [ -n "$STAGE_TMP" ]; then rm -rf "$STAGE_TMP"; fi
    return 0
}
trap cleanup EXIT

make_stage() { # sets S
    if [ "$DRY" = 1 ]; then
        [ -n "$STAGE" ] || usage_die "--dry-run needs --stage-dir DIR"
        mkdir -p "$STAGE"
        [ -z "$(ls -A "$STAGE")" ] || die "--stage-dir must be empty"
        S=$STAGE
    else
        STAGE_TMP=$(mktemp -d "${TMPDIR:-/tmp}/akam-sd.XXXXXX") || die "mktemp failed"
        S=$STAGE_TMP
    fi
}

cmd_list() {
    local dev size model tran mounts
    if [ "$HUMAN" = 0 ]; then be_list_candidates; return 0; fi
    while IFS=$'\t' read -r dev size model tran mounts; do
        printf '%-14s %8s MiB  %-24s %-4s %s\n' "$dev" "$((size / 1048576))" "$model" "$tran" "${mounts:-not mounted}"
    done < <(be_list_candidates)
    return 0
}

cmd_prepare() {
    local part
    case "$MODE" in
        backup | first-boot | flash-files) ;;
        *) usage_die "--mode must be backup, first-boot or flash-files" ;;
    esac
    if [ "$MODE" = first-boot ]; then
        [ -n "$PUBKEY" ] || usage_die "--pubkey is required for first-boot"
        check_pubkey "$PUBKEY"
    fi
    if [ "$MODE" = flash-files ]; then
        if [ -z "$IMAGE" ] || [ -z "$FLASHTOOL" ] || [ -z "$BACKUPDIR" ]; then
            usage_die "flash-files needs --image, --flash-tool and --backup-dir"
        fi
    fi
    if [ "$TESTPIN" = 1 ] && [ "$DRY" = 0 ]; then die "AKAM_TEST_DRYRUN_ONLY is for --dry-run only"; fi
    if [ "$DRY" = 1 ]; then
        [ -n "$STAGE" ] || usage_die "--dry-run needs --stage-dir DIR"
        if [ -n "$DEV" ]; then guard_device "$DEV"; fi
    else
        [ -n "$DEV" ] || usage_die "--device is required (see: $SELF list-candidates)"
        need_tools lsblk
        guard_device "$DEV"
        if [ "$MODE" != flash-files ]; then
            host_is_tty || die "refusing to erase a card without a terminal"
            need_tools wipefs sfdisk mkfs.vfat partprobe blockdev
        fi
    fi

    if [ "$MODE" = flash-files ]; then
        make_stage
        build_flash_overlay "$S" "$IMAGE" "$FLASHTOOL" "$BACKUPDIR"
        if [ "$DRY" = 1 ]; then
            log "dry run: flash files staged in $S (overlay only, no device touched). Top level:"
            ls -A "$S"
            return 0
        fi
        flash_files_to_card "$S"
        return 0
    fi

    ensure_toolkit "$TOOLKIT"
    get_wifi
    make_stage
    build_payload "$S" "$MODE" "$TOOLKIT" "$PUBKEY"
    verify_dir "$S" "$MODE" "$TOOLKIT" >/dev/null || die "internal check failed on the staged tree"
    if [ "$DRY" = 1 ]; then
        log "dry run: card tree built in $S (no device touched). Top level:"
        ls -A "$S"
        return 0
    fi

    show_device "$DEV"
    confirm_erase "$DEV"
    [ -b "$DEV" ] || die "$DEV is not a block device"
    be_unmount_all "$DEV" || die "cannot unmount the partitions of $DEV"
    part=$(be_format "$DEV") || die "formatting $DEV failed (a reader that says \"No medium found\" needs a replug)"
    MNT=$(mktemp -d "${TMPDIR:-/tmp}/akam-mnt.XXXXXX") || die "mktemp failed"
    be_mount "$part" "$MNT" || die "mount of $part failed"
    MOUNTED_BY_US=1
    cp -r "$S/." "$MNT/" || die "copy to the card failed"
    sync
    verify_dir "$MNT" "$MODE" "$TOOLKIT" || die "the card does not verify after the copy"
    $SUDO umount "$MNT" || die "umount failed"
    MOUNTED_BY_US=0
    rmdir "$MNT"
    MNT=
    sync
    log "done ($MODE). Card ready: you can remove it now."
}

flash_files_to_card() { # STAGE
    local s=$1 part node mp found="" old
    part=$(be_part_path "$DEV")
    [ -b "$part" ] || die "$part is not a block device (is the card prepared and inserted?)"
    while IFS=$'\t' read -r node mp; do
        if [ "$node" = "$part" ]; then found=$mp; fi
    done < <(be_mounts "$DEV")
    if [ -n "$found" ]; then
        MNT=$found
    else
        MNT=$(mktemp -d "${TMPDIR:-/tmp}/akam-mnt.XXXXXX") || die "mktemp failed"
        be_mount "$part" "$MNT" || die "mount of $part failed"
        MOUNTED_BY_US=1
    fi
    [ -f "$MNT/hack.sh" ] || die "$part does not look like an Akam card (no hack.sh): prepare it first"
    for old in "$MNT"/custom_firmware/*; do
        if [ -e "$old" ]; then rm -f "$old"; log "removed the previous image $(basename "$old")"; fi
    done
    cp -r "$s/." "$MNT/" || die "copy to the card failed"
    sync
    verify_dir "$MNT" flash-files "" || die "the flash files do not verify on the card"
    if [ "$MOUNTED_BY_US" = 1 ]; then
        $SUDO umount "$MNT" || die "umount failed"
        MOUNTED_BY_US=0
        rmdir "$MNT"
    fi
    MNT=
    sync
    log "done (flash-files). Card ready: you can remove it now."
}

cmd_verify() {
    local dir part tk=""
    case "$MODE" in
        backup | first-boot | flash-files) ;;
        *) usage_die "--mode must be backup, first-boot or flash-files" ;;
    esac
    if [ "$TOOLKIT_GIVEN" = 1 ]; then tk=$TOOLKIT; [ -d "$tk/sd_card" ] || die "$tk/sd_card missing"; fi
    if [ -n "$STAGE" ]; then
        dir=$STAGE
    elif [ -n "$MOUNTDIR" ]; then
        dir=$MOUNTDIR
    elif [ -n "$DEV" ]; then
        need_tools lsblk
        guard_device "$DEV"
        part=$(be_part_path "$DEV")
        [ -b "$part" ] || die "$part is not a block device"
        MNT=$(mktemp -d "${TMPDIR:-/tmp}/akam-mnt.XXXXXX") || die "mktemp failed"
        be_mount "$part" "$MNT" ro || die "read-only mount of $part failed"
        MOUNTED_BY_US=1
        dir=$MNT
    else
        usage_die "verify needs --device, --mount or --stage-dir"
    fi
    [ -d "$dir" ] || die "not a directory: $dir"
    verify_dir "$dir" "$MODE" "$tk"
}

cmd_wipe() {
    [ -n "$DEV" ] || usage_die "--device is required"
    need_tools lsblk
    guard_device "$DEV"
    if [ "$DRY" = 1 ]; then
        log "dry run: would zero-fill $DEV (${D_SIZE} bytes) with dd after the typed confirmation"
        return 0
    fi
    host_is_tty || die "refusing to erase a card without a terminal"
    need_tools dd blockdev
    show_device "$DEV"
    confirm_erase "$DEV"
    [ -b "$DEV" ] || die "$DEV is not a block device"
    be_unmount_all "$DEV" || die "cannot unmount the partitions of $DEV"
    be_zero_fill "$DEV" "$D_SIZE" || die "dd failed"
    sync
    log "done: $DEV is zero-filled."
}

main() {
    local cmd=${1:-}
    [ $# -gt 0 ] || { sed -n 2,40p "$SELF"; exit 2; }
    shift
    case "$cmd" in
        -h | --help | help) sed -n 2,40p "$SELF"; exit 0 ;;
        list-candidates | prepare | verify | wipe) ;;
        *) usage_die "unknown subcommand: $cmd" ;;
    esac
    parse_args "$@"
    case "$cmd" in
        list-candidates) cmd_list ;;
        prepare) cmd_prepare ;;
        verify) cmd_verify ;;
        wipe) cmd_wipe ;;
    esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi
