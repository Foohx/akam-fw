#!/bin/sh
# own_app_run.sh -- tiny supervisor of akam2k_rtsp (the Rotatable 2K own app) for lsc-rotatable-2k-ak3918av130.
# Started in the background by own_app_stub.sh once the stub is "healthy" (network up). Never run by hand on the
# camera, except for tests. It is a SEPARATE process: whatever happens here (a crash loop, a broken card file, a
# missing binary) cannot touch the stub's watchdog feeder or the Wi-Fi it set up. It only ever exits itself.
#
#   1  akam/bin/akam2k_rtsp is copied to /tmp (FAT exec quirks) and its sha256 must equal the sha256= of
#      akam/bin/akam2k_rtsp.BUILDINFO (and the copy is verified again): a mismatch or a missing hash tool = refuse.
#   2  options from the optional akam/akam2k.conf (key=value, # comments). Allowed keys and ranges:
#      main_kbps 64..8000 (default 2000), sub_kbps 32..1000 (default 512; the 128 KiB sub frame cap holds an IDR up to ~1 Mbit/s), gop 1..300 (default: the app's), port
#      1..65535 (default: the app's); the encoder of each stream (0 = main, 1 = sub, the defaults are unchanged): video0_kbps 64..8000 / video1_kbps 32..1000
#      (target bit rate; they win over main_kbps / sub_kbps), video<N>_max_kbps 0 (automatic, 1.25 x the target) or 64..12000 / 32..1500 (peak),
#      video<N>_fps 1..15, video<N>_min_qp and video<N>_max_qp 10..51 (min must not exceed max), video<N>_gop 1..300 and video<N>_rc vbr|cbr|avbr, video_live 0|1 (default 1: a change is tried on the running encoder before the re-open; 0 = always re-open; --video-live), passed as
#      --video<N>-kbps / --video<N>-max-kbps ... (the app checks the whole set; Home Assistant changes them at run time, see button-and-zones.md); detect 0..1 (person detection on the NPU, default 1), detect_hz 1..10 (default 5),
#      detect_threshold 500..950 (classify threshold, default 700), passed as -D/-H/-T; snapshot 0..1 (JPEG snapshot when the
#      person goes ON, default 1) and snapshot_interval 5..300 (minimum seconds between two snapshots, default 10), passed as
#      --snapshot/--snapshot-interval. Day/night and image: night auto|day|night (default auto) and ir_led auto|off (default
#      auto) are the only WORDS accepted (exact match, passed as --night/--ir-led); image_brightness, image_contrast,
#      image_sharpness 0..100 (default: not set, the ISP conf file values stay) and mains_hz 50 or 60 (default 50) are
#      passed as --image-brightness/--image-contrast/--image-sharpness/--mains-hz; wdr 0|1 (default 1: WDR re-applied after every ISP mode switch, like the stock) is passed as --wdr; led auto|on|off (default on: the front blue LED stays as the system set it) and led_boot, led_nowifi, led_person, led_stream, led_night, led_idle (a pattern on | off | N:on_ms:off_ms:pause_ms; see hal-light.md) are passed as --led / --led-boot ...; audio 0|1 (default 0: OFF, the microphone is never opened and the streams carry no audio track; 1 adds an L16/8000 microphone track to both RTSP streams) is passed as --audio; ptz_cal auto|off (default auto: the motor calibrates at boot unless the scene check proves the head did not move; ptz_scene_check 0|1 (default 1) and ptz_scene_min 0.80..0.99 (default 0.90) are passed as --ptz-scene-check / --ptz-scene-min; track_invert_pan / track_invert_tilt 0|1 (default 1 / 0) as --track-invert-pan / --track-invert-tilt; off: never touched), ptz_park PAN:TILT (default 1275:300) and ptz_privacy 0|1 (default 0; 1: no video while parked) are passed as --ptz-cal/--ptz-park/--ptz-privacy. track 0|1 (default 0: person tracking off; 1 follows a person with the motor) is passed as --track; track_area_<name>=PMIN:PMAX:TMIN:TMAX (up to 4 named motor-step rectangles inside the soft range that limit the tracking; see tracking.md) as --track-area <name>=...; button_long_ms 1500..10000 (default 2000: how long the front button is held for park / home) as --button-long-ms. (The detection zones are NOT a key here: the app reads the optional card file akam/zones
#      itself, see own-app-feasibility.md.) A bad line, an unknown key or an out-of-range value is logged and ignored
#      (the default stays): a typo never keeps the camera from streaming. Values are digits only, never evaluated.
#   3  the app runs with LD_LIBRARY_PATH=/appfs/lib (the camera's own vendor libs), stdout and stderr appended to
#      akam/akam2k.log. The log is size capped: above 256 KiB it is copied to akam2k.log.1 (one generation, so
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

