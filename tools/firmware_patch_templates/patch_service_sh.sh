#!/usr/bin/env bash
# patch_service_sh.sh -- build-time patch of the APP's sbin/service.sh (HOST tool).
#
# Usage: patch_service_sh.sh <app_tree>
#
# Layer 1 of the Telnet/FTP lockdown (layer 2 is camyn_lockdown.sh). Edits
# <app_tree>/sbin/service.sh in place, deterministically and idempotently:
#
#   1. Every non-comment line that mentions telnetd, utelnetd, ftpd, bftpd or
#      vsftpd (this also covers "tcpsvd ... ftpd") is replaced by
#          <indent>: # CAMYN-DISABLED: <original text>
#      The ":" no-op keeps the shell syntax valid when the line was the only
#      command of an if/then/function body.
#   2. Every non-comment line that runs the microSD hook (mentions
#      _ht_ap_mode.conf, hack.sh, /mnt/.../hostapd, /mnt/telnet.sh) gets the
#      guard "[ -f $CAMYN_MAINT_FLAG ] &&" so the hook only runs when the
#      maintenance flag file exists on the SD. For "if ..." lines the guard is
#      inserted into the condition; for simple commands it is a prefix.
#   3. No other line is changed.
#
# Exit codes:
#   0  patched (or already patched) and BOTH kinds of lines were handled
#   1  usage error, service.sh missing, or I/O error
#   2  a matching line has a shape this script refuses to guess about
#      (line continuation, compound command, elif/while/case ...), or the
#      patched file fails "sh -n". Nothing is written.
#   3  nothing to patch: no telnet/ftp launch line and/or no SD hook line
#      found. The file is left untouched. build_firmware.sh must treat this as
#      blocking unless CAMYN_ALLOW_UNPATCHED_SERVICE=1.
#
# Environment:
#   CAMYN_MAINT_FLAG    flag path baked into the guard (default
#                       /mnt/CAMYN_MAINTENANCE; mount point to be confirmed)
#   CAMYN_PATCH_REPORT  unified diff output (default <app_tree>.service_sh.diff)
#
# Limits (to be checked on the real dump, see audit): launches built from a
# variable defined in another file, heredocs that contain matching text, and
# daemons started from ROOTFS are not handled here.

set -u

die() {
    echo "patch_service_sh: ERROR: $*" >&2
    exit "${2:-1}"
}

[ $# -eq 1 ] || die "usage: patch_service_sh.sh <app_tree>"
APP=${1%/}
SVC=$APP/sbin/service.sh
[ -f "$SVC" ] || die "$SVC not found"
[ -r "$SVC" ] && [ -w "$SVC" ] || die "$SVC not readable/writable"

FLAG=${CAMYN_MAINT_FLAG:-/mnt/CAMYN_MAINTENANCE}
case "$FLAG" in
    /* ) ;;
    *) die "CAMYN_MAINT_FLAG must be an absolute path" ;;
esac
case "$FLAG" in
    *[!A-Za-z0-9_./-]*) die "CAMYN_MAINT_FLAG contains unsupported characters" ;;
esac
REPORT=${CAMYN_PATCH_REPORT:-$APP.service_sh.diff}

WORK=$(mktemp -d) || die "mktemp failed"
trap 'rm -rf "$WORK"' EXIT

awk -v flag="$FLAG" -v counts="$WORK/counts" '
function bad(msg) {
    printf "patch_service_sh: ERROR: line %d: %s\n    %s\n", NR, msg, $0 > "/dev/stderr"
    failed = 1
}
BEGIN {
    dre = "(^|[^A-Za-z0-9_])(utelnetd|telnetd|bftpd|vsftpd|ftpd)([^A-Za-z0-9_-]|$)"
    hre = "(_ht_ap_mode\\.conf|hack\\.sh|/mnt/[A-Za-z0-9_./-]*hostapd|/mnt/telnet\\.sh)"
    guard = "[ -f " flag " ]"
}
{
    line = $0
    if (line ~ /^[ \t]*#/ || line ~ /^[ \t]*$/) { print line; next }
    if (index(line, "CAMYN-DISABLED:") > 0) { ddone++; print line; next }
    if (index(line, "CAMYN_MAINTENANCE") > 0 || index(line, flag) > 0) { hdone++; print line; next }
    if (line ~ dre) {
        if (line ~ /\\[ \t]*$/) { bad("daemon line ends with a continuation"); print line; next }
        match(line, /^[ \t]*/)
        indent = substr(line, 1, RLENGTH)
        body = substr(line, RLENGTH + 1)
        print indent ": # CAMYN-DISABLED: " body
        dnew++
        next
    }
    if (line ~ hre) {
        if (line ~ /\\[ \t]*$/) { bad("hook line ends with a continuation"); print line; next }
        match(line, /^[ \t]*/)
        indent = substr(line, 1, RLENGTH)
        body = substr(line, RLENGTH + 1)
        if (body ~ /^if[ \t]/) {
            sub(/^if[ \t]+/, "", body)
            print indent "if " guard " && " body
        } else if (body ~ /^(elif|while|until|case|for|then|else|do|fi|done|esac|\{|\()/) {
            bad("hook line starts with an unsupported keyword"); print line; next
        } else {
            tmp = body
            sub(/[ \t]*&[ \t]*$/, "", tmp)
            if (tmp ~ /[;|#]/) { bad("hook line is a compound command or has a comment"); print line; next }
            print indent guard " && " body
        }
        hnew++
        next
    }
    print line
}
END {
    printf "%d %d %d %d %d\n", dnew + 0, hnew + 0, ddone + 0, hdone + 0, failed + 0 > counts
}
' "$SVC" >"$WORK/new" || die "awk failed"

read -r DNEW HNEW DDONE HDONE FAILED <"$WORK/counts" || die "no counts from awk"
[ "$FAILED" -eq 0 ] || die "unsupported service.sh layout, nothing written; review by hand" 2

if [ $((DNEW + DDONE)) -eq 0 ] || [ $((HNEW + HDONE)) -eq 0 ]; then
    {
        echo "patch_service_sh: WARNING: expected lines NOT found in $SVC"
        echo "    telnet/ftp launch lines: $((DNEW + DDONE))   SD hook lines: $((HNEW + HDONE))"
        echo "    The image would NOT be locked down by this patch. File left untouched."
        echo "    Inspect the real service.sh and extend the patterns."
    } >&2
    exit 3
fi

sh -n "$WORK/new" 2>"$WORK/syntax" || {
    cat "$WORK/syntax" >&2
    die "patched service.sh fails sh -n, nothing written" 2
}

diff -u "$SVC" "$WORK/new" >"$REPORT"
if [ $? -gt 1 ]; then
    die "diff failed"
fi
# cat > keeps the original file mode and ownership.
cat "$WORK/new" >"$SVC" || die "cannot write $SVC"
echo "patch_service_sh: ok: $DNEW daemon line(s) and $HNEW hook line(s) patched, $DDONE/$HDONE already patched; diff: $REPORT"
exit 0
