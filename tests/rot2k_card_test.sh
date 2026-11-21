#!/usr/bin/env bash
# Host-side test of firmware/lsc-rotatable-2k-ak3918av130/image/build_card.sh: dry runs into temp directories with a
# fake busybox and a fake Dropbear build (no network, no device, nothing outside the temp dir).
# Group "rot2k": runs in its own CI job, separate from the 1080p jobs.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BUILD=$REPO/firmware/lsc-rotatable-2k-ak3918av130/image/build_card.sh

WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { # check <description> <command...>
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}
fails() { ! "$@" >"$WORK/out.txt" 2>&1; }
outhas() { grep -q -- "$1" "$WORK/out.txt"; }

# Fake busybox (its sha256 is passed as the test-only expected value) and a fake Dropbear dist whose BUILDINFO
# carries the real hash of the current shim source, as build_dropbear.sh writes it.
printf 'fake busybox\n' >"$WORK/busybox"
BBSHA=$(sha256sum "$WORK/busybox" | cut -d' ' -f1)
DIST=$WORK/dist
mkdir -p "$DIST"
printf 'fake dropbearmulti\n' >"$DIST/dropbearmulti"
printf 'fake licence\n' >"$DIST/LICENSE.dropbear"
mkbuildinfo() { # mkbuildinfo <shim sha>
    {
        echo "programs=dropbear dropbearkey"
        echo "time32_shim_sha256=$1"
        echo "sha256_stripped=$(sha256sum "$DIST/dropbearmulti" | cut -d' ' -f1)"
    } >"$DIST/BUILDINFO.txt"
}
SHIM=$(sha256sum "$REPO/src/core/time32_shim.c" | cut -d' ' -f1)
mkbuildinfo "$SHIM"
ssh-keygen -q -t ed25519 -N '' -C test -f "$WORK/key"
KEYS=$WORK/key.pub

export AKAM_ROT2K_BUSYBOX=$WORK/busybox
export AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY=$BBSHA
export AKAM_DROPBEAR_DIST=$DIST
export AKAM_PTZ_DIST=$WORK/noptz
export AKAM_OWN_APP_DIST=$WORK/noapp

echo "== overlay card"
T=$WORK/card1
mkdir -p "$T"
check "overlay build succeeds" bash "$BUILD" "$T" --authorized-keys "$KEYS" --decloud
for f in yy_debug.ini akam_boot.sh busybox akam/common.sh akam/start_ssh.sh akam/decloud.sh akam/enable_local.sh \
    akam/restore_param.sh akam/dropbearmulti akam/authorized_keys akam/decloud; do
    check "card has $f" test -e "$T/$f"
done
check "yy_debug.ini is mode=debug with akam_boot.sh" test "$(cat "$T/yy_debug.ini")" = "$(printf 'mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh')"
check "authorized_keys comes from the given file" cmp -s "$T/akam/authorized_keys" "$KEYS"
check "no flag the user did not ask for" test ! -e "$T/akam/enable_param_conf" -a ! -e "$T/akam/no_telnet"
check "no firmware-like file on the card" test -z "$(find "$T" \( -name '*.bin' -o -name '*.zip' -o -name 'fac_av130*' -o -name 'yy_upgrade*' \))"
check "no hack.sh (flash-free overlay has its own launcher)" test ! -e "$T/hack.sh"
check "repo tracks no key under firmware/" test -z "$(cd "$REPO" && git ls-files firmware | grep -E 'authorized_keys|\.pub$|host_key' || true)"

check "no akam_ptz on the card when none was built" test ! -e "$T/akam/akam_ptz"

