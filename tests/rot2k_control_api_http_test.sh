#!/usr/bin/env bash
# Integration test of the local control API server (specs/control-api.md phase 2), no camera: the REAL HTTP server, router, rate
# limits, registry table, settings worker and key store of the 2K app (tests/rot2k_api_server.c) with FAKE hardware modules, built under
# ASan + UBSan, listening on loopback and driven by curl with the reference signer tools/akam-api-sign.sh.
# Needs: a C compiler (CC, default cc), curl, openssl, awk, xxd. Env: SH_UNDER_TEST for the signer (default sh).
# shellcheck disable=SC2015,SC2059,SC2034  # test idioms: "cond && ok || ko" reporters, printf with an escape format, loop counters
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
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
 '-DA2K_API_FAST_ACTION="ptz.vel"' -DA2K_API_RESP_MAX=18432 -DAPISIG_NONCE_CAP=224 "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$SRV" "$HERE/rot2k_api_server.c" "$REPO/platforms/anyka-ak3918av130/orient.c" "$REPO/platforms/anyka-ak3918av130/picture.c" \
    "$REPO/src/api/api_httpd.c" "$REPO/src/api/api_core.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/api_bind.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/settings.c" "$REPO/src/api/api_events.c" "$REPO/platforms/anyka-ak3918av130/detpub.c" "$REPO/src/net/http/http_req.c" "$REPO/src/api/api_limit.c" \
    "$REPO/src/api/api_keys.c" "$REPO/src/api/api_sig.c" "$REPO/src/net/crypto/sha256.c" "$REPO/src/api/json_min.c" "$REPO/src/core/settings_reg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/netreg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/cfgreg.c" \
    "$REPO/src/features/ptz/ptz_vel.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/store.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/speaker.c" "$REPO/src/features/audio/audio_out.c" "$REPO/src/features/audio/talk.c" "$REPO/src/features/audio/audio_jitter.c" "$REPO/src/features/audio/audio_vad.c" "$REPO/src/features/audio/g711.c" "$REPO/src/net/rtsp/rtp_in.c" "$REPO/src/net/rtsp/rtp_clock.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/video_cfg.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/patrol.c" "$REPO/src/features/ptz/ptz_patrol.c" "$REPO/src/features/track/track_logic.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/button_rt.c" "$REPO/src/features/button/button_cfg.c" "$REPO/src/features/button/button_gesture.c" "$REPO/src/features/button/button_logic.c" "$REPO/src/hal/hal_input.c" "$REPO/src/net/rtsp/rtsp_auth.c" "$REPO/src/net/crypto/md5.c"; then
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
# server-side observable: the number of socket fds the server process holds (accepted connections), no sleeping
sockfds() { find "/proc/$SP/fd/" -maxdepth 1 -lname 'socket:*' 2>/dev/null | wc -l; }
BASE_SOCK=$(sockfds)
exec 7<>"/dev/tcp/127.0.0.1/$PORT"
exec 8<>"/dev/tcp/127.0.0.1/$PORT"
exec 9<>"/dev/tcp/127.0.0.1/$PORT"
for _ in $(seq 1 300); do [ "$(sockfds)" -ge $((BASE_SOCK + 3)) ] && break; sleep 0.05; done
[ "$(sockfds)" -ge $((BASE_SOCK + 3)) ] || ko "the server accepted only $(($(sockfds) - BASE_SOCK)) of the 3 connections"
# send nothing: the server answers 503 at accept and closes; request bytes it never read would turn that close into a RST that can
# swallow the reply on a loaded host
refused4() { local out; out=$(exec 5<>"/dev/tcp/127.0.0.1/$PORT" && timeout 8 head -1 <&5 | tr -d '\r') || true; printf '%s' "$out"; }
check "a fourth connection from one peer is refused with 503" test "$(refused4)" = "HTTP/1.1 503 Service Unavailable"
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
# ---- speaker and two-way audio rows (the 1080p's names and ranges; the real shared engine behind them, no playback node on a host)
for k in audio.out_enable audio.out_volume audio.out_gain audio.out_amp audio.talk_enable audio.talk_duplex; do
    printf '%s' "$BODY" | grep -q "\"key\":\"$k\"" && ok "schema lists $k" || ko "schema lists $k"
done
for k in audio.out_state audio.talk_state; do
    printf '%s' "$BODY" | grep -q "\"key\":\"$k\"" && ok "schema lists $k (read only)" || ko "schema lists $k (read only)"
done
want "schema: audio.out_volume is 0..100, default 100; audio.out_enable defaults off (opt-in on the 2K)" 200 '"key":"audio.out_enable","type":"bool","default":"0".*"key":"audio.out_volume","type":"int","lo":0,"hi":100,"default":"100"'
want "schema: the audio.beep action is listed" 200 '"audio.beep"'
req "$WID" "$WSEC" GET /akam/v1/settings
want "GET /settings: the speaker values" 200 '"audio.out_volume":"100".*"audio.out_gain":"2".*"audio.talk_enable":"1".*"audio.talk_duplex":"half"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_volume":35,"audio.out_gain":4,"audio.talk_duplex":"full","audio.talk_enable":false}'
want "PATCH the speaker rows (volume, gain, duplex, talk) is applied" 200 '"audio.out_volume":\{"ok":true'
req "$WID" "$WSEC" GET /akam/v1/settings/audio.out_volume
want "GET /settings/audio.out_volume reads the engine back" 200 '"value":"35"'
req "$WID" "$WSEC" GET /akam/v1/settings/audio.talk_duplex
want "audio.talk_duplex full" 200 '"value":"full"'
req "$WID" "$WSEC" GET /akam/v1/settings/audio.talk_enable
want "audio.talk_enable 0" 200 '"value":"0"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_volume":101}'
want "volume 101 is refused (range)" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_gain":6}'
want "gain 6 is refused (range)" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.talk_duplex":"both"}'
want "duplex both is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_state":"idle"}'
want "audio.out_state is read only" 422 'readonly'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_enable":true}'
printf '%s' "$BODY" | grep -q 'setter' && ok "enabling the speaker without a playback node fails in the setter (host), nothing crashes" || ko "enable without a node: $STATUS $BODY"
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state: the speaker state is error after the failed start" 200 '"audio.out_state":"error"'
want "GET /state: the talk state" 200 '"audio.talk_state":"idle"'
req "$WID" "$WSEC" POST /akam/v1/actions/audio.beep '{}'
want "action audio.beep refuses while the speaker is not running" 409 'unavailable'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"audio.out_enable":false}'
want "audio.out_enable can be turned off again" 200 '"ok":true'
# ---- picture rows: the 1080p's names (isp.saturation, isp.denoise, img.wb), the real picture.c on a fake ISP
req "$WID" "$WSEC" GET /akam/v1/schema
for k in isp.saturation isp.denoise isp.nr2 isp.wdr img.wb; do
    printf '%s' "$BODY" | grep -q "\"key\":\"$k\"" && ok "schema lists $k" || ko "schema lists $k"
