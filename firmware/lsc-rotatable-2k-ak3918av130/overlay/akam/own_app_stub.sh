#!/bin/sh
# own_app_stub.sh -- PoC-1 stand-in for yy_cam on the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
# NOT run from the card directly: akam_boot.sh (flag akam/own_app) copies it to /tmp and bind-mounts the copy over
# /appfs/bin/yy_cam (RAM only, the read-only squashfs underneath is untouched), so the stock start.sh runs it where
# it would run yy_cam. It does the two things yy_cam did that nothing else does: feed the hardware watchdog and
# bring Wi-Fi up. Nothing else (no video, no RTSP in PoC-1).
#
# Order (the feeder comes FIRST: no gap between "armed" and "fed" longer than the 20 s timeout):
#   1  ONE watchdog feeder: card busybox "watchdog -F -t 5 -T 20 /dev/watchdog" (stock yy_cam: 20 s, fed every 5 s)
#   1b Wi-Fi enable GPIO as stock (gpio10, active low: disabled level, enabled level, 1 s), then
#   2  insmod atbm6132c.ko wifi_bt_comb=1 (stock parameters), 2 s as stock
#   3  copy akam/wpa.conf to /tmp/wpa.conf (0600); the content is NEVER read, printed or logged here
#   3b wait up to 20 s for /sys/class/net/wlan0 (the firmware load is asynchronous), "ip link set wlan0 up";
#      if it never appears: bounded diagnostics in the card log (gpio state, USB ids, dmesg tail)
#   4  wpa_supplicant as stock (3 attempts, 3 s apart, output kept in /tmp, last 5 lines minus anything that looks
#      like a credential go to the card log), udhcpc with hostname akam-rot2k
#   5  health, within 120 s: wlan0 has an IPv4 and the routing table has a default route (the gateway ping is
#      logged for information only). Healthy: boot counter reset to 0, "own_app PoC-1 healthy", then idle forever.
#      Unhealthy: the feeder is SIGKILLed so the hardware resets the camera in about 20 s; the counter akam_boot.sh
#      incremented stays, and after 3 unfinished boots the card falls back to the stock yy_cam.
#
# WATCHDOG: the stub never exits while the feeder runs. On SIGTERM/SIGINT it stops the feeder with SIGTERM: BusyBox
# watchdog.c handles SIGTERM by writing the magic close character 'V' and closing the device ("disarm"), but that
# only disarms when the Anyka driver honours the magic close (the stock app uses WDIOC_SETOPTIONS DISABLECARD
# instead, and the driver's nowayout state is UNVERIFIED). If it does not disarm, the camera resets within 20 s,
# which is the safe direction. SIGKILL of the feeder never disarms: that is the deliberate "reset me" path.
# The watchdog is not flash: a reset is a plain reboot, undone by the counter fallback or by removing the card.
#
# AUTO-START (after "healthy"): when akam/bin/akam2k_rtsp is on the card and the flag akam/no_app is absent, the stub
# starts akam/own_app_run.sh in the background: a supervisor that verifies (sha256 against the BUILDINFO), copies to
# /tmp and runs the app with restart, backoff and a crash limit, logging to a size-capped card log. It is a separate
# process: a crash loop of the app can neither stop the feeder nor touch Wi-Fi, and when it gives up the camera stays
# reachable by SSH. The app never opens /dev/watchdog: the feeder here is the only one.
# STOP ORDER on SIGTERM/SIGINT of the stub: supervisor first (it SIGTERMs the app, waits, then SIGKILLs), then the
# feeder as before. A wedged supervisor cannot hold the feeder forever: the wait for it is bounded.
#
# TIME SYNC (opt-in): the camera has no RTC and boots in 1970. When the card has the file akam/ntp (same format as the
# 1080p: one `server=NAME` line per server, at most 3, a host name or an IPv4 address) the stub starts
# `akam_ntp.sh run` (the shared 1080p script, copied unchanged to akam/akam_ntp.sh) detached after the network is
# healthy. BusyBox here has no ntpd applet: AKAM_NTPD points at akam2k_sntp, a one-shot SNTP client from the card
# (akam/bin/akam2k_sntp, sha256 checked against akam/bin/akam2k_sntp.BUILDINFO, run from a copy in /tmp). Without the
# file: no process and no traffic. The helper is stopped with the stub.
#
# Writes only to /tmp and the card (boot counter, log). No flash, no nvram. Paths are prefixed by $AKAM_ROOT.

