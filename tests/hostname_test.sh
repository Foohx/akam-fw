#!/usr/bin/env bash
# Host-side tests for akam_hostname.sh (camera script, run under busybox sh when
# available) and patch_ethernet_hostname.sh (build-time patcher). Nothing here
# touches hardware: the kernel hostname command is replaced by a recording stub.
# shellcheck disable=SC2016  # single quotes on purpose: literal $ in generated/expected shell text
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
TPL=$REPO/tools/firmware_patch_templates
HN=$TPL/akam_hostname.sh
PATCH=$TPL/patch_ethernet_hostname.sh
WRAPPER=$TPL/ak_rtsp_wrapper.sh
FIXTURE=$REPO/tests/fixtures/app_fake

if [ -z "${SH_UNDER_TEST:-}" ]; then
    if [ -n "${AKAM_BUSYBOX:-}" ]; then SH_UNDER_TEST="$AKAM_BUSYBOX sh"
    elif command -v busybox >/dev/null 2>&1; then SH_UNDER_TEST="busybox sh"
    else SH_UNDER_TEST="sh"; fi
fi
read -r -a SHCMD <<<"$SH_UNDER_TEST"
echo "shell under test: $SH_UNDER_TEST"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else ko "$d"; fi; }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else ko "$1 (got '$2', want '$3')"; fi; }

# Fake root: MAC, stub "hostname" that records its argument.
R=$WORK/root
mkdir -p "$R/sys/class/net/wlan0" "$WORK/bin"
echo "AA:BB:CC:DD:0E:F1" >"$R/sys/class/net/wlan0/address"
printf '#!/bin/sh\nif [ $# -gt 0 ]; then echo "$1" > "%s/kernel"; else cat "%s/kernel" 2>/dev/null; fi\n' "$WORK" "$WORK" >"$WORK/bin/hostname"
chmod +x "$WORK/bin/hostname"
hn() { env -i PATH="$PATH" AKAM_ROOT="$R" AKAM_HOSTNAME_CMD="$WORK/bin/hostname" "${SHCMD[@]}" "$HN" "$@"; }
HFILE=$R/etc/config/akam/hostname

echo "== default and show"
eq "default from MAC (lowercase, last 4 hex)" "$(hn show)" "akam-0ef1"
eq "default status line" "$(hn status | sed -n 3p)" "default=akam-0ef1"
echo "aa:bb" >"$R/sys/class/net/wlan0/address"
eq "short/garbage MAC -> akam" "$(hn show)" "akam"
rm -f "$R/sys/class/net/wlan0/address"
eq "missing MAC -> akam" "$(hn show)" "akam"
echo "AA:BB:CC:DD:0E:F1" >"$R/sys/class/net/wlan0/address"

echo "== validation"
for bad in "" "-a" "a-" "a.b" "a b" "12345" "a_b" "$(printf 'a%.0s' $(seq 33))" 'é' 'a;b'; do
    if hn set "$bad" >/dev/null 2>&1; then ko "rejects '$bad'"; else ok "rejects '$bad'"; fi
done
check "no file after rejections" test ! -e "$HFILE"
if hn set >/dev/null 2>&1; then ko "set without name fails"; else ok "set without name fails"; fi
if hn set a b >/dev/null 2>&1; then ko "set with 2 args fails"; else ok "set with 2 args fails"; fi
for good in "a" "cam-salon" "Cam1" "1a" "a1" "$(printf 'a%.0s' $(seq 32))"; do
    check "accepts '$good'" hn set "$good"
done