done
want "schema: isp.saturation -50..50 default 0, isp.denoise 0..4 default 0, img.wb conf only (manual is refused on the 2K)" 200 '"key":"isp.saturation","type":"int","lo":-50,"hi":50,"default":"0".*"key":"isp.denoise","type":"int","lo":0,"hi":4,"default":"0".*"key":"img.wb","type":"enum".*"conf"'
want "schema: isp.nr2 0..4 default 0, isp.wdr 0..100 default 90 (the stock's effect 40)" 200 '"key":"isp.nr2","type":"int","lo":0,"hi":4,"default":"0".*"key":"isp.wdr","type":"int","lo":0,"hi":100,"default":"90"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.saturation":-20}'
want "PATCH isp.saturation -20" 200 '"isp.saturation":\{"ok":true'
check "the ISP got effect 2 (saturation) = -20" bash -c "grep -q 'isp.effect 2 -20' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/isp.saturation
want "GET isp.saturation reads it back" 200 '"value":"-20"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.saturation":51}'
want "isp.saturation 51 is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.denoise":2}'
want "PATCH isp.denoise 2" 200 '"isp.denoise":\{"ok":true'
check "the 3D NR table was written with level 1 taken from level 3 and level 16 from itself (marker 103, 116)" bash -c "grep -q 'isp.nr3d rec1 103 rec16 116' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.denoise":5}'
want "isp.denoise 5 is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.denoise":0}'
check "denoise 0 writes the original table back (marker 101)" bash -c "grep -q 'isp.nr3d rec1 101 rec16 116' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.nr2":0}'
want "PATCH isp.nr2 0 (the default)" 200 '"isp.nr2":\{"ok":true'
check "isp.nr2 0 on an untouched camera sends no nr2 table" bash -c "! grep -q 'isp.nr2' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.nr2":2}'
want "PATCH isp.nr2 2" 200 '"isp.nr2":\{"ok":true'
check "the nr2 table was written: record 1 nr2_k 1 + 1, record 16 0 + 5 (level 2 step (2 * 20 + 4) / 8)" bash -c "grep -q 'isp.nr2 rec1 k 2 rec16 k 5' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.nr2":5}'
want "isp.nr2 5 is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.nr2":0}'
check "nr2 0 writes the original table back (record 1 k 1, record 16 k 0)" bash -c "grep -q 'isp.nr2 rec1 k 1 rec16 k 0' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.wdr":100}'
want "PATCH isp.wdr 100" 200 '"isp.wdr":\{"ok":true'
check "the ISP got effect 5 (WDR) = 50" bash -c "grep -q 'isp.effect 5 50' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/isp.wdr
want "GET isp.wdr reads it back" 200 '"value":"100"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.wdr":101}'
want "isp.wdr 101 is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"isp.wdr":90}'
check "isp.wdr 90 puts the stock's effect 40 back" bash -c "grep -q 'isp.effect 5 40' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"img.wb":"manual"}'
want "PATCH img.wb manual is refused (no way to freeze the automatic gains)" 422 '"img.wb":\{"ok":false,"error":"range"'
check "the refused manual sent nothing to the ISP (no wb type write)" bash -c "! grep -q 'isp.wbtype' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/img.wb
want "img.wb still reads conf" 200 '"value":"conf"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"img.wb":"auto"}'
want "img.wb auto is refused (conf or manual)" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"img.wb":"conf"}'
want "img.wb conf is accepted and sends nothing" 200 '"img.wb":\{"ok":true'
check "img.wb conf wrote no wb type either" bash -c "! grep -q 'isp.wbtype' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings
want "GET /settings: img.wb is conf, saturation and denoise as set" 200 '"isp.saturation":"-20"'
req "$WID" "$WSEC" GET /akam/v1/settings
want "GET /settings: writable values only" 200 '"video0.kbps":"2000"'
printf '%s' "$BODY" | grep -q 'sys.cpu_pct' && ko "GET /settings has no state keys" || ok "GET /settings has no state keys"
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state: read-only values" 200 '"rtsp.clients":"2".*"ptz.state":"parked"'
echo sys >&3
sleep 0.4
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state shows the hal_sys sample" 200 '"sys.mem_avail_kb":"9000".*"net.wifi_rssi":"-55"'
want "GET /state: ptz travel and the flash write counters by source" 200 '"ptz.travel_since_cal":"1234".*"sys.flash_writes_ptzpos":"7".*"sys.flash_writes_scene":"3".*"sys.flash_writes_store":"[0-9]+"'
want "GET /state: ptz drift rows (the last measured displacement, the corrections, the recalibration reason)" 200 '"ptz.drift_last_px":"-2,3"'
want "GET /state: ptz drift corrections" 200 '"ptz.drift_corrections":"2"'
want "GET /state: the re-home keys carry the controller's values (completed, cancelled, uptime stamp, reason)" 200 '"ptz.rehome_count":"3".*"ptz.rehome_cancel":"2".*"ptz.rehome_last_s":"4242".*"ptz.rehome_last_reason":"flat"'
want "GET /state: the drift reference is visible (modes held, age in seconds)" 200 '"ptz.drift_ref":"both".*"ptz.drift_ref_age_s":"12"'
want "GET /state: ptz recal reason" 200 '"ptz.recal_reason":"none"'
want "GET /state: ptz untrusted flag (the position is doubted: moves still allowed)" 200 '"ptz.untrusted":"0"'
req "$WID" "$WSEC" GET /akam/v1/settings/video0.fps
want "GET /settings/<key> has metadata and value" 200 '"key":"video0.fps".*"value":"15"'
req "$WID" "$WSEC" GET /akam/v1/settings/mqtt.pass
want "an unknown (or secret) key is 404 unknown_key" 404 'unknown_key'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"off","night.ir_led":"off","track.enabled":true,"track.return":"home","track.return_delay_s":0,"video1.fps":8,"image.brightness":"70"}'
want "PATCH: JSON numbers and booleans become canonical text, all ok" 200 '"led.mode":\{"ok":true,"apply":"applied","value":"off"\}.*"track.enabled":\{"ok":true.*"value":"1"'
check "the setters ran: LED mode, IR, tracker, image" bash -c "grep -q 'led.mode off' '$D2/calls.log' && grep -q 'night.ir 0' '$D2/calls.log' && grep -q 'track 1' '$D2/calls.log' && grep -q 'image 70 -1 -1 50' '$D2/calls.log'"
check "the return policy reached the tracker (home, then delay 0)" bash -c "grep -q 'track.return 1 30' '$D2/calls.log' && grep -q 'track.return 1 0' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/track.return
want "track.return reads back (enum stay|home)" 200 '"value":"home"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.return":"sideways"}'
want "track.return refuses a value outside stay|home" 422 "track.return.*range"
req "$WID" "$WSEC" GET /akam/v1/settings/track.gain_tilt
want "track.gain_tilt defaults to 384 (2 steps per px of 192, measured)" 200 '"value":"384"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.gain_pan":1200,"track.gain_tilt":900,"track.tilt_band_pct":8}'
want "PATCH: the three tracker gain rows are accepted" 200 '"track.gain_tilt":\{"ok":true'
check "the gains reached the running tracker (live)" bash -c "grep -q 'track.gains 1200 900 8' '$D2/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/track.gain_tilt
want "track.gain_tilt reads back from the tracker" 200 '"value":"900"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.gain_pan":199,"track.gain_tilt":4001,"track.tilt_band_pct":41}'
want "the gain rows refuse values outside 200..4000 / 2..40 (all three, nothing applied)" 422 'range.*range.*range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.gain_tilt":"x"}'
want "track.gain_tilt refuses a non-number" 422 'format'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.tilt":true,"track.close_scan":0,"track.close_scan_up":120}'
want "PATCH track.tilt (pan only switch) and track.close_scan are applied live together" 200 '"track.tilt":\{"ok":true.*"track.close_scan":\{"ok":true'
check "track.tilt and track.close_scan reached the running tracker" bash -c "grep -q 'track.tilt 1' '$D2/calls.log' && grep -q 'track.close_scan 0 0' '$D2/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"track.tilt":false}'
want "track.tilt 0 = pan only is accepted" 200 '"track.tilt":\{"ok":true'
req "$WID" "$WSEC" GET /akam/v1/settings/track.tilt
want "track.tilt reads back from the tracker" 200 '"value":"0"'
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
req "$WID" "$WSEC" GET /akam/v1/settings/net.wifi_band
want "net.wifi_band defaults to 2.4 without a net file" 200 '"value":"2.4"'
req "$WID" "$WSEC" GET /akam/v1/settings/net.wifi_country
want "net.wifi_country defaults to none without a net file" 200 '"value":"none"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.wifi_band":"auto"}'
want "band auto does not exist (422)" 422 'range|enum'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.wifi_band":"5"}'
want "band 5 is refused while the country is none (not reported as applied)" 422 'check'
check "...and nothing was queued" test ! -e "$D2/net.req"
req "$WID" "$WSEC" GET /akam/v1/settings/net.apply
want "net.apply is idle before any request" 200 '"value":"idle"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.hostname":"cam-salon"}'
want "net.hostname is queued for the supervisor" 200 '"net.hostname":\{"ok":true'
req "$WID" "$WSEC" GET /akam/v1/settings/net.apply
want "net.apply says queued while the request file waits" 200 '"value":"queued"'
check "the request line for akam_net.sh was written" bash -c "[ \"\$(cat '$D2/net.req')\" = 'hostname set cam-salon' ]"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.hostname":"bad_name"}'
want "an invalid hostname is refused before anything is queued" 422 'check|format'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.hostname":"cam-two"}'
want "a second net request while the first is pending is refused" 422 'setter'
rm -f "$D2/net.req"
mkdir -p "$D2/net"
printf 'cam-salon\n' >"$D2/net/hostname"
printf 'wifi_country=DE\nwifi_band=5\n' >"$D2/net/net"
printf 'refused wpa-conf\n' >"$D2/net.res"
req "$WID" "$WSEC" GET /akam/v1/settings/net.apply
want "net.apply reports the outcome akam_net.sh left (refused)" 200 '"value":"refused"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"net.wifi_country":"none"}'
want "country none is refused while on band 5" 422 'check'
req "$WID" "$WSEC" GET /akam/v1/settings/net.hostname
want "net.* values are read from the persisted files" 200 '"value":"cam-salon"'
req "$WID" "$WSEC" GET /akam/v1/settings/net.wifi_country
want "net.wifi_country read from the net file" 200 '"value":"DE"'
# the user-file rows (cfgreg.h): MQTT, ONVIF, stream password, NTP, microphone
mkdir -p "$D2/cfg"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"mqtt.host":"broker.lan"}'
want "mqtt.host needs an admin key (a write key is refused)" 422 'scope'
check "...and nothing was queued" test ! -e "$D2/cfg.req"
admin GET /akam/v1/settings/mqtt.port
want "mqtt.port defaults to 1883" 200 '"value":"1883"'
admin GET /akam/v1/settings/audio.mic_enable
want "the microphone is off by default" 200 '"value":"0"'
admin GET /akam/v1/settings/onvif.port
want "onvif.port defaults to 8000" 200 '"value":"8000"'
admin GET /akam/v1/settings/rtsp.auth
want "rtsp.auth is off without rtsp.conf" 200 '"value":"off"'
admin PATCH /akam/v1/settings '{"mqtt.enabled":true}'
want "MQTT on without a host is refused" 422 'check'
admin PATCH /akam/v1/settings '{"mqtt.host":"bad host"}'
want "a bad broker host is refused" 422 'check'
admin PATCH /akam/v1/settings '{"mqtt.password":"hunter2hunter2"}'
want "a broker password without a user is refused" 422 'check'
admin PATCH /akam/v1/settings '{"rtsp.user":"bob"}'
want "a stream user without its password is refused" 422 'check'
admin PATCH /akam/v1/settings '{"rtsp.user":"bob","rtsp.password":"short"}'
want "a stream password under 8 characters is refused" 422 'check'
admin PATCH /akam/v1/settings '{"onvif.enabled":true}'
want "ONVIF without a stream password is refused" 422 'check'
admin PATCH /akam/v1/settings '{"ntp.servers":"a.example b.example c.example d.example"}'
want "four NTP servers are refused" 422 'check'
check "...nothing was queued by any refusal" test ! -e "$D2/cfg.req"
admin PATCH /akam/v1/settings '{"mqtt.host":"broker.lan","mqtt.port":1884,"mqtt.user":"ha","mqtt.password":"hunter2hunter2","mqtt.enabled":true,"rtsp.user":"bob","rtsp.password":"correct horse","onvif.enabled":true,"ntp.servers":"pool.ntp.org","audio.mic_enable":true}'
want "one PATCH of every user-file row is accepted" 200 '"mqtt.host":\{"ok":true'
check "...the password values are not echoed" bash -c "! printf '%s' '$BODY' | grep -q 'hunter2\\|correct horse'"
want "...the secret rows say a restart is needed or applied, without a value" 200 '"mqtt.password":\{"ok":true,"apply":"[a-z_]*"\}'
check "the request holds the ten lines for the supervisor" bash -c "[ \"\$(wc -l <'$D2/cfg.req')\" = 10 ] && grep -qx 'mqtt.host=broker.lan' '$D2/cfg.req' && grep -qx 'rtsp.password=correct horse' '$D2/cfg.req'"
check "the request file is private (0600)" bash -c "[ \"\$(stat -c %a '$D2/cfg.req')\" = 600 ]"
admin GET /akam/v1/settings/cfg.apply
want "cfg.apply says queued while the request waits" 200 '"value":"queued"'
admin GET /akam/v1/settings/mqtt.password
want "a write-only row cannot be read" 404 'unknown_key|secret'
admin GET /akam/v1/settings
check "no listing shows a write-only row or a password" bash -c "! printf '%s' '$BODY' | grep -q 'mqtt.password\\|rtsp.password\\|hunter2'"
admin GET /akam/v1/schema
check "the schema never lists a write-only row" bash -c "! printf '%s' '$BODY' | grep -q 'mqtt.password\\|rtsp.password'"
rm -f "$D2/cfg.req"
printf 'ok\n' >"$D2/cfg.res"
admin GET /akam/v1/settings/cfg.apply
want "cfg.apply reports the outcome the supervisor left" 200 '"value":"ok"'
printf 'enabled=1\nhost=broker.lan\nport=1884\nuser=ha\n' >"$D2/cfg/mqtt"
printf 'bob:akam:0123456789abcdef0123456789abcdef\n' >"$D2/cfg/rtsp.conf"
printf 'server=pool.ntp.org\nserver=time.example.net\n' >"$D2/cfg/ntp"
printf '# x\nonvif=1\nonvif_port=8123\naudio=1\n' >"$D2/cfg/akam.conf"
admin GET /akam/v1/settings/mqtt.host
want "the broker host is read from its user file" 200 '"value":"broker.lan"'
admin GET /akam/v1/settings/mqtt.port
want "the broker port is read from its user file" 200 '"value":"?1884"?'
admin GET /akam/v1/settings/mqtt.user
want "the broker user is read from its user file" 200 '"value":"ha"'
admin GET /akam/v1/settings/mqtt.enabled
want "MQTT on is read from its user file" 200 '"value":"?(1|true)"?'
admin GET /akam/v1/settings/ntp.servers
want "the NTP servers is read from its user file" 200 '"value":"pool.ntp.org time.example.net"'
admin GET /akam/v1/settings/onvif.port
want "the ONVIF port is read from its user file" 200 '"value":"?8123"?'
admin GET /akam/v1/settings/onvif.enabled
want "ONVIF on is read from its user file" 200 '"value":"?(1|true)"?'
admin GET /akam/v1/settings/rtsp.user
want "the stream user is read from its user file" 200 '"value":"bob"'
admin GET /akam/v1/settings/audio.mic_enable
want "the microphone on is read from its user file" 200 '"value":"?(1|true)"?'
admin GET /akam/v1/settings/rtsp.auth
want "rtsp.auth is on with a valid rtsp.conf" 200 '"value":"on"'
admin PATCH /akam/v1/settings '{"rtsp.password":"another long one"}'
want "a new password alone keeps the stored user" 200 '"rtsp.password":\{"ok":true'
check "the lines of the second request are queued" bash -c "[ \"\$(cat '$D2/cfg.req')\" = 'rtsp.password=another long one' ]"
admin PATCH /akam/v1/settings '{"mqtt.host":"b2.lan"}'
check "a request not yet taken keeps its earlier lines (the later line of a key wins)" bash -c "[ \"\$(wc -l <'$D2/cfg.req')\" = 2 ] && grep -qx 'mqtt.host=b2.lan' '$D2/cfg.req'"
rm -f "$D2/cfg.req" "$D2/cfg.res"
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
# detections: a held long poll is woken by a published list; detect.boxes (state) holds the latest; the setting turns it off
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ2&timeout=0"
SEQ3=$(printf '%s' "$BODY" | sed -n 's/.*"seq":\([0-9]*\),"missed".*/\1/p')
(
    sleep 1
    echo "det 2" >&3
) &
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ3&timeout=10"
want "a held GET /events is answered by a detections event" 200 '"type":"detections","data":\{"ts_ms":1700000000000,"w":320,"h":192,"pan":1234,"tilt":56,"boxes":\[\{"x":312,"y":104,"w":128,"h":526,"score":900,"id":0,"target":true,"ignored":false\},\{"x":625,"y":156,"w":96,"h":369,"score":800,"id":1,"target":false,"ignored":true\}\]'
req "$WID" "$WSEC" GET /akam/v1/state
want "GET /state: detect.boxes holds the latest list" 200 '"detect.boxes":"\{\\"ts_ms\\":1700000000000,.*\\"boxes\\":\[\{'
for i in 1 2 3 4 5 6 7 8 9; do echo "det 1" >&3; echo "det 2" >&3; done
sleep 0.5
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$SEQ&timeout=0"
want "a burst of detections never evicts the other event types" 200 '"type":"button".*"type":"person".*"type":"setting"'
[ "$(printf '%s' "$BODY" | grep -o '"type":"detections"' | wc -l)" -le 4 ] && ok "the detections ring is bounded" || ko "detections ring size"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"detect.publish_hz":0}'
want "detect.publish_hz is a setting" 200
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"detect.publish_hz":2}'
req "$WID" "$WSEC" GET /akam/v1/events "" "since=1&timeout=99"
want "timeout over 25 is 400" 400 'bad_query'
# persistence: the file, the restart, the load
sleep 2.2
check "persistent settings were saved (changed keys only; image.brightness and image.contrast now persist on the 2K)" bash -c "grep -q 'track.enabled=1' '$D2/settings' && grep -q 'track.gain_tilt=900' '$D2/settings' && grep -q 'track.gain_pan=1200' '$D2/settings' && grep -q 'track.tilt_band_pct=8' '$D2/settings' && grep -q 'night.ir_led=off' '$D2/settings' && grep -q 'led.person=2:150:150:0' '$D2/settings' && ! grep -q 'video' '$D2/settings' && grep -q '^image.brightness=70' '$D2/settings' && grep -q '^image.contrast=60' '$D2/settings' && ! grep -q 'image.flip' '$D2/settings' && ! grep -q 'led.mode' '$D2/settings'"
stop_srv && ok "stop saves and exits cleanly" || ko "stop (see $D2/err)"
: >"$D2/calls.log"
start_srv "$D2" --fast
check "the saved file is loaded at start: setters ran for the saved keys" bash -c "grep -q 'track 1' '$D2/calls.log' && grep -q 'night.ir 0' '$D2/calls.log' && grep -q 'led.rule 2 2:150:150:0' '$D2/calls.log' && grep -q 'track.gains 1200 900 8' '$D2/calls.log'"
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

