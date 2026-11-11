#!/bin/sh
# ak_rtsp_wrapper.sh — process_0's actual entry point in the de-Tuyafied
# firmware (see _ht_process.ini in this same directory). Runs as a direct
# child of the stock /usr/bin/watchdog, exactly where anyka_ipc used to run,
# on a clean boot where sensor_driver.sh has already insmod'd ak_isp/
# sensor_gc20c3/ak_venc_adapter/ak_venc_bridge/ak_pcm and copied
# /tmp/sensor_isp.conf.
#
# See docs/building_your_own_firmware.md for how this fits into the patched
# APP squashfs tree.
PATH=$PATH:/bin:/sbin:/usr/bin:/usr/sbin

# --- Respawn backoff and crash counter (G04) ---
# The stock watchdog relaunches this script the moment it exits (delay = 0 in _ht_process.ini), so a
# persistent failure of ak_rtsp would loop at full speed. The state lives in /tmp (RAM, gone at reboot):
#   /tmp/akam_respawn   one line: count=<consecutive short runs> total=<exits since boot> last_rc=<code>
#                       last_run=<seconds> up=<uptime at the last exit>
#   /tmp/akam_wrapper.log  one line per ak_rtsp exit (capped), and the same lines go to syslog if `logger` exists.
# A run of at least 10 minutes counts as stable and clears `count`. Delay before the next start, by
# `count`: 1 -> 0 s (a one-off crash recovers at once), 2 -> 2 s, 3 -> 5 s, 4 -> 10 s, 5 and more -> 30 s.
# The very first start (count 0) never waits and never forks anything. ak_rtsp exit code 70 = its own
# software stall watchdog (swdog.h) fired. The /proc and /tmp prefixes are overridable for the host test.
AKAM_ROOT=${AKAM_ROOT:-}
AKAM_TMP=${AKAM_TMP_DIR:-$AKAM_ROOT/tmp}
RESPAWN_FILE=$AKAM_TMP/akam_respawn
WRAPPER_LOG=$AKAM_TMP/akam_wrapper.log
STABLE_S=${AKAM_STABLE_S:-600}
LOGGER=${AKAM_LOGGER:-logger}

wlog() {
    echo "ak_rtsp_wrapper: $*"
    echo "ak_rtsp_wrapper: $*" >>"$WRAPPER_LOG" 2>/dev/null
    if [ "$(wc -l <"$WRAPPER_LOG" 2>/dev/null)" -gt 100 ] 2>/dev/null; then
        tail -n 50 "$WRAPPER_LOG" >"$WRAPPER_LOG.tmp" 2>/dev/null && mv -f "$WRAPPER_LOG.tmp" "$WRAPPER_LOG"
    fi
    command -v "$LOGGER" >/dev/null 2>&1 && "$LOGGER" -t akam_wrapper "$*" 2>/dev/null
    return 0
}

# Sets UP to the integer seconds of the uptime (monotonic, unaffected by NTP steps).
read_uptime() {
    UP=0
    read -r UP _ <"${AKAM_PROC_DIR:-$AKAM_ROOT/proc}/uptime" 2>/dev/null
    UP=${UP%%.*}
    case "$UP" in '' | *[!0-9]*) UP=0 ;; esac
}

CRASH_N=0
CRASH_TOTAL=0
if [ -r "$RESPAWN_FILE" ]; then
    read -r _state <"$RESPAWN_FILE" 2>/dev/null
    for _kv in $_state; do
        case "$_kv" in
            count=*) CRASH_N=${_kv#count=} ;;
            total=*) CRASH_TOTAL=${_kv#total=} ;;
        esac
    done
    case "$CRASH_N" in '' | *[!0-9]*) CRASH_N=0 ;; esac
    case "$CRASH_TOTAL" in '' | *[!0-9]*) CRASH_TOTAL=0 ;; esac
fi
case "$CRASH_N" in
    0 | 1) _delay=0 ;;
    2) _delay=2 ;;
    3) _delay=5 ;;
    4) _delay=10 ;;
    *) _delay=30 ;;
esac
if [ "$_delay" -gt 0 ]; then
    wlog "restart after $CRASH_N consecutive short runs (total $CRASH_TOTAL since boot), waiting ${_delay}s"
    sleep "$_delay"
fi

