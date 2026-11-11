#!/bin/sh
# akam_mqtt.sh -- switch the optional MQTT client of ak_rtsp (Home Assistant discovery) on or off.
#
# Installed as /usr/sbin/akam_mqtt.sh (mode 755). Run it over SSH, as root:
#   akam_mqtt.sh set HOST PORT USER   save the broker and turn MQTT on. HOST: a host name or IPv4 address; PORT 1 to
#                                     65535 (plain TCP, usually 1883); USER: the broker user name, or - for none.
#                                     The password is ONE line on standard input (empty line = none), never an
#                                     argument: it would show in the process list. A password needs a user name.
#   akam_mqtt.sh option NAME VALUE    change one optional setting: base_topic, client_id, discovery (on|off),
#                                     discovery_prefix, interval (10 to 300 seconds). Needs a saved broker.
#   akam_mqtt.sh disable              turn MQTT off (settings and password stay for a later "set")
#   akam_mqtt.sh forget               turn MQTT off and delete the settings and the password file
#   akam_mqtt.sh status               on/off, the broker, the password (set or not, never its value), the state
#   akam_mqtt.sh test                 one connection attempt with the saved settings (works while MQTT is off)
# Exit status: 0 ok, 1 failure, 2 usage, 3 status found an INVALID file.
#
# Off by default and outbound only: with no file the camera has no MQTT thread and no socket. Enabled, ak_rtsp opens ONE
# TCP connection to HOST:PORT (no TLS: the password travels in clear on your network).
#
# Files (CONFIG partition, persistent, directory 0700):
#   /etc/config/akam/mqtt       mode 0600, key=value lines, LF only (see src/ak_rtsp/mqtt_svc.h for the rules):
#                                 enabled=1  host=NAME  port=N  user=NAME  [client_id=  base_topic=  discovery=
#                                 discovery_prefix=  interval=]
#   /etc/config/akam/mqtt_pass  mode 0600, the password alone on one line; absent = no password
# Both are replaced atomically. ak_rtsp reads them at start and on "MQTT reload" (port 8091, from the camera only),
# which this script sends after every change. An older ak_rtsp answers "ERR unknown command": the settings are saved
# and apply at the next reboot.
#
# POSIX sh for the camera's BusyBox 1.30.1: no tr, no mktemp, no stat (ls -ld instead). Paths are prefixed by
# $AKAM_ROOT (empty on the camera) so the host test can run the script in a fake root.

AKAM_ROOT=${AKAM_ROOT:-}
CONFIG_DIR=${AKAM_CONFIG_DIR:-$AKAM_ROOT/etc/config}
AKAM_DIR=$CONFIG_DIR/akam
MQTT_FILE=${AKAM_MQTT_FILE:-$AKAM_DIR/mqtt}
PASS_FILE=${AKAM_MQTT_PASS_FILE:-$AKAM_DIR/mqtt_pass}
AKAM_RTSP_BIN=${AKAM_RTSP_BIN:-/usr/bin/ak_rtsp}
USER_MAX=32
PASS_MAX=128
TEST_TRIES=${AKAM_MQTT_TEST_TRIES:-15}

die() {
    echo "akam_mqtt: $*" >&2
    exit 1
}

usage() {
    echo "usage: $0 set HOST PORT USER|- (password on stdin) | option NAME VALUE | disable | forget | status | test" >&2
    exit 2
}

valid_port() {
    case $1 in
        '' | *[!0-9]*) return 1 ;;
    esac
    [ "${#1}" -le 5 ] || return 1
    [ "$1" -ge 1 ] && [ "$1" -le 65535 ]
}

valid_host() {
    case $1 in
        '' | -* | .* | *[!A-Za-z0-9.-]*) return 1 ;;
    esac
    [ "${#1}" -le 63 ]
}

valid_user() {
    case $1 in
        '' | *[!A-Za-z0-9._@+-]*) return 1 ;;
    esac
    [ "${#1}" -le "$USER_MAX" ]
}

valid_id() {
    case $1 in
        '' | *[!A-Za-z0-9_-]*) return 1 ;;
    esac
    [ "${#1}" -le 32 ]
}

# A topic: 1 to $2 of A-Z a-z 0-9 _ - /, no leading, trailing or double slash.
valid_topic() {
    case $1 in
        '' | /* | */ | *//* | *[!A-Za-z0-9_/-]*) return 1 ;;
    esac
    [ "${#1}" -le "$2" ]
}

