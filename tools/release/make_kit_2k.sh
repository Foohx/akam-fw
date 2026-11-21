#!/usr/bin/env bash
# Build the Akam release kit of the card-less 2K (kind cardless-appfs): akam-kit-<model>-<version>.tar.xz + SHA256SUMS.
#
#   tools/release/make_kit_2k.sh [--model ID] [--out DIR] [--app-dist DIR] [--dropbear-dist DIR] [--busybox FILE]
#
# The 2K twin of make_kit.sh. One kit serves the two installs of this camera:
#   appfs/   the Akam payload of the card-less appfs image (read by `akam appfs-build`, internal/installer/appfs)
#   card/    the files of the first-install SD card that are not in appfs/ (read by `akam prepare-2k-sd`)
#   licenses/ SOURCES.txt supported_hardware.txt  licence texts, the source offer, the registry entry of the model
# It holds ONLY Akam files and the pinned open-source busybox: never anything from a camera dump or the vendor
# firmware (the vendor part of the appfs image comes from the USER'S backup).
#
# Inputs are the build outputs the card builders already use, with the same proofs as build_appfs.sh:
#   --app-dist DIR       akam2k_rtsp + akam2k_sntp + BUILDINFO.txt (make poc3-dist), default dist/rot2k-app
#   --dropbear-dist DIR  dropbearmulti + BUILDINFO.txt (+ LICENSE.dropbear), default dist/dropbear
#   --busybox FILE       the pinned static busybox, default tools/.cache/rot2k/<name>
# Steps: proofs -> time64 gate on dropbearmulti (blocking) -> normalised tree -> MANIFEST.txt + SHA256SUMS ->
# reproducible tarball -> SHA256SUMS next to it. Signing is a separate step (tools/release/sign_kit.sh).
#
# Environment (all optional): SOURCE_DATE_EPOCH, AKAM_KIT_VERSION, AKAM_KIT_ALLOW_DIRTY=1 (marks kit_grade=test),
# AKAM_KIT_CHECK_TIME32 (alternative gate command), SKIP_SOURCE_PROOFS=1 (skips the "built from the current sources"
# hashes), AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY (replaces the pinned busybox hash): the three are for tests and mark the kit
# kit_grade=test.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FW=$ROOT/firmware/lsc-rotatable-2k-ak3918av130
KIT_FORMAT=1

die() { echo "make_kit_2k: error: $*" >&2; exit 1; }
log() { echo "make_kit_2k: $*"; }
sha_of() { sha256sum "$1" | cut -d' ' -f1; }

BB_NAME=busybox-1.36.1-ak3918-armv5te-static
BB_SHA256=367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290
OUT="$ROOT/dist/kit-2k"
MODEL=lsc-rotatable-2k-ak3918av130
APP_DIST=$ROOT/dist/rot2k-app
DB_DIST=$ROOT/dist/dropbear
BB=$ROOT/tools/.cache/rot2k/$BB_NAME
while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        --out | --model | --app-dist | --dropbear-dist | --busybox)
            [ $# -ge 2 ] || die "$1 needs a value"
            case "$1" in
                --out) OUT=$2 ;; --model) MODEL=$2 ;; --app-dist) APP_DIST=$2 ;;
                --dropbear-dist) DB_DIST=$2 ;; --busybox) BB=$2 ;;
            esac
            shift
            ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done
for t in tar xz sha256sum git install find sort awk od; do
    command -v "$t" >/dev/null 2>&1 || die "required tool '$t' not found"
done
tar --version 2>/dev/null | grep -q 'GNU tar' || die "GNU tar is required (--sort/--mtime/--owner)"

# --- model: must be a cardless-appfs registry entry -------------------------------------------------------------
REG="$HERE/models/$MODEL.txt"
[ -s "$REG" ] || die "unknown model '$MODEL' ($REG missing)"
[ "$(sed 's/\r$//' "$REG" | sed -n 's/^model=//p')" = "$MODEL" ] || die "$REG has no 'model=$MODEL' header line"
[ "$(sed 's/\r$//' "$REG" | sed -n 's/^[[:space:]]*kind[[:space:]]*=[[:space:]]*//p' | head -n1)" = cardless-appfs ] \
    || die "$MODEL is not a cardless-appfs model: use make_kit.sh"
