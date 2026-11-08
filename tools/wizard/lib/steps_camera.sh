# shellcheck shell=bash
# Camera-side wizard steps: find-camera, backup (from the camera), flash, verify, update, restore.
# Sourced by ./akam after steps_host.sh, camsh.sh and transfer.sh.
# Expected globals (set by ./akam): ROOT, STATE_DIR, OPT_* values, UI_DRY.
#
# Rules kept everywhere in this file
#   - the PC always connects OUT to the camera;
#   - the typed YES (flash) and the typed confirmations are asked from the human; nothing here
#     ever sends "YES" by itself, and an empty stdin or a missing terminal fails the step;
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
CAM_UNTESTED_PHRASE="I UNDERSTAND THIS IS UNTESTED"

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
    pub=${OPT_PUBKEY:-$HOME/.ssh/akam.pub}
    CAM_KEY=${pub%.pub}
}

# cam_ssh_setup: build CAM_SSH. The host key is remembered the first time (accept-new) under the
# alias <ip>, the same alias reflash_over_ssh.sh uses.
cam_ssh_setup() {
    cam_key
    CAM_SSH=("${AKAM_SSH:-ssh}" -i "$CAM_KEY" -p "$(cam_port ssh)" -o BatchMode=yes -o ConnectTimeout=10
        -o StrictHostKeyChecking=accept-new -o "HostKeyAlias=$CAM_IP" -o IdentitiesOnly=yes "root@$CAM_IP")
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
    if [ "$(state_get mode)" = source ]; then printf '%s\n' "$ROOT/src/ak_rtsp/flash_tool"; else printf '%s\n' "$STATE_DIR/kit/bin/flash_tool"; fi
}

# cam_session_open: open the telnet session (or enter manual mode). Prints the hint on failure.
cam_session_open() {
    if [ "${OPT_MANUAL_TELNET:-0}" = 1 ]; then CAMSH_MANUAL=1; fi
    if camsh_open "$CAM_IP"; then return 0; fi
    ui_fail "cannot connect to $CAM_IP port $(cam_port telnet)." \
        "the maintenance telnet is only open while the toolkit card is in the camera. Insert the card, power the camera and wait about 2 minutes, or pass --manual-telnet."
    return 1
}

