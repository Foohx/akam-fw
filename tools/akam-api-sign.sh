#!/bin/sh
# akam-api-sign.sh: reference signer of the Akam control API (specs/control-api.md section 5). POSIX sh + openssl + awk.
# Prints the request headers, one "Name: value" per line; it does not send anything. Validated against
# tests/fixtures/control_api_vectors.txt by tests/rot2k_control_api_sign_test.sh.
#
# usage: akam-api-sign.sh [--body STRING | --body-file FILE] METHOD PATH [RAW_QUERY]
#   METHOD  GET POST PATCH DELETE (any case, signed upper case)
#   PATH    exactly as sent, e.g. /akam/v1/settings   (no "..", "//", "%", trailing "/")
#   RAW_QUERY  the query as sent, without '?' (it is canonicalised here: decoded, re-encoded, sorted)
# environment:
#   AKAM_KEY_ID      8 lower-case hex          AKAM_KEY_SECRET  64 hex (the 256-bit secret)
#   AKAM_CHALLENGE   32 hex from GET /hello: signs in challenge mode (X-Akam-Challenge, no timestamp/nonce)
#   AKAM_TS, AKAM_NONCE  override the clock and the random nonce (tests only)
# exit: 0 ok, 2 usage or bad key material, 3 path or query refused (the server would answer 400).
# Note: the secret is passed to openssl as an argument (-macopt hexkey:), so it is visible in `ps` while the command runs.
# Do not use this on a shared machine; use a library signer (the assistant, or the Python/Go reference).
#
# curl example (the body is sent as signed):
#   H=$(./akam-api-sign.sh --body-file b.json PATCH /akam/v1/settings) || exit
#   set --; while IFS= read -r l; do set -- "$@" -H "$l"; done <<EOF
#   $H
#   EOF
#   curl -sS -X PATCH "$AKAM_URL/akam/v1/settings" "$@" -H 'Content-Type: application/json' --data-binary @b.json
set -eu
LC_ALL=C
export LC_ALL

die() { echo "akam-api-sign: $*" >&2; exit "${2:-2}"; }

body_file=""
body_str=""
have_body_str=0
while [ $# -gt 0 ]; do
    case $1 in
    --body) [ $# -ge 2 ] || die "--body needs a value"; body_str=$2; have_body_str=1; shift 2 ;;
    --body-file) [ $# -ge 2 ] || die "--body-file needs a file"; body_file=$2; shift 2 ;;
    --) shift; break ;;
    -*) die "unknown option $1" ;;
    *) break ;;
    esac
done
[ $# -ge 2 ] && [ $# -le 3 ] || die "usage: akam-api-sign.sh [--body STRING | --body-file FILE] METHOD PATH [RAW_QUERY]"
method=$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')
path=$2
query=${3:-}
[ -z "$body_file" ] || [ "$have_body_str" = 0 ] || die "--body and --body-file are exclusive"
[ -z "$body_file" ] || [ -r "$body_file" ] || die "cannot read $body_file"

case $method in GET | POST | PATCH | DELETE) ;; *) die "method must be GET, POST, PATCH or DELETE" ;; esac
key_id=${AKAM_KEY_ID:-}
secret=${AKAM_KEY_SECRET:-}
case $key_id in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;; *) die "AKAM_KEY_ID must be 8 lower-case hex digits" ;; esac
[ "${#key_id}" = 8 ] || die "AKAM_KEY_ID must be 8 lower-case hex digits"
[ "${#secret}" = 64 ] || die "AKAM_KEY_SECRET must be 64 hex digits"
case $secret in *[!0-9a-fA-F]*) die "AKAM_KEY_SECRET must be 64 hex digits" ;; esac

# path rule (spec 5.2): refused, never normalised
case $path in
/*) ;;
*) die "path must start with /" 3 ;;
esac
case $path in
*[!A-Za-z0-9._~/-]* | */ | *//* | *..* | */./* | */.) die "path refused: $path" 3 ;;
esac
[ "${#path}" -le 128 ] || die "path too long" 3
[ "${#query}" -le 512 ] || die "query too long" 3

