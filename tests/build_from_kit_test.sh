#!/usr/bin/env bash
# Tests for src/ak_rtsp/build_firmware.sh --kit and tools/image/assemble_from_kit.sh:
# kit mode builds the same image as source mode, refuses bad kits, follows the stock
# APP compression and checks the supported hardware list. Stub binaries, synthetic APP
# fixtures and throw-away kits only: no hardware, no network, no real dump.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BUILD=$REPO/src/ak_rtsp/build_firmware.sh
MAKE=$REPO/tools/release/make_kit.sh
ASSEMBLE=$REPO/tools/image/assemble_from_kit.sh
FIXTURE=$REPO/tests/fixtures/app_fake
PART=5242880

for t in tar xz sha256sum mksquashfs unsquashfs; do
    command -v "$t" >/dev/null || { echo "missing tool: $t"; exit 1; }
done

WORK=$(mktemp -d)
trap '[ -n "${KEEP:-}" ] || rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() {
    local d=$1
    shift
    if "$@"; then ok "$d"; else ko "$d"; fi
}

# --- stubs and a test kit -------------------------------------------------------------------
S=$WORK/stub
mkdir -p "$S/smol/build/_deps" "$S/db"
printf '#!/bin/sh\necho stub ak_rtsp\n' >"$S/ak_rtsp"
printf '#!/bin/sh\necho stub flash_tool\n' >"$S/flash_tool"
printf '#!/bin/sh\necho stub dropbearmulti\n' >"$S/db/dropbearmulti"
printf 'Dropbear stub licence\n' >"$S/db/LICENSE.dropbear"
printf 'smolrtsp stub licence\n' >"$S/smol/LICENSE"
for d in slice99 metalang99 datatype99 interface99; do
    mkdir -p "$S/smol/build/_deps/$d-src"
    printf '%s stub licence\n' "$d" >"$S/smol/build/_deps/$d-src/LICENSE"
done
printf 'musl stub\n' >"$S/COPYRIGHT"
printf 'zig stub\n' >"$S/ZIGLICENSE"
printf '#!/bin/sh\nexit 0\n' >"$S/gate_ok"
chmod 755 "$S"/ak_rtsp "$S"/flash_tool "$S/db/dropbearmulti" "$S"/gate_ok

export SOURCE_DATE_EPOCH=1700000000
export SMOLRTSP_DIR=$S/smol MUSL_COPYRIGHT=$S/COPYRIGHT ZIG_LICENSE=$S/ZIGLICENSE
AKAM_KIT_NO_BUILD=1 AKAM_KIT_ALLOW_DIRTY=1 AKAM_KIT_VERSION=test1 AKAM_KIT_CHECK_TIME32=$S/gate_ok \
    AK_RTSP_BIN=$S/ak_rtsp FLASH_TOOL_BIN=$S/flash_tool DROPBEAR_MULTI=$S/db/dropbearmulti \
    bash "$MAKE" --out "$WORK/kitout" >"$WORK/make.log" 2>&1 || { cat "$WORK/make.log"; exit 1; }
TB=$WORK/kitout/akam-kit-test1.tar.xz
mkdir -p "$WORK/kx"
tar -xJf "$TB" -C "$WORK/kx"
K=$WORK/kx/akam-kit-test1

# rehash_kit <dir>: recompute MANIFEST file lines, supported_hardware_sha256 and SHA256SUMS
# after a test edited a kit file (so that only the intended defect remains).
rehash_kit() {
    local d=$1 f
    (
        cd "$d"
        sed -i "s/^supported_hardware_sha256=.*/supported_hardware_sha256=$(sha256sum supported_hardware.txt | cut -d' ' -f1)/" MANIFEST.txt
        while read -r t _ mode path; do
            [ "$t" = file ] || continue
            sed -i "s|^file [0-9a-f]* $mode $path\$|file $(sha256sum "$path" | cut -d' ' -f1) $mode $path|" MANIFEST.txt
        done < <(grep '^file ' MANIFEST.txt)
        find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' >"$d.list"
        while IFS= read -r f; do
            echo "$(sha256sum "$f" | cut -d' ' -f1)  $f"
        done <"$d.list" >"$d.sums"
        mv "$d.sums" SHA256SUMS
        rm -f "$d.list"
    )
}
# new_kit <name> -> copy of the good kit directory at $WORK/<name>
new_kit() {
    cp -a "$K" "$WORK/$1"
    echo "$WORK/$1"
}

