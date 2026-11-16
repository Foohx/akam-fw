#!/usr/bin/env bash
# Integration test of the local control API server (specs/control-api.md phase 2), no camera: the REAL HTTP server, router, rate
# limits, registry table, settings worker and key store of the 2K app (tests/rot2k_api_server.c) with FAKE hardware modules, built under
# ASan + UBSan, listening on loopback and driven by curl with the reference signer tools/akam-api-sign.sh.
# Needs: a C compiler (CC, default cc), curl, openssl, awk, xxd. Env: SH_UNDER_TEST for the signer (default sh).
# shellcheck disable=SC2015,SC2059,SC2034  # test idioms: "cond && ok || ko" reporters, printf with an escape format, loop counters
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
APP=$REPO/firmware/lsc-rotatable-2k-ak3918av130/app
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
    rm -rf "$WORK"
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
    -I"$APP" -I"$REPO/src/ak_rtsp" -I"$REPO/src/common/net" -I"$REPO/src/common/logic" -o "$SRV" "$HERE/rot2k_api_server.c" \
    "$APP/akam2k_httpd.c" "$APP/akam2k_api.c" "$APP/akam2k_settings.c" "$APP/akam2k_apievt.c" "$APP/http_req.c" "$APP/api_limit.c" \
    "$APP/api_keys.c" "$APP/api_sig.c" "$APP/sha256.c" "$APP/json_min.c" "$APP/settings_reg.c" "$APP/akam2k_netreg.c" \
    "$APP/akam2k_store.c" "$APP/akam2k_video.c" "$APP/akam2k_vidcfg.c" "$REPO/src/common/net/rtsp_auth.c" "$REPO/src/common/net/md5.c"; then
    echo "  FAIL - server harness build"
    exit 1
fi
ok "server harness builds"

# ---------------------------------------------------------------------------------------------- helpers
D=""          # state dir of the running instance
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
admin() { req "$AID" "$ASEC" "$@"; } # as the admin key of the current phase
want() { # want <description> <status> [body regex]
    if [ "$STATUS" = "$2" ] && { [ -z "${3:-}" ] || printf '%s' "$BODY" | grep -Eq -- "$3"; }; then ok "$1"; else ko "$1 (status=$STATUS body=$(printf '%s' "$BODY" | head -c 200))"; fi
}
raw() { # raw <printf-format text>: send bytes over TCP, print the first line of the reply
    local out
    out=$(exec 5<>"/dev/tcp/127.0.0.1/$PORT" && printf "$1" >&5 && timeout 8 head -1 <&5 | tr -d '\r') || true
    printf '%s' "$out"
}

# ================================================================== phase 0: no key, no port
echo "== no key: the port stays closed"
D0=$WORK/p0
start_srv "$D0" --fast
if [ "$(srvq 'port?')" = "PORT 0" ] && [ "$PORT" = "0" ]; then ok "no API key: nothing listens"; else ko "no API key: nothing listens ($PORT)"; fi
stop_srv || true