BIN=$AK/bin/akam2k_rtsp
INFO=$AK/bin/akam2k_rtsp.BUILDINFO
CONF=$AK/akam2k.conf
APPLOG=$AK/akam2k.log
RUN=$TMP/akam2k_rtsp
LIBS=${AKAM_APP_LIBS:-$ROOT/appfs/lib}
BO_MIN=${AKAM_APP_BACKOFF_MIN:-2}
BO_MAX=${AKAM_APP_BACKOFF_MAX:-60}
STABLE=${AKAM_APP_STABLE:-120}
CRASH_MAX=${AKAM_APP_CRASH_MAX:-5}
CRASH_WINDOW=${AKAM_APP_CRASH_WINDOW:-600}
LOG_MAX=${AKAM_APP_LOG_MAX:-262144}
POLL=${AKAM_APP_POLL:-5}
STOP_WAIT=${AKAM_APP_STOP_WAIT:-10}
APP=""
SLP=""
STOPPING=0

# now: seconds on a MONOTONIC clock (/proc/uptime, first field, integer part). The camera boots in 1970 and the opt-in time
# sync (akam2k_sntp) steps the wall clock forward by ~1.8e9 s once: crash windows, "stable run" and backoff must not see
# that step. Falls back to date +%s only when /proc/uptime is unreadable (tests that fake the root).
UPTIME=${AKAM_APP_UPTIME:-$ROOT/proc/uptime}
now() {
    _u=""
    read -r _u _ <"$UPTIME" 2>/dev/null
    _u=${_u%%.*}
    case $_u in '' | *[!0-9]*) date +%s 2>/dev/null || echo 0 ;; *) echo "$_u" ;; esac
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
            log own_app "akam2k_rtsp did not stop within $STOP_WAIT s: SIGKILL"
            kill -KILL "$APP" 2>/dev/null
        fi
    fi
    wait "$APP" 2>/dev/null
    APP=""
}
trap 'STOPPING=1; [ -z "$SLP" ] || kill "$SLP" 2>/dev/null; log own_app "supervisor got a stop signal: stopping akam2k_rtsp"; stop_app; log own_app "supervisor stopped"; exit 0' TERM INT

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
    [ -f "$BIN" ] || { log own_app "akam2k_rtsp: $BIN missing"; return 1; }
    want=$(sed -n 's/^sha256=//p' "$INFO" 2>/dev/null | head -n1)
    is_sha256 "$want" || { log own_app "akam2k_rtsp refused: no valid sha256= in $INFO"; return 1; }
    got=$(sha_of "$BIN") || { log own_app "akam2k_rtsp refused: cannot compute a sha256 (no applet)"; return 1; }
    [ "$got" = "$want" ] || { log own_app "akam2k_rtsp refused: sha256 of the card file does not match BUILDINFO"; return 1; }
    mkdir -p "$TMP" 2>/dev/null
    rm -f "$RUN"
    if ! { cp "$BIN" "$RUN" 2>/dev/null && chmod 755 "$RUN"; }; then
        log own_app "akam2k_rtsp refused: cannot copy it to $TMP"
        return 1
    fi
    got=$(sha_of "$RUN") || got=""
    if [ "$got" != "$want" ]; then
        log own_app "akam2k_rtsp refused: the copy in $TMP does not match BUILDINFO"
        rm -f "$RUN"
        return 1
    fi
    log own_app "akam2k_rtsp verified (sha256 $want), copied to $RUN"
    return 0
}