# Run helper: run_kit <outdir> <args...> (APP fixture, kit mode, from another cwd)
run() {
    local out=$1
    shift
    mkdir -p "$out"
    (cd "$WORK" && env APP_EXTRACTED="${APP:-$FIXTURE}" FW_DIR="$out" bash "$BUILD" "$@") >"$out.log" 2>&1
}
refused() { # <description> <expected message regex> <outdir> <args...>
    local d=$1 re=$2 out=$3
    shift 3
    if run "$out" "$@"; then ko "$d (build succeeded)"; return; fi
    if grep -qE "$re" "$out.log"; then ok "$d"; else ko "$d (message?)"; tail -3 "$out.log"; fi
}
image_of() { find "$1" -maxdepth 1 -name 'ak_rtsp_firmware_*.squashfs' | head -1; }

# --- 1. kit image == source image ---------------------------------------------------------------
echo "== 1. kit mode equals source mode"
SRCOUT=$WORK/src
run_src() {
    mkdir -p "$SRCOUT"
    (cd "$WORK" && env APP_EXTRACTED="$FIXTURE" FW_DIR="$SRCOUT" AK_RTSP_BIN="$S/ak_rtsp" DROPBEAR_MULTI="$S/db/dropbearmulti" \
        AKAM_ALLOW_UNCHECKED_TIME64=1 bash "$BUILD") >"$SRCOUT.log" 2>&1
}
check "source mode builds" run_src
check "kit mode (directory) builds" run "$WORK/kitdir" --kit "$K" --allow-test-kit --allow-unsigned --i-understand-untested
check "kit mode (tarball, unsigned allowed) builds" run "$WORK/kittar" --kit "$TB" --allow-test-kit --allow-unsigned --i-understand-untested
SRC_IMG=$(image_of "$SRCOUT")
KIT_IMG=$(image_of "$WORK/kitdir")
TAR_IMG=$(image_of "$WORK/kittar")
check "kit image is exactly $PART bytes" test "$(stat -c %s "$KIT_IMG")" = "$PART"
sig() { # tree signature without the MANIFEST (it differs by design: the kit's own travels with the image)
    (cd "$1" && find . -printf '%y %m %p -> %l\n' | LC_ALL=C sort && find . -type f ! -path ./local/akam/MANIFEST.txt -exec sha256sum {} + | LC_ALL=C sort -k2)
}
unsquashfs -no-xattrs -d "$WORK/x_src" "$SRC_IMG" >/dev/null 2>&1 || true
unsquashfs -no-xattrs -d "$WORK/x_kit" "$KIT_IMG" >/dev/null 2>&1 || true
unsquashfs -no-xattrs -d "$WORK/x_tar" "$TAR_IMG" >/dev/null 2>&1 || true
sig "$WORK/x_src" | grep -v 'local/akam/MANIFEST.txt' >"$WORK/sig.src"
sig "$WORK/x_kit" | grep -v 'local/akam/MANIFEST.txt' >"$WORK/sig.kit"
sig "$WORK/x_tar" | grep -v 'local/akam/MANIFEST.txt' >"$WORK/sig.tar"
check "signature is not empty" test "$(wc -l <"$WORK/sig.src")" -gt 30
check "kit image == source image (paths, modes, links, contents)" diff "$WORK/sig.src" "$WORK/sig.kit"
check "kit tarball image == kit directory image" diff "$WORK/sig.kit" "$WORK/sig.tar"
check "image MANIFEST.txt is the kit's MANIFEST.txt" cmp -s "$K/MANIFEST.txt" "$WORK/x_kit/local/akam/MANIFEST.txt"
check "same compression as source mode" bash -c "[ \"\$(unsquashfs -s '$SRC_IMG' | grep -E '^(Compression|Block size)')\" = \"\$(unsquashfs -s '$KIT_IMG' | grep -E '^(Compression|Block size)')\" ]"
check "report says kit mode, grade and authentication" bash -c "
    grep -q '^build mode: *kit' '$WORK/kitdir/build_report.txt' && grep -q 'kit_grade=test' '$WORK/kitdir/build_report.txt' &&
    grep -q '^kit authentication:.*UNSIGNED' '$WORK/kittar/build_report.txt'"