# ================================================================== phase 1: key tool, auth, hygiene, keys API
echo "== key tool, authentication, request hygiene, key management"
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
printf '%s' "$HELLO" | grep -qi '^Connection: close' && ok "replies say Connection: close" || ko "replies say Connection: close"
curl -sS -i "http://127.0.0.1:$PORT/akam/v1/info" | head -1 | grep -q 401 && ok "unsigned /info is 401" || ko "unsigned /info is 401"
admin GET /akam/v1/info
want "signed GET /info" 200 '"api":1.*"scope":"admin"'
# replay: the exact same signed request twice
h=$(AKAM_KEY_ID=$AID AKAM_KEY_SECRET=$ASEC "${SHCMD[@]}" "$SIGN" GET /akam/v1/info)
args=()
while IFS= read -r l; do args+=(-H "$l"); done <<<"$h"
s1=$(curl -s -o /dev/null -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
s2=$(curl -s -o /dev/null -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
[ "$s1" = 200 ] && [ "$s2" = 401 ] && ok "a replayed request is refused (200 then 401)" || ko "replay ($s1 $s2)"
req 00000000 "$ASEC" GET /akam/v1/info
want "unknown key id: 401 bad_signature" 401 'bad_signature'
req "$AID" "$(printf '00%.0s' $(seq 32))" GET /akam/v1/info
want "wrong secret: the same 401 bad_signature" 401 'bad_signature'
admin GET /akam/v1/nothing
want "unknown path is 404" 404 'unknown_path'
admin DELETE /akam/v1/info
want "wrong method is 405" 405 'method'
req "$AID" "$ASEC" GET /akam/v1/settings "" "prefix=led.+x"
want "a raw '+' is not decoded: it is signed as %2B, auth passes, the query is refused (400)" 400 'bad_query'
admin GET /akam/v1/settings "" "prefix=led."
want "GET /settings?prefix=led." 200 '"led.mode":"on"'
admin GET /akam/v1/settings "" "nope=1"
want "unknown query parameter is 400" 400 'bad_query'
check "a body over 8192 bytes is 413" test "$(raw 'POST /akam/v1/actions/x HTTP/1.1\r\nContent-Length: 9000\r\n\r\n')" = "HTTP/1.1 413 Payload Too Large"
check "chunked upload is 411" test "$(raw 'POST /akam/v1/settings HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n')" = "HTTP/1.1 411 Length Required"
big=$(head -c 5000 /dev/zero | tr '\0' a)
check "a head over 4096 bytes is 431" test "$(raw "GET /akam/v1/info HTTP/1.1\r\nX-Big: $big\r\n\r\n")" = "HTTP/1.1 431 Request Header Fields Too Large"
check "garbage is 400" test "$(raw 'GARBAGE\r\n\r\n')" = "HTTP/1.1 400 Bad Request"
check "a bare LF line end is 400" test "$(raw 'GET /akam/v1/hello HTTP/1.1\nHost: x\n\n')" = "HTTP/1.1 400 Bad Request"
check "HTTP/2 is 505" test "$(raw 'GET /akam/v1/hello HTTP/2.0\r\n\r\n')" = "HTTP/1.1 505 HTTP Version Not Supported"
check "a request line over the limit is 414" test "$(raw "GET /$(head -c 700 /dev/zero | tr '\0' a) HTTP/1.1\r\n\r\n")" = "HTTP/1.1 414 URI Too Long"
check "the server still answers after the abuse" bash -c "curl -sS http://127.0.0.1:$PORT/akam/v1/hello | grep -q challenge"
# a slow client (head never finished): timed out after 5 s, the server keeps serving
(
    exec 6<>"/dev/tcp/127.0.0.1/$PORT"
    printf 'GET /akam/v1/hello HTTP/1.1\r\nX-Slow: ' >&6
    timeout 9 head -1 <&6 | tr -d '\r' >"$WORK/slow.out"
) &
SLOWPID=$!
sleep 1.3 # /hello is limited to 2 per second per peer
check "while a slow client waits, others are served" bash -c "curl -sS --max-time 3 http://127.0.0.1:$PORT/akam/v1/hello | grep -q challenge"
wait "$SLOWPID" || true
[ "$(cat "$WORK/slow.out")" = "HTTP/1.1 408 Request Timeout" ] && ok "a head that never ends is answered 408 after 5 s" || ko "slow client ($(cat "$WORK/slow.out"))"
# connection caps: 3 per peer, 4 in total
exec 7<>"/dev/tcp/127.0.0.1/$PORT"
exec 8<>"/dev/tcp/127.0.0.1/$PORT"
exec 9<>"/dev/tcp/127.0.0.1/$PORT"
sleep 1 # the three must be accepted (a loaded host needs more than 0.3 s)
check "a fourth connection from one peer is refused with 503" test "$(raw 'GET /akam/v1/hello HTTP/1.1\r\n\r\n')" = "HTTP/1.1 503 Service Unavailable"
exec 7>&- 8>&- 9>&-
sleep 1.3
check "connections freed: served again" bash -c "curl -sS --max-time 3 http://127.0.0.1:$PORT/akam/v1/hello | grep -q challenge"

# scopes and key management over HTTP
admin POST /akam/v1/keys '{"name":"hass","scope":"read"}'
want "POST /keys (admin) creates a key, the secret is shown once" 201 '"secret":"[0-9a-f]{64}"'
RID=$(printf '%s' "$BODY" | sed -n 's/.*"id":"\([0-9a-f]*\)".*/\1/p')
RSEC=$(printf '%s' "$BODY" | sed -n 's/.*"secret":"\([0-9a-f]*\)".*/\1/p')
[ "$(stat -c %a "$D1/api_keys")" = 600 ] && ok "the key file is still 0600 after a rewrite" || ko "key file mode after rewrite"
admin POST /akam/v1/keys '{"name":"hass","scope":"read"}'
want "a duplicate key name is 409" 409 'duplicate'
admin POST /akam/v1/keys '{"name":"bad name!","scope":"read"}'
want "a bad key name is 422" 422 'args'
admin GET /akam/v1/keys
want "GET /keys lists ids and names" 200 "\"id\":\"$RID\""
printf '%s' "$BODY" | grep -q "$RSEC" && ko "GET /keys never shows a secret" || ok "GET /keys never shows a secret"
req "$RID" "$RSEC" GET /akam/v1/info
want "the new read key works" 200 '"scope":"read"'
req "$RID" "$RSEC" GET /akam/v1/keys
want "a read key may not list keys (403 scope)" 403 'scope'
req "$RID" "$RSEC" PATCH /akam/v1/settings '{"led.mode":"off"}'
want "a read key may not PATCH (403 scope)" 403 'scope'
req "$RID" "$RSEC" POST /akam/v1/actions/ptz.home
want "a read key may not run an action (403 scope)" 403 'scope'
admin GET /akam/v1/audit
want "GET /audit (admin)" 200 '"audit":\['
printf '%s' "$BODY" | grep -Eqi 'X-Akam|[0-9a-f]{32}' && ko "audit carries no signature or nonce" || ok "audit carries no signature or nonce"
req "$RID" "$RSEC" GET /akam/v1/audit
want "a read key may not read the audit (403)" 403 'scope'
admin DELETE "/akam/v1/keys/$RID"
want "DELETE /keys/<id> revokes" 200 '"ok":true'
req "$RID" "$RSEC" GET /akam/v1/info
want "a revoked key is refused at once" 401 'bad_signature'
admin DELETE "/akam/v1/keys/$AID"
want "the last admin key cannot be deleted (409 last_admin)" 409 'last_admin'
admin DELETE "/akam/v1/keys/zzzzzzzz"
want "a bad key id is 400" 400 'bad_id'
# the on-camera tool and the running server: a key added from the shell works without a restart
newkey "$D1" write tool
TID=$KID
TSEC=$KSEC
for i in 1 2 3 4 5 6; do
    req "$TID" "$TSEC" GET /akam/v1/info
    [ "$STATUS" = 200 ] && break
    sleep 1
done
want "a key made by the shell tool is picked up by the running server (within seconds)" 200 '"scope":"write"'
check "audit file holds only write events (no GET)" bash -c "! grep -q 'GET /akam/v1/info' '$D1/audit' && grep -q 'POST /akam/v1/keys' '$D1/audit'"
# secrets never leak
for s in "$ASEC" "$TSEC" "$RSEC"; do
    if grep -rqF "$s" "$D1/err" "$D1/out" "$D1/audit" 2>/dev/null; then ko "canary secret found in a log or audit file"; else ok "canary secret absent from logs and audit"; fi
done
# invalid and world-readable key files close the API (fail closed) and are kept
cp "$D1/api_keys" "$WORK/keys.bak"
chmod 644 "$D1/api_keys"
for i in 1 2 3 4 5 6; do [ "$(srvq 'keys?')" = "KEYS 0 keys_perm" ] && break; sleep 1; done
[ "$(srvq 'keys?')" = "KEYS 0 keys_perm" ] && ok "a group/world-readable key file closes the API (keys_perm)" || ko "keys_perm ($(srvq 'keys?'))"
chmod 600 "$D1/api_keys"
printf 'garbage\n' >>"$D1/api_keys"
for i in 1 2 3 4 5 6; do [ "$(srvq 'keys?')" = "KEYS 0 keys_invalid" ] && break; sleep 1; done
[ "$(srvq 'keys?')" = "KEYS 0 keys_invalid" ] && ok "an invalid key file closes the API (keys_invalid)" || ko "keys_invalid ($(srvq 'keys?'))"
grep -q garbage "$D1/api_keys" && ok "the invalid key file is kept, not wiped" || ko "the invalid key file is kept"
cp "$WORK/keys.bak" "$D1/api_keys"
chmod 600 "$D1/api_keys"
touch "$D1/api_keys"
for i in 1 2 3 4 5 6; do [ "$(srvq 'keys?')" != "KEYS 0 ok" ] && break; sleep 1; done
sleep 1.2
PORT=$(srvq 'port?' | sed 's/PORT //')
req "$TID" "$TSEC" GET /akam/v1/info
want "the API is back with the restored file" 200 '"scope":"write"'
stop_srv && ok "the server exits cleanly (ASan/UBSan clean)" || ko "the server exits cleanly (see $D1/err)"

# ================================================================== phase 2: settings, actions, events, persistence
echo "== settings, actions, events, persistence"
D2=$WORK/p2
mkdir -p "$D2"
newkey "$D2" admin main
AID=$KID
ASEC=$KSEC
newkey "$D2" write ha
WID=$KID
WSEC=$KSEC
start_srv "$D2" --fast
req "$WID" "$WSEC" GET /akam/v1/schema
want "GET /schema: settings and actions" 200 '"settings":\[.*"actions":\[.*"ptz.move"'
for k in led.mode night.ir_led track.enabled video0.kbps video1.rc image.brightness net.hostname net.wifi_band sys.cpu_pct; do
    printf '%s' "$BODY" | grep -q "\"key\":\"$k\"" && ok "schema lists $k" || ko "schema lists $k"
done
printf '%s' "$BODY" | grep -Eq 'mqtt.pass|secret' && ko "schema shows no secret row" || ok "schema shows no secret row"
req "$WID" "$WSEC" GET /akam/v1/settings
want "GET /settings: writable values only" 200 '"video0.kbps":"2000"'
printf '%s' "$BODY" | grep -q 'sys.cpu_pct' && ko "GET /settings has no state keys" || ok "GET /settings has no state keys"
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state: read-only values" 200 '"rtsp.clients":"2".*"ptz.state":"parked"'
echo sys >&3
sleep 0.4
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state shows the hal_sys sample" 200 '"sys.mem_avail_kb":"9000".*"net.wifi_rssi":"-55"'
req "$WID" "$WSEC" GET /akam/v1/settings/video0.fps
want "GET /settings/<key> has metadata and value" 200 '"key":"video0.fps".*"value":"15"'
req "$WID" "$WSEC" GET /akam/v1/settings/mqtt.pass
want "an unknown (or secret) key is 404 unknown_key" 404 'unknown_key'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"off","night.ir_led":"off","track.enabled":true,"video1.fps":8,"image.brightness":"70"}'
want "PATCH: JSON numbers and booleans become canonical text, all ok" 200 '"led.mode":\{"ok":true,"apply":"applied","value":"off"\}.*"track.enabled":\{"ok":true.*"value":"1"'
check "the setters ran: LED mode, IR, tracker, image" bash -c "grep -q 'led.mode off' '$D2/calls.log' && grep -q 'night.ir 0' '$D2/calls.log' && grep -q 'track 1' '$D2/calls.log' && grep -q 'image 70 -1 -1 50' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/led.mode
want "the new value reads back" 200 '"value":"off"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"on","video0.fps":99}'
want "PATCH is all-or-nothing: one bad value refuses the batch (422)" 422 '"video0.fps":\{"ok":false,"error":"range"'
req "$WID" "$WSEC" GET /akam/v1/settings/led.mode
want "nothing was applied by the refused batch" 200 '"value":"off"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"video0.min_qp":45,"video0.max_qp":40}'
want "a cross-field rule is refused" 422 'check|setter'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"sys.cpu_pct":5}'
want "a read-only state key is refused" 422 'readonly'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"nope":1}'
want "an unknown key is refused" 422 'unknown_key'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":null}'
want "null is bad JSON" 400 'bad_json'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"off"} trailing'
want "trailing text is bad JSON" 400 'bad_json'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{}'
want "an empty body object is 422 empty" 422 'empty'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.person":"2:150:150:0"}'
want "a pattern setting is accepted" 200 '"led.person"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.person":"2:150"}'
want "a bad pattern is refused" 422 'format|range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.hostname":"cam-salon"}'
want "net.hostname is queued for the supervisor" 200 '"net.hostname":\{"ok":true'
check "the request line for akam_net.sh was written" bash -c "[ \"\$(cat '$D2/net.req')\" = 'hostname set cam-salon' ]"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.hostname":"bad_name"}'
want "an invalid hostname is refused before anything is queued" 422 'check|format'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.wifi_band":"5"}'
want "a second net request while the first is pending is refused" 422 'setter'
rm -f "$D2/net.req"
mkdir -p "$D2/net"
printf 'cam-salon\n' >"$D2/net/hostname"
printf 'wifi_country=DE\nwifi_band=5\n' >"$D2/net/net"
req "$WID" "$WSEC" GET /akam/v1/settings/net.hostname
want "net.* values are read from the persisted files" 200 '"value":"cam-salon"'
req "$WID" "$WSEC" GET /akam/v1/settings/net.wifi_country
want "net.wifi_country read from the net file" 200 '"value":"DE"'
# actions
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.step '{"dir":"left"}'
want "action ptz.step" 200 '"ok":true'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.home
want "action ptz.home (no body)" 200 '"ok":true'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.recalibrate
want "action ptz.recalibrate" 200 '"ok":true'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.park
want "action ptz.park" 200 '"ok":true'
check "the motor module got the commands" bash -c "grep -q 'ptz.cmd 0' '$D2/calls.log' && grep -q 'ptz.cmd 4' '$D2/calls.log' && grep -q 'ptz.cmd 6' '$D2/calls.log' && grep -q 'ptz.cmd 5' '$D2/calls.log'"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.move '{"dx":120,"dy":-30}'
want "action ptz.move" 200 '"ok":true'
check "ptz.move reached the motor as a nudge" grep -q 'ptz.nudge 120 -30' "$D2/calls.log"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.move '{"dx":10}'
want "ptz.move is rate limited (429)" 429 'rate'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.move '{"dx":9999}'
want "ptz.move is bounded (422)" 422 'args|rate'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.step '{"dir":"sideways"}'
want "a bad action argument is 422" 422 'args'
req "$WID" "$WSEC" POST /akam/v1/actions/reboot
want "there is no reboot action (404)" 404 'unknown_action'
req "$WID" "$WSEC" POST /akam/v1/actions/settings.reset '{"key":"led.mode"}'
want "action settings.reset restores a default" 200 '"ok":true'
req "$WID" "$WSEC" GET /akam/v1/settings/led.mode
want "led.mode is back to its default" 200 '"value":"on"'
req "$WID" "$WSEC" POST /akam/v1/actions/settings.reset '{"key":"led.mode","prefix":"led."}'
want "reset needs exactly one of key or prefix" 422 'args'
# events: a long poll is held and woken by an event
req "$WID" "$WSEC" GET /akam/v1/events
want "GET /events without a cursor reports the current sequence, no events" 200 '"events":\[\]'
SEQ=$(printf '%s' "$BODY" | sed -n 's/.*"seq":\([0-9]*\),"missed".*/\1/p')
(
    sleep 1.5
    echo "event button" >&3
    echo "event person 1" >&3
) &
t0=$(date +%s)
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ&timeout=10"
t1=$(date +%s)
want "a held GET /events is answered by an event (button)" 200 '"type":"button"'
[ $((t1 - t0)) -le 6 ] && ok "the long poll returned on the event, not at the deadline" || ko "long poll latency $((t1 - t0)) s"
sleep 0.3
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.contrast":60}'
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ&timeout=0"
want "events since the cursor include person and the setting changes" 200 '"type":"person".*"present":true'
printf '%s' "$BODY" | grep -q '"type":"setting"' && ok "settings changes are events too" || ko "settings changes are events too"
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ&timeout=2"
SEQ2=$(printf '%s' "$BODY" | sed -n 's/.*"seq":\([0-9]*\),"missed".*/\1/p')
t0=$(date +%s)
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ2&timeout=2"
t1=$(date +%s)
want "with nothing new a long poll returns at its deadline with no events" 200 '"events":\[\]'
[ $((t1 - t0)) -ge 2 ] && ok "the deadline was honoured" || ko "deadline $((t1 - t0)) s"
req "$WID" "$WSEC" GET /akam/v1/events "" "since=1&timeout=99"
want "timeout over 25 is 400" 400 'bad_query'
# persistence: the file, the restart, the load
sleep 2.2
check "persistent settings were saved (changed keys only)" bash -c "grep -q 'track.enabled=1' '$D2/settings' && grep -q 'night.ir_led=off' '$D2/settings' && grep -q 'led.person=2:150:150:0' '$D2/settings' && ! grep -q 'video' '$D2/settings' && ! grep -q 'image' '$D2/settings' && ! grep -q 'led.mode' '$D2/settings'"
stop_srv && ok "stop saves and exits cleanly" || ko "stop (see $D2/err)"
: >"$D2/calls.log"
start_srv "$D2" --fast
check "the saved file is loaded at start: setters ran for the saved keys" bash -c "grep -q 'track 1' '$D2/calls.log' && grep -q 'night.ir 0' '$D2/calls.log' && grep -q 'led.rule 2 2:150:150:0' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/led.person
want "the value survived the restart" 200 '"value":"2:150:150:0"'
stop_srv || true
printf 'led.mode=off\nled.person=zzz\nnonsense=1\n' >"$D2/settings"
: >"$D2/calls.log"
start_srv "$D2" --fast
check "an invalid settings file is ignored as a whole" bash -c "! grep -q 'led.mode' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/led.mode
want "defaults apply after an invalid settings file" 200 '"value":"on"'
stop_srv || true

