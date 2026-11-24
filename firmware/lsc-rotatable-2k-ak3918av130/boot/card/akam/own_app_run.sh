#!/bin/sh
# own_app_run.sh -- tiny supervisor of akamd (the Rotatable 2K own app) for lsc-rotatable-2k-ak3918av130.
# Started in the background by own_app_stub.sh once the stub is "healthy" (network up). Never run by hand on the
# camera, except for tests. It is a SEPARATE process: whatever happens here (a crash loop, a broken card file, a
# missing binary) cannot touch the stub's watchdog feeder or the Wi-Fi it set up. It only ever exits itself.
#
#   1  akam/bin/akamd is copied to /tmp (FAT exec quirks) and its sha256 must equal the sha256= of
#      akam/bin/akamd.BUILDINFO (and the copy is verified again): a mismatch or a missing hash tool = refuse.
#   2  options from the optional akam/akam.conf (key=value, # comments). Allowed keys and ranges:
#      main_kbps 64..8000 (default 2000), sub_kbps 32..1000 (default 800; the sub is 640x360 H.264 Main, QP 26..44; the 128 KiB sub frame cap holds an IDR up to ~1 Mbit/s), sub_res 640x360 (the only size this SoC supports, accepted for old confs; any other value is logged and ignored), gop 1..300 (default: the app's), port
#      1..65535 (default: the app's); the encoder of each stream (0 = main, 1 = sub, the defaults are unchanged): video0_kbps 64..8000 / video1_kbps 32..1000
#      (target bit rate; they win over main_kbps / sub_kbps), video<N>_max_kbps 0 (automatic, 1.25 x the target) or 64..12000 / 32..1500 (peak),
#      video<N>_fps 1..15, video<N>_min_qp and video<N>_max_qp 10..51 (min must not exceed max), video<N>_gop 1..300 and video<N>_rc vbr|cbr|avbr, video_live 0|1 (default 1: a change is tried on the running encoder before the re-open; 0 = always re-open; --video-live), passed as
#      --video<N>-kbps / --video<N>-max-kbps ... (the app checks the whole set; Home Assistant changes them at run time, see button-and-zones.md); detect 0..1 (person detection on the NPU, default 1), detect_hz 1..10 (default 5),
#      detect_threshold 500..950 (classify threshold, default 700), passed as -D/-H/-T; detect_head_threshold 300..950 (default 600: the score a head box needs, tracking rule 6; the model runs at the lower of it and detect_threshold) as --detect-head-thr; detect_age_window 0|1 (default 0: the frame age reference is a 60 s windowed minimum instead of the all-time minimum, experiment E3) as --detect-age-window; track_lag_ms -150..300 (default 50: the video latency of the pose at capture, see tracking.md) as --track-lag-ms; detect_debug 0|1 (default 0: per-inference log of every box, both classes, with scores) and detect_model_thr 300..950 (default off: experiment E1, the model's own threshold below detect_threshold; see tracking.md) as --detect-log / --detect-model-thr; snapshot 0..1 (JPEG snapshot when the
#      person goes ON, default 1) and snapshot_interval 5..300 (minimum seconds between two snapshots, default 10), passed as
#      --snapshot/--snapshot-interval. Day/night and image: night auto|day|night (default auto) and ir_led auto|off (default
#      auto) are the only WORDS accepted (exact match, passed as --night/--ir-led); image_brightness, image_contrast,
#      image_sharpness 0..100 (default: not set, the ISP conf file values stay) and mains_hz 50 or 60 (default 50) are
#      passed as --image-brightness/--image-contrast/--image-sharpness/--mains-hz; wdr 0|1 (default 1: WDR re-applied after every ISP mode switch, like the stock) is passed as --wdr; led auto|on|off (default on: the front blue LED stays as the system set it) and led_boot, led_nowifi, led_person, led_stream, led_night, led_idle (a pattern on | off | N:on_ms:off_ms:pause_ms; see hal-light.md) are passed as --led / --led-boot ...; onvif 0|1 (default 0: OFF; 1 starts ONVIF device + media + PTZ + WS-Discovery, only when the stream credentials exist) and onvif_port 1024..65535 (default 8000) are passed as --onvif / --onvif-port; audio 0|1 (default 0: OFF, the microphone is never opened and the streams carry no audio track; 1 adds an L16/8000 microphone track to both RTSP streams) is passed as --audio; ptz_cal auto|off (default auto: the motor calibrates at boot unless the scene check proves the head did not move; ptz_scene_check 0|1 (default 1) and ptz_scene_min 0.80..0.99 (default 0.90) are passed as --ptz-scene-check / --ptz-scene-min; track_invert_pan / track_invert_tilt 0|1 (default 1 / 0) as --track-invert-pan / --track-invert-tilt; off: never touched), ptz_park PAN:TILT (default 1275:300) and ptz_privacy 0|1 (default 0; 1: no video while parked) are passed as --ptz-cal/--ptz-park/--ptz-privacy. track 0|1 (default 0: person tracking off; 1 follows a person with the motor) is passed as --track; track_area_<name>=PMIN:PMAX:TMIN:TMAX (up to 4 named motor-step rectangles inside the soft range that limit the tracking; see tracking.md) as --track-area <name>=...; button_long_ms 1500..10000 (default 2000: how long the front button is held for park / home) as --button-long-ms; ptz_travel_budget_k 10..99999 (default 200: thousands of motor steps travelled since the last calibration after which no scene reference is saved, so the next cold boot calibrates) as --ptz-travel-budget-k; person tracking tuning: track_return home|stay (default home), track_return_delay_s 0..86400 (default 30, home mode: wait after the lost-person grace), track_grace_s 0..600 (default 3), track_band_pct 2..40 (default 6), track_tilt 0|1 (default 0: opt-in until the tilt direction is verified), track_lead_pct 0..100 (default 70), track_tilt_return_s 0..3600 (default 0 = off: the home return puts both axes back; in stay mode only, the tilt goes back to the home tilt this many seconds after the lost-person grace) as --track-tilt-return-s, track_debug 0..2 (default 0: 1 = one decision line per tick into the RAM file /tmp/akam2k_track.log, capped; 2 = also the raw recorder /tmp/akam2k_track.raw, see tracking.md) as --track-debug, track_head_pct 0..50 (default 15: tilt keeps the top of the person box this % below the frame top; 0 = centre of the box) track_gain_pan / track_gain_tilt 200..4000 (default 1000 / 700: motor steps per frame width / height of offset) and track_tilt_band_pct 2..40 (default 10: the vertical dead band, the head band is half of it) as --track-gain-pan / --track-gain-tilt / --track-tilt-band-pct, track_motion continuous|steps (default continuous: one ongoing motor move re-targeted without a stop; steps = timed nudges, the fallback) as --track-motion, track_zone_pct 0..90 (default 40: the central comfort zone, % of the frame width, 0 = the old targeting), track_zone_y_pct 0..90 (default 30) and track_zone_head_pct 0..90 (default 20) as --track-zone-pct / --track-zone-y-pct / --track-zone-head-pct, track_lease_ms 150..2000 (default 450: the continuous move stops by itself when it is not refreshed this long) as --track-lease-ms, track_tilt_back_s 0..600 (default 5: a lost person with the tilt more than 250 steps above home: the tilt goes back this long after, 0 = off) as --track-tilt-back-s, track_close_scan 0|1 (default 1: a close person lost with no direction evidence is searched up and down around the loss pose before the lost policy runs) with track_close_scan_up / track_close_scan_down 0..600 (default 250 / 300 steps) and track_close_scan_cooldown_s 0..600 (default 30: at most one scan per loss episode and per this long) as --track-close-scan / --track-close-scan-up / --track-close-scan-down / --track-close-scan-cooldown-s, track_lost_policy none | steps joined by commas hold:N backsweep exit local:W:D:N route:NAME[:N] home stay (default backsweep,exit,hold:30,home = the legacy return policy; see tracking.md) as --track-lost-policy, as --track-return / --track-return-delay-s / --track-grace-s / --track-band-pct / --track-tilt / --track-lead-pct / --track-head-pct (see tracking.md). (The detection zones are NOT a key here: the app reads the optional card file akam/zones
#      itself, see own-app-feasibility.md.) A bad line, an unknown key or an out-of-range value is logged and ignored
#      (the default stays): a typo never keeps the camera from streaming. Values are digits only, never evaluated.
#      The conf is re-read before every app (re)start, so a change applies when the app restarts.
#   3  the app runs with LD_LIBRARY_PATH=/appfs/lib (the camera's own vendor libs), stdout and stderr appended to
#      akam/akam.log. The log is size capped: above 256 KiB it is copied to akam.log.1 (one generation, so
#      512 KiB at most) and truncated in place, checked every few seconds and at every start.
#   4  restart on exit with exponential backoff (2 s, 4 s ... capped at 60 s; a run of 120 s or more resets it).
#      More than 5 exits within 10 minutes: give up, log it, and exit (the camera stays reachable by SSH).
#   5  SIGTERM/SIGINT: the app gets SIGTERM, up to 10 s to exit, then SIGKILL; then this script exits 0.
# The app never touches the hardware watchdog: the busybox feeder owned by the stub keeps feeding it.
# Switch off without removing anything: the empty flag file akam/no_app (checked by the stub).
#
# Paths are prefixed by $AKAM_ROOT (tests). Tunables (tests): AKAM_APP_UPTIME (file read instead of /proc/uptime), AKAM_APP_BACKOFF_MIN/MAX, AKAM_APP_STABLE,
# AKAM_APP_CRASH_MAX, AKAM_APP_CRASH_WINDOW (s), AKAM_APP_LOG_MAX (bytes), AKAM_APP_POLL (s), AKAM_APP_STOP_WAIT (s).