# read_conf: sets ARGS (a word list of validated "-x number" options) from akam2k.conf.
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
                [ -z "$(echo "$line" | tr -d ' \t\r')" ] || log own_app "akam2k.conf line $n ignored: not key=value"
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
                    led:*) log own_app "akam2k.conf line $n ignored: led must be auto, on or off" ;;
                    *:on | *:off) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                    *:[0-9]*:[0-9]*:[0-9]*:[0-9]*)
                        case $v in
                            *[!0-9:]* | *:*:*:*:*) log own_app "akam2k.conf line $n ignored: $k is not a pattern" ;;
                            *) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                        esac
                        ;;
                    *) log own_app "akam2k.conf line $n ignored: $k must be on, off or N:on_ms:off_ms:pause_ms" ;;
                esac
                continue
                ;;
            ptz_cal | ptz_park)
                # PTZ: ptz_cal = auto|off (default auto: the motor calibrates at boot; off = never touched), ptz_park = PAN:TILT (digits
                # and one colon, the park position; the app range-checks it against the soft range)
                case $k:$v in
                    ptz_cal:auto | ptz_cal:off) ARGS="$ARGS --ptz-cal $v" ;;
                    ptz_cal:*) log own_app "akam2k.conf line $n ignored: ptz_cal must be auto or off" ;;
                    ptz_park:[0-9]*:[0-9]*)
                        case $v in
                            *[!0-9:]* | *:*:*) log own_app "akam2k.conf line $n ignored: ptz_park must be PAN:TILT" ;;
                            *) ARGS="$ARGS --ptz-park $v" ;;
                        esac
                        ;;
                    *) log own_app "akam2k.conf line $n ignored: ptz_park must be PAN:TILT" ;;
                esac
                continue
                ;;
            ptz_scene_min)
                # the scene check threshold: 0.80 .. 0.999 (the app parses it again and refuses anything else)
                case $v in
                    0.[8-9] | 0.[8-9][0-9] | 0.[8-9][0-9][0-9] | 1 | 1.0) ARGS="$ARGS --ptz-scene-min $v" ;;
                    *) log own_app "akam2k.conf line $n ignored: ptz_scene_min must be 0.80..0.99" ;;
                esac
                continue
                ;;
            track_area_*)
                # a tracking area: track_area_<name> = PMIN:PMAX:TMIN:TMAX (motor steps, digits and three colons, at most 4 areas; the
                # app checks the order and the soft range and drops a bad one). The name is [a-z][a-z0-9_]{0,15}; at most 4 areas are passed.
                an=${k#track_area_}
                case $an in
                    '' | [!a-z]* | *[!a-z0-9_]*) log own_app "akam2k.conf line $n ignored: $k has a bad area name" ;;
                    *)
                        if [ "${#an}" -gt 16 ]; then
                            log own_app "akam2k.conf line $n ignored: $k has a bad area name"
                        else
                            case $v in
                                [0-9]*:[0-9]*:[0-9]*:[0-9]*)
                                    case $v in
                                        *[!0-9:]* | *:*:*:*:* | *::* | *: | *[0-9][0-9][0-9][0-9][0-9][0-9]*) log own_app "akam2k.conf line $n ignored: $k must be PMIN:PMAX:TMIN:TMAX" ;;
                                        *)
                                            if [ "$NAREA" -ge 4 ]; then
                                                log own_app "akam2k.conf line $n ignored: at most 4 tracking areas"
                                            else
                                                NAREA=$((NAREA + 1))
                                                ARGS="$ARGS --track-area $an=$v"
                                            fi
                                            ;;
                                    esac
                                    ;;
                                *) log own_app "akam2k.conf line $n ignored: $k must be PMIN:PMAX:TMIN:TMAX" ;;
                            esac
                        fi
                        ;;
                esac
                continue
                ;;
            video0_rc | video1_rc) # the encoder rate-control mode of a stream: an exact word, never evaluated
                case $v in
                    vbr | cbr | avbr) ARGS="$ARGS --$(echo "$k" | tr _ -) $v" ;;
                    *) log own_app "akam2k.conf line $n ignored: $k must be vbr, cbr or avbr" ;;
                esac
                continue
                ;;
            night | ir_led) # the only keys that take a word: exact match against the allowed ones, never evaluated
                case $k:$v in
                    night:auto | night:day | night:night) ARGS="$ARGS --night $v" ;;
                    ir_led:auto | ir_led:off) ARGS="$ARGS --ir-led $v" ;;
                    night:*) log own_app "akam2k.conf line $n ignored: night must be auto, day or night" ;;
                    *) log own_app "akam2k.conf line $n ignored: ir_led must be auto or off" ;;
                esac
                continue
                ;;
        esac
        case $v in '' | *[!0-9]*)
            log own_app "akam2k.conf line $n ignored: $k needs a number"
            continue
            ;;
        esac
        v=$(echo "$v" | sed 's/^0*//')
        [ -n "$v" ] || v=0
        if [ "${#v}" -gt 5 ]; then
            log own_app "akam2k.conf line $n ignored: $k out of range"
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
            snapshot) lo=0 hi=1 opt=--snapshot ;;
            snapshot_interval) lo=5 hi=300 opt=--snapshot-interval ;;
            image_brightness) lo=0 hi=100 opt=--image-brightness ;;
            image_contrast) lo=0 hi=100 opt=--image-contrast ;;
            image_sharpness) lo=0 hi=100 opt=--image-sharpness ;;
            mains_hz) lo=50 hi=60 opt=--mains-hz ;;
            wdr) lo=0 hi=1 opt=--wdr ;;
            audio) lo=0 hi=1 opt=--audio ;;
            ptz_privacy) lo=0 hi=1 opt=--ptz-privacy ;;
            track) lo=0 hi=1 opt=--track ;;
            track_invert_pan) lo=0 hi=1 opt=--track-invert-pan ;;
            track_invert_tilt) lo=0 hi=1 opt=--track-invert-tilt ;;
            ptz_scene_check) lo=0 hi=1 opt=--ptz-scene-check ;;
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
                log own_app "akam2k.conf line $n ignored: unknown key"
                continue
                ;;
        esac
        if [ "$v" -lt "$lo" ] || [ "$v" -gt "$hi" ]; then
            log own_app "akam2k.conf line $n ignored: $k must be $lo..$hi"
            continue
        fi
        # a peak bit rate is 0 (automatic) or at least the lowest target of its stream
        if { [ "$k" = video0_max_kbps ] && [ "$v" -ne 0 ] && [ "$v" -lt 64 ]; } || { [ "$k" = video1_max_kbps ] && [ "$v" -ne 0 ] && [ "$v" -lt 32 ]; }; then
            log own_app "akam2k.conf line $n ignored: $k must be 0 (automatic) or at least the lowest target (64 main, 32 sub)"
            continue
        fi
        if [ "$k" = mains_hz ] && [ "$v" != 50 ] && [ "$v" != 60 ]; then
            log own_app "akam2k.conf line $n ignored: mains_hz must be 50 or 60"
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