echo "== set / persistence / reset / apply"
hn set "Salon-Cam" >/dev/null
eq "set lowercases" "$(cat "$HFILE")" "salon-cam"
eq "file mode 644" "$(stat -c %a "$HFILE")" "644"
eq "kernel hostname applied at once" "$(cat "$WORK/kernel")" "salon-cam"
eq "show after set" "$(hn show)" "salon-cam"
eq "persistent across invocations (status configured)" "$(hn status | sed -n 2p)" "configured=salon-cam"
check "no temp file left" bash -c "[ \$(ls '$R/etc/config/akam' | wc -l) -eq 1 ]"
rm -f "$WORK/kernel"
hn apply
eq "apply sets the kernel name" "$(cat "$WORK/kernel")" "salon-cam"
printf 'bad name!\n' >"$HFILE"
eq "corrupt file ignored -> default" "$(hn show)" "akam-0ef1"
hn apply
eq "apply with corrupt file -> default" "$(cat "$WORK/kernel")" "akam-0ef1"
printf '12345\n' >"$HFILE"
eq "digits-only file ignored" "$(hn show)" "akam-0ef1"
printf 'CRLF-Cam\r\n' >"$HFILE"
eq "CRLF tolerated" "$(hn show)" "crlf-cam"
hn set cam2 >/dev/null
hn reset >/dev/null
check "reset removes the file" test ! -e "$HFILE"
eq "reset applies the default" "$(cat "$WORK/kernel")" "akam-0ef1"
eq "show after reset" "$(hn show)" "akam-0ef1"
if hn bogus >/dev/null 2>&1; then ko "unknown command fails"; else ok "unknown command fails"; fi
if env -i PATH="$PATH" AKAM_ROOT="$R" AKAM_HOSTNAME_CMD=/nonexistent "${SHCMD[@]}" "$HN" apply >/dev/null 2>&1; then ok "apply never fails when hostname is missing"; else ko "apply never fails when hostname is missing"; fi
check "valid shell (sh -n)" "${SHCMD[@]}" -n "$HN"

echo "== wrapper uses the helper"
check "wrapper: no hard-coded IPCamera" bash -c "! grep -v '^[[:space:]]*#' '$WRAPPER' | grep -q IPCamera"
check "wrapper: applies hostname before wpa/udhcpc" bash -c "[ \$(grep -n 'akam_hostname.sh apply' '$WRAPPER' | head -1 | cut -d: -f1) -lt \$(grep -n 'udhcpc -i wlan0' '$WRAPPER' | head -1 | cut -d: -f1) ]"
check "wrapper: udhcpc uses AKAM_DHCP_HOSTNAME" grep -q 'udhcpc -i wlan0 -x "hostname:$AKAM_DHCP_HOSTNAME"' "$WRAPPER"
check "wrapper: valid shell (sh -n)" "${SHCMD[@]}" -n "$WRAPPER"

echo "== patch_ethernet_hostname.sh"
mkapp() { rm -rf "$1"; mkdir -p "$1/sbin"; printf '%s' "$2" >"$1/sbin/ethernet.sh"; }
patch() { bash "$PATCH" "$1" >"$WORK/p.out" 2>&1; PRC=$?; }
A=$WORK/app1
cp -a "$FIXTURE" "$A"
patch "$A"
eq "fixture: exit 0" "$PRC" "0"
check "fixture: no IPCamera left" bash -c "! grep -q IPCamera '$A/sbin/ethernet.sh'"
check "fixture: line patched" grep -qF 'udhcpc -i wlan0 -x "hostname:$(/usr/sbin/akam_hostname.sh show 2>/dev/null || echo akam)" &' "$A/sbin/ethernet.sh"
check "fixture: sh -n" "${SHCMD[@]}" -n "$A/sbin/ethernet.sh"
check "fixture: reports line number" grep -q 'line 9 ' "$WORK/p.out"
cp "$A/sbin/ethernet.sh" "$WORK/once"
patch "$A"
eq "idempotent: exit 0" "$PRC" "0"
check "idempotent: file unchanged" cmp -s "$A/sbin/ethernet.sh" "$WORK/once"
check "idempotent: reports already patched" grep -q '0 line(s) patched.*1 already' "$WORK/p.out"
# The patched command really yields the helper name (or the fallback).
sed -n 's/.*-x "\(hostname:[^"]*\)".*/echo "\1"/p' "$A/sbin/ethernet.sh" >"$WORK/expr.sh"
eq "patched expr, helper missing -> fallback" "$(env -i PATH="$PATH" "${SHCMD[@]}" "$WORK/expr.sh")" "hostname:akam"
mkdir -p "$WORK/usr-sbin"
check "patch expr uses absolute helper path" grep -q '/usr/sbin/akam_hostname.sh show' "$WORK/expr.sh"

