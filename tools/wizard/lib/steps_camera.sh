# shellcheck shell=bash
# Camera-side wizard steps: find-camera, backup (from the camera), flash, verify, update, restore.
# Sourced by ./akam after steps_host.sh, camsh.sh and transfer.sh.
# Expected globals (set by ./akam): ROOT, STATE_DIR, OPT_* values, UI_DRY.
#
# Rules kept everywhere in this file
#   - the PC always connects OUT to the camera;
#   - an irreversible action (the flash write, untested hardware) is confirmed by the human in a red
#     box with ONE yes/no choice whose default is No (no terminal, Esc, q or an empty answer = No).
#     Only after that explicit yes does the wizard answer "YES" to the camera's own flash script
#     prompt (maintenance telnet) or pass --i-understand-no-uart to the SSH reflash;
#   - nothing here ever reboots or power-cycles the camera: the user does it BY HAND;
#   - a flash is gated on image size, md5 on the PC, md5 on the camera and, after the write, the
#     md5 of /dev/mtd7 read back.
# Test hooks: AKAM_PORT_TELNET/SSH/RTSP/CTL, AKAM_PORT_XFER_IN/OUT, AKAM_SSH, AKAM_REFLASH,
# AKAM_HWCHECK, AKAM_FLASH_TOOL, AKAM_FLASH_SCRIPTS_DIR, AKAM_PORT_FTP, AKAM_PORT_TELNET23, AKAM_BACKUP_ROOT, AKAM_WAIT_SECS, AKAM_CAMSH_TIMEOUT.

CAM_IP=""
CAM_KEY=""
CAM_SSH=()
CAM_SD=""
CAM_KIND=""
CAM_LAYOUT_FILE_LINES='mtd0: 00032000 00001000 "UBOOT"
mtd1: 00001000 00001000 "ENV"
mtd2: 00001000 00001000 "ENVBK"
mtd3: 00010000 00001000 "DTB"
mtd4: 00180000 00001000 "KERNEL"
mtd5: 000fc000 00001000 "ROOTFS"
mtd6: 00040000 00001000 "CONFIG"
mtd7: 00500000 00001000 "APP"'
CAM_MTD_SIZES="204800 4096 4096 65536 1572864 1032192 262144 5242880"

cam_port() {
    case $1 in
        telnet) printf '%s\n' "${AKAM_PORT_TELNET:-24}" ;;
        ssh) printf '%s\n' "${AKAM_PORT_SSH:-22}" ;;
        rtsp) printf '%s\n' "${AKAM_PORT_RTSP:-554}" ;;
        ctl) printf '%s\n' "${AKAM_PORT_CTL:-8091}" ;;
    esac
}

cam_scripts_dir() { printf '%s\n' "${AKAM_FLASH_SCRIPTS_DIR:-$ROOT/tools/flash_scripts}"; }

# cam_valid_host STR: an IPv4 address or a plain host name (never something that looks like an option).
cam_valid_host() {
    case $1 in
        '' | -* | *[!A-Za-z0-9.-]*) return 1 ;;
    esac
    return 0
}

# cam_need_ip: CAM_IP from --ip or from find-camera.
cam_need_ip() {
    CAM_IP=${OPT_IP:-$(state_get camera_ip)}
    if [ -z "$CAM_IP" ]; then
        if [ "$UI_DRY" = 1 ]; then CAM_IP="<camera-ip>"; return 0; fi
        ui_fail "the camera address is not known yet." "run: ./akam step find-camera (or pass --ip ADDRESS)"
        return 1
    fi
    if [ "$UI_DRY" = 1 ]; then return 0; fi
    cam_valid_host "$CAM_IP" || { ui_fail "'$CAM_IP' is not a valid camera address." "use the IP address from your router's list."; return 1; }
}

# cam_key: CAM_KEY = the private key to use with ssh (never read or copied by the wizard).
cam_key() {
    local pub
    if [ -n "$OPT_IDENTITY" ]; then CAM_KEY=$OPT_IDENTITY; return 0; fi
    pub=${OPT_PUBKEY:-$(state_get ssh_pubkey)}
    pub=${pub:-$HOME/.ssh/akam.pub}
    CAM_KEY=${pub%.pub}
}

# cam_ssh_setup: build CAM_SSH. The host key is remembered the first time (accept-new) under the
# alias <ip>, the same alias reflash_over_ssh.sh uses. LogLevel=ERROR hides OpenSSH's warnings
# (for example "not using a post-quantum key exchange"); real errors are still printed.
cam_ssh_setup() {
    cam_key
    CAM_SSH=("${AKAM_SSH:-ssh}" -i "$CAM_KEY" -p "$(cam_port ssh)" -o BatchMode=yes -o ConnectTimeout=10
        -o StrictHostKeyChecking=accept-new -o "HostKeyAlias=$CAM_IP" -o IdentitiesOnly=yes -o LogLevel=ERROR "root@$CAM_IP")
}

cam_ssh_ok() {
    cam_ssh_setup
    log_line "CMD(ro) ssh root@$CAM_IP true"
    "${CAM_SSH[@]}" true >/dev/null 2>&1
}

# cam_show_fingerprint: best effort, first connection only.
cam_show_fingerprint() {
    [ -z "${AKAM_SSH:-}" ] || return 0
    host_have ssh-keyscan || return 0
    host_have ssh-keygen || return 0
    if ssh-keygen -F "$CAM_IP" >/dev/null 2>&1; then return 0; fi
    local fp
    fp=$(ssh-keyscan -T 5 -p "$(cam_port ssh)" "$CAM_IP" 2>/dev/null | ssh-keygen -lf - 2>/dev/null | head -1 || true)
    if [ -n "$fp" ]; then
        ui_say "  First SSH connection to this camera. Its host key fingerprint: $fp"
        ui_say "  It will be remembered in ~/.ssh/known_hosts (as $CAM_IP)."
    fi
}

# cam_flash_tool: path of the ARM flash_tool binary (kit or source build).
cam_flash_tool() {
    if [ -n "${AKAM_FLASH_TOOL:-}" ]; then printf '%s\n' "$AKAM_FLASH_TOOL"; return 0; fi
    if [ "$(state_get mode)" = source ]; then printf '%s\n' "$ROOT/src/ak_rtsp/flash_tool"; else printf '%s\n' "$BASE_DIR/kit/bin/flash_tool"; fi
}

# cam_session_open: open the telnet session (or enter manual mode). Prints the hint on failure.
cam_session_open() {
    if [ "${OPT_MANUAL_TELNET:-0}" = 1 ]; then CAMSH_MANUAL=1; fi
    if camsh_open "$CAM_IP"; then return 0; fi
    ui_fail "cannot connect to $CAM_IP port $(cam_port telnet)." \
        "the maintenance telnet is only open while the toolkit card is in the camera. Insert the card, power the camera and wait about 2 minutes, or pass --manual-telnet."
    return 1
}

# cam_confirm_untested APPSHA WHAT: untested hardware needs an explicit yes (red box, default No).
cam_confirm_untested() {
    local verdict vstat vdev
    verdict=$(hw_verdict "$1")
    vstat=${verdict%%|*}
    vdev=${verdict#*|}
    ui_say "  Hardware verdict for this stock APP: $vstat - $vdev"
    ui_say "  Stock APP sha256: $1"
    [ "$vstat" = validated ] && return 0
    ui_danger "Untested firmware" "$2" \
        "This stock firmware is NOT on the validated list: Akam may not work and the camera may need a restore." \
        "Hardware verdict: $vstat - $vdev" "Stock APP sha256: $1" ||
        { ui_fail "untested firmware not confirmed." "nothing was changed."; return 1; }
}

# ------------------------------------------------------------------ find-camera
cam_ask_ip() {
    local def=${OPT_IP:-$(state_get camera_ip)}
    if [ -n "${OPT_IP:-}" ]; then
        CAM_IP=$OPT_IP
    else
        ui_say "  Find the address in your router's client list (the maker is shown as 'AltoBeam')."
        ui_prompt "  Camera IP address${def:+ [$def]}:" "$def" || { ui_fail "no input." "pass --ip ADDRESS"; return 1; }
        CAM_IP=$ANSWER
    fi
    cam_valid_host "$CAM_IP" || { ui_fail "'$CAM_IP' is not a valid address." "type the IPv4 address, for example 192.168.1.50"; return 1; }
}

cmd_find_camera() {
    local tp sp rp cp tel=0 ssh=0 rtsp=0 ctl=0 kind=""
    step_need find-camera || return 1
    step_banner find-camera
    tp=$(cam_port telnet) sp=$(cam_port ssh) rp=$(cam_port rtsp) cp=$(cam_port ctl)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask for the camera address, probe the ports $tp (telnet), $sp (ssh), $rp (rtsp), $cp (control),"
        ui_say "  and tell a stock camera with the toolkit card (telnet $tp, no ssh) from a running Akam (ssh + rtsp)."
        state_set camera_ip "<camera-ip>"
        state_set camera_kind stock
        state_set camera_telnet 1
        state_mark_done find-camera
        return 0
    fi
    cam_ask_ip || return 1
    ui_say "  Probing $CAM_IP on ports $tp, $sp, $rp and $cp ..."
    net_probe "$CAM_IP" "$tp" "$sp" "$rp" "$cp"
    case " $PROBE_OPEN " in *" $tp "*) tel=1 ;; esac
    case " $PROBE_OPEN " in *" $sp "*) ssh=1 ;; esac
    case " $PROBE_OPEN " in *" $rp "*) rtsp=1 ;; esac
    case " $PROBE_OPEN " in *" $cp "*) ctl=1 ;; esac
    ui_say "  Open ports: ${PROBE_OPEN:-none}"
    if [ -z "$PROBE_OPEN" ]; then
        ui_fail "$CAM_IP does not answer on any expected port." \
            "check the address in your router; the camera needs about 2 minutes after power-on; the backup card must be in the camera (telnet appears on port $tp); the PC and the camera must be on the same network."
        return 1
    fi
    if [ "$tel" = 1 ]; then
        cam_identify_telnet || return 1
        kind=$CAM_KIND
    elif [ "$ssh" = 1 ] && [ "$rtsp" = 1 ] && [ "$ctl" = 1 ]; then
        kind=akam
        ui_say "  Ports $sp, $rp and $cp are open and telnet is closed: this is a camera running Akam."
    elif [ "$ssh" = 1 ]; then
        ui_fail "only SSH answers. A camera running Akam also opens $rp and $cp once it has finished booting." \
            "wait a minute and run this step again."
        return 1
    else
        ui_fail "port $tp is closed and this is not Akam. Nothing usable answers on $CAM_IP." \
            "is this the camera's address? Is the toolkit card in?"
        return 1
    fi
    state_set camera_ip "$CAM_IP"
    state_set camera_kind "$kind"
    state_set camera_telnet "$tel"
    state_mark_done find-camera
    case $kind in
        stock) ui_good "  Camera found: stock firmware with the toolkit shell on port $tp ($CAM_IP). The backup can start." ;;
        akam) ui_good "  Camera found: it already runs Akam ($CAM_IP). Updates go over SSH; the stock backup must already exist." ;;
    esac
}

# cam_identify_telnet: opens the shell once (no fallback here); sets CAM_KIND to stock or akam.
cam_identify_telnet() {
    local ver marker
    CAM_KIND=stock
    if [ "${OPT_MANUAL_TELNET:-0}" = 1 ]; then
        ui_say "  Manual telnet mode: not opening the shell; assuming stock firmware with the toolkit."
        return 0
    fi
    if ! camsh_open "$CAM_IP"; then
        ui_fail "port $(cam_port telnet) is open but the connection failed." "try again in a moment."
        return 1
    fi
    camsh_start 'cat /proc/version; [ -d /usr/local/akam ] && echo AKAM_PRESENT || echo AKAM_ABSENT' || true
    if ! camsh_finish 15; then
        ui_fail "port $(cam_port telnet) is open but no shell answered (it may ask for a login: that is another service, not the toolkit shell)." \
            "stop here. The toolkit shell has no login. Check the card and the address."
        camsh_close
        return 1
    fi
    camsh_close
    ver=$(printf '%s\n' "$CAMSH_OUT" | grep -m1 '^Linux version' || true)
    if [ -z "$ver" ]; then
        ui_fail "the shell on port $(cam_port telnet) did not look like the camera (no Linux version line)." "stop here and check the address."
        return 1
    fi
    ui_say "  Shell answered: $ver"
    marker=$(printf '%s\n' "$CAMSH_OUT" | grep -m1 '^AKAM_' || true)
    if [ "$marker" = AKAM_PRESENT ]; then CAM_KIND=akam; fi
}

# ------------------------------------------------------------------ backup from the camera
# bk_layout_check: /proc/mtd must be the 8-partition layout. A different table is refused.
bk_layout_check() {
    local now
    camsh_ok 'cat /proc/mtd' || { ui_fail "cannot read /proc/mtd on the camera." "check the telnet session."; return 1; }
    now=$(printf '%s\n' "$CAMSH_OUT" | grep -v '^dev:' | sed '/^[[:space:]]*$/d')
    if [ "$now" != "$CAM_LAYOUT_FILE_LINES" ]; then
        ui_err "the partition table of this camera is not the supported 8-partition layout:"
        printf '%s\n' "$now" | sed 's/^/      /'
        ui_hint "do NOT continue: this camera model or firmware layout is not supported. Nothing was changed."
        return 1
    fi
    ui_say "  Partition table: LAYOUT_OK (8 partitions, APP 00500000)."
}