check "kit mode: no time64 skip warning, none needed" bash -c "! grep -q 'SKIPPED' '$WORK/kitdir.log'"
check "kit mode: ak_rtsp is the kit's binary" cmp -s "$K/bin/ak_rtsp" "$WORK/x_kit/bin/ak_rtsp"
check "kit mode: tree has no private key" bash -c "! grep -rlE 'PRIVATE KEY' '$WORK/x_kit'"

# --- 2. refusals ----------------------------------------------------------------------------------
echo "== 2. kit refusals"
refused "a test-grade kit needs --allow-test-kit --allow-unsigned" 'kit_grade|development kits' "$WORK/r1" --kit "$K"
refused "an unsigned tarball needs --allow-unsigned" 'UNSIGNED' "$WORK/r2" --kit "$TB" --allow-test-kit
BAD=$(new_kit bad_manifest)
sed -i 's/^file [0-9a-f]* \(755 bin\/ak_rtsp\)$/file 0000000000000000000000000000000000000000000000000000000000000000 \1/' "$BAD/MANIFEST.txt"
refused "invalid MANIFEST (wrong sha256) is refused" 'FAIL|failed' "$WORK/r3" --kit "$BAD" --allow-test-kit --allow-unsigned
BAD=$(new_kit bad_binary)
printf 'tampered\n' >>"$BAD/bin/ak_rtsp"
refused "a modified binary is refused" 'FAIL|failed' "$WORK/r4" --kit "$BAD" --allow-test-kit --allow-unsigned
BAD=$(new_kit bad_gate)
sed -i 's/^time64_gate=pass$/time64_gate=fail/' "$BAD/MANIFEST.txt"
rehash_kit "$BAD"
refused "time64_gate != pass is refused" 'time64_gate' "$WORK/r5" --kit "$BAD" --allow-test-kit --allow-unsigned
BAD=$(new_kit bad_format)
sed -i 's/^kit_format=1$/kit_format=2/' "$BAD/MANIFEST.txt"
rehash_kit "$BAD"
refused "incompatible kit_format is refused" 'kit_format 2 is not supported' "$WORK/r6" --kit "$BAD" --allow-test-kit --allow-unsigned
BAD=$(new_kit extra_file)
printf 'x\n' >"$BAD/extra.txt"
refused "an extra file in the kit is refused" 'FAIL|failed' "$WORK/r7" --kit "$BAD" --allow-test-kit --allow-unsigned
refused "a missing kit is refused" 'no such file' "$WORK/r8" --kit "$WORK/nokit"
check "refused builds leave no image" bash -c "[ -z \"\$(find '$WORK'/r[1-8] -name 'ak_rtsp_firmware_*' 2>/dev/null)\" ]"