ROOT=${AKAM_ROOT:-}
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
AK=$SD/akam
# A broken card must not kill the stub: the probe runs in a subshell (a failing "." ends a non-interactive shell).
# shellcheck disable=SC1091  # the helper lives on the card at run time
if ( . "$AK/common.sh" ) >/dev/null 2>&1; then
    # shellcheck disable=SC1091  # the helper lives on the card at run time
    . "$AK/common.sh"
else
    TMP=$ROOT/tmp
    log() { :; }
fi

COUNTER=$AK/own_app_boots
IP=${AKAM_IP:-ip}
WPAS=${AKAM_WPAS:-$ROOT/appfs/bin/wpa_supplicant}
KO=${AKAM_WIFI_KO:-$ROOT/appfs/drivers/atbm6132c.ko}
HEALTH_TRIES=${AKAM_HEALTH_TRIES:-60}
HEALTH_SLEEP=${AKAM_HEALTH_SLEEP:-2}
IDLE_SLEEP=${AKAM_IDLE_SLEEP:-3600}
IFACE=wlan0
IFACE_TRIES=${AKAM_IFACE_TRIES:-20}
IFACE_SLEEP=${AKAM_IFACE_SLEEP:-1}
WPA_TRIES=${AKAM_WPA_TRIES:-3}
WPA_SLEEP=${AKAM_WPA_SLEEP:-3}
HEALTH_BUDGET=${AKAM_HEALTH_BUDGET:-120}
T0=$(date +%s 2>/dev/null || echo 0)
FEEDER=""
SUP=""
NTPH=""
SH=${AKAM_SH:-sh}
SUP_STOP_WAIT=${AKAM_SUP_STOP_WAIT:-20}

# stop_supervisor: SIGTERM, then wait at most SUP_STOP_WAIT seconds, SIGKILL the supervisor (and nothing else) if it
# is still there. The app then is an orphan of a dead supervisor, but the feeder stop follows either way.
stop_supervisor() {
    [ -n "$SUP" ] || return 0
    if kill -0 "$SUP" 2>/dev/null; then
        kill -TERM "$SUP" 2>/dev/null
        i=0
        while kill -0 "$SUP" 2>/dev/null && [ "$i" -lt "$SUP_STOP_WAIT" ]; do
            sleep 1
            i=$((i + 1))
        done
        if kill -0 "$SUP" 2>/dev/null; then
            log own_app "supervisor $SUP did not stop in $SUP_STOP_WAIT s: SIGKILL"
            kill -KILL "$SUP" 2>/dev/null
        fi
    fi
    wait "$SUP" 2>/dev/null
    SUP=""
}

stop_feeder_term() {
    [ -z "$FEEDER" ] || kill -TERM "$FEEDER" 2>/dev/null
}
stop_ntp() {
    [ -z "$NTPH" ] || kill -TERM "$NTPH" 2>/dev/null
}
trap 'log own_app "stub got a stop signal: stopping the app, then the feeder with SIGTERM"; stop_ntp; stop_supervisor; stop_feeder_term; exit 0' TERM INT

idle() { # never returns: the feeder (if any) keeps being fed by its own process
    while :; do
        sleep "$IDLE_SLEEP" &
        wait $!
    done
}

fail() { # fail <reason>: log, then make the hardware reset the camera by killing the feeder
    log own_app "own_app PoC-1 FAILED: $1"
    if [ -n "$FEEDER" ] && kill -0 "$FEEDER" 2>/dev/null; then
        log own_app "killing the watchdog feeder $FEEDER (SIGKILL): hardware reset in about 20 s"
        kill -KILL "$FEEDER" 2>/dev/null
    else
        log own_app "no live feeder: nothing is armed, the camera stays up without network (power-cycle it)"
    fi
    idle
}