# ================================================================== orientation rows (image.flip / image.mirror / image.ceiling)
echo "== image orientation: flip, mirror, ceiling mount (live, saved on a user change, an ISP refusal is rolled back)"
D2B=$WORK/p2b
mkdir -p "$D2B"
newkey "$D2B" write ha
WID=$KID
WSEC=$KSEC
start_srv "$D2B" --fast
req "$WID" "$WSEC" GET /akam/v1/schema
for k in image.flip image.mirror image.ceiling; do
    printf '%s' "$BODY" | grep -q "\"key\":\"$k\"" && ok "schema lists $k" || ko "schema lists $k"
done
req "$WID" "$WSEC" GET /akam/v1/settings/image.flip
want "image.flip defaults to off" 200 '"value":"0"'
check "nothing was sent to the ISP while the orientation stayed off" bash -c "! grep -q 'isp.flip_mirror' '$D2B/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.flip":true}'
want "PATCH image.flip is applied live" 200 '"image.flip":\{"ok":true,"apply":"applied"'
check "the ISP got flip 1, mirror 0" bash -c "grep -q 'isp.flip_mirror 1 0 rc 0' '$D2B/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.ceiling
want "ceiling reads off while only the flip is on" 200 '"value":"0"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.ceiling":true}'
want "PATCH image.ceiling turns both on" 200 '"image.ceiling":\{"ok":true'
check "the ISP got flip 1, mirror 1" bash -c "grep -q 'isp.flip_mirror 1 1 rc 0' '$D2B/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.mirror
want "image.mirror reads on" 200 '"value":"1"'
req "$WID" "$WSEC" GET /akam/v1/settings/image.ceiling
want "image.ceiling reads on while both are" 200 '"value":"1"'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.mirror":false}'
req "$WID" "$WSEC" GET /akam/v1/settings/image.ceiling
want "turning the mirror off makes the ceiling read off" 200 '"value":"0"'
: >"$D2B/calls.log"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.flip":true}'
want "setting the value it already has is accepted" 200 '"image.flip":\{"ok":true'
check "...and sends nothing to the ISP" bash -c "! grep -q 'isp.flip_mirror' '$D2B/calls.log'"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.flip":"yes"}'
want "a non boolean is refused" 422
: >"$D2B/flip_fail"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.mirror":true}'
check "an ISP refusal is reported, not applied" bash -c "printf '%s' '$BODY' | grep -q '\"image.mirror\":{\"ok\":false'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.mirror
want "...and the value stays off (the orientation is rolled back)" 200 '"value":"0"'
rm -f "$D2B/flip_fail"
sleep 2.2
check "flip is saved (a user change), the ceiling row and the off mirror are not" bash -c "grep -q '^image.flip=1' '$D2B/settings' && ! grep -q 'image.ceiling' '$D2B/settings' && ! grep -q 'image.mirror' '$D2B/settings'"
stop_srv && ok "stop saves and exits cleanly" || ko "stop (see $D2B/err)"
: >"$D2B/calls.log"
start_srv "$D2B" --fast
check "the saved orientation is applied at the next start (live setter ran with the saved value)" bash -c "grep -q 'isp.flip_mirror 1 0 rc 0' '$D2B/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.flip
want "the flip survived the restart" 200 '"value":"1"'
stop_srv || true