ROOT=${AKAM_ROOT:-}
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
AK=$SD/akam
# shellcheck disable=SC1091  # the helper lives on the card at run time
if ( . "$AK/common.sh" ) >/dev/null 2>&1; then
    # shellcheck disable=SC1091  # the helper lives on the card at run time
    . "$AK/common.sh"
else
    TMP=$ROOT/tmp
    log() { :; }
fi

BIN=$AK/bin/akamd
INFO=$AK/bin/akamd.BUILDINFO
CONF=$AK/akam.conf
# 0.1.x named the file akam2k.conf. Update from such an install: when akam.conf does not exist yet, the old file is
# copied once (the old one stays, so an old image can still be put back). When the copy is impossible (read-only
# card) the old file is read in place. The old log name akam2k.log is never read nor written: a leftover file is harmless.
if [ ! -e "$CONF" ] && [ -f "$AK/akam2k.conf" ]; then
    if cp "$AK/akam2k.conf" "$CONF.new" 2>/dev/null && mv -f "$CONF.new" "$CONF" 2>/dev/null; then
        log own_app "akam2k.conf migrated to akam.conf (the old file is kept)"
    else
        rm -f "$CONF.new" 2>/dev/null
        CONF=$AK/akam2k.conf
    fi
fi
APPLOG=$AK/akam.log
RUN=$TMP/akamd
LIBS=${AKAM_APP_LIBS:-$ROOT/appfs/lib}
BO_MIN=${AKAM_APP_BACKOFF_MIN:-2}
BO_MAX=${AKAM_APP_BACKOFF_MAX:-60}
STABLE=${AKAM_APP_STABLE:-120}
CRASH_MAX=${AKAM_APP_CRASH_MAX:-5}
CRASH_WINDOW=${AKAM_APP_CRASH_WINDOW:-600}
LOG_MAX=${AKAM_APP_LOG_MAX:-262144}
POLL=${AKAM_APP_POLL:-1}
STOP_WAIT=${AKAM_APP_STOP_WAIT:-10}
APP=""
SLP=""
STOPPING=0

# now: seconds on a MONOTONIC clock (/proc/uptime, first field, integer part). The camera boots in 1970 and the opt-in time
# sync (akam-sntp) steps the wall clock forward by ~1.8e9 s once: crash windows, "stable run" and backoff must not see
# that step. Falls back to date +%s only when /proc/uptime is unreadable (tests that fake the root).
UPTIME=${AKAM_APP_UPTIME:-$ROOT/proc/uptime}
now() {
    _u=""
    read -r _u _ <"$UPTIME" 2>/dev/null
    _u=${_u%%.*}
    case $_u in '' | *[!0-9]*) date +%s 2>/dev/null || echo 0 ;; *) echo "$_u" ;; esac
}

