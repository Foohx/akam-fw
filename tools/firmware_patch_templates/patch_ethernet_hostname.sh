#!/usr/bin/env bash
# patch_ethernet_hostname.sh -- build-time patch of the APP's sbin/ethernet.sh (HOST tool).
#
# Usage: patch_ethernet_hostname.sh <app_tree>
#
# The stock get_ip_from_ap() starts "udhcpc -i wlan0 -x hostname:IPCamera &": a
# hard-coded DHCP hostname. This rewrites every active such line so the name
# comes from akam_hostname.sh (effective name = stored name or akam-XXXX), with
# a safe fallback to "akam" when the script is missing or fails:
#
#   udhcpc -i wlan0 -x "hostname:$(/usr/sbin/akam_hostname.sh show 2>/dev/null || echo akam)" &
#
# Only the "-x hostname:IPCamera" token is replaced; the rest of the line is kept.
# `show` computes the name itself (no dependency on the boot-time `apply`), so the
# order of the boot scripts does not matter. Idempotent.
#
# Exit codes:
#   0  patched (or already patched); the patched line numbers are printed
#   1  usage error, ethernet.sh missing, or I/O error
#   2  unexpected shape (hostname option with another value, line continuation,
#      patched file fails "sh -n"): nothing written
#   3  nothing to patch: no active "-x hostname:" line. File untouched; build_firmware.sh
#      treats this as blocking.

set -u

die() {
    echo "patch_ethernet_hostname: ERROR: $*" >&2
    exit "${2:-1}"
}

[ $# -eq 1 ] || die "usage: patch_ethernet_hostname.sh <app_tree>"
APP=${1%/}
ETH=$APP/sbin/ethernet.sh
[ -f "$ETH" ] || die "$ETH not found"
{ [ -r "$ETH" ] && [ -w "$ETH" ]; } || die "$ETH not readable/writable"

WORK=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$WORK"' EXIT

awk -v counts="$WORK/counts" -v lines="$WORK/lines" '
function bad(msg) {
    printf "patch_ethernet_hostname: ERROR: line %d: %s\n    %s\n", NR, msg, $0 > "/dev/stderr"
    failed = 1
}
{
    line = $0
    if (line ~ /^[ \t]*#/ || line !~ /-x[ \t]+"?hostname:/) { print line; next }
    if (index(line, "akam_hostname.sh") > 0) { done++; print line; next }
    if (line ~ /\\[ \t]*$/) { bad("hostname line ends with a continuation"); print line; next }
    if (line !~ /(^|[ \t;&|])udhcpc[ \t]/) { bad("hostname option outside an udhcpc command"); print line; next }
    tok = "-x hostname:IPCamera"
    p = index(line, tok)
    if (p == 0) { bad("hostname option with an unexpected value"); print line; next }
    rest = substr(line, p + length(tok))
    if (rest !~ /^([ \t;&|]|$)/) { bad("unexpected text right after the hostname value"); print line; next }
    print substr(line, 1, p - 1) "-x \"hostname:$(/usr/sbin/akam_hostname.sh show 2>/dev/null || echo akam)\"" rest
    print NR > lines
    neu++
}
END { printf "%d %d %d\n", neu + 0, done + 0, failed + 0 > counts }
' "$ETH" >"$WORK/new" || die "awk failed"

read -r NEW DONE FAILED <"$WORK/counts" || die "no counts from awk"
[ "$FAILED" -eq 0 ] || die "unsupported ethernet.sh layout, nothing written; review by hand" 2
if [ $((NEW + DONE)) -eq 0 ]; then
    echo "patch_ethernet_hostname: WARNING: no active 'udhcpc -x hostname:' line in $ETH; file left untouched" >&2
    exit 3
fi

sh -n "$WORK/new" 2>"$WORK/syntax" || {
    cat "$WORK/syntax" >&2
    die "patched ethernet.sh fails sh -n, nothing written" 2
}

# cat > keeps the original file mode and ownership.
cat "$WORK/new" >"$ETH" || die "cannot write $ETH"
LN=""
[ -f "$WORK/lines" ] && LN=$(tr '\n' ' ' <"$WORK/lines")
echo "patch_ethernet_hostname: ok: $NEW line(s) patched (line ${LN:-none}), $DONE already patched"
exit 0