# wifi_diag: bounded card-log diagnostics for the "wlan0 never appeared" path. Nothing here is a secret, and the
# dmesg tail is filtered anyway (psk, ssid, passphrase, password).
wifi_diag() {
    d=$(cat "$GPIO_SYS/gpio$WIFI_EN_GPIO/direction" 2>/dev/null || echo ?)
    v=$(cat "$GPIO_SYS/gpio$WIFI_EN_GPIO/value" 2>/dev/null || echo ?)
    log own_app "diag: gpio$WIFI_EN_GPIO direction=$d value=$v (set: disabled $EN_OFF, enabled $EN_ON)"
    l=""
    for d in "$ROOT"/sys/bus/usb/devices/*; do
        [ -e "$d" ] && l="$l ${d##*/}"
    done
    log own_app "diag: /sys/bus/usb/devices:$l"
    for d in "$ROOT"/sys/bus/usb/devices/*; do
        [ -f "$d/idVendor" ] || continue
        v=$(cat "$d/idVendor" 2>/dev/null)
        p=$(cat "$d/idProduct" 2>/dev/null)
        id=${d##*/}
        log own_app "diag: usb $id idVendor=$v idProduct=$p"
    done
    log own_app "diag: dmesg (usb|atbm|hcd|wlan|80211), last 15 lines"
    dmesg 2>/dev/null | grep -i -E 'usb|atbm|hcd|wlan|80211' | grep -v -i -E 'psk|ssid|passphrase|password' |
        tail -n 15 | while IFS= read -r l; do
        log own_app "diag: dmesg: $l"
    done
    sync
}

log own_app "stub start"

# 1) the single watchdog feeder, before anything slow
"$SD/busybox" watchdog -F -t 5 -T 20 /dev/watchdog >/dev/null 2>&1 </dev/null &
FEEDER=$!
log own_app "watchdog feeder started, pid $FEEDER"

# 1b) Wi-Fi power, exactly as stock yy_cam (yy_net_manager_wifi_init 0x2ac978 + yy_net_manager_wifi_insmod_driver
#     0x2ac7dc, offline RE of yy_cam, see docs/porting/lsc-rotatable-2k/own-app-feasibility.md). The ATBM6132C is a
#     USB device whose enable pin is a GPIO. yy_cam reads [net] wifi_en_gpio_no (default 10) and
#     wifi_en_gpio_polarity (default 0 = active LOW), then drives it through sysfs (/sys/class/gpio, no /dev/mem):
#       a. export gpioN, direction out, value = the DISABLED level (1 for polarity 0)      (yy_gpio_init)
#       b. value = the ENABLED level (0 for polarity 0, 1 for polarity 1)                  (yy_gpio_set_value)
#       c. sleep 1, then "insmod /appfs/drivers/atbm6132c.ko wifi_bt_comb=1", then sleep 2
#     gpio55 (the earlier guess) is the RED LED in yy_cam, not the Wi-Fi enable. A GPIO write is RAM state only
#     (reset on reboot), never flash.
WIFI_EN_GPIO=${AKAM_WIFI_EN_GPIO:-10}
WIFI_EN_POL=${AKAM_WIFI_EN_POL:-0}
WIFI_PRE_SLEEP=${AKAM_WIFI_PRE_SLEEP:-1}
WIFI_POST_SLEEP=${AKAM_WIFI_POST_SLEEP:-2}
if [ "$WIFI_EN_POL" = 1 ]; then
    EN_ON=1
    EN_OFF=0
else
    EN_ON=0
    EN_OFF=1
fi
GPIO_SYS="$ROOT/sys/class/gpio"
if [ -d "$GPIO_SYS" ]; then
    [ -d "$GPIO_SYS/gpio$WIFI_EN_GPIO" ] || echo "$WIFI_EN_GPIO" >"$GPIO_SYS/export" 2>/dev/null
    if echo out >"$GPIO_SYS/gpio$WIFI_EN_GPIO/direction" 2>/dev/null &&
        echo "$EN_OFF" >"$GPIO_SYS/gpio$WIFI_EN_GPIO/value" 2>/dev/null; then
        log own_app "wifi enable gpio$WIFI_EN_GPIO: out, disabled level $EN_OFF (polarity $WIFI_EN_POL)"
        if echo "$EN_ON" >"$GPIO_SYS/gpio$WIFI_EN_GPIO/value" 2>/dev/null; then
            log own_app "wifi enable gpio$WIFI_EN_GPIO: enabled level $EN_ON"
        else
            log own_app "wifi enable gpio$WIFI_EN_GPIO: could not set the enabled level (continuing)"
        fi
    else
        log own_app "wifi enable gpio$WIFI_EN_GPIO could not be set (continuing)"
    fi
    sleep "$WIFI_PRE_SLEEP"
else
    log own_app "no $GPIO_SYS: wifi enable gpio$WIFI_EN_GPIO not driven"
fi

# 2) Wi-Fi driver, stock parameters
if insmod "$KO" wifi_bt_comb=1 >/dev/null 2>&1; then
    log own_app "insmod atbm6132c ok"
