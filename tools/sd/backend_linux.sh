# shellcheck shell=bash
# shellcheck disable=SC2034  # D_* results are read by sd_card.sh
# Linux backend of tools/sd/sd_card.sh (Arch, Debian/Ubuntu): device discovery, safety
# guards and the privileged disk operations. Source it, do not run it.
# Test hooks (tests only): AKAM_SYSFS overrides /sys; lsblk may be replaced by a stub on PATH.
# Tools: lsblk wipefs sfdisk mkfs.vfat partprobe blockdev mount umount sync dd.

BE_MIN_BYTES=134217728     # 128 MiB
BE_MAX_BYTES=137438953472  # 128 GiB

be_sysfs() { printf '%s\n' "${AKAM_SYSFS:-/sys}"; }

# be_kv LINE KEY: value of KEY="..." in an `lsblk -P` line (empty if absent).
be_kv() {
    local re="(^| )$2=\"([^\"]*)\""
    if [[ $1 =~ $re ]]; then printf '%s' "${BASH_REMATCH[2]}"; fi
    return 0
}

# be_size_bytes DEV: size from sysfs (512-byte sectors, readable without root); 0 if no medium.
be_size_bytes() {
    local n s
    n=$(basename "$1")
    s=$(cat "$(be_sysfs)/class/block/$n/size" 2>/dev/null) || s=0
    case $s in '' | *[!0-9]*) s=0 ;; esac
    echo $((s * 512))
}

# be_part_path DEV [N]: first (or Nth) partition node. sdb -> sdb1, mmcblk0 -> mmcblk0p1.
be_part_path() {
    case $1 in
        *[0-9]) printf '%sp%s\n' "$1" "${2:-1}" ;;
        *) printf '%s%s\n' "$1" "${2:-1}" ;;
    esac
}

# be_is_whole_disk_name DEV: /dev/sdX or /dev/mmcblkN only (never a partition, never nvme/loop).
be_is_whole_disk_name() {
    [[ $1 =~ ^/dev/(sd[a-z]+|mmcblk[0-9]+)$ ]]
}

# be_mounts DEV: lines "node<TAB>mountpoint" for every mounted node of DEV (partitions included).
be_mounts() {
    local line mp name
    while IFS= read -r line; do
        mp=$(be_kv "$line" MOUNTPOINT)
        [ -n "$mp" ] || continue
        name=$(be_kv "$line" NAME)
        printf '%s\t%s\n' "$name" "$mp"
    done < <(lsblk -pP -o NAME,TYPE,MOUNTPOINT "$1" 2>/dev/null)
}