# bk_find_sd: CAM_SD = the mount point of the card on the camera (the toolkit uses /tmp/sd).
bk_find_sd() {
    camsh_ok 'mount | grep mmcblk | sed -n "s/^[^ ]* on \([^ ]*\) .*/\1/p" | head -1' ||
        { ui_fail "cannot list the mounts on the camera." ""; return 1; }
    CAM_SD=$(camsh_line 1 | tr -d '[:space:]')
    case $CAM_SD in
        /*[!A-Za-z0-9_./-]* | '') ui_fail "the microSD card does not seem to be mounted on the camera." "power-cycle the camera with the backup card inserted, then run this step again."; return 1 ;;
        /*) ;;
        *) ui_fail "unexpected card mount point '$CAM_SD'." ""; return 1 ;;
    esac
    ui_say "  The card is mounted at $CAM_SD on the camera."
}

# bk_free_space: at least 17000 KiB free for the two dumps.
bk_free_space() {
    local avail
    camsh_ok "df -k $CAM_SD" || { ui_fail "cannot read the free space of the card." ""; return 1; }
    # shellcheck disable=SC2046
    set -- $(printf '%s\n' "$CAMSH_OUT" | sed 1d | tr '\n' ' ')
    avail=${4:-}
    case $avail in '' | *[!0-9]*) ui_fail "cannot parse the free space of the card." ""; return 1 ;; esac
    if [ "$avail" -lt 17000 ]; then
        ui_fail "the card has only $avail KiB free, 17000 KiB are needed for the two dumps." "use a card with free space."
        return 1
    fi
    ui_say "  Free space on the card: $avail KiB."
}

# bk_dump_one RUN N
bk_dump_one() {
    local r=$1 n=$2
    camsh_ok "dd if=/dev/mtdblock$n of=$CAM_SD/dump$r/mtdblock$n.bin bs=64k 2>/dev/null" 300 ||
        { ui_fail "reading mtdblock$n failed on the camera (run $r)." "nothing was changed on the camera; try again."; return 1; }
}

bk_dump_run() {
    local r=$1 n
    camsh_ok "mkdir -p $CAM_SD/dump$r" || return 1
    for n in 0 1 2 3 4 5 6 7; do
        ui_say "  Reading mtdblock$n (run $r of 2) ..."
        bk_dump_one "$r" "$n" || return 1
    done
    camsh_ok sync 120 || return 1
}

# bk_sizes_check: exact byte sizes of both dumps.
bk_sizes_check() {
    local r n want got
    camsh_ok "for f in $CAM_SD/dump1/mtdblock*.bin $CAM_SD/dump2/mtdblock*.bin; do echo \"\$(stat -c%s \$f) \$f\"; done" ||
        { ui_fail "cannot read the dump sizes on the camera." ""; return 1; }
    for r in 1 2; do
        n=0
        for want in $CAM_MTD_SIZES; do
            got=$(printf '%s\n' "$CAMSH_OUT" | sed -n "s#^\([0-9]*\) .*/dump$r/mtdblock$n\.bin\$#\1#p" | tr -d '[:space:]')
            if [ "$got" != "$want" ]; then
                ui_fail "dump$r mtdblock$n is $got bytes, expected $want." "the read is not usable; stop and try again."
                return 1
            fi
            n=$((n + 1))
        done
    done
    ui_say "  Sizes of the 16 dump files are exact."
}

# bk_hash_run R: fills the array H<R> (index = partition number) from the camera's own hashes.
bk_hash_run() {
    local r=$1 h f n
    ui_say "  Hashing dump$r on the camera ($XFER_HT, this takes a moment) ..."
    camsh_ok "cd $CAM_SD/dump$r && $XFER_HT mtdblock*.bin | tee $CAM_SD/sums_run$r.$XFER_HT" 600 ||
        { ui_fail "hashing dump$r failed on the camera." ""; return 1; }
    while read -r h f; do
        f=${f#\*}
        case $f in mtdblock[0-7].bin) n=${f#mtdblock}; n=${n%.bin} ;; *) continue ;; esac
        case $h in *[!0-9a-f]* | '') continue ;; esac
        if [ "$r" = 1 ]; then eval "BK_H1_$n=$h"; else eval "BK_H2_$n=$h"; fi
    done <<<"$CAMSH_OUT"
}

# bk_hash_get RUN N: print the recorded hash.
bk_hash_get() {
    local v="BK_H$1_$2"
    printf '%s\n' "${!v:-}"
}

# bk_compare: the two runs must be identical. mtdblock6 (CONFIG, mounted read/write on the camera)
# may differ: it is dumped again, twice at most. Any other difference is fatal.
bk_compare() {
    local n bad redo=0
    while :; do
        bad=""
        for n in 0 1 2 3 4 5 6 7; do
            if [ -z "$(bk_hash_get 1 "$n")" ] || [ "$(bk_hash_get 1 "$n")" != "$(bk_hash_get 2 "$n")" ]; then bad="$bad $n"; fi
        done
        if [ -z "$bad" ]; then
            ui_good "  READS_IDENTICAL: both dumps match on all 8 partitions."
            return 0
        fi
        if [ "$bad" != " 6" ]; then
            ui_fail "the two reads differ on mtdblock${bad# } (not CONFIG only): the read is not reliable." "do not use these dumps; try again."
            return 1
        fi
        if [ "$redo" -ge 2 ]; then
            ui_warn "mtdblock6 (CONFIG) still differs between the two reads. It is a live read/write filesystem and Akam never uses it: continuing with run 1."
            BK_CONFIG_DIFFERS=1
            return 0
        fi
        redo=$((redo + 1))
        ui_warn "only mtdblock6 (CONFIG, read/write while the camera runs) differs: reading it again ($redo of 2)."
        if ! { bk_dump_one 1 6 && bk_dump_one 2 6 && bk_hash_run 1 && bk_hash_run 2; }; then return 1; fi
    done
}

# bk_fingerprint: kernel version vs the supported list (information; the APP hash decides later).
bk_fingerprint() {
    local kfp
    camsh_ok 'uname -r; uname -v' || { ui_warn "could not read the kernel version."; return 0; }
    kfp="$(camsh_line 1 | tr -d '\r') $(camsh_line 2 | tr -d '\r')"
    BK_KERNEL=$kfp
    ui_say "  Kernel: $kfp"
    if grep -v -e '^[[:space:]]*#' "$(wiz_tool hw_list)" | awk -F'|' -v k="$kfp" '$1 == "validated" && $4 == k { f = 1 } END { exit !f }'; then
        ui_say "  The kernel build matches a validated entry of the supported hardware list."
    else
        ui_warn "this kernel build is not on the validated list ($(wiz_tool hw_list))."
    fi
}

# bk_pc_dir: a new folder ~/akam-backup-<name>-<date>[-N] (no name part for the camera "default").
bk_pc_dir() {
    local base d n=1
    base=${AKAM_BACKUP_ROOT:-$HOME}/akam-backup-$(cams_slug)$(date +%Y%m%d)
    d=$base
    while [ -e "$d" ]; do n=$((n + 1)); d="$base-$n"; done
    printf '%s\n' "$d"
}

# bk_write_pc_files DIR: SHA256SUMS, README.txt, camera_info.txt.
bk_write_pc_files() {
    local d=$1 n f
    : >"$d/SHA256SUMS"
    for n in 0 1 2 3 4 5 6 7; do
        f=mtdblock$n.bin
        printf '%s  %s\n' "$(host_sha256 "$d/$f")" "$f" >>"$d/SHA256SUMS"
    done
    {
        echo "Akam backup of the camera at $CAM_IP, made $(date -u +%Y-%m-%dT%H:%M:%SZ) by ./akam backup."
        echo "mtdblock0..7.bin are the 8 flash partitions of the ORIGINAL firmware. This is the only way back."
        echo "Keep a second copy on a different medium. NEVER publish these files: they hold proprietary firmware."
        echo "Check them with: sha256sum -c SHA256SUMS"
    } >"$d/README.txt"
    {
        echo "camera: $CAM_IP"
        echo "kernel: ${BK_KERNEL:-unknown}"
        echo "camera_hash_tool: $XFER_HT"
        echo "config_partition_differs_between_reads: ${BK_CONFIG_DIFFERS:-0}"
        echo "--- /proc/mtd"
        printf '%s\n' "$CAM_LAYOUT_FILE_LINES"
    } >"$d/camera_info.txt"
}

# bk_copy_to_pc DIR: fetch dump1 (verified against the camera's hashes, which are identical to dump2).
bk_copy_to_pc() {
    local d=$1 n
    mkdir -p "$d"
    for n in 0 1 2 3 4 5 6 7; do
        xfer_get "$CAM_SD/dump1/mtdblock$n.bin" "$d/mtdblock$n.bin" "$(printf '%s\n' "$CAM_MTD_SIZES" | cut -d' ' -f$((n + 1)))" "$(bk_hash_get 1 "$n")" ||
            return 1
    done
}

bk_manual_fallback_hint() {
    ui_say "  The automatic transfer did not work. You still have the dumps on the card:"
    ui_say "  power the camera off, put the card in the PC, copy dump1/ to a folder, then run:"
    ui_say "      ./akam backup --backup-dir THAT_FOLDER"
    ui_say "  (docs/akam_v1_backup_sd.md section 9 shows how to check the copy)."
}

backup_camera() {
    local pcdir app verdict kind
    kind=$(state_get camera_kind)
    if [ "$kind" = akam ]; then
        ui_fail "this camera already runs Akam: its flash no longer holds the original firmware." \
            "the stock backup must have been made before the first flash. Give it with: ./akam backup --backup-dir DIR"
        return 1
    fi
    cam_need_ip || return 1
    cam_session_open || return 1
    bk_layout_check || { camsh_close; return 1; }
    bk_fingerprint
    bk_find_sd || { camsh_close; return 1; }
    bk_free_space || { camsh_close; return 1; }
    xfer_hash_tool || { ui_fail "cannot tell which hash tool the camera has." ""; camsh_close; return 1; }
    camsh_exec "mount > $CAM_SD/state_mounts.txt; ls -la /etc/config > $CAM_SD/state_etc_config.txt; ps > $CAM_SD/state_ps.txt" || true
    bk_dump_run 1 || { camsh_close; return 1; }
    bk_dump_run 2 || { camsh_close; return 1; }
    bk_sizes_check || { camsh_close; return 1; }
    if ! { bk_hash_run 1 && bk_hash_run 2 && bk_compare; }; then camsh_close; return 1; fi
    pcdir=$(bk_pc_dir)
    ui_say "  Copying the verified dump to this PC: $pcdir"
    if ! bk_copy_to_pc "$pcdir"; then
        bk_manual_fallback_hint
        camsh_close
        return 1
    fi
    camsh_close
    bk_write_pc_files "$pcdir"
    ui_good "  Backup copied and verified (every file matches the camera's two reads). SHA256SUMS written."
    app=$(host_sha256 "$pcdir/mtdblock7.bin")
    if ! cam_confirm_untested "$app" "Continue with untested firmware?"; then
        ui_say "  The verified copy stays in $pcdir (accept it later with: ./akam backup --backup-dir $pcdir)."
        return 1
    fi
    backup_accept "$pcdir"
}

cmd_backup() {
    if [ -n "$OPT_BACKUP_DIR" ]; then
        backup_import
        return $?
    fi
    step_need backup || return 1
    step_banner backup
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD open the telnet shell on the camera and: check /proc/mtd is the 8-partition layout (refuse otherwise),"
        ui_say "  read the kernel version, find the card and its free space, dd the 8 partitions twice,"
        ui_say "  check the 16 sizes, hash both runs on the camera and compare them (READS_IDENTICAL),"
        ui_say "  copy dump1 to ~/akam-backup-<name>-<date>/ over nc with size+hash checks (3 tries each),"
        ui_say "  write SHA256SUMS, compare the stock APP hash with tools/release/supported_hardware.txt"
        ui_say "  (a yes/no in a red box, default No, unless validated) and ask for a second copy on another medium."
        state_set backup_dir "<backup-dir>"
        state_set backup_sha dry-run
        state_mark_done backup
        return 0
    fi
    backup_camera
}

# cam_flash_confirm LABEL MD5 ROUTE: the one confirmation before the camera's flash is written: a red box that
# says exactly what will happen, then a yes/no with the default No. Needs a real terminal.
cam_flash_confirm() {
    local label=$1 md5=$2 route=$3 what
    case $label in
        Restoring) what="put the ORIGINAL (stock) APP back on the camera" ;;
        Updating) what="write the new Akam image to the camera" ;;
        *) what="write the Akam image to the camera" ;;
    esac
    ui_danger "$label: WRITES THE CAMERA FLASH" "Write the camera flash now?" \
        "Camera: $CAM_IP (route: $route)" \
        "This will $what: it erases and rewrites the APP partition (/dev/mtd7)." \
        "Image md5 (checked on this PC and on the camera): $md5" \
        "Do NOT unplug the camera or the network until the wizard says it is finished: a power cut during the write can leave the camera dead (only a UART cable recovers it)."
}