echo "== akam_ptz on the card"
PD=$WORK/ptzdist
mkdir -p "$PD"
printf 'fake akam_ptz\n' >"$PD/akam_ptz"
mkptzinfo() { # mkptzinfo <source sha>
    {
        echo "tool=akam_ptz"
        echo "source_sha256=$1"
        echo "sha256_stripped=$(sha256sum "$PD/akam_ptz" | cut -d' ' -f1)"
    } >"$PD/BUILDINFO.txt"
}
PSRC=$(cat "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz.c" "$REPO/firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/akam_ptz_safe.h" | sha256sum | cut -d" " -f1)
mkptzinfo "$PSRC"
T=$WORK/card7
mkdir -p "$T"
check "overlay with akam_ptz builds" env AKAM_PTZ_DIST="$PD" bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "card has akam/akam_ptz (executable)" test -x "$T/akam/akam_ptz"
check "card has the akam_ptz BUILDINFO" test -f "$T/akam/akam_ptz.BUILDINFO"
mkptzinfo 0000
T=$WORK/card8
mkdir -p "$T"
check "akam_ptz built from another source is refused" fails env AKAM_PTZ_DIST="$PD" bash "$BUILD" "$T" --authorized-keys "$KEYS"
mkptzinfo "$PSRC"
printf 'tampered\n' >"$PD/akam_ptz"
check "akam_ptz not matching its BUILDINFO is refused" fails env AKAM_PTZ_DIST="$PD" bash "$BUILD" "$T" --authorized-keys "$KEYS"

echo "== akam2k_rtsp (own app) on the card"
check "no akam2k_rtsp on the card when none was built" test ! -e "$T/akam/bin"
AD=$WORK/appdist
mkdir -p "$AD"
mkelf() { # mkelf <file> <arm|x86_64> <interpreter>
    {
        printf '\177ELF\001\001\001'
        head -c 11 /dev/zero
        case $2 in arm) printf '\050\000' ;; *) printf '\076\000' ;; esac
        head -c 40 /dev/zero
        printf '%s\000' "$3"
        head -c 200 /dev/zero
    } >"$1"
}
APPSRC=$(sh "$REPO/firmware/lsc-rotatable-2k-ak3918av130/image/source_hash.sh")
mkappinfo() { # mkappinfo <source sha> <symcheck>
    {
        echo "tool=akam2k_rtsp"
        echo "source_sha256=$1"
        echo "sha256=$(sha256sum "$AD/akam2k_rtsp" | cut -d' ' -f1)"
        echo "interpreter=/lib/ld-uClibc.so.0"
        [ -z "$2" ] || echo "symcheck=$2"
    } >"$AD/BUILDINFO.txt"
}
mkelf "$AD/akam2k_rtsp" arm /lib/ld-uClibc.so.0
printf 'fake vendor lib\n' >"$AD/libplat_vi.so"
mkappinfo "$APPSRC" passed
T=$WORK/card_app1
mkdir -p "$T"
check "overlay with --own-app-bin builds" bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
check "card has akam/bin/akam2k_rtsp (executable)" test -x "$T/akam/bin/akam2k_rtsp"
check "card has its BUILDINFO" cmp -s "$T/akam/bin/akam2k_rtsp.BUILDINFO" "$AD/BUILDINFO.txt"
check "the installed binary equals the built one" cmp -s "$T/akam/bin/akam2k_rtsp" "$AD/akam2k_rtsp"
check "the supervisor script is on the card" test -x "$T/akam/own_app_run.sh"
check "no vendor library is ever copied to the card" test -z "$(find "$T" -name '*.so*' -o -name 'vendor' -o -name 'appfs')"
check "no akam2k.conf is created (optional, the user's)" test ! -e "$T/akam/akam2k.conf"
T=$WORK/card_app2
mkdir -p "$T"
check "AKAM_OWN_APP_DIST is the default dir" env AKAM_OWN_APP_DIST="$AD" bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "default dir: binary installed" test -x "$T/akam/bin/akam2k_rtsp"
T=$WORK/card_app3
mkdir -p "$T"
mkappinfo "$APPSRC" ""
check "BUILDINFO without symcheck=passed is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
check "refusal names the symcheck" outhas "symcheck"
mkappinfo "$APPSRC" failed
check "symcheck=failed is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
mkappinfo 0000 passed
check "a binary built from other sources is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
mkappinfo "$APPSRC" passed
rm -f "$AD/BUILDINFO.txt"
check "a missing BUILDINFO is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
mkappinfo "$APPSRC" passed
printf 'tampered\n' >>"$AD/akam2k_rtsp"
check "a binary not matching its BUILDINFO is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
mkelf "$AD/akam2k_rtsp" arm /lib/ld-linux-armhf.so.3
mkappinfo "$APPSRC" passed
check "a glibc interpreter is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
check "refusal names the interpreter" outhas "ld-uClibc"
mkelf "$AD/akam2k_rtsp" x86_64 /lib/ld-uClibc.so.0
mkappinfo "$APPSRC" passed
check "an x86-64 ELF is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
printf '#!/bin/sh\necho /lib/ld-uClibc.so.0\n' >"$AD/akam2k_rtsp"
mkappinfo "$APPSRC" passed
check "a non-ELF file is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$AD"
check "nothing was installed by the refusals" test ! -e "$T/akam/bin"
check "--own-app-bin with a missing directory is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app-bin "$WORK/nodir"
check "--rescue-only refuses --own-app-bin" fails bash "$BUILD" "$T" --rescue-only --own-app-bin "$AD"