# motor_stop: the app is gone (it crashed, was killed or was stopped): a continuous motor move has no hardware dead-man, so ask the app binary for ONE stop request on the motor node
# (`akamd --ptz-stop`: open, stop, exit; no other request). Best effort, bounded by the binary's own exit, never blocks the restart.
motor_stop() {
    [ -x "$RUN" ] || return 0
    LD_LIBRARY_PATH=$LIBS "$RUN" --ptz-stop >/dev/null 2>&1 </dev/null
    log own_app "motor stop requested after the app ended"
}

# lost_policy_ok <text>: 0 when it is the lost-target chain syntax of track_logic.c (tl_lost_parse): none | up to 8 of hold:N backsweep exit local:W:D:N route:name[:N] home stay (stay last)
lost_policy_ok() {
    _lp=$1
    [ -n "$_lp" ] || return 1
    case $_lp in *[!a-z0-9_:,]*) return 1 ;; esac
    [ "$_lp" = none ] && return 0
    case $_lp in ,* | *, | *,,*) return 1 ;; esac
    _n=0
    _prev=""
    _oldifs=$IFS
    IFS=,
    # shellcheck disable=SC2086  # split on commas on purpose
    set -- $_lp
    IFS=$_oldifs
    [ "$#" -le 8 ] || return 1
    for _t in "$@"; do
        _n=$((_n + 1))
        [ "$_prev" != stay ] || return 1
        case $_t in
            backsweep | exit | home | stay) ;;
            hold:*) lp_num "${_t#hold:}" 1 3600 || return 1 ;;
            local:*:*:*)
                _r=${_t#local:}
                lp_num "${_r%%:*}" 40 1500 || return 1
                _r=${_r#*:}
                lp_num "${_r%%:*}" 2 20 || return 1
                lp_num "${_r#*:}" 1 600 || return 1
                ;;
            route:*)
                _r=${_t#route:}
                _nm=${_r%%:*}
                case $_nm in [a-z]*) ;; *) return 1 ;; esac
                case $_nm in *[!a-z0-9_]*) return 1 ;; esac
                [ "${#_nm}" -le 16 ] || return 1
                if [ "$_nm" != "$_r" ]; then lp_num "${_r#*:}" 1 3600 || return 1; fi
                ;;
            *) return 1 ;;
        esac
        _prev=${_t%%:*}
    done
    return 0
}
lp_num() { # lp_num <digits> <lo> <hi>
    case $1 in '' | *[!0-9]* | ??????*) return 1 ;; esac
    [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}

# stop_app: SIGTERM, up to STOP_WAIT seconds (polled every second), then SIGKILL.
stop_app() {
    [ -n "$APP" ] || return 0
    if kill -0 "$APP" 2>/dev/null; then
        kill -TERM "$APP" 2>/dev/null
        i=0
        while kill -0 "$APP" 2>/dev/null && [ "$i" -lt "$STOP_WAIT" ]; do
            sleep 1
            i=$((i + 1))
        done
        if kill -0 "$APP" 2>/dev/null; then
            log own_app "akamd did not stop within $STOP_WAIT s: SIGKILL"
            kill -KILL "$APP" 2>/dev/null
        fi
    fi
    wait "$APP" 2>/dev/null
    APP=""
    motor_stop
}
trap 'STOPPING=1; [ -z "$SLP" ] || kill "$SLP" 2>/dev/null; log own_app "supervisor got a stop signal: stopping akamd"; stop_app; log own_app "supervisor stopped"; exit 0' TERM INT

# is_sha256 <string>: 64 lowercase hex digits.
is_sha256() {
    case $1 in *[!0-9a-f]* | '') return 1 ;; esac
    [ "${#1}" -eq 64 ]
}

# sha_of <file>: the card's busybox first (the stock one may lack the applet), then a sha256sum on PATH.
sha_of() {
    h=$("$SD/busybox" sha256sum "$1" 2>/dev/null | cut -d' ' -f1)
    is_sha256 "$h" || h=$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1)
    is_sha256 "$h" || return 1
    echo "$h"
}

# prepare: verified copy of the app in /tmp. 0 = ready, 1 = refused (logged).
prepare() {
    [ -f "$BIN" ] || { log own_app "akamd: $BIN missing"; return 1; }
    want=$(sed -n 's/^sha256=//p' "$INFO" 2>/dev/null | head -n1)
    is_sha256 "$want" || { log own_app "akamd refused: no valid sha256= in $INFO"; return 1; }
    got=$(sha_of "$BIN") || { log own_app "akamd refused: cannot compute a sha256 (no applet)"; return 1; }
    [ "$got" = "$want" ] || { log own_app "akamd refused: sha256 of the card file does not match BUILDINFO"; return 1; }
    mkdir -p "$TMP" 2>/dev/null
    rm -f "$RUN"
    if ! { cp "$BIN" "$RUN" 2>/dev/null && chmod 755 "$RUN"; }; then
        log own_app "akamd refused: cannot copy it to $TMP"
        return 1
    fi
    got=$(sha_of "$RUN") || got=""
    if [ "$got" != "$want" ]; then
        log own_app "akamd refused: the copy in $TMP does not match BUILDINFO"
        rm -f "$RUN"
        return 1
    fi
    log own_app "akamd verified (sha256 $want), copied to $RUN"
    return 0
}