# ------------------------------------------------------------------ SSH reflash (flash, update, restore)
# cam_filter_flash_output: drop the progress bars (about 80 KB) and keep what matters.
CAM_FLASH_KEEP_RE='^reflash_over_ssh:|error|fail|mismatch|md5|verif|retry|retries|fault|success|complete|refus'
cam_filter_flash_output() {
    local l
    shopt -s nocasematch
    tr '\r' '\n' | while IFS= read -r l; do
        l=${l#camera: }
        if [[ $l =~ $CAMSH_PROG_RE ]]; then
            ui_progress "${BASH_REMATCH[1]}" "$((10#${BASH_REMATCH[2]}))"
        elif [[ $l =~ $CAM_FLASH_KEEP_RE ]]; then
            ui_progress_close
            printf '    | %s\n' "$l"
        fi
    done
    ui_progress_close
}

# cam_run_flash_script LOGFILE CMD...: run it, show a filtered output, keep the full output in LOGFILE.
cam_run_flash_script() {
    local logf=$1 rcf rc
    shift
    _show_cmd "$@"
    log_line "CMD $SHOWN"
    ui_say "  Running: $SHOWN"
    ui_say "  (full output: $logf)"
    rcf=$(mktemp)
    (umask 077 && : >"$logf")
    { "$@" 2>&1; echo "$?" >"$rcf"; } | tee -a "$logf" | cam_filter_flash_output
    rc=$(cat "$rcf")
    rm -f "$rcf"
    log_line "RC $rc"
    return "${rc:-1}"
}

# flash_ssh IMG MD5 LABEL: the validated no-card path. Precheck (writes nothing), yes/no (default No), flash.
flash_ssh() {
    local img=$1 md5=$2 label=$3 ft args
    ft=$(cam_flash_tool)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run the precheck of the SSH reflash (writes nothing), ask a yes/no (default No), then:"
        ui_say "    $(wiz_tool reflash) --host $CAM_IP --identity ${CAM_KEY:-<key>} --image <image> --md5 <md5> --flash-tool <flash_tool> --i-understand-no-uart"
        ui_say "  and relay its progress (the progress bars are filtered out). It never reboots the camera."
        ui_danger "Write the camera flash" "Write the camera flash now?" "<camera ip>" "<image md5>"
        return 0
    fi
    [ -f "$(wiz_tool reflash)" ] || { ui_fail "the SSH reflash tool is missing: $(wiz_tool reflash)" ""; return 1; }
    [ -s "$ft" ] || { ui_fail "flash_tool not found: $ft" "run ./akam get-kit again (or ./akam build)."; return 1; }
    cam_ssh_setup
    cam_show_fingerprint
    if ! cam_ssh_ok; then
        ui_fail "SSH to root@$CAM_IP with the key $CAM_KEY does not work." \
            "check the key (--identity FILE), the address, and that the camera has finished booting."
        return 1
    fi
    args=(--host "$CAM_IP" --identity "$CAM_KEY" --image "$img" --md5 "$md5" --flash-tool "$ft")
    if [ -n "${AKAM_PORT_SSH:-}" ]; then args+=(--port "$AKAM_PORT_SSH"); fi
    ui_say "  Precheck: uploads two small scripts to the camera's RAM and checks it; nothing is written to flash."
    cam_run_flash_script "$STATE_DIR/reflash-precheck.log" bash "$(wiz_tool reflash)" "${args[@]}" --precheck-only ||
        { ui_fail "the precheck failed (see above)." "nothing was written to the camera."; return 1; }
    ui_blank
    ui_warn "$label over SSH has NO UART safety net. If the power is cut or the write is interrupted, the APP partition"
    ui_warn "is half written and only a UART cable can recover it. Keep the camera powered and do not touch it."
    ui_say "  Image md5 (checked on this PC): $md5"
    cam_flash_confirm "$label" "$md5" "SSH" ||
        { ui_fail "not confirmed." "nothing was written."; return 1; }
    cam_run_flash_script "$STATE_DIR/reflash.log" bash "$(wiz_tool reflash)" "${args[@]}" --i-understand-no-uart ||
        { ui_fail "the SSH reflash failed or could not be verified." \
            "DO NOT power-cycle yet. Read $STATE_DIR/reflash.log and docs/akam_v1_flash_and_restore.md (Reflashing over SSH)."; return 1; }
    ui_good "  The camera confirmed the written image (read-back md5 = $md5)."
}

# ------------------------------------------------------------------ maintenance-telnet flash
# ft_find_card: make the card visible as /mnt on the camera (the toolkit mounts it on /tmp/sd).
ft_find_card() {
    camsh_ok 'if [ -d /mnt/custom_firmware ]; then echo CARD_ON_MNT; elif [ -d /tmp/sd/custom_firmware ]; then echo CARD_ON_TMPSD; else echo CARD_NONE; fi' ||
        { ui_fail "cannot look for the flash card on the camera." ""; return 1; }
    case $(camsh_line 1 | tr -d '[:space:]') in
        CARD_ON_MNT) ui_say "  The flash card files are already on /mnt." ;;
        CARD_ON_TMPSD)
            ui_say "  The card is on /tmp/sd; binding it to /mnt (every script reads /mnt)."
            camsh_ok 'mount --bind /tmp/sd /mnt' || { ui_fail "mount --bind /tmp/sd /mnt failed." ""; return 1; }
            ;;
        *)
            ui_fail "no custom_firmware folder on the card in the camera." \
                "put the flash card (./akam step prepare-flash-sd) in the camera, power-cycle it and wait 2 minutes."
            return 1
            ;;
    esac
}

# ft_remote_md5 PATH: sets FT_RMD5 (empty when the file is missing).
ft_remote_md5() {
    FT_RMD5=""
    camsh_exec "md5sum '$1' 2>/dev/null" 120 || return 1
    [ "$CAMSH_RC" = 0 ] && FT_RMD5=$(camsh_line 1 | awk '{print $1}')
    return 0
}

# ft_ensure_file LOCAL REMOTE EXEC LABEL: the file on the card must equal LOCAL; push it if not.
ft_ensure_file() {
    local lmd5
    lmd5=$(host_md5 "$1")
    ft_remote_md5 "$2" || return 1
    if [ "$FT_RMD5" = "$lmd5" ]; then
        ui_say "  $4 on the card: md5 matches this PC."
        return 0
    fi
    if [ -z "$FT_RMD5" ]; then ui_warn "$4 is missing on the card."; else ui_warn "$4 on the card is not the one from this PC."; fi
    ui_confirm "  Push $4 from this PC to $2 over the network?" ||
        { ui_fail "$4 on the card does not match." "prepare the flash card again (./akam step prepare-flash-sd)."; return 1; }
    xfer_put "$1" "$2" || return 1
    if [ "$3" = exec ]; then camsh_ok "chmod +x '$2'" || return 1; fi
}

# ft_ensure_image IMG MD5: exactly one image on the card and it is IMG.
ft_ensure_image() {
    local img=$1 md5=$2 name list n why
    # A shell loop, not `ls -1`: the camera's ls colours its names (the escape codes are also
    # stripped by camsh, but a plain list of names needs no ls at all).
    # shellcheck disable=SC2016  # expanded by the camera's shell
    camsh_ok 'for f in /mnt/custom_firmware/*; do if [ -e "$f" ]; then echo "${f##*/}"; fi; done' ||
        { ui_fail "cannot list /mnt/custom_firmware." ""; return 1; }
    list=$CAMSH_OUT
    n=$(printf '%s\n' "$list" | grep -c . || true)
    name=$(printf '%s\n' "$list" | grep -E '^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs$' | tail -1 || true)
    why="found: $(printf '%s' "$list" | tr '\n' ' ')"
    if [ "$n" = 1 ] && [ -n "$name" ]; then
        ft_remote_md5 "/mnt/custom_firmware/$name" || return 1
        if [ "$FT_RMD5" = "$md5" ]; then
            ui_say "  Image on the card: $name, md5 $md5 (same as this PC)."
            return 0
        fi
        why="$name has md5 ${FT_RMD5:-unreadable} on the card, this PC's image has $md5"
    fi
    ui_warn "the card does not carry exactly the image you want to flash ($why)."
    ui_confirm "  Replace the image on the card with this PC's image?" ||
        { ui_fail "the image on the card does not match." "prepare the flash card again (./akam step prepare-flash-sd)."; return 1; }
    name=$(basename "$img")
    case $name in
        ak_rtsp_firmware_[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]_[0-9][0-9][0-9][0-9][0-9][0-9].squashfs) ;;
        *) name=ak_rtsp_firmware_$(date -u +%Y%m%d_%H%M%S).squashfs ;;
    esac
    camsh_ok 'rm -f /mnt/custom_firmware/ak_rtsp_firmware_*.squashfs' || { ui_fail "cannot clear the old image on the card." ""; return 1; }
    xfer_put "$img" "/mnt/custom_firmware/$name" || return 1
}

# ------------------------------------------------------------------ Wi-Fi must survive the card
# A camera that was never paired with the vendor app has no /etc/config/wpa_supplicant.conf: with the
# toolkit card the toolkit brings Wi-Fi up from the card, but the camera has NO network once the card
# is removed. tools/flash_scripts/preflight_check.sh --ensure-wifi (run ON the camera) writes the
# file from the card's custom/configs/wifi.conf, so the password never passes through this PC (not
# in a command line, not in the log). Hard gate: nothing tells the user to remove the card while the
# file is missing.

# cam_wifi_verdict OUTPUT: read the WIFI= line of preflight_check.sh --ensure-wifi; 0 = the file holds a network.
cam_wifi_verdict() {
    local code
    code=$(printf '%s\n' "$1" | sed -n 's/^WIFI=\([A-Z_]*\).*/\1/p' | tail -1)
    case $code in
        PRESENT)
            ui_say "  /etc/config/wpa_supplicant.conf holds a Wi-Fi network: the camera keeps its network without the card."
            ;;
        CREATED)
            ui_good "  /etc/config/wpa_supplicant.conf was missing (the camera was never paired with the app): it was created ON the camera from the card's Wi-Fi settings."
            ui_say "  (the password never passed through this PC's command lines or logs.)"
            ;;
        NO_CARD_CONFIG)
            ui_fail "the camera has no saved Wi-Fi (/etc/config/wpa_supplicant.conf) and the card holds no usable custom/configs/wifi.conf to create it from." \
                "without it the camera has NO network once the card is removed. Prepare the flash card again (./akam step prepare-flash-sd), or create the file by hand: docs/akam_v1_install.md (Troubleshooting: no network after removing the card)."
            ;;
        BAD_CHARS)
            ui_fail "the Wi-Fi name or password on the card holds a double quote, a backslash or a line break: the camera's wpa_supplicant.conf cannot be written safely from it." \
                "create /etc/config/wpa_supplicant.conf by hand (docs/akam_v1_install.md, Troubleshooting), or use a Wi-Fi password without those characters."
            ;;
        BAD_PSK_LENGTH)
            ui_fail "the Wi-Fi password on the card is not 8 to 63 characters long, so it is not a WPA password." \
                "check the password, then prepare the card again (./akam step prepare-flash-sd)."
            ;;
        UNSUPPORTED_SECURITY)
            ui_fail "the card's Wi-Fi settings use a security type other than WPA/WPA2-PSK (SECURITY=3) or open (SECURITY=1)." \
                "create /etc/config/wpa_supplicant.conf by hand for your network (docs/akam_v1_install.md, Troubleshooting)."
            ;;
        WRITE_FAILED)
            ui_fail "the camera could not write /etc/config/wpa_supplicant.conf." \
                "do not remove the card. Create the file by hand (docs/akam_v1_install.md, Troubleshooting)."
            ;;
        *)
            ui_fail "could not check the camera's saved Wi-Fi (no answer from preflight_check.sh --ensure-wifi)." \
                "do not remove the card; run this step again."
            ;;
    esac
}

# cam_wifi_gate_telnet: through the maintenance shell (the card is on /mnt, preflight_check.sh was just verified there).
cam_wifi_gate_telnet() {
    ui_say "  Checking that the camera will keep its Wi-Fi after the card is removed ..."
    camsh_exec 'sh /mnt/preflight_check.sh --ensure-wifi' 60 ||
        { ui_fail "cannot run preflight_check.sh on the camera." "check the telnet session."; return 1; }
    cam_wifi_verdict "$CAMSH_OUT"
}

# cam_wifi_gate_ssh: over SSH (verify, card still in). The script is sent through stdin into /tmp (RAM).
cam_wifi_gate_ssh() {
    local pf out rc=0
    pf="$(cam_scripts_dir)/preflight_check.sh"
    [ -f "$pf" ] || { ui_fail "preflight_check.sh not found: $pf" ""; return 1; }
    cam_ssh_setup
    ui_say "  Checking that the camera will keep its Wi-Fi after the card is removed ..."
    log_line "CMD ssh root@$CAM_IP 'cat > /tmp/akam_preflight.sh' < preflight_check.sh"
    "${CAM_SSH[@]}" 'cat > /tmp/akam_preflight.sh' <"$pf" ||
        { ui_fail "cannot copy preflight_check.sh over SSH." "do not remove the card yet."; return 1; }
    log_line "CMD ssh root@$CAM_IP 'sh /tmp/akam_preflight.sh --ensure-wifi'"
    out=$("${CAM_SSH[@]}" 'sh /tmp/akam_preflight.sh --ensure-wifi' 2>&1) || rc=$?
    cam_wifi_verdict "$out"
}

# Lines of the flash output that are not worth showing.
FT_SKIP_RE='^[[:space:]]*$|^[[:space:]]*[0-9]+%'

