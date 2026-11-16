#!/usr/bin/env bash
# Host test of the control API building blocks (specs/control-api.md phase 1), no camera:
#   1. builds the portable C files under ASan + UBSan and runs the unit tests (sha256/hmac, signing and verification, key
#      store, settings registry, minimal JSON) and a small deterministic fuzz of the parsers;
#   2. runs the reference signer tools/akam-api-sign.sh against tests/fixtures/control_api_vectors.txt (the same vectors
#      the C test checks), under $SH_UNDER_TEST (default sh; e.g. "busybox sh" or dash).
# Needs: a C compiler (CC, default cc), openssl, awk, xxd. Env: FUZZ_ITERS (default 200000), SH_UNDER_TEST.
# This script does not touch the Makefile or the camera build; see the doc for the standalone commands.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
APP=$REPO/firmware/lsc-rotatable-2k-ak3918av130/app
VEC=$REPO/tests/fixtures/control_api_vectors.txt
SIGN=$REPO/tools/akam-api-sign.sh
CC=${CC:-cc}
SH_UNDER_TEST=${SH_UNDER_TEST:-sh}
read -r -a SHCMD <<<"$SH_UNDER_TEST"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

SRCS="$APP/sha256.c $APP/api_keys.c $APP/api_sig.c $APP/json_min.c $APP/settings_reg.c"
CFLAGS="-std=gnu99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I$APP"

echo "== C unit tests (ASan/UBSan)"
# shellcheck disable=SC2086
if $CC $CFLAGS -o "$WORK/unit" $SRCS "$REPO/tests/rot2k_control_api_unit.c"; then
    if "$WORK/unit" "$VEC" >"$WORK/unit.log" 2>&1; then ok "unit: $(tail -1 "$WORK/unit.log")"; else ko "unit tests"; cat "$WORK/unit.log"; fi
else ko "unit tests build"; fi
echo "== fuzz of the parsers (ASan/UBSan)"
# shellcheck disable=SC2086
if $CC $CFLAGS -o "$WORK/fuzz" $SRCS "$REPO/tests/rot2k_control_api_fuzz.c"; then
    if "$WORK/fuzz" "${FUZZ_ITERS:-200000}" >"$WORK/fuzz.log" 2>&1; then ok "fuzz: $(tail -1 "$WORK/fuzz.log")"; else ko "fuzz"; tail -20 "$WORK/fuzz.log"; fi
else ko "fuzz build"; fi

echo "== transport pieces: HTTP request parser, rate limits, event ring (ASan/UBSan)"
TSRCS="$APP/http_req.c $APP/api_limit.c $APP/akam2k_apievt.c $APP/api_sig.c $APP/api_keys.c $APP/sha256.c"
# shellcheck disable=SC2086
if $CC $CFLAGS -pthread -o "$WORK/apiunit" $TSRCS "$REPO/tests/rot2k_api_unit.c"; then
    if "$WORK/apiunit" >"$WORK/apiunit.log" 2>&1; then ok "api unit: $(tail -1 "$WORK/apiunit.log")"; else ko "api unit tests"; cat "$WORK/apiunit.log"; fi
else ko "api unit tests build"; fi
echo "== fuzz of the HTTP request parser (ASan/UBSan)"
# shellcheck disable=SC2086
if $CC $CFLAGS -o "$WORK/apifuzz" "$APP/http_req.c" "$REPO/tests/rot2k_api_fuzz.c"; then
    if "$WORK/apifuzz" "${FUZZ_ITERS:-200000}" >"$WORK/apifuzz.log" 2>&1; then ok "http fuzz: $(tail -1 "$WORK/apifuzz.log")"; else ko "http fuzz"; tail -20 "$WORK/apifuzz.log"; fi
else ko "http fuzz build"; fi