echo "== akam2k_sntp + akam_ntp.sh (opt-in time sync) on the card"
T=$WORK/card_ntp0
mkdir -p "$T"
check "overlay without an sntp build still builds" bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "card has the shared 1080p akam_ntp.sh, byte for byte" cmp -s "$T/akam/akam_ntp.sh" "$REPO/src/scripts/akam_ntp.sh"
check "akam_ntp.sh is executable" test -x "$T/akam/akam_ntp.sh"
check "no akam/ntp file is ever created by the builder (off by default)" test ! -e "$T/akam/ntp"
check "no sntp binary when none was built" test ! -e "$T/akam/bin/akam2k_sntp"
SD2=$WORK/sntpdist
mkdir -p "$SD2"
mkelf "$SD2/akam2k_sntp" arm /lib/ld-uClibc.so.0
SNSRC=$(sh "$REPO/firmware/lsc-rotatable-2k-ak3918av130/image/source_hash.sh" sntp)
mksnbi() { # mksnbi <source sha> <symcheck>
    {
        echo "tool=akam2k_rtsp"
        echo "symcheck=$2"
        echo "sntp_source_sha256=$1"
        echo "sntp_sha256=$(sha256sum "$SD2/akam2k_sntp" | cut -d' ' -f1)"
    } >"$SD2/BUILDINFO.txt"
}
mksnbi "$SNSRC" passed
T=$WORK/card_ntp1
mkdir -p "$T"
check "overlay with akam2k_sntp builds" env AKAM_OWN_APP_DIST="$SD2" bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "card has akam/bin/akam2k_sntp (executable, equal to the build)" test -x "$T/akam/bin/akam2k_sntp" -a "$(cmp -s "$T/akam/bin/akam2k_sntp" "$SD2/akam2k_sntp" && echo same)" = same
check "card has akam2k_sntp.BUILDINFO with its sha256" test "$(sed -n 's/^sha256=//p' "$T/akam/bin/akam2k_sntp.BUILDINFO")" = "$(sha256sum "$SD2/akam2k_sntp" | cut -d' ' -f1)"
check "still no akam/ntp on the card" test ! -e "$T/akam/ntp"
T=$WORK/card_ntp2
mkdir -p "$T"
mksnbi 0000 passed
check "an sntp built from other sources is refused" fails env AKAM_OWN_APP_DIST="$SD2" bash "$BUILD" "$T" --authorized-keys "$KEYS"
mksnbi "$SNSRC" ""
check "an sntp without symcheck=passed is refused" fails env AKAM_OWN_APP_DIST="$SD2" bash "$BUILD" "$T" --authorized-keys "$KEYS"
mksnbi "$SNSRC" passed
printf 'tampered\n' >>"$SD2/akam2k_sntp"
check "an sntp not matching its BUILDINFO is refused" fails env AKAM_OWN_APP_DIST="$SD2" bash "$BUILD" "$T" --authorized-keys "$KEYS"
mkelf "$SD2/akam2k_sntp" x86_64 /lib/ld-uClibc.so.0
mksnbi "$SNSRC" passed
check "an x86-64 sntp is refused" fails env AKAM_OWN_APP_DIST="$SD2" bash "$BUILD" "$T" --authorized-keys "$KEYS"