# flash_telnet IMG MD5 LABEL: from the stock or maintenance telnet, with the card on /mnt.
flash_telnet() {
    local img=$1 md5=$2 label=$3 ft rc line sums
    ft=$(cam_flash_tool)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD open the telnet shell, bind the card on /mnt if it is on /tmp/sd, check that exactly one image is in"
        ui_say "  /mnt/custom_firmware with the same md5 as this PC (pushing files over nc only after your yes), then run"
        ui_say "  'sh /mnt/install_with_flash_tool.sh', show its md5 line and its prompt, and ask YOU a yes/no (default No); only after your yes does the wizard answer the script."
        ui_say "  When flashing it also runs 'sh /mnt/preflight_check.sh --ensure-wifi' on the camera: if /etc/config/wpa_supplicant.conf has no"
        ui_say "  network it is created from the card's wifi.conf (on the camera; the password never passes through this PC)."
        ui_say "  After the write: dd if=/dev/mtd7 bs=64k | md5sum on the camera must equal the image md5. No reboot is ever sent."
        ui_danger "Write the camera flash" "Write the camera flash now?" "<camera ip>" "<image md5>"
        return 0
    fi
    [ -s "$ft" ] || { ui_fail "flash_tool not found: $ft" "run ./akam get-kit again (or ./akam build)."; return 1; }
    cam_session_open || return 1
    # Same layout gate as the backup: never aim /dev/mtd7 at a camera whose table is not the supported one.
    bk_layout_check || { camsh_close; return 1; }
    ft_find_card || { camsh_close; return 1; }
    ft_ensure_image "$img" "$md5" || { camsh_close; return 1; }
    ft_ensure_file "$ft" /mnt/flash_tool exec "flash_tool" || { camsh_close; return 1; }
    ft_ensure_file "$(cam_scripts_dir)/install_with_flash_tool.sh" /mnt/install_with_flash_tool.sh plain "install_with_flash_tool.sh" || { camsh_close; return 1; }
    ft_ensure_file "$(cam_scripts_dir)/flash_guard.sh" /mnt/flash_guard.sh plain "flash_guard.sh" || { camsh_close; return 1; }
    if [ "$label" = Flashing ]; then
        # Akam does not bring Wi-Fi up by itself: the saved network must exist before the write (hard gate).
        ft_ensure_file "$(cam_scripts_dir)/preflight_check.sh" /mnt/preflight_check.sh plain "preflight_check.sh" || { camsh_close; return 1; }
        cam_wifi_gate_telnet || { camsh_close; ui_say "  Nothing was written to the camera."; return 1; }
    fi
    ui_blank
    ui_warn "$label writes the camera's flash. If the power is cut while it writes, only a UART cable can recover the camera."
    ui_warn "Keep the camera powered and do not unplug anything until this step says it is finished."
    ui_say "  Image md5 (this PC and the card): $md5"
    camsh_stream_set 1 "$FT_SKIP_RE"
    # HUP is ignored: if the telnet link drops while it writes, the kernel's hangup must not kill flash_tool.
    camsh_start "trap '' HUP; sh /mnt/install_with_flash_tool.sh" || { camsh_stream_set 0; camsh_close; return 1; }
    if [ "$CAMSH_MANUAL" = 1 ]; then
        camsh_stream_set 0
        flash_telnet_manual "$md5" "$label" || return 1
    else
        rc=0
        camsh_wait_prompt 'Type YES to continue: ' 60 || rc=$?
        camsh_stream_set 0
        if [ "$rc" -ne 0 ]; then
            ui_fail "the flash script did not reach its confirmation prompt (code $rc)." "nothing was written. Read the lines above."
            camsh_close
            return 1
        fi
        line=$(printf '%s\n' "$CAMSH_OUT" | grep -E '^[0-9a-f]{32} ' | head -1 | awk '{print $1}' || true)
        if [ "$line" != "$md5" ]; then
            ui_err "the md5 printed by the camera ('$line') is not the image md5 ($md5)."
            camsh_send_line "NO"
            camsh_finish 15 || true
            camsh_close
            return 1
        fi
        ui_say "  The camera shows the same md5 as this PC. Its script now waits for a confirmation."
        if ! cam_flash_confirm "$label" "$md5" "the maintenance telnet"; then
            ui_err "not confirmed. Aborting; nothing was written."
            camsh_send_line "NO"
            camsh_finish 15 || true
            camsh_close
            return 1
        fi
        # The only place where text goes to the flash script's prompt: "YES", sent only after the explicit yes above.
        camsh_send_line "YES" || { ui_fail "lost the camera session." "DO NOT power-cycle; check the camera."; return 1; }
        camsh_stream_set 1 "$FT_SKIP_RE"
        rc=0
        # The write is under way: Ctrl-C, hangup or kill of this wizard must not end it half way.
        ui_warn "Writing. Do NOT press Ctrl-C and do NOT unplug anything until this says it is finished."
        trap '' INT HUP TERM
        camsh_finish 900 || rc=$?
        trap - INT HUP TERM
        tui_rearm_traps
        camsh_stream_set 0
        ui_progress_close
        if [ "$rc" -ne 0 ]; then
            ui_fail "the flash script did not finish (no end of output, code $rc)." "DO NOT power-cycle yet. Read the lines above; docs/akam_v1_flash_and_restore.md (Recovery)."
            if [ "$rc" -eq 1 ]; then
                ui_warn "no output for a long time: the write may STILL be running. The session is left open and nothing is sent again;"
                ui_warn "wait several minutes, then read the partition back by hand: dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum (must be $md5)."
            else
                camsh_close
            fi
            return 1
        fi
        ft_note_script_result
    fi
    # The authoritative check is the partition itself, never the wording of the script's last line.
    ui_say "  Reading the partition back to check it ..."
    camsh_exec 'dd if=/dev/mtd7 bs=64k 2>/dev/null | md5sum' 600 || { ui_fail "cannot read /dev/mtd7 back." "DO NOT power-cycle."; camsh_close; return 1; }
    sums=$(camsh_line 1 | awk '{print $1}')
    camsh_close
    if [ "$sums" != "$md5" ]; then
        ui_fail "the partition read back as md5 '$sums', the image is $md5." \
            "DO NOT power-cycle. Run the flash again from this step or restore (docs/akam_v1_flash_and_restore.md)."
        return 1
    fi
    ui_good "  Read-back md5 of /dev/mtd7 = image md5 ($md5)."
}

# ft_note_script_result: tell what the flash script itself said (secondary information, the
# read-back md5 decides). CAMSH_OUT and CAMSH_RC are those of the script.
ft_note_script_result() {
    if camsh_out_has 'completed successfully' && [ "$CAMSH_RC" = 0 ]; then
        ui_say "  The flash script reported: completed successfully."
    elif [ "$CAMSH_RC" = 0 ]; then
        ui_warn "the flash script ended without its success line; the read-back below decides."
    else
        ui_warn "the flash script exited with code $CAMSH_RC; the read-back below decides."
    fi
}

# flash_telnet_manual MD5 LABEL: manual mode: the user runs the script and types YES in their own window.
flash_telnet_manual() {
    ui_say "  MANUAL: in your telnet window run the command above, compare the md5 it prints with this one:"
    ui_say "      $1"
    ui_say "  If it matches, type YES yourself at its prompt (in your own window). Wait until it says 'completed successfully'."
    ui_say "  Do NOT unplug anything. Then come back and paste its last lines."
    camsh_finish 900 || return 1
    ft_note_script_result
}

# ------------------------------------------------------------------ flash
# cam_image_gate IMG: size and md5. Sets IMG_MD5.
cam_image_gate() {
    local sz
    [ -f "$1" ] || { ui_fail "image not found: $1" "run ./akam step image."; return 1; }
    sz=$(wc -c <"$1" | tr -d ' ')
    if [ "$sz" != "$APP_PARTITION_BYTES" ]; then
        ui_fail "the image is $sz bytes, expected exactly $APP_PARTITION_BYTES." "do not flash it."
        return 1
    fi
    IMG_MD5=$(host_md5 "$1")
    ui_say "  Image: $1"
    ui_say "  Size $sz bytes (full APP partition), md5 $IMG_MD5."
}

cam_after_flash() {
    ui_blank
    ui_good "  Flash done and verified. The wizard NEVER power-cycles the camera."
    ui_say "  Now power-cycle it BY HAND: unplug the USB-C power, wait 5 seconds, plug it back."
    ui_say "  Keep the card in for now; then run: ./akam step verify"
}

cmd_flash() {
    local img md5 route app bk
    step_need flash || return 1
    step_banner flash
    if [ "$UI_DRY" = 1 ]; then
        cam_need_ip || return 1
        cam_key
        ui_say "  WOULD check the image (size $APP_PARTITION_BYTES, md5) and the stock APP hardware verdict (typed"
        ui_say "  a yes/no in a red box unless validated), then choose the route:"
        ui_say "    - SSH reflash when Akam already runs and your key works (no card needed), or"
        ui_say "    - the maintenance telnet with the flash card (stock firmware, or Akam with the maintenance card)."
        flash_telnet dry-run dry-run Flashing || return 1
        state_set flash_md5 dry-run
        state_mark_done flash
        return 0
    fi
    cam_need_ip || return 1
    img=$(state_get image_path)
    cam_image_gate "$img" || return 1
    if [ "$(state_get image_sha)" != "$(host_sha256 "$img")" ]; then ui_fail "the image changed since it was built." "run ./akam step image again."; return 1; fi
    md5=$IMG_MD5
    bk=$(state_get backup_dir)
    app=$(host_sha256 "$bk/mtdblock7.bin" 2>/dev/null || true)
    if [ -z "$app" ]; then
        ui_fail "the stock APP backup ($bk/mtdblock7.bin) is not readable, so the hardware cannot be checked." "run: ./akam backup --backup-dir DIR"
        return 1
    fi
    cam_confirm_untested "$app" "Flash with untested firmware?" || return 1
    cam_key
    route=telnet
    if [ "$(state_get camera_kind)" = akam ] || [ "$(state_get camera_telnet)" != 1 ]; then
        cam_ssh_setup
        if cam_ssh_ok; then
            ui_say "  Akam already runs and SSH works: the card is not needed."
            if ui_pick_route "  Reflash over SSH (recommended)? [no = use the maintenance telnet and the card]" \
                "Reflash over SSH (recommended, no card needed)" "Use the maintenance telnet and the card" "How do you want to flash?"; then route=ssh; fi
        elif [ "$(state_get camera_telnet)" != 1 ]; then
            ui_fail "no way in: SSH with $CAM_KEY does not work and the maintenance telnet is closed." \
                "check the key (--identity FILE), or put the first-boot card in the camera and run ./akam step find-camera."
            return 1
        fi
    fi
    ui_say "  Route: $route"
    if [ "$route" = ssh ]; then flash_ssh "$img" "$md5" Flashing || return 1; else flash_telnet "$img" "$md5" Flashing || return 1; fi
    state_set flash_md5 "$md5"
    state_set flash_route "$route"
    state_clear_done verify
    state_mark_done flash
    cam_after_flash
}

# ------------------------------------------------------------------ verify
# cam_wait_port PORT [SECS [WHAT]]: wait until the camera answers on PORT. Shows what it waits for
# and the elapsed time: a spinner on a terminal, one line every 15 s otherwise. 0 = answered,
# 1 = still silent after SECS (the caller explains what that probably means).
cam_wait_port() {
    local p=$1 max=${2:-${AKAM_WAIT_SECS:-240}} what=${3:-the camera} t0=$SECONDS el frame=0 lastp=-100 spin=""
    ui_say "  Waiting for $what: $CAM_IP port $p (up to $max seconds) ..."
    while :; do
        el=$((SECONDS - t0))
        if [ "${TUI_ON:-0}" = 1 ]; then
            tui_elapsed "$el"
            spin=${TUI_SPIN[frame % ${#TUI_SPIN[@]}]}
            printf '\r\e[K  %s%s%s waiting for %s  %s%s of %ss%s' "$TUI_C" "$spin" "$TUI_RST" "$what" "$TUI_D" "$REPLY" "$max" "$TUI_RST" >&2
            frame=$((frame + 1))
        elif [ $((el - lastp)) -ge 15 ]; then
            if [ "$el" -gt 0 ]; then printf '  ... still waiting for %s (%s of %s s)\n' "$what" "$el" "$max"; fi
            lastp=$el
        fi
        if net_port_open "$CAM_IP" "$p" 2; then
            if [ "${TUI_ON:-0}" = 1 ]; then printf '\r\e[K' >&2; fi
            ui_say "  Port $p answered after $((SECONDS - t0)) s."
            return 0
        fi
        if [ "$el" -ge "$max" ]; then
            if [ "${TUI_ON:-0}" = 1 ]; then printf '\r\e[K' >&2; fi
            ui_say "  No answer on port $p within $max s."
            return 1
        fi
        sleep 1
    done
}

# cam_hwcheck: copy akam_hwcheck.sh to the camera's RAM and run it. Shows PASS/FAIL/WARN lines.
cam_hwcheck() {
    local hw out rc=0
    hw=$(wiz_tool hwcheck)
    [ -f "$hw" ] || { ui_fail "akam_hwcheck.sh not found: $hw" ""; return 1; }
    cam_ssh_setup
    log_line "CMD ssh root@$CAM_IP 'cat > /tmp/akam_hwcheck.sh' < akam_hwcheck.sh"
    ui_say "  Copying akam_hwcheck.sh to the camera's RAM (/tmp, nothing is installed) ..."
    "${CAM_SSH[@]}" 'cat > /tmp/akam_hwcheck.sh' <"$hw" || { ui_fail "cannot copy akam_hwcheck.sh over SSH." ""; return 1; }
    log_line "CMD ssh root@$CAM_IP 'sh /tmp/akam_hwcheck.sh'"
    ui_say "  Running it (read-only) ..."
    out=$("${CAM_SSH[@]}" 'sh /tmp/akam_hwcheck.sh' 2>&1) || rc=$?
    printf '%s\n' "$out" | grep -E -e '^(PASS|FAIL|WARN|UNKNOWN)' -e '^INFO +RTSP auth' | sed 's/^/    /'
    return "$rc"
}

# cam_ports_check EXPECT_TELNET: open = 22 554 8091 only (24 allowed while the card is in).
cam_ports_check() {
    local p bad=0 ftp tel23
    ftp=${AKAM_PORT_FTP:-21}
    tel23=${AKAM_PORT_TELNET23:-23}
    net_probe "$CAM_IP" "$(cam_port ssh)" "$(cam_port rtsp)" "$(cam_port ctl)" "$(cam_port telnet)" "$ftp" "$tel23"
    ui_say "  Open ports: ${PROBE_OPEN:-none}"
    for p in "$(cam_port ssh)" "$(cam_port rtsp)" "$(cam_port ctl)"; do
        case " $PROBE_OPEN " in *" $p "*) ;; *) ui_err "port $p should be open and is not."; bad=1 ;; esac
    done
    for p in "$(cam_port telnet)" "$ftp" "$tel23"; do
        case " $PROBE_OPEN " in
            *" $p "*)
                if [ "$1" = closed ]; then ui_err "port $p should be closed (telnet/FTP must be off without the card) and is open."; bad=1; fi
                ;;
        esac
    done
    return "$bad"
}

