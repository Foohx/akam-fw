#!/bin/sh
# enable_local.sh -- local RTSP + ONVIF of the stock app (yy_cam) on the LSC Rotatable 2K, and a report of
# what the camera actually exposes.
#
# Usage: enable_local.sh apply | verify
#   apply    BEFORE the stock app starts (called by akam_boot.sh). Only with the card flag
#            akam/enable_param_conf: sets, in /config/param.conf, [rtsp] rtsp_on_off=1 and
#            [onvif] onvif_on_off=1. WARNING: this writes the jffs2 CONFIG partition (jffs2 mtd partition), not firmware.
#            Safeguards: the original file is copied to the card first (akam/backup/param.conf.orig, once,
#            verified with cmp, and the write is skipped if the backup fails); the edit is skipped when the
#            values are already set (idempotent); the new file is built next to the old one and moved into
#            place. Without the flag nothing is written, ever.
#   verify   AFTER the hand-off (background): waits (bounded) for the local ports, then logs what it found:
#            listening TCP ports and the relevant param.conf values. This is the evidence for the hardware
#            validation; it changes nothing.
#   (undo of "apply": restore_param.sh, next to this script)
#
# The DP path (Tuya LAN DP 237 = true at runtime, no write at all) needs the Tuya local key and a LAN client:
# it is NOT done on the camera. Host-side tooling for it lives in ../tools (later).
#
# UNVERIFIED (PLAN section 9): every fact below. They come from a third-party project and from strings of the
# stock binary, and must be confirmed on the live camera before they go into a spec:
#   - the file is /config/param.conf and yy_cam reads it at start
#   - section/key names [rtsp] rtsp_on_off and [onvif] onvif_on_off, value 1 = on
#   - ports RTSP 8554, ONVIF 5000, Tuya LAN 6668
#
# Paths prefixed by $AKAM_ROOT (see common.sh). $AKAM_PARAM_CONF overrides the param.conf path,
# $AKAM_PROC_NET_TCP the /proc/net/tcp path, $AKAM_VERIFY_TRIES / $AKAM_VERIFY_SLEEP the bounded wait.