grep -qE '^validated\|' "$REG" || die "$REG lists no validated target"

# --- identity ---------------------------------------------------------------------------------------------------
COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)" || die "not a git checkout: cannot record the source commit"
KIT_GRADE=release
if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]; then
    [ "${AKAM_KIT_ALLOW_DIRTY:-0}" = 1 ] || die "working tree is not clean (set AKAM_KIT_ALLOW_DIRTY=1 for a non-release test kit)"
fi
[ "${AKAM_KIT_ALLOW_DIRTY:-0}" != 1 ] || KIT_GRADE="test"
[ -z "${AKAM_KIT_CHECK_TIME32:-}" ] || KIT_GRADE="test"
[ "${SKIP_SOURCE_PROOFS:-0}" != 1 ] || KIT_GRADE="test"
VERSION="${AKAM_KIT_VERSION:-$(git -C "$ROOT" describe --always 2>/dev/null)}"
case "$VERSION" in '' | *[!A-Za-z0-9._+-]*) die "bad version string '$VERSION' (allowed: A-Z a-z 0-9 . _ + -)" ;; esac
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
case "$EPOCH" in '' | *[!0-9]*) die "SOURCE_DATE_EPOCH must be an integer" ;; esac
BUILT_UTC="$(date -u -d "@$EPOCH" '+%Y-%m-%dT%H:%M:%SZ')"
KITNAME="akam-kit-$MODEL-$VERSION"

# --- proofs on the inputs (the same as build_appfs.sh) ----------------------------------------------------------
for f in "$APP_DIST/akam2k_rtsp" "$APP_DIST/akam2k_sntp" "$APP_DIST/BUILDINFO.txt" "$DB_DIST/dropbearmulti" "$DB_DIST/BUILDINFO.txt"; do
    [ -f "$f" ] || die "$f not found (build it first: make -C platforms/anyka-ak3918av130 poc3-dist, tools/dropbear/build_dropbear.sh)"
done
# The test-only override of the pinned hash marks the kit "test" (verify refuses it without --allow-test-kit).
if [ -n "${AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY:-}" ]; then
    BB_SHA256=$AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY
    KIT_GRADE="test"
fi
[ -f "$BB" ] && [ "$(sha_of "$BB")" = "$BB_SHA256" ] || die "busybox $BB missing or not the pinned $BB_SHA256"
abi() { sed -n "s/^$1=//p" "$APP_DIST/BUILDINFO.txt" | head -n1; }
dbi() { sed -n "s/^$1=//p" "$DB_DIST/BUILDINFO.txt" | head -n1; }
[ "$(abi symcheck)" = passed ] || die "$APP_DIST/BUILDINFO.txt does not say symcheck=passed"
[ "$(abi sha256)" = "$(sha_of "$APP_DIST/akam2k_rtsp")" ] || die "akam2k_rtsp does not match its BUILDINFO.txt"
[ "$(abi sntp_sha256)" = "$(sha_of "$APP_DIST/akam2k_sntp")" ] || die "akam2k_sntp does not match its BUILDINFO.txt"
[ "$(dbi sha256_stripped)" = "$(sha_of "$DB_DIST/dropbearmulti")" ] || die "dropbearmulti does not match its BUILDINFO.txt"
if [ "${SKIP_SOURCE_PROOFS:-0}" != 1 ]; then
    [ "$(abi source_sha256)" = "$(sh "$FW/image/source_hash.sh")" ] || die "akam2k_rtsp was not built from the current sources"
    [ "$(abi sntp_source_sha256)" = "$(sh "$FW/image/source_hash.sh" sntp)" ] || die "akam2k_sntp was not built from the current sntp sources"
    [ "$(dbi time32_shim_sha256)" = "$(sha_of "$ROOT/src/core/time32_shim.c")" ] || die "dropbearmulti was not built with the current time32 shim"
