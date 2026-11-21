#!/usr/bin/env bash
# Build the card-less Akam appfs image (squashfs for mtd3) of the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
#
#   firmware/lsc-rotatable-2k-ak3918av130/image/build_appfs.sh --backup mtd3.bin [options]
#
# HOST ONLY. Reads the stock appfs backup, writes files under --out. It never touches a device, never writes
# flash or nvram, never prints a secret (no wifi credential or SSH key is ever put in the image: they live in
# /config/akam on the camera, see docs/porting/lsc-rotatable-2k/cardless-appfs.md).
#
# What the image is: the STOCK appfs (drivers, libs, conf, wpa_supplicant, yy_upgrade, start.sh: all untouched)
# plus
#   /appfs/script/init.sh        stock init.sh with ONE branch changed (mode "normal" -> Akam from flash; the SD
#                                override modes debug/factory behave exactly as stock, the recovery path)
#   /appfs/akam/                 akam_flash_boot.sh, own_app_stub.sh, own_app_run.sh, common.sh, decloud.sh,
#                                akam_ntp.sh, start_ssh.sh, dropbearmulti, busybox (the card's static one),
#                                bin/akam2k_rtsp (+BUILDINFO), bin/akam2k_sntp (+BUILDINFO)
#   /appfs/bin/yy_cam            REPLACED by a 3-line launcher of the stub. The stock yy_cam (3.6 MB, 1.2 MB
#                                compressed) is the only way the Akam payload fits the 0x490000 partition. The stock
#                                app (and with it the cloud OTA client) is gone from this image; the stock mtd3
#                                backup restores it.
# Options:
#   --backup FILE          stock mtd3 dump (required; must be a squashfs with magic hsqs)
#   --app-dist DIR         akam2k_rtsp + akam2k_sntp + BUILDINFO.txt from `make poc3-dist` (default dist/rot2k-app)
#   --dropbear-dist DIR    dropbearmulti + BUILDINFO.txt (default dist/dropbear)
#   --busybox FILE         the pinned static busybox of the card (default tools/.cache/rot2k/<name>)
#   --out DIR              output directory (default dist/rot2k-appfs, git-ignored)
# Output: appfs-akam.squashfs (padded with 0x00 to the partition size 0x490000), appfs-akam.squashfs.sha256,
#         appfs_sign.txt, MANIFEST.txt.
# Reproducible: fixed mkfs/file times (the stock image's), sorted, single owner, no xattrs; the same inputs give
# the same bytes with the same squashfs-tools version (recorded in MANIFEST.txt).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FW=$(cd "$HERE/.." && pwd)
REPO=$(cd "$FW/../.." && pwd)
PART=$((0x490000))
BB_NAME=busybox-1.36.1-ak3918-armv5te-static
BB_SHA256=367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290

die() { echo "build_appfs: error: $*" >&2; exit 1; }
log() { echo "build_appfs: $*"; }
sha_of() { sha256sum "$1" | cut -d' ' -f1; }

backup=""
app_dist=$REPO/dist/rot2k-app
dbear_dist=$REPO/dist/dropbear
bb=$REPO/tools/.cache/rot2k/$BB_NAME
out=$REPO/dist/rot2k-appfs
while [ "$#" -gt 0 ]; do
    [ "$#" -ge 2 ] || die "$1 needs a value"
    case $1 in
        --backup) backup=$2 ;;
        --app-dist) app_dist=$2 ;;
        --dropbear-dist) dbear_dist=$2 ;;
        --busybox) bb=$2 ;;
        --out) out=$2 ;;
        *) die "unknown option $1" ;;
    esac
    shift 2
done
[ -f "$backup" ] || die "--backup FILE is required"
if ! command -v mksquashfs >/dev/null || ! command -v unsquashfs >/dev/null; then die "squashfs-tools (mksquashfs, unsquashfs) are required"; fi
[ "$(head -c 4 "$backup")" = hsqs ] || die "$backup is not a squashfs (magic hsqs)"
[ "$(wc -c <"$backup")" = "$PART" ] || die "$backup is not $PART bytes (a full mtd3 dump)"

# --- inputs: same proofs as build_card.sh -----------------------------------------------------------------------
[ -f "$bb" ] && [ "$(sha_of "$bb")" = "$BB_SHA256" ] || die "busybox $bb missing or not the pinned $BB_SHA256"
abi() { sed -n "s/^$1=//p" "$app_dist/BUILDINFO.txt" | head -n1; }
[ "$(abi symcheck)" = passed ] || die "$app_dist/BUILDINFO.txt does not say symcheck=passed"
[ "$(abi sha256)" = "$(sha_of "$app_dist/akam2k_rtsp")" ] || die "akam2k_rtsp does not match its BUILDINFO.txt"
[ "$(abi source_sha256)" = "$(sh "$HERE/source_hash.sh")" ] || die "akam2k_rtsp was not built from the current sources"
[ "$(abi sntp_sha256)" = "$(sha_of "$app_dist/akam2k_sntp")" ] || die "akam2k_sntp does not match its BUILDINFO.txt"
[ "$(abi sntp_source_sha256)" = "$(sh "$HERE/source_hash.sh" sntp)" ] || die "akam2k_sntp was not built from the current sntp sources"
for b in akam2k_rtsp akam2k_sntp; do
    head -c 4096 "$app_dist/$b" | grep -a -q '/lib/ld-uClibc\.so\.0' || die "$b does not request /lib/ld-uClibc.so.0"
    [ "$(od -An -tu1 -j18 -N2 "$app_dist/$b" | tr -s ' ' | sed 's/^ //')" = "40 0" ] || die "$b is not an ARM executable"