# image.ceiling ALONE (the Home Assistant "Ceiling mount" switch sends exactly this): the file must get image.flip and image.mirror, and a restart must read both on.
echo "== image.ceiling alone is saved (through image.flip and image.mirror) and survives a restart"
D2C=$WORK/p2c
mkdir -p "$D2C"
newkey "$D2C" write ha
WID=$KID
WSEC=$KSEC
start_srv "$D2C" --fast
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.ceiling":true}'
want "PATCH image.ceiling alone is applied" 200 '"image.ceiling":\{"ok":true'
sleep 2.2
check "the file holds image.flip=1 and image.mirror=1 and no image.ceiling key" bash -c "grep -q '^image.flip=1' '$D2C/settings' && grep -q '^image.mirror=1' '$D2C/settings' && ! grep -q 'image.ceiling' '$D2C/settings'"
stop_srv || true
: >"$D2C/calls.log"
start_srv "$D2C" --fast
check "the restart applies flip 1 mirror 1 to the ISP" bash -c "grep -q 'isp.flip_mirror 1 1 rc 0' '$D2C/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.flip
want "flip reads 1 after the restart" 200 '"value":"1"'
req "$WID" "$WSEC" GET /akam/v1/settings/image.mirror
want "mirror reads 1 after the restart" 200 '"value":"1"'
req "$WID" "$WSEC" GET /akam/v1/settings/image.ceiling
want "ceiling reads 1 after the restart" 200 '"value":"1"'
stop_srv || true

# image.brightness / contrast / sharpness / mains_hz are saved (the ISP keeps none): written only on a user change, applied at start, never written at start or while idle.
echo "== image.* rows are saved, applied at start, and not rewritten by a restart"
D2I=$WORK/p2i
mkdir -p "$D2I"
newkey "$D2I" write hi
WID=$KID
WSEC=$KSEC
start_srv "$D2I" --fast
sleep 1.2
check "a fresh start writes no settings file" bash -c "[ ! -e '$D2I/settings' ]"
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"image.sharpness":70}'
want "PATCH image.sharpness 70" 200 '"image.sharpness":\{"ok":true'
sleep 2.2
check "the file holds image.sharpness=70 only" bash -c "grep -qx 'image.sharpness=70' '$D2I/settings' && [ \$(grep -c image '$D2I/settings') = 1 ]"
stop_srv || true
SIG1=$(stat -c '%i %Y %s' "$D2I/settings")
: >"$D2I/calls.log"
start_srv "$D2I" --fast
check "the restart applies the saved sharpness to the ISP (brightness and contrast untouched, mains 50)" bash -c "grep -q 'image -1 -1 70 50' '$D2I/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/settings/image.sharpness
want "sharpness reads 70 after the restart" 200 '"value":"70"'
sleep 2.5
check "the restart and the idle time after it rewrote nothing" bash -c "[ '$SIG1' = \"\$(stat -c '%i %Y %s' '$D2I/settings')\" ]"
stop_srv || true
check "stopping without a change rewrote nothing" bash -c "[ '$SIG1' = \"\$(stat -c '%i %Y %s' '$D2I/settings')\" ]"

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