# read_conf: sets ARGS (a word list of validated "-x number" options) from akam.conf.
ARGS=""
read_conf() {
    ARGS=""
    NAREA=0
    [ -f "$CONF" ] || return 0
    n=0
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        line=${line%%#*}
        case $line in
            *=*) ;;
            *)
                [ -z "$(echo "$line" | tr -d ' \t\r')" ] || log own_app "akam.conf line $n ignored: not key=value"
                continue
                ;;
        esac
        k=$(echo "${line%%=*}" | tr -d ' \t\r')
        v=$(echo "${line#*=}" | tr -d ' \t\r')
        case $k in
            led | led_boot | led_nowifi | led_person | led_stream | led_night | led_idle)
                # the status LED: led = auto|on|off; the rules take a pattern: on | off | N:on_ms:off_ms:pause_ms (digits and
                # colons only, never evaluated; the app validates the numbers and keeps its default for a bad one)
                case $k:$v in
                    led:auto | led:on | led:off) ARGS="$ARGS --led $v" ;;
                    led:*) log own_app "akam.conf line $n ignored: led must be auto, on or off" ;;
                    *:on | *:off) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                    *:[0-9]*:[0-9]*:[0-9]*:[0-9]*)
                        case $v in
                            *[!0-9:]* | *:*:*:*:*) log own_app "akam.conf line $n ignored: $k is not a pattern" ;;
                            *) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                        esac
                        ;;
                    *) log own_app "akam.conf line $n ignored: $k must be on, off or N:on_ms:off_ms:pause_ms" ;;
                esac
                continue
                ;;
            ptz_drift_correct | ptz_boot_calibrate | ptz_tilt_rehome)
                # ptz_drift_correct = off|adjust|full (default full: what a confident drift shift may do), ptz_boot_calibrate = auto|always (default auto: the scene proof may skip the boot calibration)
                case $k:$v in
                    ptz_drift_correct:off | ptz_drift_correct:adjust | ptz_drift_correct:full) ARGS="$ARGS --ptz-drift-correct $v" ;;
                    ptz_boot_calibrate:auto | ptz_boot_calibrate:always) ARGS="$ARGS --ptz-boot-calibrate $v" ;;
                    ptz_tilt_rehome:off | ptz_tilt_rehome:idle) ARGS="$ARGS --ptz-tilt-rehome $v" ;;
                    ptz_tilt_rehome:*) log own_app "akam.conf line $n ignored: ptz_tilt_rehome must be off or idle" ;;
                    ptz_drift_correct:*) log own_app "akam.conf line $n ignored: ptz_drift_correct must be off, adjust or full" ;;
                    *) log own_app "akam.conf line $n ignored: ptz_boot_calibrate must be auto or always" ;;
                esac
                continue
                ;;
            ptz_cal | ptz_park)
                # PTZ: ptz_cal = auto|off (default auto: the motor calibrates at boot; off = never touched), ptz_park = PAN:TILT (digits
                # and one colon, the park position; the app range-checks it against the soft range)
                case $k:$v in
                    ptz_cal:auto | ptz_cal:off) ARGS="$ARGS --ptz-cal $v" ;;
                    ptz_cal:*) log own_app "akam.conf line $n ignored: ptz_cal must be auto or off" ;;
                    ptz_park:[0-9]*:[0-9]*)
                        case $v in
                            *[!0-9:]* | *:*:*) log own_app "akam.conf line $n ignored: ptz_park must be PAN:TILT" ;;
                            *) ARGS="$ARGS --ptz-park $v" ;;
                        esac
                        ;;
                    *) log own_app "akam.conf line $n ignored: ptz_park must be PAN:TILT" ;;
                esac
                continue
                ;;
            track_lost_policy)
                # person tracking, what to do when the person is lost: "none" or steps joined by commas: hold:N backsweep exit local:W:D:N route:NAME[:N] home stay (see tracking.md).
                # Validated here word by word (an invalid value would make the app refuse to start): the whole value or nothing.
                if lost_policy_ok "$v"; then ARGS="$ARGS --track-lost-policy $v"; else log own_app "akam.conf line $n ignored: track_lost_policy is not a valid chain"; fi
                continue
                ;;
            track_motion)
                # person tracking motion: continuous (one ongoing move, re-targeted without a stop) | steps (timed nudges, the fallback); an exact word, never evaluated
                case $v in
                    continuous | steps) ARGS="$ARGS --track-motion $v" ;;
                    *) log own_app "akam.conf line $n ignored: track_motion must be continuous or steps" ;;
                esac
                continue
                ;;
            track_return)
                # person tracking, what to do when the person is gone: home | stay (default stay); an exact word, never evaluated
                case $v in
                    home | stay) ARGS="$ARGS --track-return $v" ;;
                    *) log own_app "akam.conf line $n ignored: track_return must be home or stay" ;;
                esac
                continue
                ;;
            ptz_scene_min)
                # the scene check threshold: 0.80 .. 0.999 (the app parses it again and refuses anything else)
                case $v in
                    0.[8-9] | 0.[8-9][0-9] | 0.[8-9][0-9][0-9] | 1 | 1.0) ARGS="$ARGS --ptz-scene-min $v" ;;
                    *) log own_app "akam.conf line $n ignored: ptz_scene_min must be 0.80..0.99" ;;
                esac
                continue
                ;;
            track_area_*)
                # a tracking area: track_area_<name> = PMIN:PMAX:TMIN:TMAX (motor steps, digits and three colons, at most 4 areas; the
                # app checks the order and the soft range and drops a bad one). The name is [a-z][a-z0-9_]{0,15}; at most 4 areas are passed.
                an=${k#track_area_}
                case $an in
                    '' | [!a-z]* | *[!a-z0-9_]*) log own_app "akam.conf line $n ignored: $k has a bad area name" ;;
                    *)
                        if [ "${#an}" -gt 16 ]; then
                            log own_app "akam.conf line $n ignored: $k has a bad area name"
                        else
                            case $v in
                                [0-9]*:[0-9]*:[0-9]*:[0-9]*)
                                    case $v in
                                        *[!0-9:]* | *:*:*:*:* | *::* | *: | *[0-9][0-9][0-9][0-9][0-9][0-9]*) log own_app "akam.conf line $n ignored: $k must be PMIN:PMAX:TMIN:TMAX" ;;
                                        *)
                                            if [ "$NAREA" -ge 4 ]; then
                                                log own_app "akam.conf line $n ignored: at most 4 tracking areas"
                                            else
                                                NAREA=$((NAREA + 1))
                                                ARGS="$ARGS --track-area $an=$v"
                                            fi
                                            ;;
                                    esac
                                    ;;
                                *) log own_app "akam.conf line $n ignored: $k must be PMIN:PMAX:TMIN:TMAX" ;;
                            esac
                        fi
                        ;;
                esac
                continue
                ;;
            track_lag_ms) # the only signed number: -150..300 (a negative lag is allowed), matched by exact patterns, never evaluated
                case $v in
                    0 | [1-9] | [1-9][0-9] | [12][0-9][0-9] | 300 | -[1-9] | -[1-9][0-9] | -1[0-4][0-9] | -150) ARGS="$ARGS --track-lag-ms=$v" ;;
                    *) log own_app "akam.conf line $n ignored: track_lag_ms must be -150..300" ;;
                esac
                continue
                ;;
            video0_rc | video1_rc) # the encoder rate-control mode of a stream: an exact word, never evaluated
                case $v in
                    vbr | cbr | avbr) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                    *) log own_app "akam.conf line $n ignored: $k must be vbr, cbr or avbr" ;;
                esac
                continue
                ;;
            sub_res) # 640x360 is the only sub size (3DNR buffer / venc DMA pool of the 64 MB config, venc-live-params.md section 8): exact word, never evaluated
                case $v in
                    640x360) ;;
                    *) log own_app "akam.conf line $n ignored: sub_res $v unsupported on this SoC (sub max 640x480: 3DNR buffer / venc DMA pool)" ;;
                esac
                continue
                ;;
            night | ir_led) # the only keys that take a word: exact match against the allowed ones, never evaluated
                case $k:$v in
                    night:auto | night:day | night:night) ARGS="$ARGS --night $v" ;;
                    ir_led:auto | ir_led:off) ARGS="$ARGS --ir-led $v" ;;
                    night:*) log own_app "akam.conf line $n ignored: night must be auto, day or night" ;;
                    *) log own_app "akam.conf line $n ignored: ir_led must be auto or off" ;;
                esac
                continue
                ;;
        esac
        case $v in '' | *[!0-9]*)
            log own_app "akam.conf line $n ignored: $k needs a number"
            continue
            ;;
        esac
        v=$(echo "$v" | sed 's/^0*//')
        [ -n "$v" ] || v=0
        if [ "${#v}" -gt 5 ]; then
            log own_app "akam.conf line $n ignored: $k out of range"
            continue
        fi
        case $k in
            main_kbps) lo=64 hi=8000 opt=-m ;;
            sub_kbps) lo=32 hi=1000 opt=-s ;;
            gop) lo=1 hi=300 opt=-g ;;
            port) lo=1 hi=65535 opt=-p ;;
            detect) lo=0 hi=1 opt=-D ;;
            detect_hz) lo=1 hi=10 opt=-H ;;
            detect_threshold) lo=500 hi=950 opt=-T ;;
            detect_debug) lo=0 hi=1 opt=--detect-log ;;
            detect_head_threshold) lo=300 hi=950 opt=--detect-head-thr ;;
            detect_model_thr) lo=300 hi=950 opt=--detect-model-thr ;;
            snapshot) lo=0 hi=1 opt=--snapshot ;;
            snapshot_interval) lo=5 hi=300 opt=--snapshot-interval ;;
            image_brightness) lo=0 hi=100 opt=--image-brightness ;;
            image_contrast) lo=0 hi=100 opt=--image-contrast ;;
            image_sharpness) lo=0 hi=100 opt=--image-sharpness ;;
            mains_hz) lo=50 hi=60 opt=--mains-hz ;;
            wdr) lo=0 hi=1 opt=--wdr ;;
            audio) lo=0 hi=1 opt=--audio ;;
            ptz_privacy) lo=0 hi=1 opt=--ptz-privacy ;;
            onvif) lo=0 hi=1 opt=--onvif ;;
            onvif_port) lo=1024 hi=65535 opt=--onvif-port ;;
            track) lo=0 hi=1 opt=--track ;;
            ptz_travel_budget_k) lo=10 hi=99999 opt=--ptz-travel-budget-k ;;
            track_return_delay_s) lo=0 hi=86400 opt=--track-return-delay-s ;;
            track_grace_s) lo=0 hi=600 opt=--track-grace-s ;;
            track_band_pct) lo=2 hi=40 opt=--track-band-pct ;;
            track_tilt) lo=0 hi=1 opt=--track-tilt ;;
            track_head_pct) lo=0 hi=50 opt=--track-head-pct ;;
            track_gain_pan) lo=200 hi=4000 opt=--track-gain-pan ;;
            track_gain_tilt) lo=200 hi=4000 opt=--track-gain-tilt ;;
            track_tilt_band_pct) lo=2 hi=40 opt=--track-tilt-band-pct ;;
            track_debug) lo=0 hi=2 opt=--track-debug ;;
            track_tilt_return_s) lo=0 hi=3600 opt=--track-tilt-return-s ;;
            track_lead_pct) lo=0 hi=100 opt=--track-lead-pct ;;
            detect_age_window) lo=0 hi=1 opt=--detect-age-window ;;
            track_zone_pct) lo=0 hi=90 opt=--track-zone-pct ;;
            track_zone_y_pct) lo=0 hi=90 opt=--track-zone-y-pct ;;
            track_zone_head_pct) lo=0 hi=90 opt=--track-zone-head-pct ;;
            track_lease_ms) lo=150 hi=2000 opt=--track-lease-ms ;;
            track_tilt_back_s) lo=0 hi=600 opt=--track-tilt-back-s ;;
            track_close_scan) lo=0 hi=1 opt=--track-close-scan ;;
            track_close_scan_up) lo=0 hi=600 opt=--track-close-scan-up ;;
            track_close_scan_down) lo=0 hi=600 opt=--track-close-scan-down ;;
            track_close_scan_cooldown_s) lo=0 hi=600 opt=--track-close-scan-cooldown-s ;;
            track_invert_pan) lo=0 hi=1 opt=--track-invert-pan ;;
            track_invert_tilt) lo=0 hi=1 opt=--track-invert-tilt ;;
            ptz_scene_check) lo=0 hi=1 opt=--ptz-scene-check ;;
            ptz_drift_ref) lo=0 hi=1 opt=--ptz-drift-ref ;;
            ptz_tilt_rehome_gap_min) lo=1 hi=1440 opt=--ptz-tilt-rehome-gap-min ;;
            button_long_ms) lo=1500 hi=10000 opt=--button-long-ms ;;
            video_live) lo=0 hi=1 opt=--video-live ;;
            video0_kbps) lo=64 hi=8000 opt=--video0-kbps ;;
            video0_max_kbps) lo=0 hi=12000 opt=--video0-max-kbps ;;
            video1_kbps) lo=32 hi=1000 opt=--video1-kbps ;;
            video1_max_kbps) lo=0 hi=1500 opt=--video1-max-kbps ;;
            video0_fps | video1_fps) lo=1 hi=15 opt=--$(echo "$k" | tr _ -) ;;
            video0_min_qp | video0_max_qp | video1_min_qp | video1_max_qp) lo=10 hi=51 opt=--$(echo "$k" | tr _ -) ;;
            video0_gop | video1_gop) lo=1 hi=300 opt=--$(echo "$k" | tr _ -) ;;
            *)
                log own_app "akam.conf line $n ignored: unknown key"
                continue
                ;;
        esac
        if [ "$v" -lt "$lo" ] || [ "$v" -gt "$hi" ]; then
            log own_app "akam.conf line $n ignored: $k must be $lo..$hi"
            continue
        fi
        # a peak bit rate is 0 (automatic) or at least the lowest target of its stream
        if { [ "$k" = video0_max_kbps ] && [ "$v" -ne 0 ] && [ "$v" -lt 64 ]; } || { [ "$k" = video1_max_kbps ] && [ "$v" -ne 0 ] && [ "$v" -lt 32 ]; }; then
            log own_app "akam.conf line $n ignored: $k must be 0 (automatic) or at least the lowest target (64 main, 32 sub)"
            continue
        fi
        if [ "$k" = mains_hz ] && [ "$v" != 50 ] && [ "$v" != 60 ]; then
            log own_app "akam.conf line $n ignored: mains_hz must be 50 or 60"
            continue
        fi
        ARGS="$ARGS $opt $v"
    done <"$CONF"
}

