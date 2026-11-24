#!/usr/bin/env bash
# shellcheck disable=SC2015  # ok/ko never fail: A && ok || ko is an if-then-else here
# Host test of cfg_request of the 2K supervisor (own_app_run.sh): the user-file settings of the control API (MQTT, ONVIF, stream
# password, NTP, microphone) are validated again, written only when valid and consistent, only when the content changes, with the
# right modes, in the card layout and in the card-less (/config/akam + links) layout. Run under the pinned BusyBox sh when available.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
RUN=$REPO/firmware/lsc-rotatable-2k-ak3918av130/boot/card/akam/own_app_run.sh
if [ -z "${SH_UNDER_TEST:-}" ]; then
    BB=$REPO/tools/.toolchain/busybox-1.35.0-x86_64/busybox
    if [ -x "$BB" ]; then SH_UNDER_TEST="$BB sh"
    elif [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=${TMPDIR:-$HOME/.cache/akam-tmp}/rot2k_cfg.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else ko "$1 (got '$2', want '$3')"; fi; }

# the block between the markers, run as a function library
sed -n '/^# --- cfg_request begin/,/^# --- cfg_request end/p' "$RUN" >"$WORK/block.sh"
[ -s "$WORK/block.sh" ] || { echo "no cfg_request block in $RUN"; exit 1; }

# run MODE REQUEST_TEXT: one request in a fresh or the same tree; prints nothing, the result is in $T/res
setup() { # $1 = card | flash
    T=$WORK/$1.$2
    rm -rf "$T"
    mkdir -p "$T/mnt/sdcard/akam" "$T/tmp" "$T/config/akam"
    if [ "$1" = flash ]; then ln -s /nonexistent "$T/mnt/sdcard/busybox"; fi
}
run() { # request text on $1
    printf '%s' "$1" >"$T/tmp/akam_cfg.req"
    # shellcheck disable=SC2016  # the script text is for the shell under test
    "${SHCMD[@]}" -c '
        ROOT=$1; SD=$ROOT/mnt/sdcard; AK=$SD/akam; TMP=$ROOT/tmp; AKAM_CONFIG=$ROOT/config
        log() { echo "$*" >>"$TMP/log"; }
        . "$2"
        cfg_request
    ' sh "$T" "$WORK/block.sh"
}
res() { tr -d '\n' <"$T/tmp/akam_cfg.res"; }
real() { if [ -L "$T/mnt/sdcard/busybox" ]; then echo "$T/config/akam/$1"; else echo "$T/mnt/sdcard/akam/$1"; fi; }
mode() { stat -c %a "$1"; }

