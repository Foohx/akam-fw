#!/usr/bin/env bash
# Integration test of the signed control API of the 1080p (convergence P8), no camera: the REAL HTTP server, router, rate limits, key store,
# audit, event ring and registry of the shared API with the 1080p binding (firmware/lsc-indoor-1080p-ak3918/api_bind.c, ak1080_settings.c), the real LED engine
# and FAKE hardware modules (tests/native/ak1080_fakes.h), built under ASan + UBSan, listening on loopback and driven by curl with the
# reference signer tools/akam-api-sign.sh. The generic protocol (hygiene, replay, lockout, slow clients, caps, challenge flow) is covered
# in depth by tests/rot2k_control_api_http_test.sh on the SAME shared code; this test proves what the 1080p adds or differs in: the key
# tool and the closed port without a key, the 41 registry rows and the actions of this model, no PTZ/track/button route, scopes, the
# settings reaching the module setters, events (night, setting), the flash limiter, the audit file, and that no 2K-only code is compiled.
# Needs: a C compiler (CC, default cc), curl, openssl, awk, xxd. Env: SH_UNDER_TEST for the signer (default sh).
# shellcheck disable=SC2015,SC2059,SC2034  # test idioms: "cond && ok || ko" reporters, printf with an escape format, loop counters
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
SRC=$REPO/platforms/anyka-ak3918
SIGN=$REPO/tools/akam-api-sign.sh
CC=${CC:-cc}
SH_UNDER_TEST=${SH_UNDER_TEST:-sh}
read -r -a SHCMD <<<"$SH_UNDER_TEST"
for t in curl openssl xxd; do command -v "$t" >/dev/null 2>&1 || { echo "skip: $t not installed"; exit 0; }; done
WORK=$(mktemp -d)
SRVPIDS=()
cleanup() {
    exec 3>&- 2>/dev/null || true
    for p in "${SRVPIDS[@]:-}"; do [ -z "$p" ] || kill "$p" 2>/dev/null || true; done
    if [ -n "${KEEP_WORK:-}" ]; then echo "work dir kept: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { # check <description> <command...>
    local d=$1
    shift
    if "$@" >/dev/null 2>&1; then ok "$d"; else ko "$d"; fi
}

echo "== build the server harness (ASan/UBSan)"
SRV=$WORK/apisrv
# shellcheck disable=SC2086
if ! $CC -std=gnu99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
 -I"$SRC" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" -I"$REPO/tests/native" "${SHARED_INC[@]}" -o "$SRV" "$REPO/tests/native/ak1080_api_server.c" \
    "$REPO/firmware/lsc-indoor-1080p-ak3918/api_bind.c" "$REPO/firmware/lsc-indoor-1080p-ak3918/settings.c" "$SRC/leds.c" "$SRC/video_cfg.c" \
    "$REPO/src/features/light/led_pat.c" "$REPO/src/features/light/light_logic.c" "$REPO/src/core/cfg_file.c" \
    "$REPO/src/core/settings_reg.c" "$REPO/src/api/json_min.c" "$REPO/src/api/api_keys.c" "$REPO/src/api/api_sig.c" "$REPO/src/api/api_limit.c" \
    "$REPO/src/api/api_events.c" "$REPO/src/api/api_core.c" "$REPO/src/api/api_httpd.c" "$REPO/src/net/http/http_req.c" "$REPO/src/net/crypto/sha256.c" \
    "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/crypto/md5.c"; then
    echo "  FAIL - server harness build"
    exit 1
fi
ok "server harness builds"