# rotate_log: copy-truncate at LOG_MAX (the app appends, so truncating in place is safe).
rotate_log() {
    sz=$(wc -c <"$APPLOG" 2>/dev/null) || return 0
    if [ "${sz:-0}" -gt "$LOG_MAX" ] 2>/dev/null; then
        cp "$APPLOG" "$APPLOG.1" 2>/dev/null # best effort: a full card must not keep the log growing
        : >"$APPLOG"
    fi
}

# net_request: net.* changes of the control API (docs/porting/lsc-rotatable-2k/control-api.md). The app never spawns a process: it writes ONE
# request line into $TMP/akam_net.req ("hostname set NAME" | "wifi set country XX" | "wifi set band B"). Here the line is read once, the file is
# removed (that frees the app to queue the next one), the words are matched against the fixed grammar and handed to akam_net.sh, which
# validates again, persists, and runs the Wi-Fi change detached with its dead-man. Anything else is logged and dropped.
net_request() {
    req=$TMP/akam_net.req
    [ -f "$req" ] || return 0
    line=""
    IFS= read -r line <"$req"
    rm -f "$req"
    case $line in
    *[!A-Za-z0-9\ .-]*)
        log own_app "net request refused (unexpected characters)"
        return 0
        ;;
    esac
    # shellcheck disable=SC2086  # the line holds only letters, digits, space, dot and dash (checked above): plain word splitting
    set -- $line
    [ -f "$AK/akam_net.sh" ] || { log own_app "net request dropped: no akam_net.sh"; return 0; }
    if [ "$1" = hostname ] && [ "$2" = set ] && [ $# -eq 3 ]; then
        log own_app "net request: hostname set $3"
        sh "$AK/akam_net.sh" hostname set "$3" >/dev/null 2>&1
    elif [ "$1" = wifi ] && [ "$2" = set ] && { [ "$3" = country ] || [ "$3" = band ]; } && [ $# -eq 4 ]; then
        log own_app "net request: wifi set $3 $4"
        sh "$AK/akam_net.sh" wifi set "$3" "$4" >/dev/null 2>&1
    else
        log own_app "net request refused (grammar)"
    fi
    return 0
}

# --- cfg_request begin
# cfg_request: the user-file settings of the control API (cfgreg.h: audio.mic_enable, mqtt.*, onvif.*, rtsp.user / rtsp.password, ntp.servers). The app
# writes ONE request file $TMP/akam_cfg.req (mode 0600: it may hold a password), key=value lines, one PATCH. It is taken whole (renamed, so the app can
# queue the next one), every line is validated again here, and NOTHING is written unless every line is valid and consistent. Files are written only when
# their content changes (flash wear), atomically, mode 0600. In the card-less image the files live in /config/akam and are linked into the Akam tree
# (the modules read $AK/<file>); with a card they are the card's files. The password never appears in a command line or a log (only builtins and pipes);
# the result is one word in $TMP/akam_cfg.res: ok | refused KEY | failed.
CFGD=${AKAM_CONFIG:-$ROOT/config}/akam

# cfg_target FILE: where the file really lives
cfg_target() {
    if [ -L "$SD/busybox" ]; then echo "$CFGD/$1"; else echo "$AK/$1"; fi
}

# cfg_put FILE TMPFILE: the new content (a file made next to the target) replaces the old one only when it differs
cfg_put() {
    _t=$(cfg_target "$1")
    if [ -f "$_t" ] && [ "$(md5sum <"$_t")" = "$(md5sum <"$2")" ]; then
        rm -f "$2"
        return 0
    fi
    mv -f "$2" "$_t" || {
        rm -f "$2"
        return 1
    }
    chmod 600 "$_t" 2>/dev/null
    if [ -L "$SD/busybox" ] && [ ! -e "$AK/$1" ]; then ln -sf "$_t" "$AK/$1"; fi
    return 0
}

cfg_rm() {
    rm -f "$(cfg_target "$1")"
    if [ -L "$SD/busybox" ]; then rm -f "$AK/$1"; fi
}

cfg_new() { # a private empty file next to the target of $1; its name on stdout
    _d=$(cfg_target "$1")
    mkdir -p "${_d%/*}" 2>/dev/null
    (umask 077 && : >"${_d%/*}/.$1.new") && echo "${_d%/*}/.$1.new"
}

cfg_isuser() { # 1 to 32 of A-Z a-z 0-9 . _ @ + -, not a lone -
    case $1 in '' | - | *[!A-Za-z0-9._@+-]*) return 1 ;; esac
    [ ${#1} -le 32 ]
}

cfg_ishost() { # 1 to 63 of A-Z a-z 0-9 . -
    case $1 in '' | *[!A-Za-z0-9.-]*) return 1 ;; esac
    [ ${#1} -le 63 ]
}

cfg_isname() { # a NTP server: a host name or an IPv4 address (labels of letters, digits and inner hyphens)
    case $1 in '' | *[!A-Za-z0-9.-]*) return 1 ;; -* | .* | *..* | *. | *.-) return 1 ;; esac
    [ ${#1} -le 253 ]
}

cfg_isnum() { # $1 is a number from $2 to $3 without leading zero
    case $1 in '' | *[!0-9]* | 0?*) return 1 ;; esac
    [ ${#1} -le 5 ] && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]
}

# cfg_conf_set KEY VALUE: the line key=value of akam.conf replaced (or appended), the other lines untouched
cfg_conf_set() {
    _n=$(cfg_new akam.conf) || return 1
    _f=$(cfg_target akam.conf)
    # first write after an update from 0.1.x: start from the old akam2k.conf (this write creates akam.conf)
    [ -f "$_f" ] || { _o=$(cfg_target akam2k.conf); [ ! -f "$_o" ] || _f=$_o; }
    if [ -f "$_f" ]; then
        while IFS= read -r _l || [ -n "$_l" ]; do
            _s=$(echo "${_l%%#*}" | tr -d ' \t\r')
            case $_s in "$1"=*) ;; *) printf '%s\n' "$_l" >>"$_n" ;; esac
        done <"$_f"
    fi
    printf '%s=%s\n' "$1" "$2" >>"$_n"
    cfg_put akam.conf "$_n"
}

# cfg_mqtt_get KEY: the value of KEY in the current mqtt file
cfg_mqtt_get() {
    _v=
    _f=$(cfg_target mqtt)
    [ -f "$_f" ] || return 0
    while IFS= read -r _l || [ -n "$_l" ]; do
        case $_l in "$1"=*) _v=${_l#*=} ;; esac
    done <"$_f"
    echo "${_v:-}"
}

# cfg_apply FILE: 0 applied, 1 refused (cfg_why names the key), 2 failed to write
cfg_apply() {
    s_mic='' s_me='' s_mh='' s_mp='' s_mu='' s_mw='' s_oe='' s_op='' s_ru='' s_rw='' s_ns=''
    c_mic='' c_me='' c_mh='' c_mp='' c_mu='' c_mw='' c_oe='' c_op='' c_ru='' c_rw='' c_ns='' cfg_why=''
    while IFS= read -r _line || [ -n "$_line" ]; do
        _k=${_line%%=*}
        _v=${_line#*=}
        case $_k in
        audio.mic_enable) case $_v in 0 | 1) s_mic=1 c_mic=$_v ;; *) cfg_why=$_k ;; esac ;;
        mqtt.enabled) case $_v in 0 | 1) s_me=1 c_me=$_v ;; *) cfg_why=$_k ;; esac ;;
        mqtt.host) if [ -z "$_v" ] || cfg_ishost "$_v"; then s_mh=1 c_mh=$_v; else cfg_why=$_k; fi ;;
        mqtt.port) if cfg_isnum "$_v" 1 65535; then s_mp=1 c_mp=$_v; else cfg_why=$_k; fi ;;
        mqtt.user) if [ -z "$_v" ] || cfg_isuser "$_v"; then s_mu=1 c_mu=$_v; else cfg_why=$_k; fi ;;
        mqtt.password) case $_v in *[[:cntrl:]]*) cfg_why=$_k ;; *) if [ ${#_v} -le 128 ]; then s_mw=1 c_mw=$_v; else cfg_why=$_k; fi ;; esac ;;
        onvif.enabled) case $_v in 0 | 1) s_oe=1 c_oe=$_v ;; *) cfg_why=$_k ;; esac ;;
        onvif.port) if cfg_isnum "$_v" 1024 65535; then s_op=1 c_op=$_v; else cfg_why=$_k; fi ;;
        rtsp.user) if [ -z "$_v" ] || cfg_isuser "$_v"; then s_ru=1 c_ru=$_v; else cfg_why=$_k; fi ;;
        rtsp.password) case $_v in *[[:cntrl:]]*) cfg_why=$_k ;; *) if [ ${#_v} -ge 8 ] && [ ${#_v} -le 128 ]; then s_rw=1 c_rw=$_v; else cfg_why=$_k; fi ;; esac ;;
        ntp.servers)
            c_ns=
            _cnt=0
            _bad=
            for _w in $_v; do
                _cnt=$((_cnt + 1))
                cfg_isname "$_w" || _bad=1
            done
            if [ -z "$_bad" ] && [ "$_cnt" -le 3 ]; then s_ns=1 c_ns=$_v; else cfg_why=$_k; fi
            ;;
        *) cfg_why=$_k ;;
        esac
        [ -z "$cfg_why" ] || return 1
    done <"$1"
    _auth_file=$(cfg_target rtsp.conf)
    _cur_user=
    if [ -f "$_auth_file" ]; then
        IFS= read -r _al <"$_auth_file"
        _cur_user=${_al%%:*}
        case $_al in *:akam:????????????????????????????????) ;; *) _cur_user= ;; esac
    fi
    # consistency (the same rules as the registry of the app): all or nothing
    if [ -n "$s_rw" ]; then
        if [ -n "$s_ru" ]; then [ -n "$c_ru" ] || { cfg_why=rtsp.password; return 1; }; else [ -n "$_cur_user" ] || { cfg_why=rtsp.password; return 1; }; fi
    elif [ -n "$s_ru" ] && [ -n "$c_ru" ] && [ "$c_ru" != "$_cur_user" ]; then
        cfg_why=rtsp.user
        return 1 # another user needs its password: only the password makes the hash
    fi
    if [ -n "$s_oe" ] && [ "$c_oe" = 1 ]; then
        if [ -n "$s_ru" ]; then [ -n "$c_ru" ] || { cfg_why=onvif.enabled; return 1; }; else [ -n "$_cur_user" ] || { cfg_why=onvif.enabled; return 1; }; fi
    fi
    _fh=$c_mh
    [ -n "$s_mh" ] || _fh=$(cfg_mqtt_get host)
    _fu=$c_mu
    [ -n "$s_mu" ] || _fu=$(cfg_mqtt_get user)
    _fe=$c_me
    [ -n "$s_me" ] || _fe=$(cfg_mqtt_get enabled)
    if [ "$_fe" = 1 ] && [ -z "$_fh" ]; then
        cfg_why=mqtt.enabled
        return 1
    fi
    if [ -n "$s_mw" ] && [ -n "$c_mw" ] && [ -z "$_fu" ]; then
        cfg_why=mqtt.password
        return 1
    fi
    # everything is valid: write
    if [ -n "$s_mic" ]; then cfg_conf_set audio "$c_mic" || return 2; fi
    if [ -n "$s_oe" ]; then cfg_conf_set onvif "$c_oe" || return 2; fi
    if [ -n "$s_op" ]; then cfg_conf_set onvif_port "$c_op" || return 2; fi
    if [ -n "$s_ns" ]; then
        if [ -z "$c_ns" ]; then
            cfg_rm ntp
        else
            _n=$(cfg_new ntp) || return 2
            for _w in $c_ns; do printf 'server=%s\n' "$_w" >>"$_n"; done
            cfg_put ntp "$_n" || return 2
        fi
    fi
    if [ -n "$s_ru" ] || [ -n "$s_rw" ]; then
        if [ -n "$s_ru" ] && [ -z "$c_ru" ]; then
            cfg_rm rtsp.conf
        elif [ -n "$s_rw" ]; then
            _u=$c_ru
            [ -n "$s_ru" ] || _u=$_cur_user
            _h=$(printf '%s' "$_u:akam:$c_rw" | md5sum | cut -d' ' -f1)
            _n=$(cfg_new rtsp.conf) || return 2
            printf '%s:akam:%s\n' "$_u" "$_h" >"$_n"
            cfg_put rtsp.conf "$_n" || return 2
        fi
    fi
    if [ -n "$s_me$s_mh$s_mp$s_mu" ]; then
        _fp=$c_mp
        [ -n "$s_mp" ] || _fp=$(cfg_mqtt_get port)
        if [ -z "$_fh" ]; then
            cfg_rm mqtt
            cfg_rm mqtt_pass
        else
            _n=$(cfg_new mqtt) || return 2
            printf 'enabled=%s\nhost=%s\n' "${_fe:-0}" "$_fh" >>"$_n"
            [ -z "$_fp" ] || printf 'port=%s\n' "$_fp" >>"$_n"
            [ -z "$_fu" ] || printf 'user=%s\n' "$_fu" >>"$_n"
            _f=$(cfg_target mqtt)
            if [ -f "$_f" ]; then # the options the user set by hand (client_id, base_topic, discovery ...) are kept
                while IFS= read -r _l || [ -n "$_l" ]; do
                    case $_l in enabled=* | host=* | port=* | user=* | '') ;; *) printf '%s\n' "$_l" >>"$_n" ;; esac
                done <"$_f"
            fi
            cfg_put mqtt "$_n" || return 2
            if [ -z "$_fu" ]; then cfg_rm mqtt_pass; fi
        fi
    fi
    if [ -n "$s_mw" ] && [ -n "$_fh" ]; then
        if [ -z "$c_mw" ]; then
            cfg_rm mqtt_pass
        else
            _n=$(cfg_new mqtt_pass) || return 2
            printf '%s\n' "$c_mw" >"$_n"
            cfg_put mqtt_pass "$_n" || return 2
        fi
    fi
    return 0
}