valid_interval() {
    case $1 in
        '' | *[!0-9]*) return 1 ;;
    esac
    [ "${#1}" -le 3 ] && [ "$1" -ge 10 ] && [ "$1" -le 300 ]
}

# read_cfg: sets C_EN C_HOST C_PORT C_USER C_ID C_BASE C_DISC C_PREFIX C_INT (empty = not in the file). Returns 0
# valid, 1 absent, 3 invalid (same rules as mqtt_cfg_parse in ak_rtsp: unknown or repeated key, bad value).
read_cfg() {
    C_EN='' C_HOST='' C_PORT='' C_USER='' C_ID='' C_BASE='' C_DISC='' C_PREFIX='' C_INT=''
    _seen=''
    [ -e "$MQTT_FILE" ] || return 1
    while IFS= read -r _l || [ -n "$_l" ]; do
        _l=${_l%"$(printf '\r')"}
        _k=${_l%%=*}
        [ "$_k" != "$_l" ] || return 3
        _v=${_l#*=}
        case " $_seen " in *" $_k "*) return 3 ;; esac
        _seen="$_seen $_k"
        case $_k in
            enabled) case $_v in 0 | 1) C_EN=$_v ;; *) return 3 ;; esac ;;
            host) valid_host "$_v" || return 3; C_HOST=$_v ;;
            port) valid_port "$_v" || return 3; C_PORT=$_v ;;
            user) [ -z "$_v" ] || valid_user "$_v" || return 3; C_USER=$_v ;;
            client_id) valid_id "$_v" || return 3; C_ID=$_v ;;
            base_topic) valid_topic "$_v" 64 || return 3; C_BASE=$_v ;;
            discovery) case $_v in on | off) C_DISC=$_v ;; *) return 3 ;; esac ;;
            discovery_prefix) valid_topic "$_v" 32 || return 3; C_PREFIX=$_v ;;
            interval) valid_interval "$_v" || return 3; C_INT=$_v ;;
            *) return 3 ;;
        esac
    done <"$MQTT_FILE" 2>/dev/null
    [ -n "$C_EN" ] && [ -n "$C_HOST" ] || return 3
    return 0
}

# write_private FILE: stdin to FILE atomically, mode 0600, in a 0700 directory. Runs in a pipeline (a subshell): it
# returns 1 on failure and the caller exits.
write_private() {
    umask 077
    mkdir -p "$AKAM_DIR" 2>/dev/null || { echo "akam_mqtt: cannot create $AKAM_DIR (is /etc/config writable?)" >&2; return 1; }
    chmod 700 "$AKAM_DIR" 2>/dev/null
    _tmp=$1.new.$$
    rm -f "$_tmp"
    cat >"$_tmp" 2>/dev/null || { rm -f "$_tmp"; echo "akam_mqtt: cannot write $_tmp" >&2; return 1; }
    chmod 600 "$_tmp" 2>/dev/null
    sync
    mv -f "$_tmp" "$1" 2>/dev/null || { rm -f "$_tmp"; echo "akam_mqtt: cannot replace $1" >&2; return 1; }
    sync
}

# write_cfg ENABLED: the file from the C_* variables.
write_cfg() {
    {
        printf 'enabled=%s\nhost=%s\nport=%s\n' "$1" "$C_HOST" "$C_PORT"
        printf 'user=%s\n' "$C_USER"
        [ -z "$C_ID" ] || printf 'client_id=%s\n' "$C_ID"
        [ -z "$C_BASE" ] || printf 'base_topic=%s\n' "$C_BASE"
        [ -z "$C_DISC" ] || printf 'discovery=%s\n' "$C_DISC"
        [ -z "$C_PREFIX" ] || printf 'discovery_prefix=%s\n' "$C_PREFIX"
        [ -z "$C_INT" ] || printf 'interval=%s\n' "$C_INT"
    } | write_private "$MQTT_FILE"
}

# ctl LINE...: first reply line in REPLY, exit status of "ak_rtsp ctl" in CTL_RC.
ctl() {
    REPLY=$("$AKAM_RTSP_BIN" ctl "$@" 2>&1)
    CTL_RC=$?
    REPLY=$(printf '%s\n' "$REPLY" | sed -n '1p')
}

# reload: tell ak_rtsp to read the files again. Returns 0 when applied or only pending a reboot.
reload() {
    ctl MQTT reload
    case $REPLY in
        OK*) echo "Applied now (ak_rtsp reloaded the setting)."; return 0 ;;
        *"unknown command"*) echo "Saved. This ak_rtsp is older and cannot reload: reboot the camera to apply."; return 0 ;;
    esac
    if [ "$CTL_RC" -ne 0 ] || [ -z "$REPLY" ]; then
        echo "Saved. ak_rtsp did not answer${REPLY:+ ($REPLY)}: it applies the next time ak_rtsp starts (reboot the camera)."
        return 0
    fi
    echo "akam_mqtt: saved, but ak_rtsp refused the reload: $REPLY (reboot the camera to apply)" >&2
    return 1
}