# canonical query: percent-decode, re-encode (unreserved A-Za-z0-9-._~, upper-case hex), sort by name then value
canon_query() {
    Q=$1 awk '
    BEGIN {
        for (i = 1; i < 256; i++) { c = sprintf("%c", i); ord[c] = i; chr[i] = c }
        hexd = "0123456789ABCDEF"
        q = ENVIRON["Q"]
        if (q == "") exit 0
        np = 0; pos = 1; n = length(q)
        while (pos <= n + 1) {
            e = index(substr(q, pos), "&")
            seg = (e == 0) ? substr(q, pos) : substr(q, pos, e - 1)
            if (seg == "") exit 3
            eq = index(seg, "=")
            if (eq == 0) { nm = seg; vl = "" } else { nm = substr(seg, 1, eq - 1); vl = substr(seg, eq + 1) }
            N[np] = enc(nm); V[np] = enc(vl); np++
            if (e == 0) break
            pos += e
        }
        for (i = 1; i < np; i++) {
            kn = N[i]; kv = V[i]; j = i
            while (j > 0 && (N[j-1] > kn || (N[j-1] == kn && V[j-1] > kv))) { N[j] = N[j-1]; V[j] = V[j-1]; j-- }
            N[j] = kn; V[j] = kv
        }
        out = ""
        for (i = 0; i < np; i++) out = out (i ? "&" : "") N[i] "=" V[i]
        printf "%s", out
    }
    function hv(c) { return index("0123456789abcdef", tolower(c)) - 1 }
    function enc(s,    o, i, c, b, h1, h2, l) {
        o = ""; l = length(s); i = 1
        while (i <= l) {
            c = substr(s, i, 1)
            if (c == "%") {
                if (i + 2 > l) exit 3
                h1 = hv(substr(s, i + 1, 1)); h2 = hv(substr(s, i + 2, 1))
                if (h1 < 0 || h2 < 0) exit 3
                b = h1 * 16 + h2
                if (b == 0) exit 3
                c = chr[b]; i += 3
            } else { b = ord[c]; i++ }
            if (c ~ /^[A-Za-z0-9._~-]$/) o = o c
            else o = o "%" substr(hexd, int(b / 16) + 1, 1) substr(hexd, b % 16 + 1, 1)
        }
        return o
    }'
}
cq=$(canon_query "$query") || die "query refused: $query" 3

# body hash (from a file, or the string, or empty)
if [ -n "$body_file" ]; then
    bh=$(openssl dgst -sha256 <"$body_file" | sed 's/.* //')
else
    bh=$(printf '%s' "$body_str" | openssl dgst -sha256 | sed 's/.* //')
fi

if [ -n "${AKAM_CHALLENGE:-}" ]; then
    case $AKAM_CHALLENGE in *[!0-9a-fA-F]*) die "AKAM_CHALLENGE must be 32 hex digits" ;; esac
    [ "${#AKAM_CHALLENGE}" = 32 ] || die "AKAM_CHALLENGE must be 32 hex digits"
    l6="challenge:$AKAM_CHALLENGE"
    l7=$l6
else
    l6=${AKAM_TS:-$(date +%s)}
    l7=${AKAM_NONCE:-$(openssl rand -hex 16)}
fi

canon=$(printf 'AKAM1-HMAC-SHA256\n%s\n%s\n%s\n%s\n%s\n%s\n%s' "$method" "$path" "$cq" "$bh" "$l6" "$l7" "$key_id")
sig=$(printf '%s' "$canon" | openssl dgst -sha256 -mac HMAC -macopt "hexkey:$secret" | sed 's/.* //')

echo "X-Akam-Key: $key_id"
if [ -n "${AKAM_CHALLENGE:-}" ]; then
    echo "X-Akam-Challenge: $AKAM_CHALLENGE"
else
    echo "X-Akam-Timestamp: $l6"
    echo "X-Akam-Nonce: $l7"
fi
echo "X-Akam-Signature: $sig"