else
    log own_app "insmod atbm6132c returned an error (already loaded?)"
fi
sleep "$WIFI_POST_SLEEP"

# 2b) wlan0 appears asynchronously after insmod (firmware load): wait for it, then bring it up
i=0
tw=$(date +%s 2>/dev/null || echo 0)
while [ ! -e "$ROOT/sys/class/net/$IFACE" ] && [ "$i" -lt "$IFACE_TRIES" ]; do
    sleep "$IFACE_SLEEP"
    i=$((i + 1))
done
tw=$(($(date +%s 2>/dev/null || echo 0) - tw))
if [ -e "$ROOT/sys/class/net/$IFACE" ]; then
    log own_app "$IFACE present after $tw s"
else
    log own_app "$IFACE did not appear within $tw s (continuing)"
    wifi_diag
fi
if $IP link set "$IFACE" up >/dev/null 2>&1; then
    log own_app "ip link set $IFACE up ok"
else
    log own_app "ip link set $IFACE up failed"
fi

# 3) credentials: copy only, never read
[ -f "$AK/wpa.conf" ] || fail "akam/wpa.conf missing on the card"
mkdir -p "$TMP" 2>/dev/null
( umask 077 && cp "$AK/wpa.conf" "$TMP/wpa.conf" ) || fail "could not copy wpa.conf to /tmp"
chmod 600 "$TMP/wpa.conf" 2>/dev/null

# 3a) network settings (akam_net.sh + the shared akam_hostname.sh, both optional): hostname from /config/akam/hostname
#     (default akam-rot2k) and, in the runtime /tmp/wpa.conf, the global keys country= / freq_list= from /config/akam/net
#     appended to the user's untouched wpa.conf. Any failure keeps the plain copy above and the stock hostname.
HN=akam-rot2k
NETSH=$AK/akam_net.sh
net_env() { AKAM_NET_WPA_SRC=$AK/wpa.conf AKAM_HOSTNAME_SH=$AK/akam_hostname.sh AKAM_SH=$SH "$@"; }
# shellcheck disable=SC2086  # SH may be "busybox sh" in the host test
net_sh() { net_env $SH "$NETSH" "$@"; }
if [ -f "$NETSH" ] && [ -f "$AK/akam_hostname.sh" ]; then
    nh=$(net_sh hostname apply 2>/dev/null)
    [ -z "$nh" ] || HN=$nh
    if net_sh wifi render >/dev/null 2>&1; then
        log own_app "network settings applied: hostname $HN, wifi $(net_sh wifi show 2>/dev/null | sed 's/$/ /' | sed 'N;s/\n//')"
    else
        log own_app "akam_net.sh wifi render failed: plain wpa.conf copy kept"
    fi
fi

# 4) supplicant and DHCP, as stock
# Credential-looking lines are dropped before anything reaches the card log.
log_wpa_tail() {
    tail -n 5 "$TMP/wpa.out" 2>/dev/null | grep -v -i -E 'psk|passphrase|password|ssid' | while IFS= read -r l; do
        log own_app "wpa_supplicant: $l"
    done
}
a=0
wpa_ok=0
while [ "$a" -lt "$WPA_TRIES" ]; do
    a=$((a + 1))
    if "$WPAS" -Dnl80211 -i "$IFACE" -c "$TMP/wpa.conf" -B -P "$TMP/wpa.pid" >"$TMP/wpa.out" 2>&1; then
        wpa_ok=1
        break
    fi
    log own_app "wpa_supplicant attempt $a of $WPA_TRIES failed"
    log_wpa_tail
    [ "$a" -ge "$WPA_TRIES" ] || sleep "$WPA_SLEEP"
done
[ "$wpa_ok" = 1 ] || fail "wpa_supplicant did not start after $WPA_TRIES attempts"
log own_app "wpa_supplicant started (attempt $a)"
udhcpc -b -p "$TMP/udhcpc.pid" -i "$IFACE" -x "hostname:$HN" >/dev/null 2>&1 || fail "udhcpc did not start"
log own_app "udhcpc started (hostname $HN)"