fi
for b in akam2k_rtsp akam2k_sntp; do
    head -c 4096 "$APP_DIST/$b" | grep -a -q '/lib/ld-uClibc\.so\.0' || die "$b does not request /lib/ld-uClibc.so.0"
    [ "$(od -An -tu1 -j18 -N2 "$APP_DIST/$b" | tr -s ' ' | sed 's/^ //')" = "40 0" ] || die "$b is not an ARM executable"
done
case " $(dbi programs) " in *" dropbear "*) ;; *) die "dropbearmulti lacks dropbear" ;; esac
case " $(dbi programs) " in *" dropbearkey "*) ;; *) die "dropbearmulti lacks dropbearkey" ;; esac

TMP="$(mktemp -d "${TMPDIR:-/tmp}/akam-kit2k.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
KIT="$TMP/$KITNAME"
mkdir -p "$KIT/appfs/overlay" "$KIT/appfs/bin" "$KIT/card/akam" "$KIT/licenses"

# --- time64 gate (blocking) on the musl binary; the uClibc apps use the camera's own libc ------------------------
GATE_TOOL="${AKAM_KIT_CHECK_TIME32:-$ROOT/tools/build/check_time32.sh}"
log "time64 gate on dropbearmulti"
bash "$GATE_TOOL" --require-263 "$DB_DIST/dropbearmulti" || die "time64 gate FAILED: no kit written."
GATE_DISASM="$(gdb --version 2>/dev/null | head -1 || true)"
[ -n "$GATE_DISASM" ] || GATE_DISASM=unknown

# --- the payload: the same files, in the same order, as build_appfs.sh -------------------------------------------
for f in own_app_stub.sh own_app_run.sh common.sh decloud.sh start_ssh.sh akam_net.sh akam_api_key.sh; do
    install -m 755 "$FW/boot/card/akam/$f" "$KIT/appfs/overlay/$f"
done
install -m 755 "$FW/boot/flash/akam_flash_boot.sh" "$KIT/appfs/overlay/akam_flash_boot.sh"
install -m 755 "$ROOT/src/scripts/akam_ntp.sh" "$KIT/appfs/overlay/akam_ntp.sh"
install -m 755 "$ROOT/src/scripts/akam_hostname.sh" "$KIT/appfs/overlay/akam_hostname.sh"
install -m 755 "$APP_DIST/akam2k_rtsp" "$KIT/appfs/bin/akam2k_rtsp"
install -m 644 "$APP_DIST/BUILDINFO.txt" "$KIT/appfs/bin/akam2k_rtsp.BUILDINFO"
install -m 755 "$APP_DIST/akam2k_sntp" "$KIT/appfs/bin/akam2k_sntp"
install -m 755 "$DB_DIST/dropbearmulti" "$KIT/appfs/bin/dropbearmulti"
install -m 644 "$DB_DIST/BUILDINFO.txt" "$KIT/appfs/bin/dropbearmulti.BUILDINFO"
install -m 755 "$BB" "$KIT/appfs/bin/busybox"

# --- the SD card files that are not in appfs/ (build_card.sh copies overlay/*, the rest is appfs/) --------------------
install -m 644 "$FW/boot/card/yy_debug.ini" "$KIT/card/yy_debug.ini"
install -m 755 "$FW/boot/card/akam_boot.sh" "$KIT/card/akam_boot.sh"
for f in enable_local.sh restore_param.sh; do
    install -m 755 "$FW/boot/card/akam/$f" "$KIT/card/akam/$f"
done
[ "$(cat "$KIT/card/yy_debug.ini")" = "$(printf 'mode=debug\nstart_sh=/mnt/sdcard/akam_boot.sh')" ] || die "card yy_debug.ini is not the expected mode=debug file"