# --- 3. stock APP fixtures: compression followed, unknown refused -------------------------------------
echo "== 3. stock compression and hardware check"
# mkstock <name> <mksquashfs options...>: a 5242880 byte "mtdblock7.bin" built from the fixture
mkstock() {
    local n=$1
    shift
    mkdir -p "$WORK/bk_$n"
    mksquashfs "$FIXTURE" "$WORK/bk_$n/mtdblock7.bin" -noappend -no-progress -all-root "$@" >/dev/null 2>&1 || return 1
    truncate -s "$PART" "$WORK/bk_$n/mtdblock7.bin"
}
comp_of() { unsquashfs -s "$1" | awk '/^Compression/{print $2}'; }
block_of() { unsquashfs -s "$1" | awk '/^Block size/{print $3}'; }
check "stock fixture gzip" mkstock gz -comp gzip -b 131072
check "stock fixture xz 64K" mkstock xz -comp xz -b 65536
check "stock fixture xz+bcj" mkstock bcj -comp xz -Xbcj arm -b 131072

if run "$WORK/g0" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin"; then ko "untested hardware refused by default"; else ok "untested hardware refused by default"; fi
check "  ... with the verdict in the message" grep -qE 'not validated \(unknown' "$WORK/g0.log"
check "gzip stock -> gzip image (untested flag)" run "$WORK/g1" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin" --i-understand-untested
check "  image is gzip" test "$(comp_of "$(image_of "$WORK/g1")")" = gzip
check "  report: same as stock, verdict, fingerprint" bash -c "
    grep -q 'compression vs stock: SAME' '$WORK/g1/build_report.txt' && grep -q '^hardware verdict: *unknown' '$WORK/g1/build_report.txt' &&
    grep -q '^stock APP sha256: *$(sha256sum "$WORK/bk_gz/mtdblock7.bin" | cut -d' ' -f1)' '$WORK/g1/build_report.txt' &&
    grep -q 'squashfs gzip, block 131072' '$WORK/g1/build_report.txt'"
check "  loud warning about untested hardware" grep -q 'UNTESTED hardware' "$WORK/g1.log"
check "xz stock (64K blocks) -> xz image with 64K blocks" run "$WORK/g2" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_xz/mtdblock7.bin" --i-understand-untested
check "  image is xz, block 65536" bash -c "[ \"\$(unsquashfs -s '$(image_of "$WORK/g2")' | awk '/^Compression/{print \$2}')\" = xz ] && [ \"\$(unsquashfs -s '$(image_of "$WORK/g2")' | awk '/^Block size/{print \$3}')\" = 65536 ]"
check "  no BCJ filter in the image" bash -c "! unsquashfs -s '$(image_of "$WORK/g2")' | grep -qi filter"
check "xz+bcj stock -> xz image with the BCJ filter" run "$WORK/g3" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_bcj/mtdblock7.bin" --i-understand-untested
check "  filter arm present" bash -c "unsquashfs -s '$(image_of "$WORK/g3")' | grep -qi 'Filters selected: arm'"
check "--comp xz on a gzip stock: built, with a loud warning" run "$WORK/g4" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin" --i-understand-untested --comp xz
check "  image is xz" test "$(comp_of "$(image_of "$WORK/g4")")" = xz
check "  warning and report say DIFFERS / DIFFERENT" bash -c "grep -q 'COMPRESSION DIFFERS FROM THE STOCK' '$WORK/g4.log' && grep -q 'DIFFERENT from the stock' '$WORK/g4/build_report.txt'"
refused "--comp lzo is refused" 'must be gzip or xz' "$WORK/g5" --kit "$K" --allow-test-kit --allow-unsigned --comp lzo
# unknown stock compressor: whichever of zstd/lzo/lz4 this mksquashfs can write
UNK=""
for c in zstd lzo lz4; do
    if mkstock unk -comp "$c"; then UNK=$c; break; fi
done
if [ -n "$UNK" ]; then
    refused "stock compressed with $UNK is refused" "compression '$UNK'" "$WORK/u1" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_unk/mtdblock7.bin" --i-understand-untested
    refused "  ... even with --comp alone" "compression '$UNK'" "$WORK/u2" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_unk/mtdblock7.bin" --i-understand-untested --comp xz
    refused "  ... and --allow-unknown-stock-comp alone (no --comp)" "compression '$UNK'" "$WORK/u3" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_unk/mtdblock7.bin" --i-understand-untested --allow-unknown-stock-comp
    check "  ... explicit flags: built with warning" run "$WORK/u4" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_unk/mtdblock7.bin" --i-understand-untested --comp xz --allow-unknown-stock-comp
    check "  ... the warning is loud" grep -q "not reproducible" "$WORK/u4.log"