# ================================================================== phase 5: PTZ patrol (specs/ptz-patrol.md)
echo "== PTZ patrol: presets, routes, actions, events, priorities, flash policy"
D6=$WORK/p6
mkdir -p "$D6"
newkey "$D6" write ha
WID=$KID
WSEC=$KSEC
newkey "$D6" read ro
RID=$KID
RSEC=$KSEC
start_srv "$D6" --fast
st_val() { # st_val <state key>: the value from /state
    req "$WID" "$WSEC" GET /akam/v1/settings/"$1"
    printf '%s' "$BODY" | sed -n 's/.*"value":"\([^"]*\)".*/\1/p'
}
wait_st() { # wait_st <state key> <value> [seconds]
    local i
    for i in $(seq 1 $((${3:-8} * 10))); do [ "$(st_val "$1")" = "$2" ] && return 0; sleep 0.1; done
    return 1
}
evlog() { req "$WID" "$WSEC" GET /akam/v1/events "" "since=$1&timeout=0"; }
seqnow() { req "$WID" "$WSEC" GET /akam/v1/events; printf '%s' "$BODY" | sed -n 's/^{"seq":\([0-9]*\).*/\1/p'; }
pw() { st_val sys.flash_writes_patrol; }
sw() { st_val sys.flash_writes_store; }
want_ev() { # want_ev <desc> <since> <regex>: the events after <since> match
    evlog "$2"
    if printf '%s' "$BODY" | grep -Eq -- "$3"; then ok "$1"; else ko "$1 ($(printf '%s' "$BODY" | head -c 300))"; fi
}
req "$RID" "$RSEC" GET /akam/v1/ptz/presets
want "GET /ptz/presets (read key): empty at first, max 16" 200 '"presets":\[\],"max":16'
req "$RID" "$RSEC" POST /akam/v1/ptz/presets '{"name":"door","pan":900,"tilt":1200}'
want "POST /ptz/presets needs a write key (403)" 403 'scope'
check "patrol off by default: no patrol call at all before a start" bash -c "! grep -q 'patrol.move' '$D6/calls.log' 2>/dev/null"
[ "$(st_val ptz.patrol.state)" = idle ] && [ "$(st_val ptz.patrol.step)" = -1 ] && ok "ptz.patrol.state is idle, step -1 (default off)" || ko "ptz.patrol.state idle"
[ "$(pw)" = 0 ] && ok "no patrol flash write so far" || ko "no patrol flash write so far"
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"door","pan":900,"tilt":1200}'
want "POST /ptz/presets creates a preset" 200 '"ok":true,"created":true,"name":"door"'
[ "$(pw)" = 1 ] && ok "one flash write for the first change" || ko "one flash write for the first change ($(pw))"
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"door","pan":900,"tilt":1200}'
want "the same preset again: ok, created false" 200 '"created":false'
[ "$(pw)" = 1 ] && ok "an unchanged config is not written again" || ko "an unchanged config is not written again ($(pw))"
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"here1","here":true}'
want "POST /ptz/presets here:true records the current position" 200 '"created":true'
req "$WID" "$WSEC" GET /akam/v1/ptz/presets/here1
want "...the fake motor sits at 1275/1300" 200 '"pan":1275,"tilt":1300'
[ "$(pw)" = 2 ] && ok "two changes, two flash writes" || ko "two changes, two flash writes ($(pw))"
req "$WID" "$WSEC" GET /akam/v1/ptz/presets
want "GET /ptz/presets lists both" 200 '"name":"door","pan":900,"tilt":1200.*"name":"here1"'
req "$WID" "$WSEC" GET /akam/v1/ptz/presets/door
want "GET /ptz/presets/<name>" 200 '^\{"name":"door","pan":900,"tilt":1200\}$'
req "$WID" "$WSEC" GET /akam/v1/ptz/presets/nope
want "an unknown preset is 404 not_found" 404 'not_found'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"far","pan":5000,"tilt":1200}'
want "a position outside the soft range is 422 range" 422 '"error":"range"'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"Bad Name","pan":900,"tilt":1200}'
want "a bad name is 422 bad_name" 422 'bad_name'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"x","pan":900,"tilt":1200,"extra":1}'
want "an unknown key is 400 bad_json" 400 'bad_json'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets 'not json'
want "garbage is 400 bad_json" 400 'bad_json'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"x","pan":900.5,"tilt":1200}'
want "a fraction is refused (400)" 400 'bad_json'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets/porch '{"pan":1500,"tilt":1100}'
want "POST /ptz/presets/<name> takes the name from the path" 200 '"name":"porch"'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets/porch '{"name":"other","pan":1500,"tilt":1100}'
want "a body name that differs from the path is refused" 422 'bad_name'
[ "$(pw)" = 3 ] && ok "refused requests wrote nothing (3 writes)" || ko "refused requests wrote nothing ($(pw))"
# routes
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"yard","mode":"loop","steps":[{"preset":"door","speed":3,"dwell_ms":300},{"pan":1500,"tilt":1300,"speed":5,"dwell_ms":300},{"preset":"porch"}]}'
want "POST /ptz/routes creates a route (presets and explicit steps)" 200 '"created":true,"name":"yard"'
req "$WID" "$WSEC" GET /akam/v1/ptz/routes/yard
want "GET /ptz/routes/<name>: the full step list" 200 '"mode":"loop","steps":\[\{"preset":"door","speed":3,"dwell_ms":300\},\{"pan":1500,"tilt":1300,"speed":5,"dwell_ms":300\},\{"preset":"porch","speed":0,"dwell_ms":0\}\]'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad1","steps":[{"preset":"nothere"}]}'
want "a step that names an unknown preset is 422 no_preset" 422 'no_preset'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad2","steps":[{"pan":900,"tilt":1200,"speed":25}]}'
want "a speed outside 2..20 is 422 speed" 422 '"error":"speed"'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad3","steps":[{"pan":100,"tilt":1200}]}'
want "a step outside the soft range is 422 range" 422 '"error":"range"'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad4","mode":"zigzag","steps":[{"pan":900,"tilt":1200}]}'
want "an unknown mode is 422 bad_mode" 422 'bad_mode'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad5","steps":[{"pan":900,"tilt":1200,"dwell_ms":4000000}]}'
want "a dwell over 3600000 is 422 dwell" 422 '"error":"dwell"'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad6","steps":[]}'
want "no step is 422 empty" 422 '"error":"empty"'
many=$(for i in $(seq 1 33); do printf '{"pan":900,"tilt":1200},'; done)
req "$WID" "$WSEC" POST /akam/v1/ptz/routes "{\"name\":\"bad7\",\"steps\":[${many%,}]}"
want "33 steps are refused (422 too_many_steps)" 422 'too_many_steps'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad8","steps":[{"preset":"door","pan":900,"tilt":1200}]}'
want "a step with a preset AND a position is refused (400)" 400 'bad_json'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"bad9","steps":[{"pan":900,"tilt":1200}],"pattern":{"type":"sweep_h","pan_from":400,"pan_to":900,"tilt":1200,"step":250}}'
want "steps and a pattern together are refused (400)" 400 'bad_json'
[ "$(pw)" = 4 ] && ok "every refused route wrote nothing (4 writes)" || ko "every refused route wrote nothing ($(pw))"
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"sweep","mode":"pingpong","pattern":{"type":"sweep_h","pan_from":400,"pan_to":3000,"tilt":1300,"step":500,"speed":4,"dwell_ms":300}}'
want "a sweep_h pattern is stored" 200 '"created":true'
req "$WID" "$WSEC" GET /akam/v1/ptz/routes/sweep
want "...expanded into 7 plain steps ending exactly at pan_to" 200 '"pan":400,"tilt":1300,"speed":4,"dwell_ms":300\},\{"pan":900.*\{"pan":2900,"tilt":1300,"speed":4,"dwell_ms":300\},\{"pan":3000,"tilt":1300,"speed":4,"dwell_ms":300\}\]'
n=$(printf '%s' "$BODY" | grep -o '"pan"' | wc -l)
[ "$n" = 7 ] && ok "sweep_h: 7 steps" || ko "sweep_h: 7 steps ($n)"
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"scan","mode":"loop","pattern":{"type":"raster","pan_from":600,"pan_to":2400,"tilt_from":800,"tilt_to":1600,"rows":3,"cols":3,"speed":6,"dwell_ms":200}}'
want "a raster pattern is stored" 200 '"created":true'
req "$WID" "$WSEC" GET /akam/v1/ptz/routes/scan
n=$(printf '%s' "$BODY" | grep -o '"pan"' | wc -l)
[ "$n" = 9 ] && ok "raster: 3 rows x 3 columns = 9 steps" || ko "raster: 9 steps ($n)"
want "...boustrophedon (the second row runs back)" 200 '"pan":2400,"tilt":1200,"speed":6,"dwell_ms":200\},\{"pan":1500,"tilt":1200.*\{"pan":600,"tilt":1200'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"big","pattern":{"type":"sweep_h","pan_from":300,"pan_to":3650,"tilt":1300,"step":50}}'
want "a sweep that needs over 32 steps is refused (422 too_many_steps)" 422 'too_many_steps'
req "$WID" "$WSEC" GET /akam/v1/ptz/routes
want "GET /ptz/routes: summaries, max 8, no default yet" 200 '"name":"yard","mode":"loop","steps":3,"running":false.*"max":8,"default":""'
req "$WID" "$WSEC" DELETE /akam/v1/ptz/presets/door
want "a preset used by a route cannot be deleted (409 in_use)" 409 'in_use'
req "$WID" "$WSEC" DELETE /akam/v1/ptz/presets/here1
want "an unused preset is deleted" 200 '"deleted":"here1"'
req "$WID" "$WSEC" DELETE /akam/v1/ptz/presets/here1
want "...and then it is 404" 404 'not_found'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"once1","mode":"once","steps":[{"pan":1000,"tilt":1300,"dwell_ms":250},{"pan":1100,"tilt":1300,"dwell_ms":250}]}'
want "a once route" 200 '"created":true'
[ "$(pw)" = 8 ] && ok "8 real changes (presets, routes, a delete), 8 flash writes (one per change)" || ko "one write per change ($(pw))"
# (the signed-request nonce cache holds 64 requests per minute: the run checks use fresh servers and long-poll the events instead of polling)
stop_srv && ok "stage 1 server exits cleanly (config edits)" || ko "stage 1 server exit (see $D6/err)"
waitev() { # waitev <since> <regex> [seconds]: long-polls the events until the accumulated text matches
    local since=$1 re=$2 end=$((SECONDS + ${3:-8}))
    ACC=""
    while [ "$SECONDS" -lt "$end" ]; do
        req "$WID" "$WSEC" GET /akam/v1/events "" "since=$since&timeout=2"
        [ "$STATUS" = 200 ] || return 1
        ACC="$ACC$BODY"
        printf '%s' "$ACC" | grep -Eq -- "$re" && return 0
        since=$(printf '%s' "$BODY" | sed -n 's/^{"seq":\([0-9]*\).*/\1/p')
    done
    return 1
}
wev() { # wev <description> <since> <regex> [seconds]
    if waitev "$2" "$3" "${4:-8}"; then ok "$1"; else ko "$1 ($(printf '%s' "$ACC" | head -c 300) status=$STATUS $(printf '%s' "$BODY" | head -c 150))"; fi
}
stv() { # stv <key>: one value from /state
    req "$WID" "$WSEC" GET /akam/v1/state
    printf '%s' "$BODY" | sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p"
}
echo "== PTZ patrol: run, tracking priority, manual stop, events (config persisted from stage 1)"
start_srv "$D6" --fast
S0=$(seqnow)
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"nothere"}'
want "start of an unknown route is 404" 404 'not_found'
req "$RID" "$RSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"yard"}'
want "ptz.patrol.start needs a write key (403)" 403 'scope'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"yard"}'
want "ptz.patrol.start {route}" 200 '"ok":true'
wev "events: patrol started, then a step" "$S0" '"event":"started","route":"yard","step":0.*"event":"step","route":"yard","step":'
req "$WID" "$WSEC" GET /akam/v1/state
want "ptz.patrol.state is running on route yard" 200 '"ptz.patrol.state":"running".*"ptz.patrol.route":"yard"'
sleep 6
check "the moves went to the motor through its safe move (preset door at speed 3, explicit at 5, porch at the default 3)" bash -c "grep -q 'patrol.move 900 1200 3' '$D6/calls.log' && grep -q 'patrol.move 1500 1300 5' '$D6/calls.log' && grep -q 'patrol.move 1500 1100 3' '$D6/calls.log'"
req "$WID" "$WSEC" GET /akam/v1/ptz/routes
want "GET /ptz/routes marks the running route" 200 '"name":"yard","mode":"loop","steps":3,"running":true'
req "$WID" "$WSEC" POST /akam/v1/ptz/routes '{"name":"yard","steps":[{"pan":900,"tilt":1200}]}'
want "replacing the running route is refused (409 running)" 409 'running'
req "$WID" "$WSEC" DELETE /akam/v1/ptz/routes/yard
want "deleting the running route is refused (409 running)" 409 'running'
# tracking preempts and the patrol resumes at the interrupted step
S1=$(seqnow)
echo "person 1" >&3
wev "a tracked person pauses the patrol (interrupted, reason tracking)" "$S1" '"event":"interrupted","route":"yard","step":[0-9]+,"reason":"tracking"'
req "$WID" "$WSEC" GET /akam/v1/state
want "ptz.patrol.state is paused, the route and step are kept" 200 '"ptz.patrol.state":"paused".*"ptz.patrol.route":"yard"'
M0=$(grep -c 'patrol.move' "$D6/calls.log")
sleep 0.8
echo "person 1" >&3
sleep 0.8
M1=$(grep -c 'patrol.move' "$D6/calls.log")
[ "$M0" = "$M1" ] && ok "no patrol move while paused (and a new detection restarts the resume timer)" || ko "no patrol move while paused ($M0 $M1)"
wev "no person for the return time: the patrol resumes at the interrupted step" "$S1" '"event":"resumed","route":"yard","step":[0-9]+'
req "$WID" "$WSEC" GET /akam/v1/state
want "...and it runs again" 200 '"ptz.patrol.state":"running"'
# a manual command stops it
S2=$(seqnow)
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.step '{"dir":"left"}'
want "a manual PTZ command (ptz.step)" 200 '"ok":true'
wev "...stops the patrol (stopped, reason manual)" "$S2" '"event":"stopped","route":"yard","step":[0-9]+,"reason":"manual"'
req "$WID" "$WSEC" GET /akam/v1/state
want "idle: route empty, step -1" 200 '"ptz.patrol.state":"idle".*"ptz.patrol.route":"".*"ptz.patrol.step":"-1"'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.stop
want "ptz.patrol.stop while idle is ok (idempotent)" 200 '"ok":true'
# replace, park
S3=$(seqnow)
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"sweep"}'
want "start sweep" 200 '"ok":true'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"scan"}'
want "starting another route replaces the running one" 200 '"ok":true'
wev "events: stopped (replaced) for the old route, started for the new one" "$S3" '"event":"stopped","route":"sweep","step":[0-9]+,"reason":"replaced".*"event":"started","route":"scan"'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.park
wev "ptz.park stops the patrol (reason park)" "$S3" '"event":"stopped","route":"scan","step":[0-9]+,"reason":"park"'
# once
S4=$(seqnow)
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"once1"}'
want "start the once route" 200 '"ok":true'
wev "a once route ends by itself (stopped, reason done)" "$S4" '"event":"stopped","route":"once1","step":1,"reason":"done"'
check "the once route made exactly its two moves" bash -c "[ \$(grep -c 'patrol.move 1[01]00 1300' '$D6/calls.log') = 2 ]"
# goto preset
S5=$(seqnow)
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.start '{"route":"yard"}'
sleep 0.5
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.goto_preset '{"name":"door"}'
want "ptz.goto_preset {name}" 200 '"ok":true'
check "...a manual move to the preset" grep -q 'ptz.goto 900 1200' "$D6/calls.log"
wev "...and it stopped the running patrol (reason manual)" "$S5" '"event":"stopped","route":"yard","step":[0-9]+,"reason":"manual"'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.goto_preset '{"name":"nope"}'
want "an unknown preset is 404" 404 'not_found'
req "$WID" "$WSEC" GET /akam/v1/state
want "zero patrol flash writes while patrolling, pausing, resuming, stopping" 200 '"sys.flash_writes_patrol":"0"'
stop_srv && ok "stage 2 server exits cleanly (ASan/UBSan clean)" || ko "stage 2 server exit (see $D6/err)"