echo "== refusals"
mkapp "$WORK/r1" $'#!/bin/sh\nudhcpc -i wlan0 -x hostname:Other &\n'
patch "$WORK/r1"; eq "other hostname value: exit 2" "$PRC" "2"
check "other value: file untouched" grep -q 'hostname:Other' "$WORK/r1/sbin/ethernet.sh"
mkapp "$WORK/r2" $'#!/bin/sh\nudhcpc -i wlan0 -x hostname:IPCamera \\\n  &\n'
patch "$WORK/r2"; eq "continuation: exit 2" "$PRC" "2"
mkapp "$WORK/r3" $'#!/bin/sh\necho nothing here\n'
patch "$WORK/r3"; eq "no hostname line: exit 3" "$PRC" "3"
mkapp "$WORK/r4" $'#!/bin/sh\n# udhcpc -i wlan0 -x hostname:IPCamera &\n'
patch "$WORK/r4"; eq "commented line only: exit 3" "$PRC" "3"
mkapp "$WORK/r5" $'#!/bin/sh\nfoo -x hostname:IPCamera\n'
patch "$WORK/r5"; eq "not an udhcpc command: exit 2" "$PRC" "2"
mkapp "$WORK/r6" $'#!/bin/sh\nudhcpc -x hostname:IPCameraX &\n'
patch "$WORK/r6"; eq "value with a suffix: exit 2" "$PRC" "2"
patch "$WORK/nonexistent"; eq "missing ethernet.sh: exit 1" "$PRC" "1"
mkapp "$WORK/r7" $'#!/bin/sh\nudhcpc -i wlan0 -x hostname:IPCamera &\nudhcpc -i eth0 -x hostname:IPCamera\n'
patch "$WORK/r7"; eq "two lines both patched" "$PRC" "0"
check "two lines: none left" bash -c "! grep -q IPCamera '$WORK/r7/sbin/ethernet.sh'"

echo "== real stock ethernet.sh (copy, skipped without a dump)"
REAL=${APP_EXTRACTED:-}/sbin/ethernet.sh
if [ -f "$REAL" ]; then
    mkdir -p "$WORK/real/sbin"
    cp "$REAL" "$WORK/real/sbin/ethernet.sh"
    patch "$WORK/real"
    eq "real: exit 0" "$PRC" "0"
    check "real: sh -n" "${SHCMD[@]}" -n "$WORK/real/sbin/ethernet.sh"
    check "real: no IPCamera left" bash -c "! grep -q IPCamera '$WORK/real/sbin/ethernet.sh'"
    check "real: only the hostname token changed" bash -c "[ \$(diff '$REAL' '$WORK/real/sbin/ethernet.sh' | grep -c '^>') -eq 1 ]"
else
    echo "  skip - no extracted APP at $REAL"
fi

echo "== image contains the script (image_files.txt)"
check "image_files.txt installs akam_hostname.sh 755" grep -q '^file 755 overlay/sbin/akam_hostname.sh sbin/akam_hostname.sh tools/firmware_patch_templates/akam_hostname.sh$' "$REPO/tools/release/image_files.txt"
check "image_files.txt lists the ethernet patcher" grep -q 'patches/patch_ethernet_hostname.sh - tools/firmware_patch_templates/patch_ethernet_hostname.sh$' "$REPO/tools/release/image_files.txt"
# Full image check lives in build_firmware_test.sh (sbin/akam_hostname.sh + patched ethernet.sh).

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