# ================================================================== phase 3: default limits (lockout, rate)
echo "== rate limit and lockout with the spec defaults"
D3=$WORK/p3
mkdir -p "$D3"
newkey "$D3" admin main
AID=$KID
ASEC=$KSEC
start_srv "$D3"
n429=0
for i in $(seq 1 20); do
    admin GET /akam/v1/info
    [ "$STATUS" = 429 ] && n429=$((n429 + 1))
done
[ "$n429" -ge 4 ] && ok "burst 14 then 0.4 req/s: the excess is 429 ($n429 of 20)" || ko "rate limit ($n429 of 20 refused)"
printf '%s' "$HDRS" | grep -qi '^Retry-After: [0-9]' && ok "a 429 carries Retry-After" || ko "a 429 carries Retry-After"
stop_srv || true
D4=$WORK/p4
mkdir -p "$D4"
newkey "$D4" admin main
AID=$KID
ASEC=$KSEC
start_srv "$D4" --fast
bad=$(printf '00%.0s' $(seq 32))
for i in 1 2 3 4 5; do req "$AID" "$bad" GET /akam/v1/info; done
want "the 5th bad signature is still a 401" 401 'bad_signature'
req "$AID" "$bad" GET /akam/v1/info
want "after 5 failures the peer is locked (429 locked)" 429 'locked'
printf '%s' "$HDRS" | grep -qi '^Retry-After: [0-9]' && ok "the lock carries Retry-After" || ko "the lock carries Retry-After"
admin GET /akam/v1/info
want "a locked peer is refused even with a good signature" 429 'locked'
sleep 2.2
admin GET /akam/v1/info
want "after the 1 s lock the owner is served again" 200 '"api":1'
stop_srv || true