# verify_hostname: after a good verify, give the camera the network name chosen for it (never fails the step).
verify_hostname() {
    if [ "$CAM_NAME" = default ]; then
        if [ "$UI_DRY" = 1 ]; then ui_say "  WOULD also set the camera's network name over SSH when you gave the camera a name (./akam --camera NAME)."; fi
        return 0
    fi
    cam_hostname_apply "$CAM_NAME" || ui_say "  You can try again later with: ./akam hostname $CAM_NAME"
}

cmd_verify() {
    step_need verify || return 1
    step_banner verify
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD wait for the camera, check SSH with your key, copy and run akam_hwcheck.sh (expecting no FAIL),"
        ui_say "  check that the open ports are 22, 554 and 8091 (24 allowed while the card is in), make sure the camera has a saved"
        ui_say "  Wi-Fi network (preflight_check.sh --ensure-wifi on the camera; it is created from the card's wifi.conf when missing,"
        ui_say "  and you are never asked to remove the card before that is done), then ask you to"
        ui_say "  unplug the power, take the card out and plug the power back in BY HAND, and repeat the checks:"
        ui_say "  telnet 24 and FTP 21 must then be closed. Only then is the step marked done."
        state_mark_done verify
        verify_hostname
        rtsp_auth_offer
        wipe_offer
        return 0
    fi
    cam_need_ip || return 1
    cam_key
    cam_wait_port "$(cam_port ssh)" "${AKAM_WAIT_SECS:-240}" "the camera to answer on SSH" ||
        { ui_fail "the camera does not answer on SSH." "it may still be starting (wait a minute and run this step again), the address may have changed (check your router), or the camera may have no network: see docs/akam_v1_flash_and_restore.md (Recovery). The step is NOT done."; return 1; }
    cam_ssh_setup
    cam_show_fingerprint
    cam_ssh_ok || { ui_fail "SSH with $CAM_KEY was refused." "is this the key you put on the flash card (--identity FILE)? The step is NOT done: do not remove the card."; return 1; }
    ui_good "  SSH with your key works."
    cam_hwcheck || { ui_fail "akam_hwcheck.sh reported a failure (lines above)." "do not remove the card yet; read docs/akam_v1_test_plan.md."; return 1; }
    cam_ports_check card || { ui_fail "unexpected port state." "do not remove the card yet."; return 1; }
    ui_good "  Checks passed with the card in."
    # Hard gate: never invite the user to remove the card while the camera has no saved Wi-Fi.
    cam_wifi_gate_ssh || { ui_say "  The step is NOT done: do not remove the card."; return 1; }
    ui_blank
    ui_say "  Now the real test: without the card. By hand:"
    ui_say "    1. unplug the camera's USB-C power  2. take the microSD card out  3. plug the power back in."
    ui_prompt "  Press Enter when the camera is powered again (or q to stop):" "" || { ui_fail "no input." "run ./akam step verify again."; return 1; }
    if [ "$ANSWER" = q ]; then ui_fail "stopped." "run ./akam step verify again when ready."; return 1; fi
    cam_wait_port "$(cam_port ssh)" "${AKAM_WAIT_SECS:-240}" "the camera to boot without the card and answer on SSH" ||
        { ui_fail "the camera is not on the network ${AKAM_WAIT_SECS:-240} seconds after the power-up without the card." \
            "most likely its Wi-Fi is not saved on the camera (no /etc/config/wpa_supplicant.conf): put the card back, power-cycle, run this step again (it creates the file) and see docs/akam_v1_install.md (Troubleshooting: no network after removing the card). Also check the address in your router: it may have changed."; return 1; }
    cam_ports_check closed || { ui_fail "port check failed without the card." "see docs/akam_v1_ssh_and_lockdown.md."; return 1; }
    cam_hwcheck || { ui_fail "akam_hwcheck.sh reported a failure without the card." ""; return 1; }
    state_mark_done verify
    ui_good "  Verified: SSH works, only 22, 554 and 8091 are open, telnet and FTP are off."
    verify_hostname
    rtsp_auth_offer
    wipe_offer
}

# ------------------------------------------------------------------ host name
# The camera's network name (what the router lists) comes from the name given to the camera here. The
# camera-side command /usr/sbin/akam_hostname.sh set|show|status NAME stores it in /etc/config/akam/hostname
# and DHCP announces it at the next boot. An older firmware does not have it: that is only a note.
HOSTNAME_CMD=/usr/sbin/akam_hostname.sh
HOSTNAME_MAX=32

# cam_hostname_valid NAME: a lowercase RFC 1123 label, at most 32 characters.
cam_hostname_valid() {
    case $1 in
        '' | -* | *- | *[!a-z0-9-]*) return 1 ;;
    esac
    [ "${#1}" -le "$HOSTNAME_MAX" ]
}

# cam_hostname_apply NAME: over SSH. Returns 0 also when the firmware lacks the command (a note is shown).
cam_hostname_apply() {
    local name=$1 out
    cam_hostname_valid "$name" || { ui_warn "'$name' is not a valid network name (lowercase letters, digits and '-', at most $HOSTNAME_MAX)."; return 1; }
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run over SSH: test -x $HOSTNAME_CMD, then $HOSTNAME_CMD set $name (applies after the next power cycle)."
        return 0
    fi
    cam_ssh_setup
    log_line "CMD ssh root@$CAM_IP test -x $HOSTNAME_CMD"
    if ! "${CAM_SSH[@]}" "test -x $HOSTNAME_CMD" >/dev/null 2>&1; then
        ui_say "  Note: this firmware does not have $HOSTNAME_CMD yet, so the camera keeps its default network name."
        ui_say "  Update to a newer Akam image, then run: ./akam hostname $name"
        return 0
    fi
    log_line "CMD ssh root@$CAM_IP $HOSTNAME_CMD set $name"
    if ! out=$("${CAM_SSH[@]}" "$HOSTNAME_CMD set $name" 2>&1); then
        ui_warn "setting the network name failed: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
        return 1
    fi
    ui_good "  The camera's network name is set to '$name'. It applies after the next power cycle (unplug, wait 5 seconds, plug back)."
}

# cmd_hostname [NAME]: ./akam hostname [NAME]; NAME defaults to the camera's name.
cmd_hostname() {
    local name=${1:-}
    step_banner_free "Set the camera's network name"
    ui_say "  Why: your router will list the camera under this name instead of a number. Risk: none (a small file on the camera)."
    if [ -z "$name" ] && [ "$CAM_NAME" != default ]; then name=$CAM_NAME; fi
    if [ -z "$name" ]; then
        if [ "$UI_DRY" = 1 ]; then
            name="<name>"
        else
            ui_prompt "  Network name for the camera (lowercase letters, digits, '-'):" "" || { ui_fail "no input." "run ./akam hostname NAME"; return 1; }
            name=$ANSWER
        fi
    fi
    if [ "$UI_DRY" != 1 ] && ! cam_hostname_valid "$name"; then
        ui_fail "'$name' is not a valid network name." "use lowercase letters, digits and '-' (at most $HOSTNAME_MAX, not starting or ending with '-')."
        return 1
    fi
    cam_need_ip || return 1
    cam_key
    if [ "$UI_DRY" != 1 ]; then
        cam_ssh_ok || { ui_fail "SSH to root@$CAM_IP with the key $CAM_KEY does not work." "the camera must run Akam and be on the network (--identity FILE, --ip ADDRESS)."; return 1; }
    fi
    cam_hostname_apply "$name" || return 1
}

# ------------------------------------------------------------------ rtsp-auth
# Optional Digest login on the video stream (RTSP, port 554), managed over SSH with the camera
# command akam_rtsp_auth.sh (specs/03-spec-rtsp-auth.md). The password is typed here, without
# echo, and hashed here: only the line "user:akam:HA1" (HA1 = md5 of "user:akam:password",
# the password-equivalent of Digest) travels, through the stdin of ssh. Neither the password
# nor the HA1 is ever in a command line, in the state file or in the log.
RTSP_AUTH_CMD=/usr/sbin/akam_rtsp_auth.sh
RTSP_AUTH_REALM=akam
RTSP_AUTH_MIN_PASS=8

rtsp_user_valid() {
    case $1 in '' | *[!A-Za-z0-9._@+-]*) return 1 ;; esac
    [ "${#1}" -le 32 ]
}

# rtsp_remote ARGS...: run the camera command over SSH (stdin passes through).
rtsp_remote() {
    log_line "CMD ssh root@$CAM_IP $RTSP_AUTH_CMD $*"
    "${CAM_SSH[@]}" "$RTSP_AUTH_CMD $*"
}

# rtsp_connect: IP, key and a working SSH session, or a clear failure.
rtsp_connect() {
    cam_need_ip || return 1
    cam_key
    cam_ssh_ok || { ui_fail "cannot log in to $CAM_IP over SSH with $CAM_KEY." "the camera must run Akam (see ./akam step verify); pass --identity FILE if your key is elsewhere."; return 1; }
}

# rtsp_status: RTSP_STATE = enabled | disabled | invalid | unknown; RTSP_TEXT = the camera's words.
rtsp_status() {
    RTSP_TEXT=$(rtsp_remote status 2>&1) || true
    case $RTSP_TEXT in
        enabled*) RTSP_STATE=enabled ;;
        disabled*) RTSP_STATE=disabled ;;
        INVALID*) RTSP_STATE=invalid ;;
        *) RTSP_STATE=unknown ;;
    esac
}

rtsp_stream_hint() {
    ui_say "  Stream URL: rtsp://$1:PASSWORD@$CAM_IP:554/"
    ui_say "  Characters such as @ : / ? # % and spaces in the password must be URL-encoded in that address"
    ui_say "  (@ = %40, : = %3A, / = %2F, ? = %3F, # = %23, % = %25): see docs/troubleshooting.md."
}

rtsp_auth_set() {
    local user pw pw2 ha1 err
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask a user name and a password (twice, not shown, at least $RTSP_AUTH_MIN_PASS characters)."
        ui_say "  WOULD compute md5(user:$RTSP_AUTH_REALM:password) here and send ONLY the line user:$RTSP_AUTH_REALM:HA1 on stdin:"
        ui_say "  WOULD run: ssh root@${CAM_IP:-<camera-ip>} $RTSP_AUTH_CMD set-hash"
        return 0
    fi
    host_have md5sum || { ui_fail "md5sum is not installed on this computer." "install coreutils."; return 1; }
    rtsp_connect || return 1
    ui_prompt "  User name for the video stream [viewer]:" viewer || { ui_fail "no input." ""; return 1; }
    user=$ANSWER
    rtsp_user_valid "$user" || { ui_fail "invalid user name." "use 1 to 32 letters, digits or . _ @ + - (no spaces, no colon)."; return 1; }
    ui_secret "  Password (at least $RTSP_AUTH_MIN_PASS characters, not shown):" || { ui_fail "no input." ""; return 1; }
    pw=$SECRET
    ui_secret "  Type it again:" || { ui_fail "no input." ""; return 1; }
    pw2=$SECRET
    SECRET=""
    if [ "$pw" != "$pw2" ]; then ui_fail "the two passwords differ." "nothing was changed; try again."; return 1; fi
    if [ "${#pw}" -lt "$RTSP_AUTH_MIN_PASS" ]; then ui_fail "the password is too short." "use at least $RTSP_AUTH_MIN_PASS characters; nothing was changed."; return 1; fi
    # printf is a bash builtin and md5sum reads a pipe: the password is in no command line.
    ha1=$(printf '%s' "$user:$RTSP_AUTH_REALM:$pw" | md5sum | cut -d' ' -f1)
    log_add_secret "$ha1"
    pw="" pw2=""
    ui_say "  Sending the login (as a hash, over SSH) ..."
    err="$STATE_DIR/rtsp_auth.err"
    if ! printf '%s\n' "$user:$RTSP_AUTH_REALM:$ha1" | rtsp_remote set-hash >/dev/null 2>"$err"; then
        ui_fail "the camera refused it: $(head -n 3 "$err" | tr '\n' ' ')" "does this image have $RTSP_AUTH_CMD? Build and flash a current image (./akam update)."
        rm -f "$err"
        return 1
    fi
    rm -f "$err"
    ui_good "  The video stream now asks for a user name and password (user: $user)."
    ui_say "  Players that are watching are disconnected within a second and ask again."
    rtsp_stream_hint "$user"
}

rtsp_auth_disable() {
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask: Switch the password off? Anyone on your network could then watch the camera. [y/N]"
        ui_say "  WOULD run: ssh root@${CAM_IP:-<camera-ip>} $RTSP_AUTH_CMD disable"
        return 0
    fi
    rtsp_connect || return 1
    ui_require_tty "switching the stream password off" || return 1
    ui_ask_yn "  Switch the video stream password off (anyone on your network could watch the camera)?" || { ui_say "  Left as it was."; return 0; }
    rtsp_remote disable || { ui_fail "the camera could not remove the credentials." "does this image have $RTSP_AUTH_CMD? (./akam update)"; return 1; }
    ui_good "  The video stream is open again (no password)."
}