# ---------------------------------------------------------------------------------------------- helpers
D=""
PORT=""
SP=""
start_srv() { # start_srv <dir> [harness flags...]; leaves stdin open on fd 3
    D=$1
    shift
    mkdir -p "$D"
    rm -f "$D/in" "$D/out"
    mkfifo "$D/in"
    "$SRV" "$D" "$@" <"$D/in" >"$D/out" 2>"$D/err" &
    SP=$!
    SRVPIDS+=("$SP")
    exec 3>"$D/in"
    local i
    for i in $(seq 1 100); do grep -q '^PORT' "$D/out" 2>/dev/null && break; sleep 0.1; done
    PORT=$(sed -n 's/^PORT //p' "$D/out" | head -1)
}
stop_srv() {
    echo quit >&3 2>/dev/null || true
    exec 3>&-
    wait "$SP" 2>/dev/null
    local rc=$?
    SP=""
    return $rc
}
srvq() { # srvq <command>: ask the harness a question on stdin, print the answer line
    local before
    before=$(wc -l <"$D/out")
    echo "$1" >&3
    local i
    for i in $(seq 1 50); do [ "$(wc -l <"$D/out")" -gt "$before" ] && break; sleep 0.1; done
    tail -1 "$D/out"
}
newkey() { # newkey <dir> <scope> <name>: sets KID, KSEC
    local out
    out=$("$SRV" "$1" key new --scope "$2" --name "$3")
    KID=$(printf '%s\n' "$out" | sed -n 's/^key_id: *//p')
    KSEC=$(printf '%s\n' "$out" | sed -n 's/^secret: *//p')
}
STATUS=""
BODY=""
HDRS=""
req() { # req <key id> <secret> METHOD PATH [BODY] [QUERY]  -> STATUS, BODY, HDRS (signed request)
    local kid=$1 sec=$2 m=$3 p=$4 b=${5:-} q=${6:-} h l out args=()
    h=$(AKAM_KEY_ID=$kid AKAM_KEY_SECRET=$sec "${SHCMD[@]}" "$SIGN" ${b:+--body "$b"} "$m" "$p" "$q") || { STATUS=signer; return 0; }
    while IFS= read -r l; do args+=(-H "$l"); done <<<"$h"
    out=$(curl -sS -i --max-time 40 -X "$m" "http://127.0.0.1:$PORT$p${q:+?$q}" "${args[@]}" ${b:+-H 'Content-Type: application/json' --data-binary "$b"} || true)
    STATUS=$(printf '%s' "$out" | head -1 | awk '{print $2}')
    HDRS=$(printf '%s' "$out" | sed -n '1,/^\r\?$/p')
    BODY=$(printf '%s' "$out" | sed '1,/^\r\?$/d')
}
admin() { req "$AID" "$ASEC" "$@"; }
want() { # want <description> <status> [body regex]
    if [ "$STATUS" = "$2" ] && { [ -z "${3:-}" ] || printf '%s' "$BODY" | grep -Eq -- "$3"; }; then ok "$1"; else ko "$1 (status=$STATUS body=$(printf '%s' "$BODY" | head -c 200))"; fi
}
vals() { srvq 'vals?'; }
wantvals() { # wantvals <description> <regex>
    local v
    v=$(vals)
    if printf '%s' "$v" | grep -Eq -- "$2"; then ok "$1"; else ko "$1 ($v)"; fi
}

# ================================================================== phase 0: no key, no port
echo "== no key: nothing listens"
D0=$WORK/p0
start_srv "$D0" --fast
if [ "$(srvq 'port?')" = "PORT 0" ] && [ "$PORT" = "0" ]; then ok "no API key: nothing listens"; else ko "no API key: nothing listens ($PORT)"; fi
stop_srv || true