echo "== rescue card"
T=$WORK/card2
mkdir -p "$T"
check "rescue build succeeds" bash "$BUILD" "$T" --rescue-only
for f in yy_debug.ini hack.sh telnetd busybox; do check "rescue has $f" test -e "$T/$f"; done
check "rescue yy_debug.ini is mode=debug with hack.sh" test "$(cat "$T/yy_debug.ini")" = "$(printf 'mode=debug\nstart_sh=/mnt/sdcard/hack.sh')"
check "rescue has no overlay, no key" test ! -e "$T/akam"
check "rescue takes no key" fails bash "$BUILD" "$T" --rescue-only --authorized-keys "$KEYS"

echo "== own_app (PoC-1) card"
T=$WORK/card_oa
mkdir -p "$T/akam"
SECRET="S3CR3T-PSK-$$"
printf 'network={ psk="%s" }\n' "$SECRET" >"$T/akam/wpa.conf"
W0=$(sha256sum "$T/akam/wpa.conf")
bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app >"$WORK/out.txt" 2>&1 || true
check "--own-app creates the flag" test -e "$T/akam/own_app"
check "--own-app: the stub is on the card" test -s "$T/akam/own_app_stub.sh"
check "an existing akam/wpa.conf is left untouched" test "$(sha256sum "$T/akam/wpa.conf")" = "$W0"
check "builder output never shows the credentials" bash -c "! grep -q -- '$SECRET' '$WORK/out.txt'"
T=$WORK/card_oa2
mkdir -p "$T"
bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app >"$WORK/out.txt" 2>&1 || true
check "without --wpa-conf the builder never creates akam/wpa.conf" test ! -e "$T/akam/wpa.conf"
check "a missing wpa.conf is warned about" outhas "no akam/wpa.conf"
T=$WORK/card_oa3
mkdir -p "$T"
printf 'network={ psk="%s" }\n' "$SECRET" >"$WORK/mine.conf"
bash "$BUILD" "$T" --authorized-keys "$KEYS" --own-app --wpa-conf "$WORK/mine.conf" >"$WORK/out.txt" 2>&1 || true
check "--wpa-conf copies the user's file" cmp -s "$T/akam/wpa.conf" "$WORK/mine.conf"
check "--wpa-conf: mode 600" test "$(stat -c %a "$T/akam/wpa.conf")" = 600
check "--wpa-conf: content not printed" bash -c "! grep -q -- '$SECRET' '$WORK/out.txt'"
T=$WORK/card_oa4
mkdir -p "$T"
check "no --own-app: no flag, no wpa.conf" bash -c "bash '$BUILD' '$T' --authorized-keys '$KEYS' >/dev/null 2>&1 && test ! -e '$T/akam/own_app' -a ! -e '$T/akam/wpa.conf'"
check "--wpa-conf with a missing file is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS" --wpa-conf "$WORK/none.conf"
check "--rescue-only refuses --own-app" fails bash "$BUILD" "$T" --rescue-only --own-app
check "--rescue-only refuses --wpa-conf" fails bash "$BUILD" "$T" --rescue-only --wpa-conf "$WORK/mine.conf"
check "the repository ignores any wpa.conf" git -C "$REPO" check-ignore -q firmware/lsc-rotatable-2k-ak3918av130/akam/wpa.conf

echo "== refusals"
check "no argument is refused" fails bash "$BUILD"
check "a missing directory is refused" fails bash "$BUILD" "$WORK/nope" --rescue-only
printf 'x' >"$WORK/afile"
check "a file is refused" fails bash "$BUILD" "$WORK/afile" --rescue-only
check "/ is refused" fails bash "$BUILD" / --rescue-only
check "/ refusal says why" outhas "refusing to use /"
check "overlay without keys is refused" fails bash "$BUILD" "$WORK/card1"
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\n' >"$WORK/priv"
check "a private key file is refused" fails bash "$BUILD" "$WORK/card1" --authorized-keys "$WORK/priv"
check "an unknown option is refused" fails bash "$BUILD" "$WORK/card1" --format