log own_app "supervisor start"
prepare || {
    log own_app "supervisor: not starting the app"
    exit 1
}
WANT=$(sed -n 's/^sha256=//p' "$INFO" 2>/dev/null | head -n1)
read_conf
log own_app "akam2k_rtsp options:${ARGS:- (defaults)}"

bo=$BO_MIN
exits=""
while [ "$STOPPING" = 0 ]; do
    rotate_log
    # the /tmp copy is verified again before EVERY exec (restarts included): nothing runs that is not the hash
    if [ "$(sha_of "$RUN")" != "$WANT" ]; then
        log own_app "akam2k_rtsp refused: $RUN no longer matches BUILDINFO, supervisor exits"
        rm -f "$RUN"
        exit 1
    fi
    t0=$(now)
    # shellcheck disable=SC2086  # ARGS is a validated word list of "-x number" pairs
    LD_LIBRARY_PATH=$LIBS "$RUN" $ARGS >>"$APPLOG" 2>&1 </dev/null &
    APP=$!
    log own_app "akam2k_rtsp started, pid $APP"
    # Under memory pressure the OOM killer should take the app, never the stub or the feeder.
    echo 500 >"$ROOT/proc/$APP/oom_score_adj" 2>/dev/null
    while kill -0 "$APP" 2>/dev/null; do
        sleep "$POLL" &
        SLP=$!
        wait "$SLP"
        SLP=""
        rotate_log
        net_request
    done
    wait "$APP" 2>/dev/null
    rc=$?
    APP=""
    t1=$(now)
    run=$((t1 - t0))
    log own_app "akam2k_rtsp exited with status $rc after $run s"
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
        log own_app "akam2k_rtsp exited $cnt times within $CRASH_WINDOW s: giving up (SSH and the network are unaffected; reboot to try again)"
        exit 0
    fi
    [ "$run" -lt "$STABLE" ] || bo=$BO_MIN
    log own_app "restarting akam2k_rtsp in $bo s"
    sleep "$bo" &
    SLP=$!
    wait "$SLP"
    SLP=""
    bo=$((bo * 2))
    [ "$bo" -le "$BO_MAX" ] || bo=$BO_MAX
done