echo "== PTZ patrol: settings, idle trigger, persistence"
start_srv "$D6" --fast
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"ptz.patrol.route_default":"Bad Name"}'
want "route_default: a bad name is refused" 422 'check'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"ptz.patrol.idle_start_s":100000}'
want "idle_start_s: over 86400 is refused" 422 'range'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"ptz.patrol.route_default":"yard"}'
want "ptz.patrol.route_default is a setting" 200 '"ptz.patrol.route_default":\{"ok":true'
req "$WID" "$WSEC" DELETE /akam/v1/ptz/routes/yard
want "the default route cannot be deleted (409 in_use)" 409 'in_use'
req "$WID" "$WSEC" GET /akam/v1/ptz/routes
want "GET /ptz/routes shows the default" 200 '"default":"yard"'
sleep 2.5
req "$WID" "$WSEC" GET /akam/v1/state
want "a default route alone starts nothing (idle_start_s 0 = off)" 200 '"ptz.patrol.state":"idle"'
S6=$(seqnow)
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"ptz.patrol.idle_start_s":2}'
want "ptz.patrol.idle_start_s 2" 200 '"ptz.patrol.idle_start_s":\{"ok":true'
sleep 0.8
req "$WID" "$WSEC" GET /akam/v1/state
want "idle trigger: not before the idle time" 200 '"ptz.patrol.state":"idle"'
wev "idle trigger: the default route starts after 2 s without person or command" "$S6" '"event":"started","route":"yard"' 6
echo "person 1" >&3
sleep 0.4
req "$WID" "$WSEC" GET /akam/v1/state
want "a person also pauses an idle-started patrol" 200 '"ptz.patrol.state":"paused"'
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.patrol.stop
want "ptz.patrol.stop (also from paused)" 200 '"ok":true'
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"ptz.patrol.idle_start_s":0}'
sleep 2.8
req "$WID" "$WSEC" GET /akam/v1/state
want "idle_start_s 0: it stays off" 200 '"ptz.patrol.state":"idle"'
want "the config was not written by any of this (0 patrol writes)" 200 '"sys.flash_writes_patrol":"0"'
req "$WID" "$WSEC" GET /akam/v1/schema
want "the schema lists the patrol actions" 200 '"ptz.patrol.start".*"ptz.patrol.stop".*"ptz.goto_preset"'
printf '%s' "$BODY" | grep -q '"key":"ptz.patrol.idle_start_s"' && ok "schema lists ptz.patrol.idle_start_s" || ko "schema lists ptz.patrol.idle_start_s"
printf '%s' "$BODY" | grep -q '"key":"ptz.patrol.route_default"' && ok "schema lists ptz.patrol.route_default" || ko "schema lists ptz.patrol.route_default"
stop_srv && ok "stage 3 server exits cleanly" || ko "stage 3 server exit (see $D6/err)"
check "the config is one file /config/akam/ptz_patrol (text lines)" bash -c "grep -q '^p door 900 1200\$' '$D6/ptz_patrol' && grep -q '^r yard loop\$' '$D6/ptz_patrol' && grep -q '^s door 3 300\$' '$D6/ptz_patrol'"
start_srv "$D6" --fast
req "$WID" "$WSEC" GET /akam/v1/ptz/routes/yard
want "after a restart the routes are back" 200 '"preset":"door","speed":3,"dwell_ms":300'
req "$WID" "$WSEC" GET /akam/v1/ptz/presets
want "...and the presets" 200 '"name":"door".*"name":"porch"'
sleep 2.5
req "$WID" "$WSEC" GET /akam/v1/state
want "a restart starts no patrol and writes nothing (default OFF: idle_start_s is back from the settings file only if it was saved)" 200 '"sys.flash_writes_patrol":"0"'
stop_srv || true
# an invalid file is ignored, and the config starts empty
printf 'garbage line\n' >"$D6/ptz_patrol"
start_srv "$D6" --fast
req "$WID" "$WSEC" GET /akam/v1/ptz/presets
want "an invalid ptz_patrol file is ignored (empty config)" 200 '"presets":\[\]'
check "...and the bad file was left alone" grep -q 'garbage line' "$D6/ptz_patrol"
stop_srv || true
# the write limiter: a burst of 8 real changes, then 429 with Retry-After
D7=$WORK/p7
mkdir -p "$D7"
newkey "$D7" write ha
WID=$KID
WSEC=$KSEC
start_srv "$D7" --fast
for i in 1 2 3 4 5 6 7 8; do req "$WID" "$WSEC" POST /akam/v1/ptz/presets "{\"name\":\"p$i\",\"pan\":$((400 + i * 100)),\"tilt\":1000}"; done
want "8 config writes in a burst are accepted" 200 '"name":"p8"'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"p9","pan":1000,"tilt":1000}'
want "the 9th is rate limited (429 rate)" 429 '"error":"rate"'
printf '%s' "$HDRS" | grep -qi '^Retry-After: [0-9]' && ok "...with Retry-After" || ko "429 carries Retry-After"
req "$WID" "$WSEC" GET /akam/v1/ptz/presets/p9
want "...and nothing was applied (all or nothing)" 404 'not_found'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"p8","pan":1300,"tilt":1000}'
want "a change is limited even when it replaces" 429 'rate'
req "$WID" "$WSEC" POST /akam/v1/ptz/presets '{"name":"p8","pan":1200,"tilt":1000}'
want "an identical request needs no write, so it is not limited" 200 '"created":false'
stop_srv && ok "the limiter server exits cleanly" || ko "limiter server exit"