# 5) health check
health_loop() { # health_loop <budget seconds from T0>
    i=0
    healthy=0
    gw=""
    while [ "$i" -lt "$HEALTH_TRIES" ]; do
        now=$(date +%s 2>/dev/null || echo 0)
        [ "$((now - T0))" -lt "$1" ] || break
        if $IP addr show "$IFACE" 2>/dev/null | grep -q 'inet ' && $IP route show 2>/dev/null | grep -q '^default'; then
            gw=$($IP route show 2>/dev/null | sed -n 's/^default via \([0-9.]*\).*/\1/p' | head -n1)
            healthy=1
            break
        fi
        i=$((i + 1))
        sleep "$HEALTH_SLEEP"
    done
}
# A saved band (2.4 or 5) that no longer reaches the AP must not strand the camera: halfway through the budget the
# stub renders band auto, restarts wpa_supplicant and udhcpc, and waits the rest. Only when a band is set.
FIRST_BUDGET=$HEALTH_BUDGET
if [ -f "$NETSH" ] && [ -f "$AK/akam_hostname.sh" ] && ! net_sh wifi show 2>/dev/null | grep -q '^band=auto$'; then
    FIRST_BUDGET=$((HEALTH_BUDGET / 2))
fi
health_loop "$FIRST_BUDGET"
if [ "$healthy" != 1 ] && [ "$FIRST_BUDGET" != "$HEALTH_BUDGET" ]; then
    log own_app "no network with the saved Wi-Fi band after $FIRST_BUDGET s: falling back to band auto"
    if net_sh wifi fallback >/dev/null 2>&1; then
        health_loop "$HEALTH_BUDGET"
    else
        log own_app "wifi fallback failed"
    fi
fi
[ "$healthy" = 1 ] || fail "no IPv4 address and default route on $IFACE (health budget used up)"

if [ -n "$gw" ] && command -v ping >/dev/null 2>&1; then
    if ping -c 1 -W 2 "$gw" >/dev/null 2>&1; then
        log own_app "gateway $gw answers ping"
    else
        log own_app "gateway $gw does not answer ping (informational)"
    fi
fi

echo 0 >"$COUNTER" 2>/dev/null
sync
log own_app "own_app PoC-1 healthy"

# 5b) opt-in time sync (see the header). Nothing here can fail the stub.
start_ntp() {
    ntpd=""
    if [ -f "$AK/bin/akam2k_sntp" ]; then
        want=$(sed -n 's/^sha256=//p' "$AK/bin/akam2k_sntp.BUILDINFO" 2>/dev/null | head -n1)
        got=$("$SD/busybox" sha256sum "$AK/bin/akam2k_sntp" 2>/dev/null | cut -d' ' -f1)
        [ -n "$got" ] || got=$(sha256sum "$AK/bin/akam2k_sntp" 2>/dev/null | cut -d' ' -f1)
        if [ -z "$want" ] || [ "$got" != "$want" ]; then
            log own_app "time sync: akam2k_sntp refused (sha256 does not match akam/bin/akam2k_sntp.BUILDINFO)"
        elif cp "$AK/bin/akam2k_sntp" "$TMP/akam2k_sntp" 2>/dev/null && chmod 755 "$TMP/akam2k_sntp"; then
            ntpd=$TMP/akam2k_sntp
        else
            log own_app "time sync: cannot copy akam2k_sntp to $TMP"
        fi
    else
        log own_app "time sync: no akam/bin/akam2k_sntp on the card (BusyBox has no ntpd): not started"
    fi
    [ -n "$ntpd" ] || return 0
    AKAM_NTPD=$ntpd AKAM_NTP_FILE=$AK/ntp AKAM_CONFIG_DIR=$SD AKAM_TMP_DIR=$TMP \
        $SH "$AK/akam_ntp.sh" run >/dev/null 2>&1 </dev/null &
    NTPH=$!
    log own_app "time sync helper started, pid $NTPH (akam/ntp present)"
}
if [ ! -f "$AK/ntp" ]; then
    :
elif [ ! -f "$AK/akam_ntp.sh" ]; then
    log own_app "time sync: akam/ntp present but akam/akam_ntp.sh missing: not started"
else
    start_ntp
fi

# 6) the app, under its supervisor (see the header). Nothing here can fail the stub.
if [ ! -f "$AK/bin/akam2k_rtsp" ]; then
    log own_app "no akam/bin/akam2k_rtsp on the card: no app started"
elif flag no_app; then
    log own_app "flag akam/no_app: app not started"
elif [ ! -f "$AK/own_app_run.sh" ]; then
    log own_app "akam/own_app_run.sh missing: no app started"
else
    $SH "$AK/own_app_run.sh" >/dev/null 2>&1 </dev/null &
    SUP=$!
    log own_app "app supervisor started, pid $SUP"
fi
idle