HERE=${0%/*}
[ -r "$HERE/common.sh" ] || HERE=${AKAM_SD:-${AKAM_ROOT:-}/mnt/sdcard}/akam
# shellcheck disable=SC1091  # the helper lives on the card at run time
. "$HERE/common.sh"

PARAM=${AKAM_PARAM_CONF:-$ROOT/config/param.conf} # UNVERIFIED (PLAN section 9)
BACKUP_DIR=$AK/backup
BACKUP=$BACKUP_DIR/param.conf.orig
NETTCP=${AKAM_PROC_NET_TCP:-$ROOT/proc/net/tcp}
RTSP_PORT=8554  # UNVERIFIED (PLAN section 9)
ONVIF_PORT=5000 # UNVERIFIED (PLAN section 9)
TUYA_PORT=6668  # UNVERIFIED (PLAN section 9)
CR=$(printf '\r')

# trim <text>: sets _T to the text without the blanks around it. No subshell, no fork: apply runs BEFORE the
# hand-off to start.sh, so it must stay fast even on a big file (watchdog).
trim() {
    _T=$1
    _T=${_T#"${_T%%[! ]*}"}
    _T=${_T%"${_T##*[! ]}"}
}

# ini_get <file> <section> <key>: prints the value ("" when absent).
ini_get() {
    _in=0
    while IFS= read -r _l || [ -n "$_l" ]; do
        _l=${_l%"$CR"}
        case $_l in
            '['*']'*)
                if [ "$_l" = "[$2]" ]; then _in=1; else _in=0; fi
                ;;
            *=*)
                if [ "$_in" = 1 ]; then
                    trim "${_l%%=*}"
                    if [ "$_T" = "$3" ]; then
                        trim "${_l#*=}"
                        printf '%s' "$_T"
                        return 0
                    fi
                fi
                ;;
        esac
    done <"$1"
    return 0
}

# ini_set <file> <section> <key> <value>: prints the file with the key set (every copy of it in the section
# is replaced; the key is appended to the section, and the section to the file, when missing).
ini_set() {
    _in=0
    _done=0
    _seen=0
    while IFS= read -r _l || [ -n "$_l" ]; do
        _c=${_l%"$CR"}
        case $_c in
            '['*']'*)
                if [ "$_in" = 1 ] && [ "$_done" = 0 ]; then
                    printf '%s=%s\n' "$3" "$4"
                    _done=1
                fi
                if [ "$_c" = "[$2]" ]; then
                    _in=1
                    _seen=1
                else
                    _in=0
                fi
                ;;
            *=*)
                if [ "$_in" = 1 ]; then
                    trim "${_c%%=*}"
                    if [ "$_T" = "$3" ]; then
                        if [ "$_done" = 0 ]; then printf '%s=%s\n' "$3" "$4"; fi
                        _done=1
                        continue
                    fi
                fi
                ;;
        esac
        printf '%s\n' "$_l"
    done <"$1"
    if [ "$_seen" = 0 ]; then
        printf '[%s]\n%s=%s\n' "$2" "$3" "$4"
    elif [ "$_done" = 0 ]; then
        printf '%s=%s\n' "$3" "$4"
    fi
}

# listening_ports: TCP ports in state LISTEN (0A) from /proc/net/tcp, as decimal numbers, one per line.
listening_ports() {
    [ -r "$NETTCP" ] || return 1
    while read -r _sl _local _rem _st _rest; do
        [ "$_st" = 0A ] || continue
        printf '%d\n' "0x${_local##*:}"
    done <"$NETTCP"
}

port_open() {
    listening_ports | grep -qx "$1"
}

log_param_values() {
    if [ ! -r "$PARAM" ]; then
        log local "param.conf not found at $PARAM"
        return 0
    fi
    log local "param.conf [rtsp] rtsp_on_off='$(ini_get "$PARAM" rtsp "rtsp_on_off")' [onvif] onvif_on_off='$(ini_get "$PARAM" onvif "onvif_on_off")'"
}

# log_ptz_state: read-only evidence for the PTZ investigation (docs/porting/lsc-rotatable-2k/ptz-findings.md).
# Logs the [ptz] values of param.conf and which processes hold the motor nodes open.
# UNVERIFIED (PLAN section 9): the stock app opens the motor node named tymotor (strings of the stock binary).
log_ptz_state() {
    if [ -r "$PARAM" ]; then
        _in=0
        _vals=""
        while IFS= read -r _l || [ -n "$_l" ]; do
            _l=${_l%"$CR"}
            case $_l in
                '['*']'*)
                    if [ "$_l" = "[ptz]" ]; then _in=1; else _in=0; fi
                    ;;
                *=*) [ "$_in" = 1 ] && _vals="$_vals $_l" ;;
            esac
        done <"$PARAM"
        log local "param.conf [ptz]:${_vals:- (section absent)}"
    fi
    # Names only (no device path is built or opened here): which processes hold a motor node open.
    # One ls -l over all fd links (a single process), parsed with shell builtins only: no fork per fd.
    _h_t=""
    _h_m=""
    while IFS= read -r _l; do
        case $_l in
            *" -> "*) ;;
            *) continue ;;
        esac
        _tgt=${_l#* -> }
        _p=${_l%% -> *}
        _p=${_p##*/proc/}
        _p=${_p%%/*}
        case $_p in '' | *[!0-9]*) continue ;; esac
        case $_tgt in
            *tymotor) _h_t="$_h_t $_p" ;;
            *ms41929) _h_m="$_h_m $_p" ;;
        esac
    done <<EOF_LS
$(ls -l "$ROOT"/proc/[0-9]*/fd/* 2>/dev/null)
EOF_LS
    log local "motor node tymotor open by pid:${_h_t:- none}"
    log local "motor node ms41929 open by pid:${_h_m:- none}"
    if [ -r "$ROOT/proc/devices" ]; then
        _m=$(grep -i 'motor\|ms41929' "$ROOT/proc/devices" 2>/dev/null | tr '\n' ';')
        log local "proc/devices motor entries: ${_m:-none}"
    fi
    return 0
}

do_apply() {
    if ! flag enable_param_conf; then
        log local "param.conf edit not enabled (no flag akam/enable_param_conf): nothing is written"
        return 0
    fi
    if [ ! -r "$PARAM" ]; then
        log local "param.conf edit requested but $PARAM is missing: skipped"
        return 0
    fi
    _r=$(ini_get "$PARAM" rtsp rtsp_on_off)
    _o=$(ini_get "$PARAM" onvif onvif_on_off)
    if [ "$_r" = 1 ] && [ "$_o" = 1 ]; then
        log local "param.conf already has rtsp_on_off=1 and onvif_on_off=1: not written"
        return 0
    fi
    mkdir -p "$BACKUP_DIR" 2>/dev/null
    if [ ! -f "$BACKUP" ]; then
        if cp "$PARAM" "$BACKUP" 2>/dev/null && same "$PARAM" "$BACKUP"; then
            log local "param.conf backed up to $BACKUP"
        else
            rm -f "$BACKUP"
            log local "WARNING: cannot back up param.conf to the card: NOT writing the config partition"
            return 0
        fi
    fi
    _new=$PARAM.akam.new
    if ini_set "$PARAM" rtsp rtsp_on_off 1 >"$_new.1" 2>/dev/null &&
        ini_set "$_new.1" onvif onvif_on_off 1 >"$_new" 2>/dev/null &&
        [ -s "$_new" ] &&
        [ "$(wc -l <"$_new")" -ge "$(wc -l <"$PARAM")" ] &&
        [ "$(ini_get "$_new" rtsp rtsp_on_off)" = 1 ] &&
        [ "$(ini_get "$_new" onvif onvif_on_off)" = 1 ]; then
        rm -f "$_new.1"
        sync
        if mv -f "$_new" "$PARAM"; then
            sync
            log local "param.conf updated: [rtsp] rtsp_on_off=1 [onvif] onvif_on_off=1 (config partition written)"
        else
            rm -f "$_new"
            log local "WARNING: could not move the new param.conf into place"
        fi
    else
        rm -f "$_new" "$_new.1"
        log local "WARNING: could not build the new param.conf: left untouched"
    fi
    return 0
}

# log_ai_state: read-only evidence for the AI detection investigation (docs/porting/lsc-rotatable-2k/npu-findings.md).
# Logs the AI keys of param.conf ([dsp] ivs_*, [alg] ak_av130_*), which stock log-switch files exist, whether the NPU
# module is loaded, who holds the npu node, and the CPU ticks of the stock detector threads (named Y_ALG_*): two verify
# runs while somebody walks in front of the camera show whether the detector works. Nothing is written.
# UNVERIFIED (PLAN section 9): the names come from the strings of the stock binary.
log_ai_state() {
    if [ -r "$PARAM" ]; then
        _sec=""
        _vals=""
        while IFS= read -r _l || [ -n "$_l" ]; do
            _l=${_l%"$CR"}
            case $_l in
                '['*']'*)
                    _sec=${_l#\[}
                    _sec=${_sec%%\]*}
                    ;;
                *=*)
                    case "$_sec:${_l%%=*}" in
                        dsp:ivs_* | alg:ak_av130_*) _vals="$_vals $_sec.$_l" ;;
                    esac
                    ;;
            esac
        done <"$PARAM"
        log local "param.conf AI keys:${_vals:- none (stock defaults apply)}"
    fi
    _sw=""
    for _f in "$SD/yy_sdcard_log" "$SD/yy_log_debug" "$ROOT/config/yy_log_debug"; do
        if [ -e "$_f" ]; then _sw="$_sw ${_f#"$ROOT"}:present"; else _sw="$_sw ${_f#"$ROOT"}:absent"; fi
    done
    log local "stock log switches:$_sw"
    if [ -r "$ROOT/proc/modules" ]; then
        _m=$(grep '^ak_npu ' "$ROOT/proc/modules" 2>/dev/null | head -n 1)
        log local "npu module: ${_m:-not loaded}"
    fi
    _h=""
    while IFS= read -r _l; do
        case $_l in
            *" -> "*) ;;
            *) continue ;;
        esac
        _tgt=${_l#* -> }
        _p=${_l%% -> *}
        _p=${_p##*/proc/}
        _p=${_p%%/*}
        case $_p in '' | *[!0-9]*) continue ;; esac
        case $_tgt in *npu) _h="$_h $_p" ;; esac
    done <<EOF_NPU
$(ls -l "$ROOT"/proc/[0-9]*/fd/* 2>/dev/null)
EOF_NPU
    log local "npu node open by pid:${_h:- none}"
    _t=""
    for _c in "$ROOT"/proc/[0-9]*/task/[0-9]*/comm; do
        [ -r "$_c" ] || continue
        read -r _name <"$_c" || continue
        case $_name in Y_ALG_*) ;; *) continue ;; esac
        _d=${_c%/comm}
        read -r _st <"$_d/stat" || continue
        _st=${_st#*) }
        # shellcheck disable=SC2086  # word splitting of the stat fields is intended
        set -- $_st
        _tid=${_d##*/}
        _t="$_t $_name(tid $_tid):utime=${12:-?} stime=${13:-?}"
    done
    log local "AI detector threads (cpu ticks):${_t:- none (Y_ALG_* thread not found)}"
    return 0
}

do_verify() {
    _n=0
    _tries=${AKAM_VERIFY_TRIES:-90}
    while [ "$_n" -lt "$_tries" ]; do
        if port_open "$RTSP_PORT" || port_open "$ONVIF_PORT"; then break; fi
        sleep "${AKAM_VERIFY_SLEEP:-2}"
        _n=$((_n + 1))
    done
    _ports=""
    for _p in $(listening_ports 2>/dev/null); do _ports="$_ports $_p"; done
    log local "listening TCP ports:${_ports:- none (or /proc/net/tcp unreadable)}"
    for _p in "$RTSP_PORT:rtsp" "$ONVIF_PORT:onvif" "$TUYA_PORT:tuya-lan" 22:ssh 23:telnet; do
        if port_open "${_p%%:*}"; then
            log local "port ${_p%%:*} (${_p#*:}): open"
        else
            log local "port ${_p%%:*} (${_p#*:}): closed"
        fi
    done
    log_param_values
    log_ptz_state
    log_ai_state
    return 0
}

case ${1:-} in
    apply) do_apply ;;
    verify) do_verify ;;
    *)
        echo "usage: $0 apply|verify" >&2
        exit 2
        ;;
esac