else
    echo "  skip - this mksquashfs has no zstd/lzo/lz4 compressor"
fi
check "kit mode without any fingerprint is 'unknown' and refused" bash -c "! (cd '$WORK' && APP_EXTRACTED='$FIXTURE' FW_DIR='$WORK/n1' bash '$BUILD' --kit '$K' --allow-test-kit --allow-unsigned) >'$WORK/n1.log' 2>&1 && grep -q 'no stock APP fingerprint' '$WORK/n1.log'"
check "source mode without fingerprint still builds (check skipped)" grep -q '^hardware verdict: *not-checked' "$SRCOUT/build_report.txt"
printf 'compression=gzip\nblock_size=131072\nbcj=none\napp_sha256=%064d\n' 0 >"$WORK/stock.info"
check "stock-info file is followed" run "$WORK/si" --kit "$K" --allow-test-kit --allow-unsigned --stock-info "$WORK/stock.info" --i-understand-untested
check "  image is gzip" test "$(comp_of "$(image_of "$WORK/si")")" = gzip

echo "== 4. supported hardware list"
# A kit whose supported_hardware.txt lists the fixture dump as validated.
GZSHA=$(sha256sum "$WORK/bk_gz/mtdblock7.bin" | cut -d' ' -f1)
VK=$(new_kit validated_kit)
printf 'supported_hardware_format=1\nvalidated|fixture camera|SOC|4.4.302-cip94 #1 test|8 partitions; mtd7=APP 00500000|squashfs gzip block=131072 bcj=none|%s|1.30.1\n' "$GZSHA" >"$VK/supported_hardware.txt"
rehash_kit "$VK"
check "validated fingerprint: builds without the flag" run "$WORK/v1" --kit "$VK" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin"
check "  verdict validated in the report" grep -q '^hardware verdict: *validated' "$WORK/v1/build_report.txt"
check "  no untested warning" bash -c "! grep -q UNTESTED '$WORK/v1.log'"
FK=$(new_kit format_kit)
sed 's/block=131072/block=65536/' "$VK/supported_hardware.txt" >"$FK/supported_hardware.txt"
rehash_kit "$FK"
refused "validated sha but other squashfs format: refused" 'squashfs format differs' "$WORK/v2" --kit "$FK" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin"
# a full backup directory: 8 partitions and a kernel header
mkdir -p "$WORK/bk_full"
for i in 0 1 2 3 5 6; do printf 'p%s' "$i" >"$WORK/bk_full/mtdblock$i.bin"; done
cp "$WORK/bk_gz/mtdblock7.bin" "$WORK/bk_full/mtdblock7.bin"
mkkernel() { # <release>
    { printf '\047\005\031\126'; head -c 28 /dev/zero; printf 'Linux-%s' "$1"; head -c 64 /dev/zero; } >"$WORK/bk_full/mtdblock4.bin"
}
mkkernel 4.4.302-cip94
check "backup dir with matching kernel: validated" run "$WORK/v3" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_full"
check "  kernel recorded" grep -q '^stock kernel: *4.4.302-cip94' "$WORK/v3/build_report.txt"
mkkernel 5.10.0-other
refused "backup dir with another kernel: refused" 'kernel differs' "$WORK/v4" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_full"
rm "$WORK/bk_full/mtdblock6.bin"
mkkernel 4.4.302-cip94
refused "backup dir with 7 partitions (other MTD layout): refused" 'MTD layout' "$WORK/v5" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_full" --i-understand-untested
mkdir -p "$WORK/bk_small"
head -c 1000000 "$WORK/bk_gz/mtdblock7.bin" >"$WORK/bk_small/mtdblock7.bin"
refused "APP dump of another size: refused even with the flag" 'MTD layout' "$WORK/v6" --kit "$VK" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_small/mtdblock7.bin" --i-understand-untested
check "the real supported_hardware.txt lists the stock sha256 as validated" grep -q '^validated|.*|14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294|' "$REPO/tools/release/supported_hardware.txt"