echo "== busybox verification"
T=$WORK/card3
mkdir -p "$T"
export AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY=0000000000000000000000000000000000000000000000000000000000000000
check "busybox sha256 mismatch fails hard" fails bash "$BUILD" "$T" --rescue-only
check "mismatch is reported" outhas "sha256 mismatch"
check "nothing written on a mismatch" test -z "$(ls -A "$T")"
export AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY=$BBSHA
check "the pinned sha256 is the documented one" grep -q '367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290' "$BUILD"
check "the busybox missing override file is refused" env AKAM_ROT2K_BUSYBOX="$WORK/none" bash -c "! bash '$BUILD' '$T' --rescue-only >/dev/null 2>&1"

echo "== the test-only sha256 override cannot weaken a real build"
T=$WORK/card3b
mkdir -p "$T"
# No local busybox file, so the fake hash must be ignored: with curl unreachable the build fails, and if the
# repository cache happens to hold the real binary the result must carry the REAL hash, never a fake one.
env -u AKAM_ROT2K_BUSYBOX PATH="$WORK/nocurl:$PATH" AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY="$BBSHA" \
    bash -c "mkdir -p '$WORK/nocurl'; printf '#!/bin/sh\nexit 7\n' >'$WORK/nocurl/curl'; chmod 755 '$WORK/nocurl/curl'; bash '$BUILD' '$T' --rescue-only" >/dev/null 2>&1 || true
if [ -e "$T/busybox" ]; then
    check "override without a local file: only the pinned busybox is accepted" test "$(sha256sum "$T/busybox" | cut -d' ' -f1)" = 367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290
else
    check "override without a local file: build refused (pinned hash enforced)" test -z "$(ls -A "$T")"
fi

echo "== dropbear must be the shimmed build"
T=$WORK/card4
mkdir -p "$T"
mkbuildinfo 0000000000000000000000000000000000000000000000000000000000000000
check "a Dropbear built without the current shim is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "refusal names the shim" outhas "time32_shim"
mkbuildinfo "$SHIM"
printf 'tampered\n' >>"$DIST/dropbearmulti"
check "a dropbearmulti that does not match BUILDINFO is refused" fails bash "$BUILD" "$T" --authorized-keys "$KEYS"
mv "$DIST/dropbearmulti" "$DIST/x"
check "a missing dropbearmulti is refused with the build hint" fails bash "$BUILD" "$T" --authorized-keys "$KEYS"
check "hint names build_dropbear.sh" outhas "build_dropbear.sh"
check "nothing written when Dropbear is refused" test -z "$(ls -A "$T")"

echo "== upgrade / factory triggers"
T=$WORK/card5
mkdir -p "$T"
: >"$T/fac_av130.zip"
check "a card holding fac_av130.zip is refused" fails bash "$BUILD" "$T" --rescue-only
check "refusal is explicit" outhas "REFUSED"
check "the trigger file is left alone (never deleted)" test -e "$T/fac_av130.zip"
check "nothing else was written" test "$(ls -A "$T")" = fac_av130.zip
rm -f "$T/fac_av130.zip"
: >"$T/yy_upgrade.pkg"
check "a card holding a yy_upgrade file is refused" fails bash "$BUILD" "$T" --rescue-only
rm -f "$T/yy_upgrade.pkg"
mkdir -p "$T/old"
: >"$T/old/mtd0.bin"
check "an old .bin only warns" bash "$BUILD" "$T" --rescue-only
check "and is not copied or used" test ! -e "$T/mtd0.bin"
# A card that holds a stale mode=factory ini gets the debug one.
T=$WORK/card6
mkdir -p "$T"
printf 'mode=factory\n' >"$T/yy_debug.ini"
bash "$BUILD" "$T" --rescue-only >/dev/null
check "a stale mode=factory ini is overwritten by mode=debug" test "$(grep -c factory "$T/yy_debug.ini")" = 0

echo "== no device access in the builder"
check "build_card.sh has no dd/mkfs/parted/mount/umount/mke2fs/fdisk" test -z "$(sed -e 's/^[[:space:]]*#.*$//' "$BUILD" | grep -E '^[[:space:]]*(sudo[[:space:]]+)?(dd|mkfs[.a-z0-9]*|parted|mount|umount|fdisk|wipefs|losetup)[[:space:]]' || true)"

echo "rot2k card test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