# ================================================================== phase 8: the programmable button
echo "== programmable button: bindings, modes, simulated presses, events, persistence, flash policy"
D8=$WORK/p8
mkdir -p "$D8"
newkey "$D8" write ha
WID=$KID
WSEC=$KSEC
newkey "$D8" read ro
RID=$KID
RSEC=$KSEC
start_srv "$D8" --fast
DEFJ='{"timing":{"debounce_ms":50,"short_max_ms":1000,"gap_ms":400,"long_ms":2000,"very_long_ms":5000},"led_ack":true,"bindings":[{"gesture":"short","actions":["none"]},{"gesture":"long","actions":["ptz.toggle_park"]}]}'
bw() { st_val sys.flash_writes_button; }
req "$RID" "$RSEC" GET /akam/v1/button
want "GET /button (read key): the defaults are today's button (short -> none, long -> ptz.toggle_park)" 200 'bindings.*"gesture":"short","actions":\["none"\].*"gesture":"long","actions":\["ptz.toggle_park"\]'
[ "$BODY" = "$DEFJ" ] && ok "the default config is the golden JSON" || ko "default config JSON ($BODY)"
req "$RID" "$RSEC" POST /akam/v1/button "$DEFJ"
want "POST /button needs a write key (403)" 403 'scope'
req "$RID" "$RSEC" POST /akam/v1/modes/night '{"settings":{"led.mode":"off"}}'
want "POST /modes/<name> needs a write key (403)" 403 'scope'
req "$RID" "$RSEC" DELETE /akam/v1/modes/night
want "DELETE /modes/<name> needs a write key (403)" 403 'scope'
req "$RID" "$RSEC" GET /akam/v1/modes
want "GET /modes: empty at first" 200 '\{"modes":\[\]\}'
[ "$(bw)" = 0 ] && ok "no button flash write so far (sys.flash_writes_button 0)" || ko "button writes at start ($(bw))"
S0=$(seqnow)
# the factory behaviour: short = event + blink only, long = the park toggle; nothing else is called
echo "btn S" >&3
wev "default short press: the API event {gesture short, actions none, result ok}" "$S0" '"type":"button","data":\{"gesture":"short","actions":\["none"\],"result":"ok"\}'
check "... one MQTT publish for it, the double blink cue, no action run" bash -c "grep -q '^btn.mqtt short {\"gesture\":\"short\"' '$D8/calls.log' && grep -q '^btn.cue 1\$' '$D8/calls.log' && ! grep -q '^btn.run' '$D8/calls.log'"
S1=$(seqnow)
echo "btn L" >&3
wev "default long press: the API event {gesture long, actions ptz.toggle_park}" "$S1" '"gesture":"long","actions":\["ptz.toggle_park"\],"result":"ok"'
check "... the park toggle ran (once), nothing else" bash -c "[ \$(grep -c '^btn.run 3 ' '$D8/calls.log') = 1 ] && [ \$(grep -c '^btn.run' '$D8/calls.log') = 1 ]"
[ "$(st_val button.last_gesture)" = long ] && ok "state button.last_gesture = long" || ko "button.last_gesture ($(st_val button.last_gesture))"
echo "btn S S" >&3
sleep 0.3
S2=$(seqnow)
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$S1&timeout=0"
want "with only short and long bound nothing waits: S S is two short events, no delay" 200 '"gesture":"short".*"gesture":"short"'
echo "locked 1" >&3
echo "btn S" >&3
sleep 0.3
req "$WID" "$WSEC" GET /akam/v1/events "" "since=$S2&timeout=0"
want "during the PTZ calibration a press publishes nothing" 200 '"events":\[\]'
echo "locked 0" >&3
# (the server is restarted between the parts: a signed request refused for its scope leaves its challenge pending for 30 s and the pool holds 8)
stop_srv && ok "part 1 server exits cleanly" || ko "part 1 server exit"
start_srv "$D8" --fast
# modes
req "$WID" "$WSEC" POST /akam/v1/modes/night '{"settings":{"led.mode":"off","image.brightness":30}}'
want "POST /modes/night: created" 200 '"created":true,"name":"night"'
[ "$(bw)" = 1 ] && ok "one flash write for it (sys.flash_writes_button 1)" || ko "button writes after the first mode ($(bw))"
req "$WID" "$WSEC" POST /akam/v1/modes/night '{"settings":{"led.mode":"off","image.brightness":30}}'
want "the same mode again: accepted, no new write" 200 '"created":false'
[ "$(bw)" = 1 ] && ok "... still 1 write" || ko "writes after an identical mode ($(bw))"
req "$WID" "$WSEC" POST /akam/v1/modes/badmix '{"settings":{"led.mode":"on","video0.min_qp":40,"video0.max_qp":20}}'
want "a mode whose pairs are each valid is accepted (the cross-field rule is judged at apply)" 200 '"created":true'
req "$WID" "$WSEC" POST /akam/v1/modes/bad1 '{"settings":{"net.hostname":"cam"}}'
want "a network key in a mode is refused" 422 'net.hostname'
req "$WID" "$WSEC" POST /akam/v1/modes/bad2 '{"settings":{"api.keys":"3"}}'
want "a read-only (state) key in a mode is refused" 422 'api.keys'
req "$WID" "$WSEC" POST /akam/v1/modes/bad3 '{"settings":{"led.mode":"purple"}}'
want "an invalid value in a mode is refused" 422 'led.mode'
req "$WID" "$WSEC" POST /akam/v1/modes/bad4 '{"settings":{"nope.key":"1"}}'
want "an unknown key in a mode is refused" 422 'nope.key'
req "$WID" "$WSEC" POST /akam/v1/modes/Bad '{"settings":{"led.mode":"on"}}'
want "a bad mode name is refused" 422 'mode name'
req "$WID" "$WSEC" POST /akam/v1/modes/empty '{"settings":{}}'
want "an empty mode is refused" 422 'value'
req "$WID" "$WSEC" POST /akam/v1/modes/nine '{"settings":{"led.mode":"on","image.brightness":1,"image.contrast":1,"image.sharpness":1,"detect.publish_hz":1,"track.enabled":0,"led.person":"on","led.boot":"on","led.night":"on"}}'
want "a mode with 9 pairs is refused (limit 8)" 422 'limit'
req "$WID" "$WSEC" POST /akam/v1/modes/bad5 '{"settings":{"led.mode":"on"'
want "malformed JSON is 400" 400 'syntax'
[ "$(bw)" = 2 ] && ok "none of the refusals wrote anything (2 writes: night, badmix)" || ko "writes after refusals ($(bw))"
req "$RID" "$RSEC" GET /akam/v1/modes
want "GET /modes lists both" 200 '"name":"night".*"name":"badmix"'
req "$RID" "$RSEC" GET /akam/v1/modes/night
want "GET /modes/night" 200 '\{"name":"night","settings":\{"led.mode":"off","image.brightness":"30"\}\}'
req "$RID" "$RSEC" GET /akam/v1/modes/ghost
want "GET /modes/ghost is 404" 404 'not_found'
# mode.apply: all or nothing through the registry
req "$WID" "$WSEC" POST /akam/v1/actions/mode.apply '{"name":"night"}'
want "action mode.apply night" 200 '"ok":true'
[ "$(st_val led.mode)" = off ] && [ "$(st_val image.brightness)" = 30 ] && ok "both settings of the mode were applied" || ko "mode night applied (led.mode=$(st_val led.mode) image.brightness=$(st_val image.brightness))"
req "$WID" "$WSEC" POST /akam/v1/actions/mode.apply '{"name":"badmix"}'
want "mode.apply of a mode that fails its cross-field rule is refused" 422 'mode_refused'
[ "$(st_val led.mode)" = off ] && [ "$(st_val video0.min_qp)" != 40 ] && ok "ALL OR NOTHING: led.mode (valid, first in the mode) was not applied either" || ko "all or nothing (led.mode=$(st_val led.mode) min_qp=$(st_val video0.min_qp))"
req "$WID" "$WSEC" POST /akam/v1/actions/mode.apply '{"name":"ghost"}'
want "mode.apply of an unknown mode is 404" 404 'not_found'
req "$WID" "$WSEC" POST /akam/v1/actions/mode.apply '{"name":"Night"}'
want "mode.apply with a bad name is 422" 422 'args'
req "$RID" "$RSEC" POST /akam/v1/actions/mode.apply '{"name":"night"}'
want "mode.apply needs a write key (403)" 403 'scope'
req "$RID" "$RSEC" GET /akam/v1/schema
want "the schema lists mode.apply" 200 '"mode.apply"'
printf '%s' "$BODY" | grep -q '"key":"button.last_gesture"' && ok "schema lists button.last_gesture (state)" || ko "schema lists button.last_gesture"
printf '%s' "$BODY" | grep -q '"key":"sys.flash_writes_button"' && ok "schema lists sys.flash_writes_button" || ko "schema lists sys.flash_writes_button"
# bindings
stop_srv && ok "part 2 server exits cleanly" || ko "part 2 server exit"
start_srv "$D8" --fast
[ "$(bw)" = 0 ] && ok "the modes are on the flash; a restart counts its own writes from 0" || ko "writes after the restart ($(bw))"
BIND='{"timing":{"gap_ms":300},"bindings":[{"gesture":"short","actions":["none"]},{"gesture":"double","actions":["mode night","ptz.home","led pattern ack"]},{"gesture":"triple","actions":["mode badmix","ptz.park"]},{"gesture":"long","actions":["ptz.toggle_park"]},{"gesture":"S S L","actions":["track toggle","patrol stop"]},{"gesture":"very_long","actions":["ptz.privacy on"]}]}'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"double","actions":["mode ghost"]}]}'
want "a binding to a mode that does not exist is refused" 422 'no_such_mode'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"double","actions":["reboot"]}]}'
want "a destructive action (reboot) is refused" 422 'not an allowed action'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"double","actions":["settings.reset"]}]}'
want "settings.reset is not a button action" 422 'not an allowed action'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"double","actions":["net.wifi_band 5g"]}]}'
want "a network change is not a button action" 422 'not an allowed action'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"double","actions":["none","none","none","none","none"]}]}'
want "5 actions in a binding: refused" 422 'limit'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"short","actions":["none"]},{"gesture":"S","actions":["none"]}]}'
want "the same gesture twice: refused" 422 'same gesture'
req "$WID" "$WSEC" POST /akam/v1/button '{"timing":{"gap_ms":20},"bindings":[]}'
want "a timing out of range: refused" 422 'timing'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":[{"gesture":"S S S S S S S","actions":["none"]}]}'
want "7 elements in a gesture: refused" 422 'gesture'
req "$WID" "$WSEC" POST /akam/v1/button '{"bindings":'
want "malformed JSON: 400" 400 'syntax'
req "$WID" "$WSEC" GET /akam/v1/button
[ "$BODY" = "$DEFJ" ] && ok "none of the refusals changed the config (all or nothing)" || ko "config after refusals ($BODY)"
[ "$(bw)" = 0 ] && ok "... or wrote anything" || ko "writes after refusals ($(bw))"
req "$WID" "$WSEC" POST /akam/v1/button "$BIND"
want "POST /button: applied" 200 '"ok":true,"changed":true'
[ "$(bw)" = 1 ] && ok "one flash write (sys.flash_writes_button 1)" || ko "button writes after the config ($(bw))"
check "MQTT was told to announce the new triggers" grep -q '^btn.changed' "$D8/calls.log"
req "$RID" "$RSEC" GET /akam/v1/button
want "GET /button returns it" 200 '"gap_ms":300.*"gesture":"double","actions":\["mode night","ptz.home","led pattern ack"\]'
req "$WID" "$WSEC" POST /akam/v1/button "$BIND"
want "the same config again: accepted, changed false" 200 '"changed":false'
[ "$(bw)" = 1 ] && ok "... and no flash write (still 1)" || ko "writes after an identical config ($(bw))"
req "$WID" "$WSEC" DELETE /akam/v1/modes/night
want "DELETE of a mode a binding uses: 409" 409 'in_use'
req "$WID" "$WSEC" DELETE /akam/v1/button
want "DELETE /button: 405" 405 'method'
# simulated presses with the new bindings
: >"$D8/calls.log"
S3=$(seqnow)
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"on","image.brightness":55}'
echo "btn S S" >&3
wev "double: event with the action list and result ok" "$S3" '"gesture":"double","actions":\["mode night","ptz.home","led pattern ack"\],"result":"ok"'
[ "$(st_val led.mode)" = off ] && [ "$(st_val image.brightness)" = 30 ] && ok "double applied the mode night (both settings)" || ko "double -> mode night (led.mode=$(st_val led.mode))"
check "... then ptz.home and the LED pattern ran, in order" bash -c "grep '^btn.run' '$D8/calls.log' | sed -n '1p' | grep -q 'btn.run 1 ' && grep '^btn.run' '$D8/calls.log' | sed -n '2p' | grep -q 'btn.run 11 2:150:150:0'"
[ "$(st_val button.last_gesture)" = double ] && ok "button.last_gesture = double" || ko "last_gesture ($(st_val button.last_gesture))"
S4=$(seqnow)
req "$WID" "$WSEC" PATCH /akam/v1/settings '{"led.mode":"on"}'
echo "btn S S S" >&3
wev "triple: mode badmix is refused as a whole, ptz.park still runs: result partial" "$S4" '"gesture":"triple".*"result":"partial"'
[ "$(st_val led.mode)" = on ] && ok "the refused mode applied NOTHING (led.mode still on)" || ko "all or nothing on a button mode (led.mode=$(st_val led.mode))"
S5=$(seqnow)
echo "btn S S L" >&3
wev "S S L: track toggle + patrol stop" "$S5" '"gesture":"S S L","actions":\["track toggle","patrol stop"\],"result":"ok"'
S6=$(seqnow)
echo "btn V" >&3
wev "very long press: ptz.privacy on" "$S6" '"gesture":"very_long","actions":\["ptz.privacy on"\],"result":"ok"'
check "... run as privacy on (kind 4, arg on)" grep -q '^btn.run 4 on' "$D8/calls.log"
S9=$(seqnow)
echo "btn S L" >&3
wev "an unbound gesture (S L: S L is bound to nothing) is an event too: result unbound, no action" "$S9" '"gesture":"S L","actions":\[\],"result":"unbound"'
check "... and an MQTT publish" grep -q '^btn.mqtt S L ' "$D8/calls.log"
S7=$(seqnow)
echo "btn S" >&3
wev "short (now waits for a possible double): still reported once" "$S7" '"gesture":"short"'
[ "$(bw)" = 1 ] && ok "all those presses wrote nothing to the flash (still 1)" || ko "writes after presses ($(bw))"
check "every gesture was published to MQTT" bash -c "for g in double triple 'S S L' very_long short; do grep -q \"^btn.mqtt \$g \" '$D8/calls.log' || exit 1; done"
check "persisted as the two files /config/akam/button and /config/akam/modes (JSON)" bash -c "grep -q '\"gesture\":\"S S L\"' '$D8/button' && grep -q '\"name\":\"night\"' '$D8/modes'"
req "$WID" "$WSEC" GET /akam/v1/button
BEFORE=$BODY
stop_srv && ok "button server exits cleanly" || ko "button server exit (see $D8/err)"
start_srv "$D8" --fast
req "$WID" "$WSEC" GET /akam/v1/button
[ "$BODY" = "$BEFORE" ] && ok "after a restart the bindings are back" || ko "bindings after a restart ($BODY)"
req "$WID" "$WSEC" GET /akam/v1/modes
want "... and the modes" 200 '"name":"night".*"name":"badmix"'
[ "$(bw)" = 0 ] && ok "a restart writes nothing" || ko "writes at restart ($(bw))"
S8=$(seqnow)
echo "btn S S" >&3
wev "the restored bindings work (double)" "$S8" '"gesture":"double"'
stop_srv || true
printf '{"bindings":[{"gesture":"short","actions":["reboot"]}]}' >"$D8/button"
start_srv "$D8" --fast
req "$WID" "$WSEC" GET /akam/v1/button
[ "$BODY" = "$DEFJ" ] && ok "an invalid button file is ignored as a whole (the defaults)" || ko "invalid file ($BODY)"
check "... and left alone" grep -q reboot "$D8/button"
stop_srv || true
# the write limiter
D9=$WORK/p9
mkdir -p "$D9"
newkey "$D9" write ha
WID=$KID
WSEC=$KSEC
start_srv "$D9" --fast
for i in 1 2 3 4 5 6 7 8; do req "$WID" "$WSEC" POST /akam/v1/button "{\"timing\":{\"gap_ms\":$((200 + i))},\"bindings\":[{\"gesture\":\"short\",\"actions\":[\"none\"]}]}"; done
want "8 config writes in a burst are accepted" 200 '"changed":true'
req "$WID" "$WSEC" POST /akam/v1/button '{"timing":{"gap_ms":300},"bindings":[{"gesture":"short","actions":["none"]}]}'
want "the 9th is rate limited (429 rate)" 429 '"error":"rate"'
printf '%s' "$HDRS" | grep -qi '^Retry-After: [0-9]' && ok "...with Retry-After" || ko "429 carries Retry-After"
stop_srv && ok "the button limiter server exits cleanly" || ko "button limiter server exit"