# pass_state: sets PASS_MSG: "none", "set" or a problem. Never reads the value out.
pass_state() {
    PASS_MSG=none
    [ -e "$PASS_FILE" ] || return 0
    _lsl=$(ls -ld "$PASS_FILE" 2>/dev/null) || { PASS_MSG="unreadable"; return 1; }
    case $_lsl in
        '-rw-------'* | '-r--------'*) PASS_MSG='set' ;;
        *) PASS_MSG="the password file is not private (mode 0600 needed): ak_rtsp ignores it"; return 1 ;;
    esac
    return 0
}

do_set() {
    [ "$#" -eq 3 ] || usage
    _host=$1
    _port=$2
    _user=$3
    valid_host "$_host" || { echo "akam_mqtt: HOST must be a host name or an IPv4 address (A-Z a-z 0-9 . -, at most 63)" >&2; exit 2; }
    valid_port "$_port" || { echo "akam_mqtt: PORT must be a whole number from 1 to 65535" >&2; exit 2; }
    if [ "$_user" = - ]; then
        _user=
    else
        valid_user "$_user" || { echo "akam_mqtt: USER must be 1 to $USER_MAX of A-Z a-z 0-9 . _ @ + - (or - for none)" >&2; exit 2; }
    fi
    _pw=
    IFS= read -r _pw || [ -n "$_pw" ] || _pw=
    _pw=${_pw%"$(printf '\r')"}
    [ "${#_pw}" -le "$PASS_MAX" ] || { _pw=; echo "akam_mqtt: the password is longer than $PASS_MAX characters; nothing changed." >&2; exit 2; }
    case $_pw in *[[:cntrl:]]*) _pw=; echo "akam_mqtt: the password has a control character; nothing changed." >&2; exit 2 ;; esac
    if [ -n "$_pw" ] && [ -z "$_user" ]; then
        _pw=
        echo "akam_mqtt: a password needs a user name (MQTT 3.1.1); nothing changed." >&2
        exit 2
    fi
    read_cfg   # keep the optional settings of a valid file
    case $? in 0) ;; *) C_ID='' C_BASE='' C_DISC='' C_PREFIX='' C_INT='' ;; esac
    C_HOST=$_host
    C_PORT=$_port
    C_USER=$_user
    # the password first: a crash in between must not leave "enabled" with a stale password
    if [ -n "$_pw" ]; then
        printf '%s\n' "$_pw" | write_private "$PASS_FILE" || exit 1
    else
        rm -f "$PASS_FILE"
    fi
    _pw=
    write_cfg 1 || exit 1
    echo "MQTT enabled: broker $_host:$_port, user ${_user:-none}, password $([ -e "$PASS_FILE" ] && echo set || echo none)."
    echo "This is plain TCP: the password travels in clear on your network. The camera makes one outgoing connection."
    reload
}

do_option() {
    [ "$#" -eq 2 ] || usage
    read_cfg
    case $? in
        0) ;;
        1) die "no MQTT broker saved yet: run '$0 set HOST PORT USER' first" ;;
        *) die "the MQTT file is INVALID: run '$0 set HOST PORT USER' to write it again" ;;
    esac
    case $1 in
        base_topic) valid_topic "$2" 64 || { echo "akam_mqtt: base_topic: 1 to 64 of A-Z a-z 0-9 _ - /, no leading, trailing or double /" >&2; exit 2; }; C_BASE=$2 ;;
        client_id) valid_id "$2" || { echo "akam_mqtt: client_id: 1 to 32 of A-Z a-z 0-9 _ -" >&2; exit 2; }; C_ID=$2 ;;
        discovery) case $2 in on | off) C_DISC=$2 ;; *) echo "akam_mqtt: discovery must be on or off" >&2; exit 2 ;; esac ;;
        discovery_prefix) valid_topic "$2" 32 || { echo "akam_mqtt: discovery_prefix: 1 to 32 of A-Z a-z 0-9 _ - /" >&2; exit 2; }; C_PREFIX=$2 ;;
        interval) valid_interval "$2" || { echo "akam_mqtt: interval must be a whole number of seconds from 10 to 300" >&2; exit 2; }; C_INT=$2 ;;
        *) echo "akam_mqtt: unknown option $1 (base_topic, client_id, discovery, discovery_prefix, interval)" >&2; exit 2 ;;
    esac
    write_cfg "$C_EN" || exit 1
    echo "Saved: $1=$2."
    reload
}