# ================================================================== phase 1: key tool, auth, scopes
echo "== key tool, authentication, scopes"
D1=$WORK/p1
mkdir -p "$D1"
newkey "$D1" admin main
AID=$KID
ASEC=$KSEC
[ "$(stat -c %a "$D1/api_keys")" = 600 ] && ok "key tool: api_keys is 0600" || ko "key tool: api_keys is 0600"
[ ${#ASEC} = 64 ] && ok "key tool: secret is 64 hex" || ko "key tool: secret is 64 hex"
check "key tool: list shows the id and no secret" bash -c "'$SRV' '$D1' key list | grep -q '$AID' && ! '$SRV' '$D1' key list | grep -q '$ASEC'"
check "key tool: a duplicate name is refused" bash -c "! '$SRV' '$D1' key new --scope read --name main"
check "key tool: the last admin cannot be revoked" bash -c "! '$SRV' '$D1' key revoke main"
check "key tool: bad scope is a usage error" bash -c "! '$SRV' '$D1' key new --scope root --name x"
start_srv "$D1" --fast
[ -n "$PORT" ] && [ "$PORT" != 0 ] && ok "listening once a key exists (port $PORT)" || ko "listening once a key exists"
HELLO=$(curl -sS -i "http://127.0.0.1:$PORT/akam/v1/hello")
printf '%s' "$HELLO" | head -1 | grep -q 200 && printf '%s' "$HELLO" | grep -Eq '"challenge":"[0-9a-f]{32}"' && ok "hello: unsigned, 32 hex challenge" || ko "hello"
curl -sS -i "http://127.0.0.1:$PORT/akam/v1/info" | head -1 | grep -q 401 && ok "unsigned /info is 401" || ko "unsigned /info is 401"
admin GET /akam/v1/info
want "signed GET /info names the 1080p" 200 '"model":"lsc-smart-indoor-1080p".*"scope":"admin"'
want "info: version string and the event cursor" 200 '"app":"b1-test".*"events_seq":[0-9]+'
h=$(AKAM_KEY_ID=$AID AKAM_KEY_SECRET=$ASEC "${SHCMD[@]}" "$SIGN" GET /akam/v1/info)
args=()
while IFS= read -r l; do args+=(-H "$l"); done <<<"$h"
s1=$(curl -s -o /dev/null -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
s2=$(curl -s -o /dev/null -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
[ "$s1" = 200 ] && [ "$s2" = 401 ] && ok "a replayed request is refused (200 then 401)" || ko "replay ($s1 $s2)"
req 00000000 "$ASEC" GET /akam/v1/info
want "unknown key id: 401 bad_signature" 401 'bad_signature'
admin POST /akam/v1/keys '{"name":"viewer","scope":"read"}'
want "POST /keys makes a read key (201, secret once)" 201 '"secret":"[0-9a-f]{64}"'
RID=$(printf '%s' "$BODY" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
RSEC=$(printf '%s' "$BODY" | sed -n 's/.*"secret":"\([0-9a-f]*\)".*/\1/p')
admin POST /akam/v1/keys '{"name":"tool","scope":"write"}'
WID=$(printf '%s' "$BODY" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
WSEC=$(printf '%s' "$BODY" | sed -n 's/.*"secret":"\([0-9a-f]*\)".*/\1/p')
req "$RID" "$RSEC" GET /akam/v1/state
want "a read key reads /state" 200 '"sys.cpu_pct":"37"'
req "$RID" "$RSEC" PATCH /akam/v1/settings '{"led.mode":"off"}'
want "a read key cannot PATCH (403 scope)" 403 'scope'
req "$WID" "$WSEC" GET /akam/v1/keys
want "a write key cannot list keys (403 scope)" 403 'scope'
req "$WID" "$WSEC" GET /akam/v1/audit
want "a write key cannot read the audit (403 scope)" 403 'scope'
req "$RID" "$RSEC" POST /akam/v1/actions/led.blip_stop
want "a read key cannot call an action (403)" 403 'scope'
wantvals "a refused write changed nothing" 'led=on '

# ================================================================== phase 2: the rows of this model
echo "== schema, settings, state: the rows of the 1080p"
admin GET /akam/v1/schema
want "schema: led, night, image, video0, onvif, mqtt rows" 200 '"key":"led.mode".*"key":"night.ir_led".*"key":"image.brightness".*"key":"video0.kbps".*"key":"onvif.enabled".*"key":"mqtt.enabled"'
want "schema: the registry names of the 2K (patterns, state rows)" 200 '"key":"led.stream".*"key":"sys.cpu_pct".*"key":"net.wifi_rssi".*"key":"night.active"'
want "schema: video rows apply at the next start, led rows live" 200 '"key":"video0.kbps".*"apply":"restart_component".*"key":"led.mode".*"apply":"live"|"key":"led.mode".*"apply":"live".*"key":"video0.kbps".*"apply":"restart_component"'
want "schema: the three actions of this model" 200 '"actions":\[\{"name":"led.blip".*led.blip_stop.*settings.reset'
printf '%s' "$BODY" | grep -Eq 'ptz|track\.|detect\.|zones|button|wifi_band|wifi_country|video1' && ko "schema has no PTZ, tracking, detection, zones, button, 5 GHz or second stream row" || ok "schema has no PTZ, tracking, detection, zones, button, 5 GHz or second stream row"
NROWS=$(printf '%s' "$BODY" | grep -o '"key":' | wc -l | tr -d ' ')
[ "$NROWS" = 41 ] && ok "schema: 41 rows" || ko "schema: $NROWS rows (want 41)"
admin GET /akam/v1/settings
want "settings: writable values as canonical text" 200 '"led.mode":"on".*"video0.kbps":"1536"|"video0.kbps":"1536".*"led.mode":"on"'
printf '%s' "$BODY" | grep -q 'sys.cpu_pct' && ko "settings does not list read-only state" || ok "settings does not list read-only state"
admin GET /akam/v1/settings "" "prefix=video0."
want "settings?prefix=video0. lists only video0 writable rows" 200 '"video0.fps":"15"'
printf '%s' "$BODY" | grep -q 'led.mode' && ko "prefix filter" || ok "prefix filter"
admin GET /akam/v1/settings/led.stream
want "one setting with metadata" 200 '"key":"led.stream".*"grammar":"led".*"default":"1:1000:1000:0".*"value":"1:1000:1000:0"'
admin GET /akam/v1/settings/ptz.pan
want "an absent PTZ row is 404 unknown_key" 404 'unknown_key'
admin GET /akam/v1/state
want "state: hal_sys rows" 200 '"sys.cpu_pct":"37".*"sys.mem_avail_kb":"21000"|"sys.mem_avail_kb":"21000".*"sys.cpu_pct":"37"'
want "state: rtsp, night, service rows" 200 '"rtsp.clients":"1".*"rtsp.auth":"on"|"rtsp.auth":"on".*"rtsp.clients":"1"'
want "state: night.active day, api.keys, unknown Wi-Fi level" 200 '"night.active":"0"'
printf '%s' "$BODY" | grep -Eq '"api.keys":"3"' && ok "state: api.keys counts the three keys" || ko "state: api.keys ($(printf '%s' "$BODY" | head -c 300))"
printf '%s' "$BODY" | grep -Eq '"net.wifi_rssi":"-1000000"' && ok "state: an unknown value is -1000000" || ko "state: unknown value"
printf '%s' "$BODY" | grep -Eq 'net.ssid|net.mac|net.ip|password|secret' && ko "state: no identity data and no secret" || ok "state: no identity data and no secret"
admin GET /akam/v1/ptz/presets
want "no PTZ route: 404 unknown_path" 404 'unknown_path'
admin GET /akam/v1/button
want "no button route: 404 unknown_path" 404 'unknown_path'
admin GET /akam/v1/modes
want "no modes route: 404 unknown_path" 404 'unknown_path'

# ================================================================== phase 3: PATCH reaches the modules
echo "== PATCH: the setters of the modules"
srvq 'flash 100' >/dev/null # the limiter has its own phase below
admin PATCH /akam/v1/settings '{"led.mode":"auto"}'
want "led.mode=auto: applied" 200 '"led.mode":\{"ok":true,"apply":"applied"'
wantvals "...the 8091 setter received led.status=auto (persist, authenticated)" 'led=auto '
admin PATCH /akam/v1/settings '{"image.contrast":-5,"image.brightness":12,"night.ir_led":"off"}'
want "JSON integers become canonical text, three rows in one PATCH" 200 '"image.contrast":\{"ok":true'
wantvals "...every setter ran" 'brightness=12 contrast=-5 .* ir=off '
admin PATCH /akam/v1/settings '{"image.contrast":51}'
want "out of range: 422 range" 422 '"error":"range"'
admin PATCH /akam/v1/settings '{"image.contrast":"+5"}'
want "not canonical: 422 format" 422 '"error":"format"'
admin PATCH /akam/v1/settings '{"sys.cpu_pct":5}'
want "a state row is read-only: 422 readonly" 422 'readonly'
admin PATCH /akam/v1/settings '{"ptz.pan":5}'
want "an absent row: 422 unknown_key" 422 'unknown_key'
admin PATCH /akam/v1/settings '{"image.brightness":20,"image.contrast":99}'
want "all or nothing: one bad value refuses the batch" 422 'not_applied'
wantvals "...the good one was not applied" 'brightness=12 '
admin PATCH /akam/v1/settings '{"video0.kbps":2000,"video0.fps":"25","video0.gop":3}'
want "video rows: saved, applied at the next start" 200 '"apply":"restart_component"'
wantvals "...the video module got the three values" 'kbps=2000 fps=25 gop=3 '
srvq 'flash 100' >/dev/null
admin PATCH /akam/v1/settings '{"video0.fps":30}'
want "fps 30 is not an option: 422 range" 422 'range'
admin PATCH /akam/v1/settings '{"video0.min_qp":51}'
want "min_qp above max_qp over the whole patch: 422 check" 422 '"error":"check"'
admin PATCH /akam/v1/settings '{"video0.min_qp":30,"video0.max_qp":45}'
want "both QP in one patch: applied" 200 '"video0.max_qp":\{"ok":true'
admin PATCH /akam/v1/settings '{"led.nowifi":"3:250:250:2000","led.idle":"on"}'
want "LED patterns: applied" 200 '"led.nowifi":\{"ok":true'
admin PATCH /akam/v1/settings '{"led.nowifi":"1:10:10:0"}'
want "a pattern the engine cannot do: 422 check" 422 'check'
admin GET /akam/v1/settings/led.nowifi
want "the pattern reads back" 200 '"value":"3:250:250:2000"'
admin PATCH /akam/v1/settings '{"night.mode":"night"}'
want "night.mode" 200 '"night.mode":\{"ok":true'
admin PATCH /akam/v1/settings '{}'
want "empty patch: 422 empty" 422 'empty'
admin PATCH /akam/v1/settings '{"onvif.enabled":true}'
want "onvif.enabled=true (boolean JSON): applied, the flag file written" 200 '"onvif.enabled":\{"ok":true'
check "...the flag file says enabled=1" grep -q '^enabled=1' "$D/onvif"
admin GET /akam/v1/state
want "...and the service state follows" 200 '"onvif.state":"on:80"'
admin PATCH /akam/v1/settings '{"mqtt.enabled":true}'
want "mqtt.enabled without a broker file: refused (422 setter)" 422 'setter'

# ================================================================== phase 4: actions
echo "== actions"
srvq 'flash 100' >/dev/null # the limiter has its own phase below
admin POST /akam/v1/actions/led.blip '{"pattern":"off","ms":2000}'
want "led.blip: ok" 200 '"ok":true'
[ "$(srvq 'led?')" = "LED 0" ] && ok "...the LED went dark" || ko "...the LED went dark ($(srvq 'led?'))"
admin POST /akam/v1/actions/led.blip_stop
want "led.blip_stop: ok" 200 '"ok":true'
[ "$(srvq 'led?')" = "LED 1" ] && ok "...the rules came back, solid on" || ko "...the rules came back ($(srvq 'led?'))"
admin POST /akam/v1/actions/led.blip '{"pattern":"x"}'
want "led.blip with a bad pattern: 422 args" 422 'args'
admin POST /akam/v1/actions/led.blip '{"pattern":"on","ms":999999}'
want "led.blip with a bad duration: 422 args" 422 'args'
admin POST /akam/v1/actions/nope
want "unknown action: 404" 404 'unknown_action'
admin POST /akam/v1/actions/ptz.home
want "a PTZ action does not exist: 404" 404 'unknown_action'
admin POST /akam/v1/actions/settings.reset '{"prefix":"led."}'
want "settings.reset led.: ok" 200 'reset'
admin GET /akam/v1/settings
want "...the LED rows are back to their defaults" 200 '"led.mode":"on".*"led.nowifi":"1:500:500:0"|"led.nowifi":"1:500:500:0".*"led.mode":"on"'
wantvals "...through the setter" 'led=on '
admin POST /akam/v1/actions/settings.reset '{"prefix":"video0."}'
want "settings.reset video0.: one patch, the QP pair is checked together" 200 'reset 5 failed 0'
wantvals "...the video module is back to the defaults" 'kbps=1536 fps=15 gop=2 minqp=28 maxqp=43'
admin POST /akam/v1/actions/settings.reset '{"key":"nope"}'
want "settings.reset of an unknown key: 404" 404 'unknown_key'
admin POST /akam/v1/actions/settings.reset '{"key":"led.mode","prefix":"led."}'
want "settings.reset needs exactly one of key and prefix: 422" 422 'args'
admin POST /akam/v1/actions/settings.reset '{"prefix":""}'
want "settings.reset with an empty prefix is refused (it would reset every row): 422" 422 'args'

# ================================================================== phase 5: events
echo "== events"
srvq 'flash 100' >/dev/null # the limiter has its own phase below
admin GET /akam/v1/events
want "events without a cursor: the sequence, no events" 200 '"events":\[\]'
SEQ=$(printf '%s' "$BODY" | sed -n 's/.*"seq":\([0-9]*\).*/\1/p' | head -1)
srvq 'night 1' >/dev/null
admin GET /akam/v1/events "" "since=$SEQ"
want "the night event: the day/night switch" 200 '"type":"night","data":\{"night":true,"ir_on":true\}'
SEQ=$(printf '%s' "$BODY" | sed -n 's/^{"seq":\([0-9]*\).*/\1/p' | head -1)
admin PATCH /akam/v1/settings '{"image.saturation":3}'
admin GET /akam/v1/events "" "since=$SEQ"
want "a PATCH is a setting event naming the keys" 200 '"type":"setting","data":\{"keys":"image.saturation"\}'
SEQ=$(printf '%s' "$BODY" | sed -n 's/^{"seq":\([0-9]*\).*/\1/p' | head -1)
( sleep 1; srvq 'night 0' >/dev/null ) &
BG=$!
t0=$(date +%s)
admin GET /akam/v1/events "" "since=$SEQ&timeout=10"
t1=$(date +%s)
wait "$BG" 2>/dev/null || true
want "a long poll is answered by the event, not at its deadline" 200 '"type":"night","data":\{"night":false'
[ $((t1 - t0)) -lt 8 ] && ok "...within $((t1 - t0)) s" || ko "...took $((t1 - t0)) s"
admin GET /akam/v1/events "" "since=abc"
want "a bad cursor: 400 bad_query" 400 'bad_query'

# ================================================================== phase 6: the flash limiter
echo "== flash limiter"
srvq 'flash 1' >/dev/null
admin PATCH /akam/v1/settings '{"image.brightness":1}'
want "one token left: applied" 200 '"ok":true'
admin PATCH /akam/v1/settings '{"image.brightness":2}'
want "no token: 429 flash_rate" 429 'flash_rate'
printf '%s' "$HDRS" | grep -qi '^Retry-After:' && ok "...with Retry-After" || ko "...with Retry-After"
wantvals "...and nothing was applied" 'brightness=1 '
srvq 'flash 1' >/dev/null
admin PATCH /akam/v1/settings '{"image.contrast":777}'
want "a refused patch gives its token back (422)" 422 'range'
admin PATCH /akam/v1/settings '{"image.contrast":4}'
want "...so the next one is applied" 200 '"ok":true'
admin GET /akam/v1/settings
want "reads are not limited by the flash limiter" 200 '"image.contrast":"4"'
srvq 'flash 6' >/dev/null

# ================================================================== phase 7: audit and keys
echo "== audit, key management"
admin GET /akam/v1/audit
want "audit: the writes are in the ring with the key id and the peer" 200 "\"key\":\"$AID\".*\"req\":\"PATCH /akam/v1/settings\""
printf '%s' "$BODY" | grep -Eq 'secret|[0-9a-f]{64}' && ko "audit holds no secret" || ok "audit holds no secret"
for i in $(seq 1 40); do [ -s "$D/audit" ] && break; sleep 0.25; done
[ -s "$D/audit" ] && ok "the audit file of the writes is persisted (mode $(stat -c %a "$D/audit"))" || ko "the audit file is persisted"
[ "$(stat -c %a "$D/audit")" = 600 ] && ok "...0600" || ko "...0600"
grep -q '"what":"[^"]*"' "$D/audit" && ! grep -q 'GET /akam/v1/state' "$D/audit" && ok "...writes only, no read in the file" || ko "...writes only"
admin GET /akam/v1/keys
want "GET /keys lists three keys and no secret" 200 '"name":"viewer"'
printf '%s' "$BODY" | grep -q "$RSEC" && ko "key list shows no secret" || ok "key list shows no secret"
admin DELETE "/akam/v1/keys/$RID"
want "DELETE /keys/<id> revokes" 200 '"ok":true'
req "$RID" "$RSEC" GET /akam/v1/state
want "a revoked key is refused" 401 'bad_signature'
admin DELETE "/akam/v1/keys/$AID"
want "the last admin cannot be revoked (409 last_admin)" 409 'last_admin'
stop_srv && ok "the server exits cleanly under ASan/UBSan" || ko "the server exits cleanly"
grep -E "ERROR|runtime error" "$D/err" >/dev/null 2>&1 && { ko "no sanitizer report"; head -5 "$D/err"; } || ok "no sanitizer report"

# ================================================================== phase 8: restart, the port closes without a key
echo "== persistence of the audit, the port follows the key file"
start_srv "$D1" --fast
admin GET /akam/v1/audit
want "after a restart the persisted writes are in the audit" 200 'PATCH /akam/v1/settings'
"$SRV" "$D1" key revoke tool >/dev/null
"$SRV" "$D1" key revoke "$AID" --force >/dev/null
for i in $(seq 1 30); do [ "$(srvq 'port?')" = "PORT 0" ] && break; sleep 0.3; done
[ "$(srvq 'port?')" = "PORT 0" ] && ok "revoking the last key closes the port, without a restart" || ko "the port closes"
newkey "$D1" admin again
for i in $(seq 1 30); do [ "$(srvq 'port?')" != "PORT 0" ] && break; sleep 0.3; done
[ "$(srvq 'port?')" != "PORT 0" ] && ok "a new key opens it again, without a restart" || ko "the port opens"
stop_srv || true

# ================================================================== phase 9: a file that is not private closes the API
echo "== key file hygiene"
D9=$WORK/p9
mkdir -p "$D9"
newkey "$D9" admin main
chmod 644 "$D9/api_keys"
start_srv "$D9" --fast
[ "$PORT" = 0 ] && ok "a group/world readable key file keeps the API closed (fail closed)" || ko "a readable key file keeps the API closed"
[ -f "$D9/api_keys" ] && ok "...and the file is kept, never wiped" || ko "...the file is kept"
chmod 600 "$D9/api_keys"
for i in $(seq 1 30); do [ "$(srvq 'port?')" != "PORT 0" ] && break; sleep 0.3; done
[ "$(srvq 'port?')" != "PORT 0" ] && ok "...and mode 0600 opens it" || ko "mode 0600 opens it"
stop_srv || true

# ================================================================== static: no 2K-only code in the 1080p
echo "== no 2K-only code compiled into the 1080p"
check "1080p model.mk: PTZ BUTTON SCENE PATROL TRACK are 0" bash -c "grep -Eq '^AKAM_FEAT_PTZ +:= 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_BUTTON +:= 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_SCENE +:= 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_PATROL +:= 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_TRACK +:= 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk'"
check "1080p model.mk: API, SYS and LIGHT are 1" bash -c "grep -Eq '^AKAM_FEAT_API +:= 1' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_SYS +:= 1' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' && grep -Eq '^AKAM_FEAT_LIGHT +:= 1' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk'"
check "make features: PTZ BUTTON SCENE PATROL TRACK are off" bash -c "make -s -C '$SRC' features | grep '^off:' | grep -q 'PTZ BUTTON SCENE PATROL TRACK'"
check "the 1080p SRCS name no ptz, button, scene, patrol, track or 5 GHz object" bash -c "! sed -n '/^SRCS/,\$p' '$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk' | grep -Eq 'ptz|button|scene|patrol|track|wifi5|hal_input|akam2k'"
check "the API binding has no extra route (no PTZ, no button)" bash -c "grep -q 'env->n_ext = 0' '$REPO/firmware/lsc-indoor-1080p-ak3918/api_bind.c'"

echo
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" = 0 ]