# --- Network hostname ---
# Applied first, synchronously (it only reads two tiny files and calls hostname),
# so the kernel name is right before anything networked starts. Never fatal.
if [ -x /usr/sbin/akam_hostname.sh ]; then
    /usr/sbin/akam_hostname.sh apply </dev/null >/dev/null 2>&1
fi
# DHCP hostname: effective name, "akam" if the helper is missing or fails.
AKAM_DHCP_HOSTNAME=$(/usr/sbin/akam_hostname.sh show 2>/dev/null)
[ -n "$AKAM_DHCP_HOSTNAME" ] || AKAM_DHCP_HOSTNAME=akam

# --- Akam background helpers (SSH, lockdown) ---
# Fire and forget: detached, output discarded, exit status ignored, so a
# missing or failing helper can never delay or affect ak_rtsp below. Each
# helper has its own anti-duplicate guard (this wrapper may be respawned).
if [ -x /usr/sbin/akam_ssh.sh ]; then
    ( /usr/sbin/akam_ssh.sh start </dev/null >/dev/null 2>&1 & )
fi
if [ -x /usr/sbin/akam_lockdown.sh ]; then
    ( /usr/sbin/akam_lockdown.sh </dev/null >/dev/null 2>&1 & )
fi
# Wi-Fi supervisor (G03): re-checks wpa_supplicant, udhcpc and the IPv4 address every 30 s. Its first
# check comes after the first interval, so it never races the bring-up below.
if [ -x /usr/sbin/akam_net.sh ]; then
    ( /usr/sbin/akam_net.sh start </dev/null >/dev/null 2>&1 & )
fi

# Time sync (G08), OFF by default: the helper starts only when the user saved a server with ./akam ntp set
# (/etc/config/akam/ntp exists). Detached like the others; it syncs once the network is up, then every 6 hours.
if [ -f "$AKAM_ROOT/etc/config/akam/ntp" ] && [ -x /usr/sbin/akam_ntp.sh ]; then
    ( /usr/sbin/akam_ntp.sh run </dev/null >/dev/null 2>&1 & )
fi

# --- Manual WiFi bring-up ---
# ethernet.sh's wifi_driver.sh install() deliberately SKIPS starting
# wpa_supplicant and assigning an IP whenever /tmp/_ak39_factory.ini exists
# (its own factory/production-test gate). If you keep _ak39_factory.ini on
# the SD card (e.g. as a quick fallback path to stock factory-demo RTSP),
# ethernet.sh will skip WiFi bring-up entirely — so we do it ourselves here
# instead. Plain DHCP only. Guarded by a running-check so a watchdog respawn
# of this script after a crash doesn't spawn a second wpa_supplicant/udhcpc.
WPA_CONF=/etc/config/wpa_supplicant.conf
# pgrep may not be built into the camera BusyBox, so keep ps|grep (SC2009 is deliberate).
# shellcheck disable=SC2009
if [ -f "$WPA_CONF" ] && ! ps | grep -v grep | grep -q wpa_supplicant; then
    echo "ak_rtsp_wrapper: starting wpa_supplicant + udhcpc"
    wpa_supplicant -Dnl80211 -i wlan0 -c "$WPA_CONF" -B
    udhcpc -i wlan0 -x "hostname:$AKAM_DHCP_HOSTNAME" &
    touch /tmp/connect_type_wifi
fi

read_uptime
START_UP=$UP
${AKAM_RTSP_BIN:-/usr/bin/ak_rtsp}
RET=$?
read_uptime
RUN_S=$((UP - START_UP))
[ "$RUN_S" -ge 0 ] || RUN_S=0
if [ "$RUN_S" -ge "$STABLE_S" ]; then CRASH_N=0; else CRASH_N=$((CRASH_N + 1)); fi
CRASH_TOTAL=$((CRASH_TOTAL + 1))
echo "count=$CRASH_N total=$CRASH_TOTAL last_rc=$RET last_run=$RUN_S up=$UP" >"$RESPAWN_FILE.tmp" 2>/dev/null &&
    mv -f "$RESPAWN_FILE.tmp" "$RESPAWN_FILE"
_why=
[ "$RET" = 70 ] && _why=" (software stall watchdog)"
wlog "ak_rtsp exited with code $RET${_why} after ${RUN_S}s, short runs in a row: $CRASH_N, exits since boot: $CRASH_TOTAL"
exit $RET