# rtsp_auth_offer: the question at the end of verify. Never fatal, default No.
rtsp_auth_offer() {
    ui_blank
    ui_say "  Optional: protect the video stream with a password."
    ui_say "  Without one, anyone on your network can watch the camera at rtsp://${CAM_IP:-<camera-ip>}:554/."
    ui_say "  With one, players ask for a user name and password (Digest login: VLC, ffmpeg, go2rtc and Frigate support it)."
    ui_say "  You can change your mind at any time: ./akam rtsp-auth set | disable | status"
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD ask: Protect the video stream with a username/password? [y/N] (default: No)"
        rtsp_auth_set
        return 0
    fi
    if ! host_is_tty; then
        ui_say "  No terminal: the stream stays open. Run ./akam rtsp-auth set later."
        return 0
    fi
    rtsp_status
    if [ "$RTSP_STATE" = enabled ]; then
        ui_say "  Already on: $RTSP_TEXT. Change it with ./akam rtsp-auth set."
        return 0
    fi
    if ui_ask_yn "  Protect the video stream with a username/password?"; then
        rtsp_auth_set || ui_warn "the stream password was not set (the verification above still stands). Run ./akam rtsp-auth set later."
    else
        ui_say "  Stream left open. Run ./akam rtsp-auth set whenever you want a password."
    fi
    return 0
}

cmd_rtsp_auth() {
    local action=${1:-status}
    case $action in set | disable | status) ;; *) echo "akam: rtsp-auth needs set, disable or status (see ./akam help)" >&2; return 2 ;; esac
    step_banner_free "RTSP password: $action"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        case $action in
            status) ui_say "  WOULD run (read-only): ssh root@$CAM_IP $RTSP_AUTH_CMD status" ;;
            set) rtsp_auth_set ;;
            disable) rtsp_auth_disable ;;
        esac
        return 0
    fi
    case $action in
        status)
            rtsp_connect || return 1
            rtsp_status
            case $RTSP_STATE in
                enabled) ui_say "  RTSP password: $RTSP_TEXT. Players must log in (Digest)." ;;
                disabled) ui_say "  RTSP password: disabled. The stream is open to anyone on your network." ;;
                invalid) ui_warn "the credentials file is invalid: the camera refuses every protected request. Run ./akam rtsp-auth set or disable."; return 1 ;;
                *) ui_fail "the camera did not answer like Akam ($RTSP_AUTH_CMD missing?)." "update the image (./akam update) to get the RTSP password command."; return 1 ;;
            esac
            ;;
        set) rtsp_auth_set ;;
        disable) rtsp_auth_disable ;;
    esac
}

# ------------------------------------------------------------------ led
# The front status LED and the infrared (night) LEDs, set over SSH through the camera's own control
# port (8091, reached from the camera itself: 127.0.0.1) with the keys led.status and night.ir_led
# (specs/06-spec-leds.md). The camera saves them, so they survive a reboot. Nothing secret, nothing
# destructive: no confirmation is asked.
#   ./akam led                      show both settings
#   ./akam led status on|off|auto   front LED (auto: blinks while starting or without Wi-Fi, slowly
#                                   while someone watches the video, solid otherwise)
#   ./akam led ir auto|off          IR LEDs at night (off: the image still switches to night mode)
LED_PORT=8091

# led_remote LINE...: send the control-port lines (one SET/GET each) from the camera to its own
# port; prints the answers without the LOG lines the port pushes by itself.
led_remote() {
    local line script=""
    for line in "$@"; do script="$script$line\\n"; done
    log_line "CMD ssh root@$CAM_IP printf <$#> | nc -w 2 127.0.0.1 $LED_PORT"
    "${CAM_SSH[@]}" "printf '$script' | nc -w 2 127.0.0.1 $LED_PORT" 2>&1 | grep -v '^LOG ' || true
}

# led_get KEY OUT: the value of KEY in the answers in OUT (empty when absent).
led_get() {
    local l v=""
    while IFS= read -r l; do
        case $l in "$1="*) v=${l#*=}; v=${v%% *} ;; esac
    done <<<"$2"
    printf '%s' "$v"
}

led_show() {
    local out st state ir
    out=$(led_remote "GET led.status" "GET led.state" "GET night.ir_led")
    st=$(led_get led.status "$out")
    state=$(led_get led.state "$out")
    ir=$(led_get night.ir_led "$out")
    if [ -z "$st" ] || [ -z "$ir" ]; then
        ui_fail "the camera did not answer the LED settings." "it may run an older Akam image without them (./akam update), or its control port may be busy; try again."
        return 1
    fi
    case $st in
        on) ui_say "  Front LED: on (left as the camera sets it)." ;;
        off) ui_say "  Front LED: off." ;;
        auto) ui_say "  Front LED: auto (now: ${state:-unknown}). Blinks while the camera starts or has no Wi-Fi, blinks slowly while someone watches the video, solid otherwise." ;;
        *) ui_say "  Front LED: $st." ;;
    esac
    case $ir in
        auto) ui_say "  IR LEDs at night: auto (they light in night mode)." ;;
        off) ui_say "  IR LEDs at night: off (never lit; the night image still switches on)." ;;
        *) ui_say "  IR LEDs at night: $ir." ;;
    esac
    ui_say "  Both settings are saved on the camera and kept after a reboot."
}

# led_check_set KEY VALUE ANSWERS: 0 when the camera answered "OK KEY=VALUE"; warns when it could not save.
led_check_set() {
    local line
    line=$(printf '%s\n' "$3" | grep -m1 -E "^(OK $1=$2|ERR .*$1)" || true)
    case $line in
        "OK $1=$2"*)
            case $line in
                *"not saved"*) ui_warn "$1 is $2 now, but the camera could not save it: it will be lost at the next reboot ($line)." ;;
            esac
            return 0
            ;;
        ERR*) ui_fail "the camera refused $1 $2: ${line#ERR }" "this image may not have the LED settings (./akam update)."; return 1 ;;
    esac
    ui_fail "the camera did not confirm $1 $2." "it may run an older Akam image without the LED settings (./akam update), or its control port may be busy; try again."
    return 1
}

cmd_led() {
    local status="" ir="" show=0 out
    while [ $# -gt 0 ]; do
        case $1 in
            show) show=1; shift ;;
            status)
                case ${2:-} in on | off | auto) ;; *) echo "akam: led status needs on, off or auto (see ./akam help)" >&2; return 2 ;; esac
                [ -z "$status" ] || { echo "akam: led status given twice (see ./akam help)" >&2; return 2; }
                status=$2; shift 2
                ;;
            ir)
                case ${2:-} in auto | off) ;; *) echo "akam: led ir needs auto or off (see ./akam help)" >&2; return 2 ;; esac
                [ -z "$ir" ] || { echo "akam: led ir given twice (see ./akam help)" >&2; return 2; }
                ir=$2; shift 2
                ;;
            *) echo "akam: led: unknown word '$1' (see ./akam help)" >&2; return 2 ;;
        esac
    done
    if [ "$show" = 1 ] && { [ -n "$status" ] || [ -n "$ir" ]; }; then
        echo "akam: led: show cannot be combined with status or ir (see ./akam help)" >&2
        return 2
    fi
    step_banner_free "Front LED and IR LEDs"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 $LED_PORT"
        [ -z "$status" ] || ui_say "  WOULD send: SET led.status $status"
        [ -z "$ir" ] || ui_say "  WOULD send: SET night.ir_led $ir"
        [ -n "$status$ir" ] || ui_say "  WOULD send (read-only): GET led.status, GET led.state, GET night.ir_led"
        return 0
    fi
    rtsp_connect || return 1
    if [ -n "$status$ir" ]; then
        local lines=()
        [ -z "$status" ] || lines+=("SET led.status $status")
        [ -z "$ir" ] || lines+=("SET night.ir_led $ir")
        out=$(led_remote "${lines[@]}")
        [ -z "$status" ] || led_check_set led.status "$status" "$out" || return 1
        [ -z "$ir" ] || led_check_set night.ir_led "$ir" "$out" || return 1
        ui_good "  Done."
    fi
    led_show
}

# ------------------------------------------------------------------ audio and picture
# Speaker settings and picture settings, set over SSH through the camera's own control port (like ./akam led:
# the lines go to 127.0.0.1:8091 on the camera, which accepts writes from itself only). Small reversible
# settings, so no confirmation is asked (same as ./akam led). One SET per ssh call.

# ctl_set KEY VALUE: SET one key; 0 when the camera answered OK (a note such as "restart to apply" or
# "not saved" is kept in CTL_NOTE and warned about), 1 otherwise with the reason shown.
CTL_NOTE=""
ctl_set() {
    local out line
    CTL_NOTE=""
    out=$(led_remote "SET $1 $2")
    line=$(printf '%s\n' "$out" | grep -m1 -E '^(OK|ERR) ' || true)
    case $line in
        OK*)
            case $line in
                *"not saved"*) ui_warn "$1 is $2 now, but the camera could not save it: it will be lost at the next reboot ($line)." ;;
                *"restart to apply"*) CTL_NOTE=restart ;;
            esac
            return 0
            ;;
        ERR*) ui_fail "the camera refused $1 $2: ${line#ERR }" "this image may not have that setting (./akam update)."; return 1 ;;
    esac
    ui_fail "the camera did not confirm $1 $2." "it may run an older Akam image (./akam update), or its control port may be busy; try again."
    return 1
}

# ctl_onoff VALUE: 1 -> on, 0 -> off.
ctl_onoff() { case $1 in 1) printf on ;; 0) printf off ;; *) printf '%s' "${1:-unknown}" ;; esac; }

audio_show() {
    local out vol en talk state under drops
    out=$(led_remote "GET audio.out_volume" "GET audio.out_enable" "GET audio.talk_enable" "GET audio.out_state" \
        "GET audio.out_underruns" "GET audio.out_drops")
    vol=$(led_get audio.out_volume "$out")
    en=$(led_get audio.out_enable "$out")
    talk=$(led_get audio.talk_enable "$out")
    state=$(led_get audio.out_state "$out")
    under=$(led_get audio.out_underruns "$out")
    drops=$(led_get audio.out_drops "$out")
    if [ -z "$vol" ] || [ -z "$en" ]; then
        ui_fail "the camera did not answer the audio settings." "it may run an older Akam image without audio output (./akam update), or its control port may be busy; try again."
        return 1
    fi
    ui_say "  Speaker output: $(ctl_onoff "$en") (state: ${state:-unknown}); the on/off choice is read at start-up."
    ui_say "  Volume: $vol (0 to 100)."
    ui_say "  Talk (RTSP audio backchannel): $(ctl_onoff "${talk:-}")."
    [ -z "$under$drops" ] || ui_say "  Playback counters: underruns ${under:-?}, drops ${drops:-?}."
    ui_say "  Volume, speaker and talk switches are saved on the camera and kept after a reboot."
}

cmd_audio() {
    local action=show value="" key="" val=""
    if [ $# -gt 0 ]; then
        action=$1
        case $action in
            show | beep | on | off) [ $# -eq 1 ] || { echo "akam: audio $action takes no more words (see ./akam help)" >&2; return 2; } ;;
            volume)
                [ $# -eq 2 ] || { echo "akam: audio volume needs a number from 0 to 100 (see ./akam help)" >&2; return 2; }
                case $2 in
                    "" | *[!0-9]* | ????*) echo "akam: audio volume needs a number from 0 to 100 (see ./akam help)" >&2; return 2 ;;
                esac
                [ "$2" -le 100 ] || { echo "akam: audio volume needs a number from 0 to 100 (see ./akam help)" >&2; return 2; }
                value=$((10#$2))
                ;;
            talk)
                [ $# -eq 2 ] || { echo "akam: audio talk needs on or off (see ./akam help)" >&2; return 2; }
                case $2 in on | off) value=$2 ;; *) echo "akam: audio talk needs on or off (see ./akam help)" >&2; return 2 ;; esac
                ;;
            *) echo "akam: audio: unknown word '$1' (see ./akam help)" >&2; return 2 ;;
        esac
    fi
    case $action in
        volume) key=audio.out_volume; val=$value ;;
        beep) key=audio.out_test; val=1 ;;
        on) key=audio.out_enable; val=1 ;;
        off) key=audio.out_enable; val=0 ;;
        talk) key=audio.talk_enable; val=$([ "$value" = on ] && echo 1 || echo 0) ;;
    esac
    step_banner_free "Camera speaker"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 $LED_PORT"
        if [ -n "$key" ]; then ui_say "  WOULD send: SET $key $val"; else ui_say "  WOULD send (read-only): GET audio.out_volume, audio.out_enable, audio.talk_enable, audio.out_state, audio.out_underruns, audio.out_drops"; fi
        return 0
    fi
    rtsp_connect || return 1
    if [ -n "$key" ]; then
        ctl_set "$key" "$val" || return 1
        case $action in
            beep) ui_good "  Done. A short beep plays on the speaker (if you heard nothing: volume, docs/troubleshooting.md, audio output)." ;;
            on)
                if [ "$CTL_NOTE" = restart ]; then
                    ui_warn "the speaker is enabled and saved, but it only starts after the camera application restarts: reboot the camera (power cycle) to apply."
                else
                    ui_good "  Done. The speaker is enabled."
                fi
                ;;
            off) ui_good "  Done. The speaker is off now and stays off after a reboot (./akam audio on to enable it again)." ;;
            *) ui_good "  Done." ;;
        esac
    fi
    audio_show
}

# picture: isp.brightness/contrast/saturation/sharpness are saved on the camera and applied at start-up;
# img.wb (conf | manual) and night.mode (auto | day | night) are RAM-only: they go back at the next reboot.
picture_show() {
    local out br co sa sh wb nm
    out=$(led_remote "GET isp.brightness" "GET isp.contrast" "GET isp.saturation" "GET isp.sharpness" "GET img.wb" "GET night.mode")
    br=$(led_get isp.brightness "$out")
    co=$(led_get isp.contrast "$out")
    sa=$(led_get isp.saturation "$out")
    sh=$(led_get isp.sharpness "$out")
    wb=$(led_get img.wb "$out")
    nm=$(led_get night.mode "$out")
    if [ -z "$br" ] || [ -z "$co" ] || [ -z "$sa" ] || [ -z "$sh" ]; then
        ui_fail "the camera did not answer the picture settings." "its control port may be busy; try again."
        return 1
    fi
    ui_say "  Brightness $br, contrast $co, saturation $sa, sharpness $sh (each -50 to 50, 0 = unchanged). Saved on the camera."
    ui_say "  White balance: ${wb:-unknown} (conf = the camera's calibration, manual = Akam's own block). Saved on the camera."
    ui_say "  Day/night: ${nm:-unknown} (auto, day or night). Saved on the camera."
}