# ================================================================== phase 4: clock not set: challenge flow
echo "== clock not set: the challenge flow"
D5=$WORK/p5
mkdir -p "$D5"
newkey "$D5" admin main
AID=$KID
ASEC=$KSEC
start_srv "$D5" --fast --no-clock
CH=$(curl -sS "http://127.0.0.1:$PORT/akam/v1/hello")
printf '%s' "$CH" | grep -q '"clock_set":false' && ok "hello says clock_set:false" || ko "hello says clock_set:false ($CH)"
chal=$(printf '%s' "$CH" | sed -n 's/.*"challenge":"\([0-9a-f]*\)".*/\1/p')
h=$(AKAM_KEY_ID=$AID AKAM_KEY_SECRET=$ASEC AKAM_CHALLENGE=$chal "${SHCMD[@]}" "$SIGN" GET /akam/v1/info)
args=()
while IFS= read -r l; do args+=(-H "$l"); done <<<"$h"
s1=$(curl -s -o "$WORK/ch.out" -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
s2=$(curl -s -o /dev/null -w '%{http_code}' "${args[@]}" "http://127.0.0.1:$PORT/akam/v1/info")
[ "$s1" = 200 ] && [ "$s2" = 401 ] && ok "a challenge signs one request, a reuse is refused" || ko "challenge ($s1 $s2)"
admin GET /akam/v1/info
want "a timestamp request is refused while the clock is not set" 401 'clock'
stop_srv || true

echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" = 0 ]