echo "== 4b. review hardening"
refused "a kit DIRECTORY needs --allow-unsigned (cannot be authenticated)" 'cannot be authenticated' "$WORK/d1" --kit "$K" --allow-test-kit
BAD=$(new_kit extra_overlay)
mkdir -p "$BAD/overlay/sbin"
printf '#!/bin/sh\n' >"$BAD/overlay/sbin/extra.sh"
chmod 755 "$BAD/overlay/sbin/extra.sh"
printf 'file %064d 755 overlay/sbin/extra.sh\n' 0 >>"$BAD/MANIFEST.txt"
rehash_kit "$BAD"
refused "a kit with an overlay file this script does not know is refused" 'does not install.*overlay/sbin/extra.sh' "$WORK/d2" --kit "$BAD" --allow-test-kit --allow-unsigned
# backup dir whose mtdblock4 is not a uImage: the kernel cannot be compared, no "validated"
mkdir -p "$WORK/bk_nok"
for i in 0 1 2 3 5 6; do printf 'p%s' "$i" >"$WORK/bk_nok/mtdblock$i.bin"; done
cp "$WORK/bk_gz/mtdblock7.bin" "$WORK/bk_nok/mtdblock7.bin"
printf 'not a kernel image, long enough to have a name field xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' >"$WORK/bk_nok/mtdblock4.bin"
refused "backup dir whose mtdblock4 is not a uImage: not validated" 'kernel of the backup could not be read' "$WORK/d3" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_nok"
: >"$WORK/bk_nok/mtdblock4.bin"
refused "backup dir with an empty mtdblock4: MTD layout refused" 'mtdblock4.bin is missing or empty' "$WORK/d4" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_nok" --i-understand-untested
rm "$WORK/bk_nok/mtdblock4.bin"
printf 'x' >"$WORK/bk_nok/mtdblock8.bin"
refused "backup dir with mtdblock8 instead of mtdblock4: MTD layout refused" 'mtdblock4.bin is missing or empty' "$WORK/d5" --kit "$VK" --allow-test-kit --allow-unsigned --backup-dir "$WORK/bk_nok" --i-understand-untested
check "stock fixture xz with several BCJ filters" mkstock bcj2 -comp xz -Xbcj x86,arm
refused "  ... filters Akam cannot reproduce: refused" 'filter: x86, arm' "$WORK/d6" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_bcj2/mtdblock7.bin" --i-understand-untested
# unsquashfs -s output we cannot parse must stop the build (never "no fingerprint, default xz")
mkdir -p "$WORK/fakebin"
REAL_UNSQ=$(command -v unsquashfs)
printf '#!/bin/sh\nif [ "$1" = -s ]; then echo "SQUASHFS something else"; exit 0; fi\nexec %s "$@"\n' "$REAL_UNSQ" >"$WORK/fakebin/unsquashfs"
chmod 755 "$WORK/fakebin/unsquashfs"
if (PATH="$WORK/fakebin:$PATH" run "$WORK/d7" --kit "$K" --allow-test-kit --allow-unsigned --stock-app "$WORK/bk_gz/mtdblock7.bin" --i-understand-untested); then
    ko "unparseable unsquashfs -s output is refused (build succeeded)"
else
    if grep -q 'cannot read the compression' "$WORK/d7.log"; then ok "unparseable unsquashfs -s output is refused"; else ko "unparseable unsquashfs -s output is refused (message?)"; fi