picture_num() { # WORD: a whole number from -50 to 50
    case $1 in
        "" | - | *[!0-9-]* | ?*-*) return 1 ;;
    esac
    local n=${1#-}
    [ -n "$n" ] && [ "${#n}" -le 3 ] && [ "$((10#$n))" -le 50 ]
}

picture_norm() { # WORD (already valid): no leading zeros, no "-0"
    local n=$((10#${1#-}))
    if [ "${1#-}" != "$1" ] && [ "$n" != 0 ]; then printf -- '-%s' "$n"; else printf '%s' "$n"; fi
}

cmd_picture() {
    local keys=() vals=() reset=0 show=0 k
    while [ $# -gt 0 ]; do
        case $1 in
            show) show=1; shift ;;
            reset) reset=1; shift ;;
            brightness | contrast | saturation | sharpness)
                picture_num "${2:-}" || { echo "akam: picture $1 needs a whole number from -50 to 50 (see ./akam help)" >&2; return 2; }
                for k in ${keys[@]+"${keys[@]}"}; do [ "$k" != "isp.$1" ] || { echo "akam: picture $1 given twice (see ./akam help)" >&2; return 2; }; done
                keys+=("isp.$1"); vals+=("$(picture_norm "$2")")
                shift 2
                ;;
            wb)
                case ${2:-} in conf | manual) ;; *) echo "akam: picture wb needs conf or manual (see ./akam help)" >&2; return 2 ;; esac
                for k in ${keys[@]+"${keys[@]}"}; do [ "$k" != img.wb ] || { echo "akam: picture wb given twice (see ./akam help)" >&2; return 2; }; done
                keys+=(img.wb); vals+=("$2"); shift 2
                ;;
            night)
                case ${2:-} in auto | day | night) ;; *) echo "akam: picture night needs auto, day or night (see ./akam help)" >&2; return 2 ;; esac
                for k in ${keys[@]+"${keys[@]}"}; do [ "$k" != night.mode ] || { echo "akam: picture night given twice (see ./akam help)" >&2; return 2; }; done
                keys+=(night.mode); vals+=("$2"); shift 2
                ;;
            *) echo "akam: picture: unknown word '$1' (see ./akam help)" >&2; return 2 ;;
        esac
    done
    if [ "$((show + reset))" -gt 0 ] && { [ "$((show + reset))" -gt 1 ] || [ "${#keys[@]}" -gt 0 ]; }; then
        echo "akam: picture: show and reset take no other word (see ./akam help)" >&2
        return 2
    fi
    if [ "$reset" = 1 ]; then
        keys=(isp.brightness isp.contrast isp.saturation isp.sharpness); vals=(0 0 0 0)
    fi
    step_banner_free "Picture settings"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run over SSH on the camera: printf '<GET/SET lines>' | nc -w 2 127.0.0.1 $LED_PORT"
        local i
        for i in "${!keys[@]}"; do ui_say "  WOULD send: SET ${keys[$i]} ${vals[$i]}"; done
        [ "${#keys[@]}" -gt 0 ] || ui_say "  WOULD send (read-only): GET isp.brightness, isp.contrast, isp.saturation, isp.sharpness, img.wb, night.mode"
        return 0
    fi
    rtsp_connect || return 1
    if [ "${#keys[@]}" -gt 0 ]; then
        local i
        for i in "${!keys[@]}"; do ctl_set "${keys[$i]}" "${vals[$i]}" || return 1; done
        if [ "$reset" = 1 ]; then ui_good "  Done. Brightness, contrast, saturation and sharpness are back to 0 and the saved file is removed."; else ui_good "  Done."; fi
    fi
    picture_show
}

# ------------------------------------------------------------------ debug
# Port 8091 is read-only for the network: reads (LIST, GET, metrics) work, SET/SAVE/RESET are refused
# unless the command comes from the camera itself (SSH helpers such as ./akam led) or the debug flag
# is on (specs/07-spec-standard-protocols.md section 4). This command turns that RAM-only flag on or
# off over SSH with akam_debug.sh, for example to use the PC tuning tool. Off at every reboot.
#   ./akam debug status        is it on, and for how long
#   ./akam debug on [MINUTES]  allow writes from the network (default 60 minutes, 1 to 1440)
#   ./akam debug off           read-only again
DEBUG_CMD=/usr/sbin/akam_debug.sh

cmd_debug() {
    local action=${1:-status} min=""
    case $action in
        on)
            [ $# -le 2 ] || { echo "akam: debug on takes at most one number of minutes (see ./akam help)" >&2; return 2; }
            min=${2:-}
            if [ $# -eq 2 ]; then
                case $min in '' | *[!0-9]*) echo "akam: debug on: MINUTES must be a whole number from 1 to 1440 (see ./akam help)" >&2; return 2 ;; esac
                if [ "$min" -lt 1 ] || [ "$min" -gt 1440 ]; then
                    echo "akam: debug on: MINUTES must be a whole number from 1 to 1440 (see ./akam help)" >&2
                    return 2
                fi
            fi
            ;;
        off | status) [ $# -le 1 ] || { echo "akam: debug $action takes no argument (see ./akam help)" >&2; return 2; } ;;
        *) echo "akam: debug needs on [MINUTES], off or status (see ./akam help)" >&2; return 2 ;;
    esac
    local remote="$DEBUG_CMD $action${min:+ $min}" out rc
    step_banner_free "Debug mode of the control port: $action"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        [ "$action" != on ] || ui_say "  WOULD ask: Let anyone on your network change the camera settings through port 8091? [y/N]"
        ui_say "  WOULD run over SSH on the camera: $remote"
        return 0
    fi
    rtsp_connect || return 1
    if [ "$action" = on ]; then
        ui_require_tty "allowing writes on the control port" || return 1
        ui_ask_yn "  Let anyone on your network change the camera settings through port 8091 for a while?" || { ui_say "  Left as it was (read-only)."; return 0; }
    fi
    log_line "CMD ssh root@$CAM_IP $remote"
    out=$("${CAM_SSH[@]}" "$remote" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -ne 0 ]; then
        [ -z "$out" ] || ui_say "  $out"
        ui_fail "the camera did not run $DEBUG_CMD $action." "it may run an older Akam image without the debug command (./akam update), or ak_rtsp may not be running."
        return 1
    fi
    ui_say "  $out"
    case $action in
        on) ui_warn "anyone on your network can now change the camera settings through port 8091 until it is switched off: ./akam debug off." ;;
        off) ui_good "  Port 8091 is read-only for the network again." ;;
    esac
    return 0
}

# ------------------------------------------------------------------ talk, say
# Speak to the camera's speaker from this computer, with the ONVIF audio backchannel on the RTSP
# port (specs/07-spec-standard-protocols.md section 1). The audio is captured or converted here
# (ffmpeg, or arecord on Linux) as 8 kHz mono and sent by akam-talk (tools/akam-talk), which logs
# in with Digest. Talk needs the RTSP password: the camera never offers it without one. The
# password is typed here (not shown) or taken from AKAM_RTSP_PASSWORD, and reaches akam-talk only
# through its environment: never a command line, the state file or the log.
#   ./akam talk            microphone of this computer until Ctrl-C
#   ./akam talk 30         the same, at most 30 seconds
#   ./akam talk test       a 2 second test tone (no microphone needed)
#   ./akam say FILE        play an audio file (any format ffmpeg reads; a WAV 8 kHz mono needs no ffmpeg)
# Test hooks: AKAM_TALK (akam-talk to run), AKAM_TALK_CAPTURE_CMD (shell command that writes raw
# 8 kHz mono signed 16-bit little-endian audio on stdout, instead of ffmpeg or arecord).
TALK_URL_PORT=554

# talk_find_bin: TALK_BIN = an akam-talk that runs here. Order: $AKAM_TALK, a binary shipped next to
# the wizard (tools/akam-talk/bin/<os>-<cpu>/akam-talk), one built earlier in the state folder, then a
# build from tools/akam-talk with the C compiler or the pinned Zig.
talk_find_bin() {
    local os cpu src cache cc="" f
    TALK_BIN=""
    if [ -n "${AKAM_TALK:-}" ]; then
        [ -x "$AKAM_TALK" ] || { ui_fail "AKAM_TALK is set but is not executable: $AKAM_TALK" ""; return 1; }
        TALK_BIN=$AKAM_TALK
        return 0
    fi
    os=$(uname -s | tr '[:upper:]' '[:lower:]')
    cpu=$(host_arch)
    f="$ROOT/tools/akam-talk/bin/$os-$cpu/akam-talk"
    if [ -x "$f" ]; then TALK_BIN=$f; return 0; fi
    src="$ROOT/tools/akam-talk/akam_talk.c"
    cache="$STATE_DIR/akam-talk"
    if [ -x "$cache" ] && [ "$cache" -nt "$src" ] && [ "$cache" -nt "$ROOT/src/ak_rtsp/md5.c" ] && [ "$cache" -nt "$ROOT/src/ak_rtsp/g711.c" ]; then
        TALK_BIN=$cache
        return 0
    fi
    [ -f "$src" ] || { ui_fail "akam-talk is not part of this copy of Akam." "get a kit that has it, or set AKAM_TALK to a built akam-talk."; return 1; }
    if host_have cc; then
        cc=cc
    elif host_have gcc; then
        cc=gcc
    elif host_have clang; then
        cc=clang
    elif [ -f "$ROOT/tools/build/env.sh" ] && { . "$ROOT/tools/build/env.sh" >/dev/null 2>&1 || true; [ -n "${ZIG:-}" ]; }; then
        cc="$ZIG cc"
    fi
    [ -n "$cc" ] || { ui_fail "no C compiler to build akam-talk, and no prebuilt one for $os-$cpu." "$(host_pkg_install_hint gcc), or run tools/build/bootstrap.sh for the pinned Zig."; return 1; }
    ui_say "  Building akam-talk (once) ..."
    log_line "CMD $cc -o $cache akam_talk.c md5.c g711.c"
    # shellcheck disable=SC2086  # $cc may be "zig cc"
    if ! $cc -std=gnu99 -O2 -I"$ROOT/src/ak_rtsp" -o "$cache" "$src" "$ROOT/src/ak_rtsp/md5.c" "$ROOT/src/ak_rtsp/g711.c" 2>"$STATE_DIR/akam-talk.err"; then
        ui_fail "building akam-talk failed: $(head -n 3 "$STATE_DIR/akam-talk.err" | tr '\n' ' ')" "set AKAM_TALK to a prebuilt akam-talk."
        return 1
    fi
    TALK_BIN=$cache
}

# talk_login: the RTSP password must be on; sets TALK_USER and TALK_PASS.
talk_login() {
    local out
    rtsp_connect || return 1
    rtsp_status
    if [ "$RTSP_STATE" = disabled ]; then
        ui_say "  Talk needs the video stream password: the camera never offers talk without one."
        ui_say "  (Anyone on your network could otherwise speak through the camera.)"
        if host_is_tty && ui_ask_yn "  Set a password now?"; then
            rtsp_auth_set || return 1
            rtsp_status
        else
            ui_fail "no stream password, no talk." "run ./akam rtsp-auth set, then try again."
            return 1
        fi
    fi
    case $RTSP_STATE in
        enabled) ;;
        invalid) ui_fail "the camera's credentials file is invalid." "run ./akam rtsp-auth set or disable."; return 1 ;;
        *) ui_fail "the camera did not answer like Akam ($RTSP_AUTH_CMD missing?)." "update the image (./akam update)."; return 1 ;;
    esac
    out=$(rtsp_remote show 2>/dev/null || true)
    TALK_USER=${out#user: }
    TALK_USER=${TALK_USER%%[[:space:]]*}
    rtsp_user_valid "$TALK_USER" || { ui_fail "could not read the stream user name from the camera." "check ./akam rtsp-auth status."; return 1; }
    TALK_PASS=${AKAM_RTSP_PASSWORD:-}
    if [ -z "$TALK_PASS" ]; then
        ui_secret "  Stream password of user $TALK_USER (not shown):" || { ui_fail "no input." ""; return 1; }
        TALK_PASS=$SECRET
        SECRET=""
    fi
    log_add_secret "$TALK_PASS"
}

# talk_capture SECONDS: TALK_CAP = the command that writes the microphone as raw 8 kHz mono s16le.
talk_capture() {
    local secs=$1 os
    TALK_CAP=()
    if [ -n "${AKAM_TALK_CAPTURE_CMD:-}" ]; then
        TALK_CAP=(bash -c "$AKAM_TALK_CAPTURE_CMD")
        return 0
    fi
    os=$(uname -s)
    case $os in
        Linux)
            if host_have ffmpeg && host_have pactl && pactl info >/dev/null 2>&1; then
                TALK_CAP=(ffmpeg -hide_banner -loglevel error -f pulse -i default)
            elif host_have arecord; then
                TALK_CAP=(arecord -q -t raw -f S16_LE -r 8000 -c 1)
                [ "$secs" = 0 ] || TALK_CAP+=(-d "$secs")
                return 0
            elif host_have ffmpeg; then
                TALK_CAP=(ffmpeg -hide_banner -loglevel error -f alsa -i default)
            fi
            ;;
        Darwin)
            if host_have ffmpeg; then TALK_CAP=(ffmpeg -hide_banner -loglevel error -f avfoundation -i ":default"); fi
            ;;
    esac
    if [ "${#TALK_CAP[@]}" -eq 0 ]; then
        ui_fail "no way to read the microphone here (ffmpeg or arecord not found)." "$(host_pkg_install_hint ffmpeg); or set AKAM_TALK_CAPTURE_CMD to a command that writes raw 8 kHz mono s16le on stdout. ./akam talk test and ./akam say FILE do not need a microphone."
        return 1
    fi
    [ "$secs" = 0 ] || TALK_CAP+=(-t "$secs")
    # Speech normaliser: a headset microphone is far quieter than the test tone. speechnorm raises
    # the voice in real time without look-ahead delay; p=0.6 (peak target) was tuned by ear on the
    # camera speaker at audio.out_volume 100. Not available with arecord (raw ALSA capture).
    TALK_CAP+=(-af "speechnorm=e=12.5:r=0.0001:l=1:p=0.6" -ar 8000 -ac 1 -f s16le -)
}