done
dbi() { sed -n "s/^$1=//p" "$dbear_dist/BUILDINFO.txt" | head -n1; }
[ "$(dbi sha256_stripped)" = "$(sha_of "$dbear_dist/dropbearmulti")" ] || die "dropbearmulti does not match its BUILDINFO.txt"
[ "$(dbi time32_shim_sha256)" = "$(sha_of "$REPO/src/core/time32_shim.c")" ] || die "dropbearmulti was not built with the current time32 shim"
case " $(dbi programs) " in *" dropbear "*) ;; *) die "dropbearmulti lacks dropbear" ;; esac
case " $(dbi programs) " in *" dropbearkey "*) ;; *) die "dropbearmulti lacks dropbearkey" ;; esac

# the stock image's creation time (superblock bytes 8..11), reused so the output does not depend on the build date
MKFS_TIME=$(od -An -tu4 -j8 -N4 "$backup" | tr -d ' ')

# --- unpack the stock appfs ----------------------------------------------------------------------------------------
mkdir -p "$out"
tree=$out/tree
rm -rf "$tree"
unsquashfs -no-xattrs -d "$tree" "$backup" >/dev/null || die "unsquashfs failed"
[ -f "$tree/script/init.sh" ] && [ -f "$tree/script/start.sh" ] && [ -f "$tree/bin/yy_cam" ] || die "the backup does not look like the stock appfs"
log "stock appfs unpacked to $tree"

# --- init.sh: the stock file with the one branch changed ---------------------------------------------------------
python3 -I - "$tree/script/init.sh" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
old = "        normal)\n            /appfs/script/start.sh &\n            ;;\n"
new = (
    "        normal)\n"
    "            # AKAM: no override card (no yy_debug.ini, or mode=normal): start Akam from THIS image. The\n"
    "            # override modes below (debug, factory) are untouched stock code, so the Akam SD card\n"
    "            # (mode=debug, start_sh=/mnt/sdcard/akam_boot.sh) always replaces the flashed app = recovery.\n"
    "            # Without the launcher (a broken image) fall back to the plain stock hand-off.\n"
    "            if [ -f /appfs/akam/akam_flash_boot.sh ]; then\n"
    "                /bin/sh /appfs/akam/akam_flash_boot.sh &\n"
    "            else\n"
    "                /appfs/script/start.sh &\n"
    "            fi\n"
    "            ;;\n"
)
if s.count(old) != 1:
    sys.exit("init.sh: the stock 'normal)' branch was not found exactly once")
open(p, "w", encoding="utf-8").write(s.replace(old, new))
PY
sh -n "$tree/script/init.sh" || die "patched init.sh has a syntax error"

# --- the Akam payload ---------------------------------------------------------------------------------------------
ak=$tree/akam
mkdir -p "$ak/bin"
for f in own_app_stub.sh own_app_run.sh common.sh decloud.sh start_ssh.sh akam_net.sh akam_api_key.sh; do
    install -m 755 "$FW/boot/card/akam/$f" "$ak/$f"