# cam_confirm_untested APPSHA WHAT: refuse silently-untested hardware (typed phrase).
cam_confirm_untested() {
    local verdict vstat vdev
    verdict=$(hw_verdict "$1")
    vstat=${verdict%%|*}
    vdev=${verdict#*|}
    ui_say "  Hardware verdict for this stock APP: $vstat - $vdev"
    ui_say "  Stock APP sha256: $1"
    [ "$vstat" = validated ] && return 0
    ui_warn "this firmware is NOT on the validated list. Akam may not work and the camera may need a restore."
    ui_require_tty "confirming an untested firmware" || return 1
    ui_confirm_typed "$2" "$CAM_UNTESTED_PHRASE" ||
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

# bk_pc_dir: a new folder ~/akam-backup-<date>[-N].
bk_pc_dir() {
    local base d n=1
    base=${AKAM_BACKUP_ROOT:-$HOME}/akam-backup-$(date +%Y%m%d)
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
        ui_say "  copy dump1 to ~/akam-backup-<date>/ over nc with size+hash checks (3 tries each),"
        ui_say "  write SHA256SUMS, compare the stock APP hash with tools/release/supported_hardware.txt"
        ui_say "  (typed '$CAM_UNTESTED_PHRASE' unless validated) and ask for a second copy on another medium."
        state_set backup_dir "<backup-dir>"
        state_set backup_sha dry-run
        state_mark_done backup
        return 0
    fi
    backup_camera
}

# ------------------------------------------------------------------ SSH reflash (flash, update, restore)
# cam_filter_flash_output: drop the progress bars (about 80 KB) and keep what matters.
cam_filter_flash_output() {
    tr '\r' '\n' | grep --line-buffered -i -E '^reflash_over_ssh:|error|fail|mismatch|md5|verif|retry|retries|fault|success|complete|refus' || true
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
    { "$@" 2>&1; echo "$?" >"$rcf"; } | tee -a "$logf" | cam_filter_flash_output | sed 's/^/    | /'
    rc=$(cat "$rcf")
    rm -f "$rcf"
    log_line "RC $rc"
    return "${rc:-1}"
}

# flash_ssh IMG MD5 LABEL: the validated no-card path. Precheck (writes nothing), typed YES, flash.
flash_ssh() {
    local img=$1 md5=$2 label=$3 ft args
    ft=$(cam_flash_tool)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD run the precheck of the SSH reflash (writes nothing), ask for a typed YES, then:"
        ui_say "    $(wiz_tool reflash) --host $CAM_IP --identity ${CAM_KEY:-<key>} --image <image> --md5 <md5> --flash-tool <flash_tool> --i-understand-no-uart"
        ui_say "  and relay its progress (the progress bars are filtered out). It never reboots the camera."
        ui_confirm_typed "Write the camera flash now?" YES
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
    ui_require_tty "writing the camera flash" || return 1
    ui_confirm_typed "Write the camera flash now?" YES ||
        { ui_fail "not confirmed (YES must be typed by you)." "nothing was written."; return 1; }
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
    local img=$1 md5=$2 name list n
    camsh_ok 'ls -1 /mnt/custom_firmware' || { ui_fail "cannot list /mnt/custom_firmware." ""; return 1; }
    list=$CAMSH_OUT
    n=$(printf '%s\n' "$list" | grep -c . || true)
    name=$(printf '%s\n' "$list" | grep -E '^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs$' | tail -1 || true)
    if [ "$n" = 1 ] && [ -n "$name" ]; then
        ft_remote_md5 "/mnt/custom_firmware/$name" || return 1
        if [ "$FT_RMD5" = "$md5" ]; then
            ui_say "  Image on the card: $name, md5 $md5 (same as this PC)."
            return 0
        fi
    fi
    ui_warn "the card does not carry exactly the image you want to flash (found: $(printf '%s' "$list" | tr '\n' ' '))."
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

# Lines of the flash output that are not worth showing.
FT_SKIP_RE='^[[:space:]]*$|^[[:space:]]*[0-9]+%'

# flash_telnet IMG MD5 LABEL: from the stock or maintenance telnet, with the card on /mnt.
flash_telnet() {
    local img=$1 md5=$2 label=$3 ft rc line sums
    ft=$(cam_flash_tool)
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD open the telnet shell, bind the card on /mnt if it is on /tmp/sd, check that exactly one image is in"
        ui_say "  /mnt/custom_firmware with the same md5 as this PC (pushing files over nc only after your yes), then run"
        ui_say "  'sh /mnt/install_with_flash_tool.sh', show its md5 line and its prompt, and ask YOU to type YES (relayed as typed)."
        ui_say "  After the write: dd if=/dev/mtd7 bs=64k | md5sum on the camera must equal the image md5. No reboot is ever sent."
        ui_confirm_typed "Write the camera flash now?" YES
        return 0
    fi
    [ -s "$ft" ] || { ui_fail "flash_tool not found: $ft" "run ./akam get-kit again (or ./akam build)."; return 1; }
    cam_session_open || return 1
    ft_find_card || { camsh_close; return 1; }
    ft_ensure_image "$img" "$md5" || { camsh_close; return 1; }
    ft_ensure_file "$ft" /mnt/flash_tool exec "flash_tool" || { camsh_close; return 1; }
    ft_ensure_file "$(cam_scripts_dir)/install_with_flash_tool.sh" /mnt/install_with_flash_tool.sh plain "install_with_flash_tool.sh" || { camsh_close; return 1; }
    ft_ensure_file "$(cam_scripts_dir)/flash_guard.sh" /mnt/flash_guard.sh plain "flash_guard.sh" || { camsh_close; return 1; }
    ui_blank
    ui_warn "$label writes the camera's flash. If the power is cut while it writes, only a UART cable can recover the camera."
    ui_warn "Keep the camera powered and do not unplug anything until this step says it is finished."
    ui_say "  Image md5 (this PC and the card): $md5"
    camsh_stream_set 1 "$FT_SKIP_RE"
    camsh_start 'sh /mnt/install_with_flash_tool.sh' || { camsh_stream_set 0; camsh_close; return 1; }
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
        ui_say "  The camera shows the same md5 as this PC. It now waits for YES."
        if ! { ui_require_tty "writing the camera flash" && ui_confirm_typed "Write the camera flash now?" YES; }; then
            ui_err "not confirmed: YES must be typed by you. Aborting; nothing was written."
            camsh_send_line "NO"
            camsh_finish 15 || true
            camsh_close
            return 1
        fi
        # The only place where text goes to the flash script's prompt: what the human just typed.
        camsh_send_line "YES" || { ui_fail "lost the camera session." "DO NOT power-cycle; check the camera."; return 1; }
        camsh_stream_set 1 "$FT_SKIP_RE"
        rc=0
        camsh_finish 900 || rc=$?
        camsh_stream_set 0
        if [ "$rc" -ne 0 ] || [ "$CAMSH_RC" != 0 ] || ! printf '%s\n' "$CAMSH_OUT" | grep -q 'completed successfully'; then
            ui_fail "the flash did not report success." "DO NOT power-cycle yet. Read the lines above; docs/akam_v1_flash_and_restore.md (Recovery)."
            camsh_close
            return 1
        fi
    fi
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

# flash_telnet_manual MD5 LABEL: manual mode: the user runs the script and types YES in their own window.
flash_telnet_manual() {
    ui_say "  MANUAL: in your telnet window run the command above, compare the md5 it prints with this one:"
    ui_say "      $1"
    ui_say "  If it matches, type YES yourself at its prompt. Wait until it says 'completed successfully'."
    ui_say "  Do NOT unplug anything. Then come back and paste its last lines."
    camsh_finish 900 || return 1
    if [ "$CAMSH_RC" != 0 ] || ! printf '%s\n' "$CAMSH_OUT" | grep -q 'completed successfully'; then
        ui_fail "the flash did not report success." "DO NOT power-cycle yet; docs/akam_v1_flash_and_restore.md (Recovery)."
        return 1
    fi
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
        ui_say "  '$CAM_UNTESTED_PHRASE' unless validated), then choose the route:"
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
    if [ -n "$app" ]; then cam_confirm_untested "$app" "Flash with untested firmware?" || return 1; fi
    cam_key
    route=telnet
    if [ "$(state_get camera_kind)" = akam ] || [ "$(state_get camera_telnet)" != 1 ]; then
        cam_ssh_setup
        if cam_ssh_ok; then
            ui_say "  Akam already runs and SSH works: the card is not needed."
            if ui_confirm "  Reflash over SSH (recommended)? [no = use the maintenance telnet and the card]"; then route=ssh; fi
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
cam_wait_port() { # port secs
    local p=$1 max=${2:-${AKAM_WAIT_SECS:-240}} t=0
    while [ "$t" -lt "$max" ]; do
        if net_port_open "$CAM_IP" "$p" 3; then return 0; fi
        t=$((t + 5))
        sleep 2
    done
    return 1
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
    printf '%s\n' "$out" | grep -E '^(PASS|FAIL|WARN|UNKNOWN)' | sed 's/^/    /'
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

cmd_verify() {
    step_need verify || return 1
    step_banner verify
    if [ "$UI_DRY" = 1 ]; then
        ui_say "  WOULD wait for the camera, check SSH with your key, copy and run akam_hwcheck.sh (expecting no FAIL),"
        ui_say "  check that the open ports are 22, 554 and 8091 (24 allowed while the card is in), then ask you to"
        ui_say "  unplug the power, take the card out and plug the power back in BY HAND, and repeat the checks:"
        ui_say "  telnet 24 and FTP 21 must then be closed. Only then is the step marked done."
        state_mark_done verify
        wipe_offer
        return 0
    fi
    cam_need_ip || return 1
    cam_key
    ui_say "  Waiting for the camera to answer on SSH (up to ${AKAM_WAIT_SECS:-240} seconds) ..."
    cam_wait_port "$(cam_port ssh)" || { ui_fail "the camera does not answer on SSH." "wait, check the address, see docs/akam_v1_flash_and_restore.md (Recovery). The step is NOT done."; return 1; }
    cam_ssh_setup
    cam_show_fingerprint
    cam_ssh_ok || { ui_fail "SSH with $CAM_KEY was refused." "is this the key you put on the flash card (--identity FILE)? The step is NOT done: do not remove the card."; return 1; }
    ui_good "  SSH with your key works."
    cam_hwcheck || { ui_fail "akam_hwcheck.sh reported a failure (lines above)." "do not remove the card yet; read docs/akam_v1_test_plan.md."; return 1; }
    cam_ports_check card || { ui_fail "unexpected port state." "do not remove the card yet."; return 1; }
    ui_good "  Checks passed with the card in."
    ui_blank
    ui_say "  Now the real test: without the card. By hand:"
    ui_say "    1. unplug the camera's USB-C power  2. take the microSD card out  3. plug the power back in."
    ui_prompt "  Press Enter when the camera is powered again (or q to stop):" "" || { ui_fail "no input." "run ./akam step verify again."; return 1; }
    if [ "$ANSWER" = q ]; then ui_fail "stopped." "run ./akam step verify again when ready."; return 1; fi
    cam_wait_port "$(cam_port ssh)" || { ui_fail "the camera does not answer on SSH without the card." "put the card back and look at docs/akam_v1_flash_and_restore.md (Recovery)."; return 1; }
    cam_ports_check closed || { ui_fail "port check failed without the card." "see docs/akam_v1_ssh_and_lockdown.md."; return 1; }
    cam_hwcheck || { ui_fail "akam_hwcheck.sh reported a failure without the card." ""; return 1; }
    state_mark_done verify
    ui_good "  Verified: SSH works, only 22, 554 and 8091 are open, telnet and FTP are off."
    wipe_offer
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
        ui_say "  maintenance telnet, and ask for a typed YES. Same gates as flash; no reboot is ever sent."
        flash_telnet dry-run dry-run Restoring || return 1
        return 0
    fi
    [ -n "$bk" ] && [ -f "$bk/mtdblock7.bin" ] || { ui_fail "no backup found." "pass --backup-dir DIR (it must hold mtdblock7.bin)."; return 1; }
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
        if ui_confirm "  Restore over SSH (no card needed)? [no = use the maintenance telnet and the card]"; then route=ssh; fi
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