# --- licences and the source offer -----------------------------------------------------------------------------
install -m 644 "$ROOT/LICENSE" "$KIT/licenses/LICENSE.akam"
install -m 644 "$ROOT/THIRD_PARTY_NOTICES.md" "$KIT/licenses/THIRD_PARTY_NOTICES.md"
[ -s "$DB_DIST/LICENSE.dropbear" ] || die "$DB_DIST/LICENSE.dropbear missing (the licence ships with the binary)"
install -m 644 "$DB_DIST/LICENSE.dropbear" "$KIT/licenses/LICENSE.dropbear"
install -m 644 "$REG" "$KIT/supported_hardware.txt"
{
    echo "Akam source offer"
    echo "akam_repository=https://github.com/Foohx/akam-fw"
    echo "source_commit=$COMMIT"
    echo "The complete corresponding sources are the git tree at that commit."
    echo "busybox=$BB_NAME sha256=$BB_SHA256 (alex-so-3/lsc-ak3918-yyos-root, release busybox-v1.36.1; GPLv2, source: busybox.net)"
} >"$KIT/licenses/SOURCES.txt"
chmod 644 "$KIT/licenses/SOURCES.txt"

# --- content guard ---------------------------------------------------------------------------------------------
BAD="$( (grep -rIlE -e 'BEGIN [A-Z0-9 ]*PRIVATE KEY' "$KIT" 2>/dev/null || true
    find "$KIT" \( -name '*_host_key*' -o -name 'id_rsa*' -o -name 'id_ed25519*' -o -name '*.pem' -o -name '*.squashfs' -o -name '*.img' -o -name '*.ko' -o -name 'mtd*' -o -name 'fac_av130*' \)) | sort -u)"
[ -z "$BAD" ] || die "forbidden content in the kit (key, image, dump, vendor file):
$BAD"
find "$KIT" -type d -exec chmod 755 {} +

# --- MANIFEST.txt and SHA256SUMS (read by internal/installer/appfs: kind, time64_gate=pass exactly once) -------------
sha() { sha_of "$1"; }
{
    echo "kit_format=$KIT_FORMAT"
    echo "kind=cardless-appfs"
    echo "kit_grade=$KIT_GRADE"
    echo "akam_version=$VERSION"
    echo "model=$MODEL"
    echo "source_commit=$COMMIT"
    echo "built_utc=$BUILT_UTC"
    echo "target=arm-linux"
    echo "time64_gate=pass"
    echo "time64_gate_tool=tools/build/check_time32.sh sha256=$(sha "$ROOT/tools/build/check_time32.sh")"
    echo "supported_hardware_sha256=$(sha "$REG")"
    echo "supported_hardware=supported_hardware.txt"
    echo "# file <sha256> <mode> <path>"
    (cd "$KIT" && find . -type f ! -name MANIFEST.txt | LC_ALL=C sort | sed 's|^\./||') | while IFS= read -r f; do
        echo "file $(sha "$KIT/$f") $(stat -c %a "$KIT/$f") $f"
    done
} >"$KIT/MANIFEST.txt"
chmod 644 "$KIT/MANIFEST.txt"
(cd "$KIT" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | while IFS= read -r f; do
    echo "$(sha_of "$f")  $f"
done) >"$TMP/SHA256SUMS.inner"
install -m 644 "$TMP/SHA256SUMS.inner" "$KIT/SHA256SUMS"

# --- reproducible tarball ----------------------------------------------------------------------------------------
mkdir -p "$OUT"
TARBALL="$OUT/$KITNAME.tar.xz"
rm -f "$TARBALL" "$OUT/SHA256SUMS" "$OUT/SHA256SUMS.sig"
(cd "$TMP" && LC_ALL=C tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner \
    --format=gnu -cf - "$KITNAME") | xz -9e -T1 >"$TARBALL"
(cd "$OUT" && sha256sum "$KITNAME.tar.xz" >SHA256SUMS)
# The disassembler that ran the time64 gate depends on the build machine: it is NOT in the hashed MANIFEST (the tarball
# is reproducible on any machine), only in this note next to the tarball (not part of the kit, never released).
printf 'kit=%s\ntime64_gate_disassembler=%s\n' "$KITNAME" "$GATE_DISASM" >"$OUT/$KITNAME.build-note.txt"
log "wrote $TARBALL (kit_grade=$KIT_GRADE)"
cat "$OUT/SHA256SUMS"