# talk_run_client ARGS...: run akam-talk with the password in its environment only, input on stdin.
# Exit codes of akam-talk: 0 done, 2 network, 3 the camera refuses talk, 4 login refused, 6 no audio.
talk_run_client() {
    local rc=0
    log_line "CMD akam-talk --user $TALK_USER $* rtsp://$CAM_IP:$TALK_URL_PORT  (password in the environment)"
    AKAM_RTSP_PASSWORD=$TALK_PASS "$TALK_BIN" --user "$TALK_USER" "$@" "rtsp://$CAM_IP:$TALK_URL_PORT" || rc=$?
    case $rc in
        0) return 0 ;;
        2) ui_fail "the camera did not answer on port $TALK_URL_PORT." "check ./akam status and the network." ;;
        3) ui_fail "the camera does not offer talk." "it needs the stream password (it is on), audio out enabled (audio.out_enable=1) and audio.talk_enable=1; see docs/troubleshooting.md (Talk)." ;;
        4) ui_fail "the camera refused the user name or password." "check ./akam rtsp-auth status; run ./akam rtsp-auth set to choose a new one." ;;
        6) ui_fail "no audio was read from the input." "for a file: ffmpeg could not decode it; for the microphone: check that it is not muted and is the default input." ;;
        *) ui_fail "akam-talk stopped with status $rc." "run it with -v for details: set AKAM_TALK to a wrapper, or see docs/troubleshooting.md (Talk)." ;;
    esac
    return 1
}

# The capture (or conversion) tool runs as a tracked background job in its own session (setsid when
# available), feeding akam-talk through a fifo, and is stopped on EVERY way out: normal end, akam-talk
# failure, Ctrl-C, TERM, HUP (TERM, then KILL, on the whole group). Nothing may keep recording the
# microphone after the wizard returned. If the wizard itself is killed with SIGKILL nothing of it can
# run, so a small watchdog process notices that the wizard is gone (within a second) and stops the
# capture: the fifo alone would not do it, its reader (akam-talk) is still alive and still reading.
TALK_CAP_PID=""
TALK_CAP_GROUP=0
TALK_SIGNALED=""
TALK_CLIENT_PID=""
TALK_WATCHDOG_PID=""

talk_capture_stop() {
    local pid=$TALK_CAP_PID target
    [ -n "$pid" ] || return 0
    TALK_CAP_PID=""
    target=$pid
    [ "$TALK_CAP_GROUP" = 1 ] && target=-$pid
    # The group, and the process itself: before setsid has run (or while it is still blocked opening
    # the fifo) the group does not exist yet, and the wait below would never return.
    kill -TERM -- "$target" 2>/dev/null || true
    kill -TERM "$pid" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    kill -KILL -- "$target" 2>/dev/null || true
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# The watchdog outlives nothing: it ends with the wizard (normally, or killed) and stops the capture
# when the wizard is gone without having done it. talk_watchdog_stop ends it on the normal paths.
talk_watchdog_start() {
    local wiz=$$ cap=$TALK_CAP_PID grp=$TALK_CAP_GROUP
    (
        s=""
        trap '[ -z "$s" ] || kill "$s" 2>/dev/null; exit 0' TERM INT HUP
        while kill -0 "$wiz" 2>/dev/null; do
            sleep 1 &
            s=$!
            wait "$s" 2>/dev/null
        done
        [ "$grp" = 1 ] && kill -TERM -- "-$cap" 2>/dev/null
        kill -TERM "$cap" 2>/dev/null
        exit 0
    ) </dev/null >/dev/null 2>&1 &
    TALK_WATCHDOG_PID=$!
}

talk_watchdog_stop() {
    local pid=$TALK_WATCHDOG_PID
    [ -n "$pid" ] || return 0
    TALK_WATCHDOG_PID=""
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# A trap only runs between commands, so the client runs in the background and the wizard waits for it:
# `wait` returns as soon as a trapped signal arrives. INT: the terminal also sent it to akam-talk, which
# ends cleanly; the capture is stopped here, and akam-talk then sees the end of its input anyway.
talk_on_signal() {
    case $1 in
        INT) ;;
        *)
            TALK_SIGNALED=$1
            [ -z "$TALK_CLIENT_PID" ] || kill -TERM "$TALK_CLIENT_PID" 2>/dev/null || true
            ;;
    esac
    talk_capture_stop
}

# talk_pipe_capture ARGS...: run TALK_CAP and feed its output to akam-talk ARGS... Returns akam-talk's status.
talk_pipe_capture() {
    local dir fifo rc=0
    dir=$(mktemp -d "${STATE_DIR:-${TMPDIR:-/tmp}}/talk.XXXXXX") || { ui_fail "cannot create a temporary folder." ""; return 1; }
    fifo=$dir/audio
    mkfifo "$fifo" || { rm -rf "$dir"; ui_fail "cannot create a fifo." ""; return 1; }
    TALK_SIGNALED=""
    # Ctrl-C ends the talk, not the wizard; TERM and HUP end both (after the cleanup below).
    trap 'talk_on_signal INT' INT
    trap 'talk_on_signal TERM' TERM
    trap 'talk_on_signal HUP' HUP
    TALK_CAP_GROUP=0
    if host_have setsid; then
        setsid "${TALK_CAP[@]}" >"$fifo" &
        TALK_CAP_GROUP=1
    else
        "${TALK_CAP[@]}" >"$fifo" &
    fi
    TALK_CAP_PID=$!
    talk_watchdog_start
    talk_run_client "$@" <"$fifo" &
    TALK_CLIENT_PID=$!
    wait "$TALK_CLIENT_PID" || rc=$?
    # interrupted by a trapped signal: the client is still ending (TEARDOWN); wait for it for real
    while kill -0 "$TALK_CLIENT_PID" 2>/dev/null; do wait "$TALK_CLIENT_PID" || rc=$?; done
    TALK_CLIENT_PID=""
    talk_capture_stop
    talk_watchdog_stop
    trap - INT TERM HUP
    tui_rearm_traps
    rm -rf "$dir"
    case $TALK_SIGNALED in TERM) exit 143 ;; HUP) exit 129 ;; esac
    return "$rc"
}

cmd_talk() {
    local arg=${1:-} secs=0 test=0
    case $arg in
        "") ;;
        test) test=1 ;;
        *[!0-9]* | 0*) echo "akam: talk needs nothing, a number of seconds (1 to 3600) or 'test' (see ./akam help)" >&2; return 2 ;;
        *) secs=$arg; [ "$secs" -le 3600 ] || { echo "akam: talk: at most 3600 seconds" >&2; return 2; } ;;
    esac
    [ $# -le 1 ] || { echo "akam: talk takes at most one word (see ./akam help)" >&2; return 2; }
    step_banner_free "Talk to the camera"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD check over SSH that the stream password is on ($RTSP_AUTH_CMD status) and read the user name (show)."
        ui_say "  WOULD ask the stream password (not shown) unless AKAM_RTSP_PASSWORD is set."
        if [ "$test" = 1 ]; then
            ui_say "  WOULD run: akam-talk --user <user> --tone 2 rtsp://$CAM_IP:$TALK_URL_PORT   (password in the environment)"
        else
            ui_say "  WOULD run: <microphone as 8 kHz mono s16le> | akam-talk --user <user> --format s16le rtsp://$CAM_IP:$TALK_URL_PORT"
        fi
        return 0
    fi
    talk_find_bin || return 1
    talk_login || return 1
    if [ "$test" = 1 ]; then
        ui_say "  Sending a 2 second test tone to the speaker ..."
        talk_run_client --tone 2 || return 1
        ui_good "  Done. Heard it? If not: ./akam audio (volume, beep) and docs/troubleshooting.md, audio output."
        return 0
    fi
    talk_capture "$secs" || return 1
    if [ "$secs" = 0 ]; then
        ui_say "  Speak now. The camera plays your voice; its own microphone is muted while you speak."
        ui_say "  Press Ctrl-C to stop."
    else
        ui_say "  Speak now, for at most $secs seconds."
    fi
    log_line "CMD ${TALK_CAP[0]} ... | akam-talk"
    talk_pipe_capture --format s16le || return 1
    ui_good "  Talk ended."
}

cmd_say() {
    local file=${1:-}
    [ -n "$file" ] && [ $# -eq 1 ] || { echo "akam: say needs one audio file (see ./akam help)" >&2; return 2; }
    step_banner_free "Play a file on the camera"
    cam_need_ip || return 1
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD convert $file to 8 kHz mono (ffmpeg) and send it with akam-talk to rtsp://$CAM_IP:$TALK_URL_PORT (stream password needed)."
        return 0
    fi
    [ -f "$file" ] && [ -r "$file" ] || { ui_fail "cannot read $file." "give the path of an audio file."; return 1; }
    talk_find_bin || return 1
    local conv=()
    if host_have ffmpeg; then
        conv=(ffmpeg -hide_banner -loglevel error -i "$file" -ar 8000 -ac 1 -f s16le -)
    elif [ "$(head -c 4 "$file" 2>/dev/null)" = RIFF ]; then
        conv=()  # akam-talk reads a WAV 8 kHz mono 16-bit itself
    else
        ui_fail "ffmpeg is needed to convert $file." "$(host_pkg_install_hint ffmpeg); or give a WAV file at 8000 Hz, mono, 16-bit."
        return 1
    fi
    talk_login || return 1
    ui_say "  Playing $file on the camera ..."
    if [ "${#conv[@]}" -gt 0 ]; then
        log_line "CMD ffmpeg -i <file> ... | akam-talk"
        TALK_CAP=("${conv[@]}")
        talk_pipe_capture --format s16le || return 1
    else
        log_line "CMD akam-talk --input <file>"
        talk_run_client --input "$file" || return 1
    fi
    ui_good "  Done."
}

# ------------------------------------------------------------------ update
cmd_update() {
    local img md5 rc=0
    step_banner_free "Update Akam with a new image (SSH, no card)"
    cam_need_ip || return 1
    cam_key
    img=${OPT_IMAGE:-$(state_get image_path)}
    if [ "$UI_DRY" = 1 ]; then
        flash_ssh "${img:-<image>}" dry-run Updating || return 1
        return 0
    fi
    [ -n "$img" ] || { ui_fail "no image given." "build one with ./akam step image or pass --image FILE."; return 1; }
    cam_image_gate "$img" || return 1
    md5=$IMG_MD5
    flash_ssh "$img" "$md5" Updating || return 1
    state_set flash_md5 "$md5"
    state_clear_done verify
    cam_after_flash
}

step_banner_free() {
    ui_blank
    ui_bold "$1"
}

# ------------------------------------------------------------------ restore
cmd_restore() {
    local bk app md5 route=telnet
    step_banner_free "Restore the original (stock) APP from your backup"
    ui_say "  Why: puts the original application partition back. UBOOT, kernel, ROOTFS and CONFIG are never touched."
    ui_say "  Risk: WRITES the camera flash; a power cut can brick it."
    cam_need_ip || return 1
    cam_key
    bk=${OPT_BACKUP_DIR:-$(state_get backup_dir)}
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD check $bk/mtdblock7.bin (size $APP_PARTITION_BYTES, sha256 and md5), choose SSH (Akam running) or the"
        ui_say "  maintenance telnet, and ask a yes/no (default No). Same gates as flash; no reboot is ever sent."
        flash_telnet dry-run dry-run Restoring || return 1
        return 0
    fi
    if [ -z "$bk" ] || [ ! -f "$bk/mtdblock7.bin" ]; then
        ui_fail "no backup found." "pass --backup-dir DIR (it must hold mtdblock7.bin)."
        return 1
    fi
    cam_image_gate "$bk/mtdblock7.bin" || return 1
    md5=$IMG_MD5
    if [ -f "$bk/SHA256SUMS" ] && ! (cd "$bk" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
        ui_fail "SHA256SUMS does not match the files in $bk." "this backup is damaged: use your second copy."
        return 1
    fi
    app=$(host_sha256 "$bk/mtdblock7.bin")
    ui_say "  Stock APP sha256: $app"
    cam_ssh_setup
    if [ "$(state_get camera_kind)" = akam ] && cam_ssh_ok; then
        ui_say "  Akam runs and SSH works."
        if ui_pick_route "  Restore over SSH (no card needed)? [no = use the maintenance telnet and the card]" \
            "Restore over SSH (no card needed)" "Use the maintenance telnet and the card" "How do you want to restore?"; then route=ssh; fi
    fi
    ui_say "  Route: $route"
    if [ "$route" = ssh ]; then flash_ssh "$bk/mtdblock7.bin" "$md5" Restoring || return 1; else flash_telnet "$bk/mtdblock7.bin" "$md5" Restoring || return 1; fi
    state_clear_done flash
    state_clear_done verify
    ui_blank
    ui_good "  Restore written and verified (read-back md5 = $md5)."
    ui_say "  Power-cycle the camera BY HAND (unplug the USB-C power, wait 5 seconds, plug it back)."
    ui_say "  CONFIG was not touched, so the Wi-Fi settings stay. Details: docs/akam_v1_flash_and_restore.md section 5."
}