do_disable() {
    read_cfg
    case $? in
        1) echo "MQTT was already disabled."; return 0 ;;
        3) echo "akam_mqtt: the MQTT file is INVALID (MQTT is off anyway); '$0 forget' removes it." ; return 0 ;;
    esac
    C_PORT=${C_PORT:-1883}
    write_cfg 0 || exit 1
    echo "MQTT disabled (the broker settings are kept: '$0 forget' deletes them)."
    reload
}

do_forget() {
    rm -f "$MQTT_FILE" "$PASS_FILE"
    sync
    echo "MQTT turned off; the settings and the password file are deleted."
    reload
}

do_status() {
    read_cfg
    _rc=$?
    case $_rc in
        1) echo "MQTT: disabled (default)" ;;
        3) echo "MQTT: INVALID file $MQTT_FILE (treated as disabled; run set, disable or forget)" ;;
        *)
            if [ "$C_EN" = 1 ]; then echo "MQTT: enabled"; else echo "MQTT: disabled"; fi
            echo "broker: $C_HOST:$C_PORT, user ${C_USER:-none}"
            echo "options: discovery ${C_DISC:-on}, prefix ${C_PREFIX:-homeassistant}, base topic ${C_BASE:-akam/<hostname>}, client id ${C_ID:-<hostname>}, interval ${C_INT:-30} s"
            ;;
    esac
    pass_state
    echo "password: $PASS_MSG"
    ctl GET mqtt.state
    case $REPLY in
        mqtt.state=*) echo "state: ${REPLY#mqtt.state=}" ;;
        *) echo "state: unknown (this ak_rtsp does not report it)" ;;
    esac
    [ "$_rc" -ne 3 ] || return 3
    return 0
}

do_test() {
    read_cfg
    case $? in
        1) die "no MQTT broker saved yet: run '$0 set HOST PORT USER' first" ;;
        3) die "the MQTT file is INVALID: run '$0 set HOST PORT USER' to write it again" ;;
    esac
    ctl MQTT test
    case $REPLY in
        "OK mqtt.test=running") ;;
        *"unknown command"*) die "this ak_rtsp is older and has no connection test: update the image" ;;
        *) die "ak_rtsp did not start the test${REPLY:+ ($REPLY)}" ;;
    esac
    _n=0
    while [ "$_n" -lt "$TEST_TRIES" ]; do
        ctl GET mqtt.test
        case $REPLY in
            mqtt.test=ok)
                echo "MQTT test: ok (the broker at $C_HOST:$C_PORT accepted the login)."
                return 0
                ;;
            mqtt.test=error:*)
                _r=${REPLY#mqtt.test=error:}
                case $_r in
                    dns) _m="the name $C_HOST could not be resolved (use the broker's IP address)" ;;
                    connect) _m="nothing answered at $C_HOST:$C_PORT (wrong address or port, broker down, firewall)" ;;
                    timeout) _m="the broker did not answer in time" ;;
                    closed) _m="the broker closed the connection" ;;
                    protocol) _m="the answer was not MQTT (is $C_PORT really an MQTT port, not TLS?)" ;;
                    refused_credentials) _m="the broker refused the user name or password" ;;
                    refused_not_authorized) _m="the broker says this user is not authorised" ;;
                    refused_id) _m="the broker refused the client id" ;;
                    refused_protocol) _m="the broker does not speak MQTT 3.1.1" ;;
                    refused_unavailable) _m="the broker is not available" ;;
                    config) _m="the saved settings are not usable" ;;
                    *) _m="refused ($_r)" ;;
                esac
                echo "MQTT test: FAILED: $_m." >&2
                return 1
                ;;
        esac
        _n=$((_n + 1))
        sleep 1
    done
    echo "MQTT test: no result after $TEST_TRIES seconds." >&2
    return 1
}

case ${1:-} in
    set) shift; do_set "$@" ;;
    option) shift; do_option "$@" ;;
    disable) [ "$#" -eq 1 ] || usage; do_disable ;;
    forget) [ "$#" -eq 1 ] || usage; do_forget ;;
    status) [ "$#" -eq 1 ] || usage; do_status ;;
    test) [ "$#" -eq 1 ] || usage; do_test ;;
    *) usage ;;
esac