# ---------------------------------------------------------------------------------------------- ptz.vel: the fast action and its own budget
echo "== ptz.vel (continuous manual motion): own rate budget, still signed, write scope"
DV=$WORK/pv
mkdir -p "$DV"
newkey "$DV" write ha
WID=$KID
WSEC=$KSEC
newkey "$DV" read ro
RID=$KID
RSEC=$KSEC
newkey "$DV" admin adm
AID=$KID
ASEC=$KSEC
start_srv "$DV"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":"0.5","tilt":-1,"lease_ms":300}'
want "ptz.vel is accepted" 200 '"ok":true'
check "... and reached the motor: pan 0.5 -> divisor 4, tilt -1 -> down, the diagonal cap 3, lease 300 ms" grep -q '^ptz.vel 1 -1 4 3 300$' "$DV/calls.log"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":1}'
want "an integer velocity and the default lease" 200 '"ok":true'
check "... pan 1 -> divisor 2, lease 450" grep -q '^ptz.vel 1 0 2 0 450$' "$DV/calls.log"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":0,"tilt":"0.03"}'
want "both axes in the dead zone: a stop" 200 '"ok":true'
check "... it reached the controller as a stop" grep -q '^ptz.stop$' "$DV/calls.log"
sleep 1 # let the budget of the action refill before the argument checks
for bad in '{}' '{"pan":"fast"}' '{"pan":2}' '{"pan":"1.5"}' '{"pan":"0.5","lease_ms":100}' '{"pan":"0.5","lease_ms":2001}' '{"pan":"0.5","lease_ms":"abc"}' '{"tilt":"1e0"}'; do
    sleep 0.3 # below the 5/s of the action: these are argument checks, not budget checks
    n0=$(wc -l <"$DV/calls.log")
    req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel "$bad"
    [ "$STATUS" = 422 ] && [ "$(wc -l <"$DV/calls.log")" = "$n0" ] && ok "bad arguments $bad: 422 and the motor is not touched" || ko "bad arguments $bad ($STATUS)"
done
sleep 1
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":0.5}'
want "a JSON fraction is refused by the flat parser (400), never half read" 400 'bad_json'
req "$RID" "$RSEC" POST /akam/v1/actions/ptz.vel '{"pan":"1"}'
want "a read key may not move the head (403 scope)" 403 'scope'
echo "velrc -3" >&3
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":"1"}'
want "parked / privacy: refused (409 unavailable), not a silent success" 409 'unavailable.*parked'
echo "velrc -2" >&3
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":"1"}'
want "calibrating: refused (409 unavailable)" 409 'unavailable.*calibrating'
echo "velrc 0" >&3
sleep 1
n0=$(grep -c '^ptz.vel' "$DV/calls.log")
c200=0
c429=0
for i in $(seq 1 24); do
    req "$WID" "$WSEC" POST /akam/v1/actions/ptz.vel '{"pan":"0.5"}'
    case $STATUS in
        200) c200=$((c200 + 1)) ;;
        429) c429=$((c429 + 1)); printf '%s' "$BODY" | grep -q '"error":"rate"' || ko "429 carries error rate"; LASTH=$HDRS ;;
    esac
done
[ "$c429" -ge 3 ] && ok "a burst of 24 calls hits the budget of the action (429 rate: $c200 accepted, $c429 refused)" || ko "ptz.vel budget ($c200 / $c429)"
[ "$c200" -ge 5 ] && ok "... after the burst allowance of 5 (and its 5/s refill) was served, far above the 0.4/s of the other requests" || ko "ptz.vel served too few ($c200)"
[ "$(($(grep -c '^ptz.vel' "$DV/calls.log") - n0))" = "$c200" ] && ok "only the accepted calls reached the motor" || ko "calls reached the motor"
printf '%s' "${LASTH:-}" | grep -qi '^Retry-After: [0-9]' && ok "the 429 carries Retry-After" || ko "429 Retry-After"
req "$WID" "$WSEC" GET /akam/v1/info
want "the budget of the other signed requests is separate: GET /info still works" 200 '"scope":"write"'
req "$WID" "$WSEC" GET /akam/v1/state
want "... and so is /state" 200 '"rtsp.clients"'
req "$AID" "$ASEC" GET /akam/v1/audit
printf '%s' "$BODY" | tr '}' '\n' | grep 'ptz.vel' | grep -q '"res":200' && ko "a working ptz.vel is not written to the audit log" || ok "a working ptz.vel is not written to the audit log (it would flood the log and the flash); refusals are"
printf '%s' "$BODY" | grep -q 'ptz.vel' && ok "... failures of the action are audited as every other failure" || ko "failed ptz.vel calls are audited"
req "$WID" "$WSEC" POST /akam/v1/actions/ptz.home
want "the other actions are unaffected: ptz.home" 200 '"ok":true'
stop_srv && ok "the ptz.vel server exits cleanly" || ko "ptz.vel server exit"

echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" = 0 ]