echo "== tools/akam-api-sign.sh against the vectors (shell: $SH_UNDER_TEST)"
cases=0
cur_kind=""
declare -A F
flush() {
    local body="$WORK/body" sig out rc
    if [ "$cur_kind" = case ]; then
        : >"$body"
        if [ -n "${F[body_hex]:-}" ]; then printf '%s' "${F[body_hex]}" | xxd -r -p >"$body"; fi
        if [ -n "${F[body_fill]:-}" ]; then
            head -c "${F[body_fill]#*:}" /dev/zero | tr '\0' "\\$(printf '%03o' $((16#${F[body_fill]%%:*})))" >"$body"
        fi
        set +e
        out=$(AKAM_KEY_ID="${F[key_id]}" AKAM_KEY_SECRET="${F[secret]}" AKAM_TS="${F[timestamp]}" AKAM_NONCE="${F[nonce]}" \
            "${SHCMD[@]}" "$SIGN" --body-file "$body" "${F[method]}" "${F[path]}" "${F[query]}" 2>"$WORK/err")
        rc=$?
        set -e
        sig=$(printf '%s\n' "$out" | sed -n 's/^X-Akam-Signature: //p')
        if [ "${F[timestamp]#challenge:}" != "${F[timestamp]}" ]; then
            out=$(AKAM_KEY_ID="${F[key_id]}" AKAM_KEY_SECRET="${F[secret]}" AKAM_CHALLENGE="${F[timestamp]#challenge:}" \
                "${SHCMD[@]}" "$SIGN" --body-file "$body" "${F[method]}" "${F[path]}" "${F[query]}")
            sig=$(printf '%s\n' "$out" | sed -n 's/^X-Akam-Signature: //p')
            printf '%s\n' "$out" | grep -q "^X-Akam-Challenge: ${F[timestamp]#challenge:}$" || sig=BADHEADER
        fi
        if [ "$rc" = 0 ] && [ "$sig" = "${F[signature]}" ]; then ok "sign ${F[name]}"; else ko "sign ${F[name]} (rc=$rc got=$sig want=${F[signature]})"; cat "$WORK/err"; fi
        cases=$((cases + 1))
    elif [ "$cur_kind" = reject ]; then
        set +e
        if [ "${F[kind]}" = path ]; then
            AKAM_KEY_ID=3fa91c07 AKAM_KEY_SECRET=$(printf '00%.0s' $(seq 32)) AKAM_TS=1 AKAM_NONCE=00 "${SHCMD[@]}" "$SIGN" GET "${F[value]}" >/dev/null 2>&1
        else
            AKAM_KEY_ID=3fa91c07 AKAM_KEY_SECRET=$(printf '00%.0s' $(seq 32)) AKAM_TS=1 AKAM_NONCE=00 "${SHCMD[@]}" "$SIGN" GET /akam/v1/info "${F[value]}" >/dev/null 2>&1
        fi
        rc=$?
        set -e
        if [ "$rc" = 3 ]; then ok "refuse ${F[kind]} ${F[value]}"; else ko "refuse ${F[kind]} '${F[value]}' (rc=$rc)"; fi
    fi
    F=()
}
while IFS= read -r line || [ -n "$line" ]; do
    case $line in '#'* | '') continue ;; esac
    k=${line%%:*}
    v=${line#*:}
    v=${v# }
    case $k in
    "case" | reject) flush; cur_kind=$k; F[name]=$v ;;
    *) F[$k]=$v ;;
    esac
done <"$VEC"
flush
if [ "$cases" -ge 16 ]; then ok "all $cases vector cases signed"; else ko "too few vector cases ($cases)"; fi

echo "== signer argument handling"
good_env() { env AKAM_KEY_ID=3fa91c07 AKAM_KEY_SECRET=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f "$@"; }
expect_rc() { # expect_rc <rc> <description> <env+command...>
    local want=$1 d=$2 rc
    shift 2
    set +e
    "$@" >/dev/null 2>&1
    rc=$?
    set -e
    if [ "$rc" = "$want" ]; then ok "$d"; else ko "$d (rc=$rc)"; fi
}
expect_rc 2 "no key id" env -u AKAM_KEY_ID AKAM_KEY_SECRET=00 "${SHCMD[@]}" "$SIGN" GET /a
expect_rc 2 "short secret" env AKAM_KEY_ID=3fa91c07 AKAM_KEY_SECRET=00 "${SHCMD[@]}" "$SIGN" GET /a
expect_rc 2 "upper-case key id" env AKAM_KEY_ID=3FA91C07 AKAM_KEY_SECRET=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f "${SHCMD[@]}" "$SIGN" GET /a
expect_rc 2 "bad method" good_env "${SHCMD[@]}" "$SIGN" PUT /a
expect_rc 2 "missing path" good_env "${SHCMD[@]}" "$SIGN" GET
expect_rc 0 "fresh timestamp and nonce" good_env "${SHCMD[@]}" "$SIGN" GET /akam/v1/info
out=$(good_env "${SHCMD[@]}" "$SIGN" GET /akam/v1/info)
if printf '%s\n' "$out" | grep -Eq '^X-Akam-Nonce: [0-9a-f]{32}$' && printf '%s\n' "$out" | grep -Eq '^X-Akam-Timestamp: [0-9]{9,11}$'; then ok "nonce and timestamp format"; else ko "nonce and timestamp format"; fi
n1=$(good_env "${SHCMD[@]}" "$SIGN" GET /akam/v1/info | sed -n 's/^X-Akam-Nonce: //p')
n2=$(good_env "${SHCMD[@]}" "$SIGN" GET /akam/v1/info | sed -n 's/^X-Akam-Nonce: //p')
if [ "$n1" != "$n2" ]; then ok "nonce differs between calls"; else ko "nonce differs between calls"; fi
# --body STRING equals --body-file with the same bytes
printf '%s' '{"a":1}' >"$WORK/b.json"
s1=$(good_env AKAM_TS=5 AKAM_NONCE=ab "${SHCMD[@]}" "$SIGN" --body '{"a":1}' POST /akam/v1/x | sed -n 's/^X-Akam-Signature: //p')
s2=$(good_env AKAM_TS=5 AKAM_NONCE=ab "${SHCMD[@]}" "$SIGN" --body-file "$WORK/b.json" POST /akam/v1/x | sed -n 's/^X-Akam-Signature: //p')
if [ -n "$s1" ] && [ "$s1" = "$s2" ]; then ok "--body equals --body-file"; else ko "--body equals --body-file"; fi

echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" = 0 ]