fi
printf 'compression=gzip\nblock_size=131072\napp_sha256=zz\n' >"$WORK/stock_bad.info"
refused "stock-info with a malformed app_sha256 is refused" 'app_sha256 must be 64 hex' "$WORK/d8" --kit "$K" --allow-test-kit --allow-unsigned --stock-info "$WORK/stock_bad.info" --i-understand-untested
# APP with a read-only directory, unpacked by a normal user: builds, leaves nothing behind
cp -a "$FIXTURE" "$WORK/app_ro"
mkdir -p "$WORK/app_ro/rodir/sub"
printf 'x\n' >"$WORK/app_ro/rodir/sub/f"
chmod 555 "$WORK/app_ro/rodir/sub" "$WORK/app_ro/rodir"
mkdir -p "$WORK/bk_ro" "$WORK/tmp_ro"
for i in 0 1 2 3 4 5 6; do printf 'p%s' "$i" >"$WORK/bk_ro/mtdblock$i.bin"; done
mksquashfs "$WORK/app_ro" "$WORK/bk_ro/mtdblock7.bin" -noappend -no-progress -all-root -comp gzip >/dev/null 2>&1
truncate -s "$PART" "$WORK/bk_ro/mtdblock7.bin"
chmod -R u+rwX "$WORK/app_ro"
check "wrapper: APP with read-only directories builds" bash -c "
    TMPDIR='$WORK/tmp_ro' bash '$ASSEMBLE' --backup-dir '$WORK/bk_ro' --kit '$K' --allow-test-kit --allow-unsigned --i-understand-untested --out '$WORK/ro1' >'$WORK/ro1.log' 2>&1"
check "  ... and no temporary tree is left behind" test -z "$(find "$WORK/tmp_ro" -mindepth 1 -maxdepth 1 2>/dev/null)"
check "  ... the read-only directory is in the image" bash -c "unsquashfs -ll '$(image_of "$WORK/ro1")' | grep -q 'rodir/sub/f'"

# --- 5. user-facing wrapper -------------------------------------------------------------------------------
echo "== 5. tools/image/assemble_from_kit.sh"
mkdir -p "$WORK/bk_w"
for i in 0 1 2 3 4 5 6; do printf 'p%s' "$i" >"$WORK/bk_w/mtdblock$i.bin"; done
cp "$WORK/bk_gz/mtdblock7.bin" "$WORK/bk_w/mtdblock7.bin"
check "wrapper builds from a backup dir" bash -c "
    bash '$ASSEMBLE' --backup-dir '$WORK/bk_w' --kit '$K' --allow-test-kit --allow-unsigned --i-understand-untested --out '$WORK/w1' >'$WORK/w1.log' 2>&1"
check "  summary: image, md5, sha256, free bytes, compression, verdict" bash -c "
    grep -q '^image: ' '$WORK/w1.log' && grep -qE '^md5: +[0-9a-f]{32}\$' '$WORK/w1.log' && grep -qE '^sha256: +[0-9a-f]{64}\$' '$WORK/w1.log' &&
    grep -qE '^free bytes: +[0-9]+ bytes' '$WORK/w1.log' && grep -q '^compression: *gzip' '$WORK/w1.log' && grep -q '^hardware: *unknown' '$WORK/w1.log'"
check "  the wrapper's image is gzip like the stock" test "$(comp_of "$(image_of "$WORK/w1")")" = gzip
check "wrapper without --i-understand-untested refuses" bash -c "! bash '$ASSEMBLE' --backup-dir '$WORK/bk_w' --kit '$K' --allow-test-kit --allow-unsigned --out '$WORK/w2' >'$WORK/w2.log' 2>&1"
check "wrapper does not touch the backup" test "$(sha256sum "$WORK/bk_gz/mtdblock7.bin" | cut -d' ' -f1)" = "$GZSHA"
check "no stray private key in any image" bash -c "! grep -rlE 'PRIVATE KEY' '$WORK/x_kit' '$WORK/w1/app_patched'"

echo
echo "build_from_kit_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