for L in card flash; do
    echo "== layout: $L"
    setup "$L" 1
    run $'mqtt.host=broker.lan\nmqtt.port=1884\nmqtt.user=ha\nmqtt.password=s3cret pass\nmqtt.enabled=1\n'
    eq "$L: result ok" "$(res)" "ok"
    eq "$L: mqtt file" "$(cat "$(real mqtt)")" $'enabled=1\nhost=broker.lan\nport=1884\nuser=ha'
    eq "$L: mqtt_pass file" "$(cat "$(real mqtt_pass)")" "s3cret pass"
    eq "$L: mqtt mode 600" "$(mode "$(real mqtt)")" 600
    eq "$L: mqtt_pass mode 600" "$(mode "$(real mqtt_pass)")" 600
    if [ -e "$T/mnt/sdcard/akam/mqtt" ]; then ok "$L: the app sees akam/mqtt"; else ko "$L: the app sees akam/mqtt"; fi
    if grep -q 's3cret' "$T/tmp/log"; then ko "$L: the password is in the log"; else ok "$L: the password is not in the log"; fi
    if [ ! -e "$T/tmp/akam_cfg.req" ] && [ ! -e "$T/tmp/akam_cfg.req.work" ]; then ok "$L: no request file left"; else ko "$L: request files left"; fi
    # unchanged content: the file is not replaced
    i1=$(ls -i "$(real mqtt)")
    run $'mqtt.host=broker.lan\nmqtt.port=1884\n'
    eq "$L: same content, same file (no flash write)" "$(ls -i "$(real mqtt)")" "$i1"
    # options set by hand survive
    printf 'client_id=cam1\nbase_topic=home/cam\n' >>"$(real mqtt)"
    run $'mqtt.host=other.lan\n'
    eq "$L: hand-made options kept" "$(grep -c 'client_id=cam1' "$(real mqtt)")" 1
    eq "$L: host replaced" "$(grep '^host=' "$(real mqtt)")" "host=other.lan"
    # disable keeps the settings
    run $'mqtt.enabled=0\n'
    eq "$L: disabled" "$(grep '^enabled=' "$(real mqtt)")" "enabled=0"
    # empty host forgets MQTT
    run $'mqtt.enabled=0\nmqtt.host=\n'
    [ ! -e "$(real mqtt)" ] && [ ! -e "$(real mqtt_pass)" ] && ok "$L: empty host removes mqtt and mqtt_pass" || ko "$L: mqtt files remain"

    echo "== refusals write nothing ($L)"
    setup "$L" 2
    run $'mqtt.host=bad host\n'; eq "$L: bad host" "$(res)" "refused mqtt.host"
    run $'mqtt.host=x.lan\nmqtt.port=70000\n'; eq "$L: bad port" "$(res)" "refused mqtt.port"
    run $'mqtt.port=1883\nmqtt.enabled=1\n'; eq "$L: enable without host" "$(res)" "refused mqtt.enabled"
    run $'mqtt.host=x.lan\nmqtt.password=abc\n'; eq "$L: password without user" "$(res)" "refused mqtt.password"
    run $'rtsp.user=bob\n'; eq "$L: a new user without password" "$(res)" "refused rtsp.user"
    run $'rtsp.password=longenough1\n'; eq "$L: password without a user" "$(res)" "refused rtsp.password"
    run $'rtsp.user=bob\nrtsp.password=short\n'; eq "$L: password under 8" "$(res)" "refused rtsp.password"
    run $'onvif.enabled=1\n'; eq "$L: onvif without stream password" "$(res)" "refused onvif.enabled"
    run $'ntp.servers=a.b c.d e.f g.h\n'; eq "$L: four ntp servers" "$(res)" "refused ntp.servers"
    run $'ntp.servers=-bad.org\n'; eq "$L: bad ntp name" "$(res)" "refused ntp.servers"
    run $'unknown.key=1\n'; eq "$L: unknown key" "$(res)" "refused unknown.key"
    run $'audio.mic_enable=2\n'; eq "$L: bad bool" "$(res)" "refused audio.mic_enable"
    run $'onvif.port=80\n'; eq "$L: onvif port range" "$(res)" "refused onvif.port"
    n=$(find "$T/mnt/sdcard/akam" "$T/config/akam" -type f -o -type l | wc -l)
    eq "$L: nothing was written" "$n" 0
    run $'mqtt.host=x.lan\nmqtt.port=1884\nntp.servers=a.b\nonvif.enabled=1\n'
    eq "$L: one bad line refuses the whole request" "$(res)" "refused onvif.enabled"
    n=$(find "$T/mnt/sdcard/akam" "$T/config/akam" -type f -o -type l | wc -l)
    eq "$L: the valid lines of a refused request are not applied" "$n" 0

    echo "== stream password, ONVIF, NTP, microphone ($L)"
    setup "$L" 3
    printf '# my options\nmain_kbps=2000\n onvif = 0 \naudio=1 # old\n' >"$(real akam.conf)"
    [ "$L" = flash ] && ln -s "$(real akam.conf)" "$T/mnt/sdcard/akam/akam.conf"
    run $'rtsp.user=bob\nrtsp.password=pa:ss word\nonvif.enabled=1\nonvif.port=8001\naudio.mic_enable=0\nntp.servers=pool.ntp.org time.example.net\n'
    eq "$L: result ok" "$(res)" "ok"
    want=$(printf '%s' 'bob:akam:pa:ss word' | md5sum | cut -d' ' -f1)
    eq "$L: rtsp.conf is user:akam:HA1" "$(cat "$(real rtsp.conf)")" "bob:akam:$want"
    eq "$L: rtsp.conf mode 600" "$(mode "$(real rtsp.conf)")" 600
    if grep -rq 'pa:ss word' "$T/mnt/sdcard/akam" "$T/config/akam" "$T/tmp"; then ko "$L: the clear password is stored or logged"; else ok "$L: the clear password is stored nowhere"; fi
    eq "$L: conf keeps comments and other keys" "$(grep -c '^# my options$\|^main_kbps=2000$' "$(real akam.conf)")" 2
    eq "$L: onvif=1 replaces ' onvif = 0 '" "$(grep -c '^onvif=1$' "$(real akam.conf)")" 1
    eq "$L: onvif_port added" "$(grep -c '^onvif_port=8001$' "$(real akam.conf)")" 1
    eq "$L: no second onvif= line" "$(grep -c '^ *onvif *=' "$(real akam.conf)")" 1
    eq "$L: audio=0 replaces audio=1 # old" "$(grep -c '^audio=0$' "$(real akam.conf)")" 1
    eq "$L: ntp file" "$(cat "$(real ntp)")" $'server=pool.ntp.org\nserver=time.example.net'
    # password change keeps the user
    run $'rtsp.password=another-long-one\n'
    want=$(printf '%s' 'bob:akam:another-long-one' | md5sum | cut -d' ' -f1)
    eq "$L: a password alone keeps the user" "$(cat "$(real rtsp.conf)")" "bob:akam:$want"
    # the same user without a password is a no-op
    run $'rtsp.user=bob\n'
    eq "$L: the same user alone is accepted" "$(res)" "ok"
    eq "$L: and keeps the hash" "$(cat "$(real rtsp.conf)")" "bob:akam:$want"
    # ntp off, rtsp off
    run $'ntp.servers=\nrtsp.user=\n'
    [ ! -e "$(real ntp)" ] && [ ! -e "$(real rtsp.conf)" ] && ok "$L: empty value removes ntp and rtsp.conf" || ko "$L: ntp / rtsp.conf remain"
    if [ "$L" = flash ]; then
        [ ! -e "$T/mnt/sdcard/akam/ntp" ] && [ ! -L "$T/mnt/sdcard/akam/ntp" ] && ok "flash: the dangling link is removed" || ko "flash: a dangling link remains"
    fi
done

echo "rot2k cfg: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