done
install -m 755 "$FW/boot/flash/akam_flash_boot.sh" "$ak/akam_flash_boot.sh"
install -m 755 "$REPO/src/scripts/akam_ntp.sh" "$ak/akam_ntp.sh"
install -m 755 "$REPO/src/scripts/akam_hostname.sh" "$ak/akam_hostname.sh"
install -m 755 "$dbear_dist/dropbearmulti" "$ak/dropbearmulti"
install -m 755 "$bb" "$ak/busybox"
install -m 755 "$app_dist/akam2k_rtsp" "$ak/bin/akam2k_rtsp"
install -m 644 "$app_dist/BUILDINFO.txt" "$ak/bin/akam2k_rtsp.BUILDINFO"
install -m 755 "$app_dist/akam2k_sntp" "$ak/bin/akam2k_sntp"
printf 'tool=akam2k_sntp\nsha256=%s\n' "$(abi sntp_sha256)" >"$ak/bin/akam2k_sntp.BUILDINFO"
chmod 644 "$ak/bin/akam2k_sntp.BUILDINFO"
for f in "$ak"/*.sh; do sh -n "$f" || die "$f has a syntax error"; done

# yy_cam: a launcher of the stub. start.sh runs it (when nvram STATUS=active); the SD overlay bind-mounts its own
# stub over it, so the file must exist.
cat >"$tree/bin/yy_cam" <<'EOF'
#!/bin/sh
# Akam card-less image: stand-in for the stock yy_cam (see /appfs/akam/own_app_stub.sh).
exec /bin/sh /appfs/akam/own_app_stub.sh
EOF
chmod 755 "$tree/bin/yy_cam"

# --- squashfs, the stock parameters (unsquashfs -s of the backup: xz, 128 KiB blocks, exportable, no xattrs) ----------
img=$out/appfs-akam.squashfs
rm -f "$img" "$img.unpadded"
mksquashfs "$tree" "$img.unpadded" -comp xz -b 131072 -noappend -xattrs -all-root \
    -mkfs-time "$MKFS_TIME" -all-time "$MKFS_TIME" -no-progress -quiet >/dev/null
used=$(od -An -tu8 -j40 -N8 "$img.unpadded" | tr -d ' ')
size=$(wc -c <"$img.unpadded")
[ "$size" -le "$PART" ] || die "image is $size bytes, over the partition ($PART)"
# pad with 0x00 to the full partition (the stock dump is padded the same way)
cp "$img.unpadded" "$img"
truncate -s "$PART" "$img"
rm -f "$img.unpadded"
[ "$(wc -c <"$img")" = "$PART" ] || die "padding failed"
unsquashfs -s "$img" >"$out/unsquashfs-s.txt" 2>&1 || die "the built image does not parse"
unsquashfs -l "$img" >/dev/null || die "the built image cannot be listed"
unsquashfs -s "$backup" 2>&1 | grep -E 'Compression|Block size|exportable|Inodes are|Data are|Uids|Fragments are|Tailends|Duplicates|Xattrs|xattr ids' >"$out/.stock-params"
grep -E 'Compression|Block size|exportable|Inodes are|Data are|Uids|Fragments are|Tailends|Duplicates|Xattrs|xattr ids' "$out/unsquashfs-s.txt" | diff - "$out/.stock-params" >/dev/null ||
    die "squashfs parameters differ from the stock image (see $out/unsquashfs-s.txt)"
rm -f "$out/.stock-params"
# The camera kernel has no loop device and no spare partition: this image cannot be mounted before it is flashed.
# So the superblock must equal the stock one in kind: magic, block size (12..15), compressor, block log, flags,
# id count, version (20..31), xattr table start (56..63). Only sizes, counts and table offsets may differ.
for r in "0 4" "12 4" "20 12" "56 8"; do
    # shellcheck disable=SC2086  # "offset length"
    [ "$(od -An -tx1 -j${r% *} -N${r#* } "$img")" = "$(od -An -tx1 -j${r% *} -N${r#* } "$backup")" ] || die "superblock bytes $r differ from the stock image"
done
# every block of the image must decompress and equal the tree it was built from
rm -rf "$out/verify"
unsquashfs -no-xattrs -d "$out/verify" "$img" >/dev/null 2>&1 || die "the built image does not extract"
diff -r "$tree" "$out/verify" >/dev/null || die "the extracted image differs from the tree it was built from"
rm -rf "$out/verify"

sign=$(python3 -I "$FW/tools/calc_partition_sign.py" appfs "$img" | sed -n "s/.*sign=//p" | tail -n1)
imgsha=$(sha_of "$img")
echo "$imgsha  appfs-akam.squashfs" >"$out/appfs-akam.squashfs.sha256"
echo "$sign" >"$out/appfs_sign.txt"
{
    echo "image=appfs-akam.squashfs"
    echo "size=$(wc -c <"$img") (partition $PART, padded with 0x00)"
    echo "squashfs_bytes_used=$used (0x$(printf '%x' "$used"))"
    echo "sha256=$imgsha"
    echo "appfs_sign=$sign"
    echo "mksquashfs=$(mksquashfs -version | head -n1)"
    echo "params=-comp xz -b 131072 -noappend -xattrs -all-root -mkfs-time $MKFS_TIME -all-time $MKFS_TIME"
    echo "stock_backup_sha256=$(sha_of "$backup")"
    echo "akam2k_rtsp_sha256=$(abi sha256)"
    echo "akam2k_sntp_sha256=$(abi sntp_sha256)"
    echo "app_source_sha256=$(abi source_sha256)"
    echo "dropbearmulti_sha256=$(dbi sha256_stripped)"
    echo "busybox_sha256=$BB_SHA256"
    echo "init_sh_sha256=$(sha_of "$tree/script/init.sh")"
    echo "flash_boot_sha256=$(sha_of "$FW/boot/flash/akam_flash_boot.sh")"
    echo "own_app_stub_sha256=$(sha_of "$FW/boot/card/akam/own_app_stub.sh")"
    echo "git_head=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
} >"$out/MANIFEST.txt"
log "image $img ($size bytes used by squashfs, padded to $PART)"
log "sha256 $imgsha"
log "appfs_sign $sign"