# be_carries_system DEV: print the first system mountpoint (/, /boot, /home, swap...) and return 0.
be_carries_system() {
    local node mp
    while IFS=$'\t' read -r node mp; do
        case $mp in
            / | /boot | /boot/* | /usr | /usr/* | /var | /var/* | /etc | /home | /home/* | /opt | /srv | /nix | /nix/* | '[SWAP]')
                printf '%s' "$mp"
                return 0 ;;
        esac
    done < <(be_mounts "$1")
    return 1
}

# be_check_disk DEV: sets D_SIZE D_MODEL D_TRAN D_MOUNTS (comma list node:mountpoint).
# Returns 0 when DEV is an eligible card; otherwise sets D_WHY (the reason) and returns 1.
be_check_disk() {
    local dev=$1 line type rm tran name mp node sysmp kind
    D_SIZE=0 D_MODEL="" D_TRAN="" D_MOUNTS="" D_WHY=""
    if ! be_is_whole_disk_name "$dev"; then
        D_WHY="$dev is not a whole-disk node like /dev/sdb or /dev/mmcblk0 (partitions are refused)"
        return 1
    fi
    line=$(lsblk -dpP -o NAME,TYPE,RM,TRAN,MODEL "$dev" 2>/dev/null | head -n1) || line=""
    if [ -z "$line" ]; then D_WHY="$dev is unknown to lsblk"; return 1; fi
    type=$(be_kv "$line" TYPE)
    rm=$(be_kv "$line" RM)
    tran=$(be_kv "$line" TRAN)
    D_MODEL=$(be_kv "$line" MODEL)
    D_MODEL=${D_MODEL//\\x20/ }
    D_MODEL=${D_MODEL//$'\t'/ }
    D_TRAN=$tran
    [ "$type" = disk ] || { D_WHY="$dev is not a disk (type ${type:-?})"; return 1; }
    if sysmp=$(be_carries_system "$dev"); then
        D_WHY="$dev carries the system path $sysmp"
        return 1
    fi
    name=$(basename "$dev")
    case $name in
        mmcblk*)
            kind=$(cat "$(be_sysfs)/class/block/$name/device/type" 2>/dev/null) || kind=""
            [ "$kind" = SD ] || { D_WHY="$dev is not an SD card (device type ${kind:-unknown})"; return 1; } ;;
        *)
            [ "$rm" = 1 ] || { D_WHY="$dev is not removable (RM=${rm:-?})"; return 1; }
            [ "$tran" = usb ] || { D_WHY="$dev is not USB-attached (TRAN=${tran:-none})"; return 1; } ;;
    esac
    D_SIZE=$(be_size_bytes "$dev")
    if [ "$D_SIZE" -le 0 ]; then D_WHY="$dev reports no medium (size 0)"; return 1; fi
    if [ "$D_SIZE" -lt "$BE_MIN_BYTES" ] || [ "$D_SIZE" -gt "$BE_MAX_BYTES" ]; then
        D_WHY="$dev size $D_SIZE bytes is outside 128 MiB .. 128 GiB"
        return 1
    fi
    while IFS=$'\t' read -r node mp; do
        if [ -n "$D_MOUNTS" ]; then D_MOUNTS+=","; fi
        D_MOUNTS+="$node:$mp"
    done < <(be_mounts "$dev")
    return 0
}

# be_list_candidates: TSV "device size_bytes model tran mounts" for every eligible whole disk.
be_list_candidates() {
    local line dev
    while IFS= read -r line; do
        dev=$(be_kv "$line" NAME)
        [ -n "$dev" ] || continue
        if be_check_disk "$dev" >/dev/null; then
            printf '%s\t%s\t%s\t%s\t%s\n' "$dev" "$D_SIZE" "$D_MODEL" "$D_TRAN" "$D_MOUNTS"
        fi
    done < <(lsblk -dpP -o NAME,TYPE,RM,TRAN,MODEL 2>/dev/null)
    return 0
}

# be_unmount_all DEV: unmount every mounted node of DEV (needs $SUDO set by the caller).
be_unmount_all() {
    local node mp
    while IFS=$'\t' read -r node mp; do
        $SUDO umount "$node" || return 1
    done < <(be_mounts "$1")
    return 0
}

# be_wait_medium DEV [SECONDS]: some readers answer "No medium found" for a while after wipefs.
be_wait_medium() {
    local i=0 max=${2:-60} s
    while [ "$i" -lt "$max" ]; do
        s=$($SUDO blockdev --getsize64 "$1" 2>/dev/null) || s=0
        if [ "${s:-0}" -gt 0 ]; then return 0; fi
        i=$((i + 1))
        sleep 1
    done
    return 1
}

# be_format DEV: wipe signatures, one MBR partition, FAT32 label AKAMSD. Prints the partition node.
be_format() {
    local dev=$1 part i=0
    $SUDO wipefs -a "$dev" >/dev/null || return 1
    be_wait_medium "$dev" 60 || return 2
    printf 'label: dos\n, , c, *\n' | $SUDO sfdisk -q "$dev" || return 1
    $SUDO partprobe "$dev" 2>/dev/null || true
    part=$(be_part_path "$dev")
    while [ ! -b "$part" ] && [ "$i" -lt 30 ]; do
        sleep 1
        i=$((i + 1))
    done
    [ -b "$part" ] || return 3
    be_wait_medium "$dev" 30 || return 2
    $SUDO umount "$part" 2>/dev/null || true # a desktop automounter may have grabbed it
    $SUDO mkfs.vfat -F 32 -n AKAMSD "$part" >/dev/null || return 1
    printf '%s\n' "$part"
}

# be_mount PART DIR [ro]: mount a FAT32 card owned by the calling user, umask 022.
be_mount() {
    local opt
    opt="uid=$(id -u),gid=$(id -g),umask=022"
    if [ "${3:-}" = ro ]; then opt+=",ro"; fi
    $SUDO mount -o "$opt" "$1" "$2"
}

# be_zero_fill DEV SIZE_BYTES: write exactly SIZE bytes of zeros, so dd ends cleanly.
be_zero_fill() {
    head -c "$2" /dev/zero | $SUDO dd of="$1" bs=4M iflag=fullblock conv=fsync status=progress
}