cfg_request() {
    req=$TMP/akam_cfg.req
    [ -f "$req" ] || return 0
    mv -f "$req" "$req.work" 2>/dev/null || return 0
    cfg_apply "$req.work"
    _rc=$?
    rm -f "$req.work"
    case $_rc in
    0) echo ok >"$TMP/akam_cfg.res"; log own_app "cfg request applied" ;;
    1) echo "refused $cfg_why" >"$TMP/akam_cfg.res"; log own_app "cfg request refused ($cfg_why)" ;;
    *) echo failed >"$TMP/akam_cfg.res"; log own_app "cfg request failed (a file could not be written)" ;;
    esac
    return 0
}
# --- cfg_request end

log own_app "supervisor start"
prepare || {
    log own_app "supervisor: not starting the app"
    exit 1
}
WANT=$(sed -n 's/^sha256=//p' "$INFO" 2>/dev/null | head -n1)

bo=$BO_MIN
exits=""
while [ "$STOPPING" = 0 ]; do
    rotate_log
    # the /tmp copy is verified again before EVERY exec (restarts included): nothing runs that is not the hash
    if [ "$(sha_of "$RUN")" != "$WANT" ]; then
        log own_app "akamd refused: $RUN no longer matches BUILDINFO, supervisor exits"
        rm -f "$RUN"
        exit 1
    fi
    # the conf is re-read before EVERY (re)start: a change applies with an app restart, no supervisor restart needed
    read_conf
    log own_app "akamd options:${ARGS:- (defaults)}"
    t0=$(now)
    # shellcheck disable=SC2086  # ARGS is a validated word list of "-x number" pairs
    LD_LIBRARY_PATH=$LIBS "$RUN" $ARGS >>"$APPLOG" 2>&1 </dev/null &
    APP=$!
    log own_app "akamd started, pid $APP"
    # Under memory pressure the OOM killer should take the app, never the stub or the feeder.
    echo 500 >"$ROOT/proc/$APP/oom_score_adj" 2>/dev/null
    while kill -0 "$APP" 2>/dev/null; do
        sleep "$POLL" &
        SLP=$!
        wait "$SLP"
        SLP=""
        rotate_log
        net_request
        cfg_request
    done
    wait "$APP" 2>/dev/null
    rc=$?
    APP=""
    motor_stop
    t1=$(now)
    run=$((t1 - t0))
    log own_app "akamd exited with status $rc after $run s"
    # crash accounting: the exits that fall inside the window
    keep=""
    cnt=0
    for e in $exits $t1; do
        if [ $((t1 - e)) -lt "$CRASH_WINDOW" ]; then
            keep="$keep $e"
            cnt=$((cnt + 1))
        fi
    done
    exits=$keep
    if [ "$cnt" -gt "$CRASH_MAX" ]; then
        log own_app "akamd exited $cnt times within $CRASH_WINDOW s: giving up (SSH and the network are unaffected; reboot to try again)"
        exit 0
    fi
    [ "$run" -lt "$STABLE" ] || bo=$BO_MIN
    log own_app "restarting akamd in $bo s"
    sleep "$bo" &
    SLP=$!
    wait "$SLP"
    SLP=""
    bo=$((bo * 2))
    [ "$bo" -le "$BO_MAX" ] || bo=$BO_MAX
done
