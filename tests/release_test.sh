#!/usr/bin/env bash
# Tests for tools/release/{gather_release,make_release}.sh (firmware and assistant families) on synthetic fixtures:
# the kit is built from stub binaries (as in kit_test.sh), the assistant files and the source tarball are fake. Nothing here
# touches hardware, the network or the repository tree; keys are throw-away ones.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
MAKEKIT=$REPO/tools/release/make_kit.sh
GATHER=$REPO/tools/release/gather_release.sh
MAKEREL=$REPO/tools/release/make_release.sh
SIGN=$REPO/tools/release/sign_kit.sh

for t in tar xz gzip sha256sum ssh-keygen git; do
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
quiet() { "$@" >/dev/null 2>&1; }
# fails_with <pattern> <cmd...>: the command must fail and its output must match the pattern.
fails_with() {
    local pat=$1 rc=0
    shift
    "$@" >"$WORK/last.out" 2>&1 || rc=$?
    [ "$rc" != 0 ] && grep -qE -e "$pat" "$WORK/last.out"
}

# --- a stub kit (test grade) --------------------------------------------------------------
S=$WORK/stub
mkdir -p "$S/smol/build/_deps" "$S/db"
printf '#!/bin/sh\necho stub\n' >"$S/ak_rtsp"
cp "$S/ak_rtsp" "$S/flash_tool"
cp "$S/ak_rtsp" "$S/db/dropbearmulti"
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

V=9.9.9
MODEL=lsc-indoor-1080p-ak3918
KIT=$WORK/kit
(
    export AKAM_KIT_NO_BUILD=1 AKAM_KIT_ALLOW_DIRTY=1 AKAM_KIT_VERSION=$V SOURCE_DATE_EPOCH=1700000000
    export AK_RTSP_BIN=$S/ak_rtsp FLASH_TOOL_BIN=$S/flash_tool DROPBEAR_MULTI=$S/db/dropbearmulti
    export SMOLRTSP_DIR=$S/smol MUSL_COPYRIGHT=$S/COPYRIGHT ZIG_LICENSE=$S/ZIGLICENSE AKAM_KIT_CHECK_TIME32=$S/gate_ok
    bash "$MAKEKIT" --out "$KIT" >"$WORK/kit.log" 2>&1
) || { cat "$WORK/kit.log"; echo "cannot build the stub kit"; exit 1; }

# --- fake assistant files and source tarball -------------------------------------------------
ASS=$WORK/ass
mkdir -p "$ASS"
for t in linux-amd64 linux-arm64 darwin-amd64 darwin-arm64 windows-amd64 windows-arm64; do
    case $t in
        windows-*) printf 'fake %s\n' "$t" >"$ASS/akam-$t.exe"; printf 'fake zip %s\n' "$t" >"$ASS/akam-$t.zip" ;;
        *) printf 'fake %s\n' "$t" >"$ASS/akam-$t"; printf 'fake tgz %s\n' "$t" >"$ASS/akam-$t.tar.gz" ;;
    esac
done
printf 'licence\n' >"$ASS/LICENSE"
printf 'notices\n' >"$ASS/THIRD_PARTY_NOTICES.md"
printf 'stale assistant sums\n' >"$ASS/SHA256SUMS"
# A source tarball made like make_release.sh does it, so it carries the commit id (one file only).
HEADSHA=$(git -C "$REPO" rev-parse HEAD)
git -C "$REPO" archive --format=tar --prefix="akam-firmware-$V/" HEAD tests/release_test.sh | gzip -n >"$WORK/src.tar.gz"
export AKAM_EXPECT_COMMIT=$HEADSHA
KITFILE=akam-kit-$MODEL-$V.tar.xz

OUT=$WORK/out/firmware-v$V-test
AOUT=$WORK/out/assistant-v$V-test
GF() { bash "$GATHER" firmware "$V" --kit-dir "$KIT" --source "$WORK/src.tar.gz" "$@"; }
GA() { bash "$GATHER" assistant "$V" --assistant-dir "$ASS" "$@"; }

echo "== gather firmware (test grade, fixtures)"
check "gathers a test firmware release" bash -c "bash '$GATHER' firmware $V --kit-dir '$KIT' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >'$WORK/g.out' 2>&1"
check "one SHA256SUMS, no .sig" bash -c "[ -f '$OUT/SHA256SUMS' ] && [ ! -e '$OUT/SHA256SUMS.sig' ] && [ \$(ls '$OUT' | grep -c '^SHA256SUMS') = 1 ]"
check "it lists the kit, the source, the licences and the marker (5 lines), and no assistant file" bash -c "
    cd '$OUT' && [ \$(wc -l <SHA256SUMS) = 5 ] && grep -q ' TEST_BUILD_NOT_PUBLISHABLE.txt\$' SHA256SUMS && grep -q ' $KITFILE\$' SHA256SUMS && grep -q ' akam-firmware-$V-src.tar.gz\$' SHA256SUMS &&
    grep -q ' LICENSE\$' SHA256SUMS && grep -q ' THIRD_PARTY_NOTICES.md\$' SHA256SUMS && ! grep -q ' akam-linux' SHA256SUMS"
check "it is sorted by name and in sha256sum format" bash -c "
    cd '$OUT' && sed 's/^[0-9a-f]*  //' SHA256SUMS | LC_ALL=C sort -c && grep -qE '^[0-9a-f]{64}  [^ ]+\$' SHA256SUMS && sha256sum -c --quiet SHA256SUMS"
check "test marker written, and no OUT.tmp left" bash -c "[ -s '$OUT/TEST_BUILD_NOT_PUBLISHABLE.txt' ] && [ ! -e '$OUT.tmp' ]"
check "outer SHA256SUMS lets verify_kit accept the kit" quiet bash "$REPO/tools/release/verify_kit.sh" --allow-unsigned --allow-test-kit "$OUT/$KITFILE"
check "regathering into the same directory works and is deterministic" bash -c "
    cp '$OUT/SHA256SUMS' '$WORK/sums.first' && bash '$GATHER' firmware $V --kit-dir '$KIT' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >/dev/null 2>&1 && cmp -s '$WORK/sums.first' '$OUT/SHA256SUMS'"

echo "== gather assistant (test grade, fixtures)"
check "gathers a test assistant release" bash -c "bash '$GATHER' assistant $V --assistant-dir '$ASS' --out '$AOUT' --grade test >'$WORK/ga.out' 2>&1"
check "it lists the six targets, the licences and the marker (15 lines), no kit" bash -c "
    cd '$AOUT' && [ \$(wc -l <SHA256SUMS) = 15 ] && grep -q ' akam-windows-arm64.exe\$' SHA256SUMS && grep -q ' akam-darwin-amd64.tar.gz\$' SHA256SUMS &&
    grep -q ' THIRD_PARTY_NOTICES.md\$' SHA256SUMS && ! grep -q 'akam-kit' SHA256SUMS && sed 's/^[0-9a-f]*  //' SHA256SUMS | LC_ALL=C sort -c && sha256sum -c --quiet SHA256SUMS"
check "the stale assistant SHA256SUMS was not copied in" bash -c "! grep -q 'stale' '$AOUT/SHA256SUMS'"

echo "== gather refusals"
check "the family is required" fails_with 'family must be' bash "$GATHER" "$V" --kit-dir "$KIT" --source "$WORK/src.tar.gz" --out "$WORK/o_fam"
check "release grade refuses a test-grade kit" fails_with 'verify_kit|kit_grade|refused' GF --out "$WORK/o_rel"
check "release grade refuses a version that is not X.Y.Z" fails_with 'plain X.Y.Z' bash "$GATHER" firmware "v$V" --kit-dir "$KIT" --source "$WORK/src.tar.gz" --out "$WORK/o_v"
check "wrong kit version: no tarball" fails_with 'is not for version' bash "$GATHER" firmware 1.2.3 --kit-dir "$KIT" --source "$WORK/src.tar.gz" --out "$WORK/o_k" --grade test
check "firmware refuses --assistant-dir" fails_with 'assistant family' GF --assistant-dir "$ASS" --out "$WORK/o_fa" --grade test
check "assistant refuses --kit-dir" fails_with 'firmware family' GA --kit-dir "$KIT" --out "$WORK/o_af" --grade test
mkdir -p "$WORK/kit_odd"
cp "$KIT/$KITFILE" "$WORK/kit_odd/akam-kit-other-model-$V.tar.xz"
check "a kit whose top directory differs from its file name is refused" fails_with 'top directory' \
    bash "$GATHER" firmware "$V" --kit-dir "$WORK/kit_odd" --source "$WORK/src.tar.gz" --out "$WORK/o_km" --grade test
mkdir -p "$WORK/kit_old"
cp "$KIT/$KITFILE" "$WORK/kit_old/"
: >"$WORK/kit_old/notes.txt"
check "an unexpected file in the kit directory is refused" fails_with 'unexpected file' \
    bash "$GATHER" firmware "$V" --kit-dir "$WORK/kit_old" --source "$WORK/src.tar.gz" --out "$WORK/o_ko" --grade test
cp -a "$ASS" "$WORK/ass_few"
rm "$WORK/ass_few/akam-darwin-arm64" "$WORK/ass_few/akam-darwin-arm64.tar.gz"
check "release grade needs all six targets" fails_with 'missing assistant file' \
    bash "$GATHER" assistant "$V" --assistant-dir "$WORK/ass_few" --out "$WORK/o_f"
for bad in app.squashfs mtd7.bin firmware.img mtdblock3 dump.dump rootfs.sqfs; do
    cp -a "$ASS" "$WORK/ass_bad"
    : >"$WORK/ass_bad/$bad"
    check "assistant dir with $bad is refused" fails_with 'forbidden' \
        bash "$GATHER" assistant "$V" --assistant-dir "$WORK/ass_bad" --out "$WORK/o_b" --grade test
    rm -rf "$WORK/ass_bad"
done
cp -a "$ASS" "$WORK/ass_x"
: >"$WORK/ass_x/notes.txt"
check "an unexpected assistant file is refused" fails_with 'unexpected file' \
    bash "$GATHER" assistant "$V" --assistant-dir "$WORK/ass_x" --out "$WORK/o_x" --grade test
mkdir -p "$WORK/srcfx/akam-firmware-$V/pkg/testdata"
: >"$WORK/srcfx/akam-firmware-$V/pkg/testdata/tiny.sqfs"
tar -C "$WORK/srcfx" -czf "$WORK/src_fx.tar.gz" "akam-firmware-$V"
check "a tiny test fixture under testdata/ in the source is accepted" quiet bash "$GATHER" firmware "$V" --kit-dir "$KIT" --source "$WORK/src_fx.tar.gz" --out "$WORK/o_fx" --grade test
mkdir -p "$WORK/srcbad/akam-firmware-$V"
: >"$WORK/srcbad/akam-firmware-$V/app_dump.squashfs"
tar -C "$WORK/srcbad" -czf "$WORK/src_bad.tar.gz" "akam-firmware-$V"
check "a source tarball holding an image is refused" fails_with 'forbidden entry' \
    bash "$GATHER" firmware "$V" --kit-dir "$KIT" --source "$WORK/src_bad.tar.gz" --out "$WORK/o_s" --grade test
mkdir -p "$WORK/precious"
printf 'keep\n' >"$WORK/precious/file"
check "an unrelated non-empty output directory is not cleared" fails_with 'not empty' GF --out "$WORK/precious" --grade test
check "  ... and is left alone" test -f "$WORK/precious/file"

echo "== check-signed"
ssh-keygen -q -t ed25519 -N '' -C test -f "$WORK/key" >/dev/null
printf 'akam-release namespaces="akam-release" %s\n' "$(cut -d' ' -f1,2 "$WORK/key.pub")" >"$WORK/allowed"
printf '# placeholder only\n' >"$WORK/allowed_empty"
BLOB=$(cut -d' ' -f2 "$WORK/key.pub")
check "a test build is refused by --check-signed" fails_with 'TEST build' bash "$GATHER" --check-signed firmware "$OUT"
check "sign_kit refuses a test build directory" fails_with 'TEST build' bash "$SIGN" "$WORK/key" "$AOUT"
CS() { env AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed" "$@"; }
resum() { (cd "$1" && rm -f SHA256SUMS SHA256SUMS.sig && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS); }

# an assistant "release" directory: the test one without its marker, binaries that embed the key, sums regenerated
AREL=$WORK/rel/assistant-v$V
mkdir -p "$WORK/rel"
cp -a "$AOUT" "$AREL"
rm "$AREL/TEST_BUILD_NOT_PUBLISHABLE.txt"
for f in "$AREL"/akam-linux-amd64 "$AREL"/akam-linux-arm64 "$AREL"/akam-darwin-amd64 "$AREL"/akam-darwin-arm64 "$AREL"/akam-windows-amd64.exe "$AREL"/akam-windows-arm64.exe; do
    printf '%s\n' "$BLOB" >>"$f"
done
resum "$AREL"
check "sign_kit signs the SHA256SUMS" quiet env AKAM_ALLOWED_SIGNERS="$WORK/allowed" bash "$SIGN" "$WORK/key" "$AREL"
check "the family must be firmware or assistant" fails_with 'family must be' CS bash "$GATHER" --check-signed kit "$AREL"
check "the directory must be named after the family" fails_with 'named firmware-vX.Y.Z' CS bash "$GATHER" --check-signed firmware "$AREL"
check "placeholder allowed_signers is refused" fails_with 'no ssh-ed25519 key line' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed_empty" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed assistant "$AREL"
check "embedded copy that differs is refused" fails_with 'embedded copy' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed assistant "$AREL"
check "signature by another key is refused" bash -c "
    ssh-keygen -q -t ed25519 -N '' -C other -f '$WORK/key2' >/dev/null &&
    printf 'akam-release namespaces=\"akam-release\" %s\n' \"\$(cut -d' ' -f1,2 '$WORK/key2.pub')\" >'$WORK/allowed2' &&
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed2' AKAM_EMBEDDED_SIGNERS='$WORK/allowed2' bash '$GATHER' --check-signed assistant '$AREL' >'$WORK/c.out' 2>&1 &&
    grep -q 'does not verify' '$WORK/c.out'"
check "six binaries with the key pass the key check, then the run stops at the missing notes" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed assistant '$AREL' >'$WORK/c.out' 2>&1 &&
    grep -q 'six binaries embed the release key' '$WORK/c.out' && grep -q 'notes.md not found' '$WORK/c.out'"
printf 'Release <key> and <changelog>\n' >"$WORK/rel/assistant-v$V-notes.md"
check "a notes file with placeholders is refused" fails_with 'placeholder' CS bash "$GATHER" --check-signed assistant "$AREL"
printf 'Release key %s\n' "$BLOB" >"$WORK/rel/assistant-v$V-notes.md"
check "filled notes: the assistant release is signed and consistent" bash -c "
    AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed assistant '$AREL' >'$WORK/c.out' 2>&1 && grep -q 'is signed and consistent' '$WORK/c.out'"
mkdir -p "$WORK/relnk"
cp -a "$AREL" "$WORK/relnk/assistant-v$V"
printf 'fake no key\n' >"$WORK/relnk/assistant-v$V/akam-darwin-arm64"
resum "$WORK/relnk/assistant-v$V"
ssh-keygen -Y sign -f "$WORK/key" -n akam-release "$WORK/relnk/assistant-v$V/SHA256SUMS" >/dev/null 2>&1
printf 'Release key %s\n' "$BLOB" >"$WORK/relnk/assistant-v$V-notes.md"
check "a binary built before the key was committed is refused" fails_with 'akam-darwin-arm64 does not embed' CS bash "$GATHER" --check-signed assistant "$WORK/relnk/assistant-v$V"
check "a file changed after signing fails the check" bash -c "
    cp -a '$AREL' '$WORK/rel3' && printf x >>'$WORK/rel3/LICENSE' &&
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed assistant '$WORK/rel3' >'$WORK/c.out' 2>&1"

# a firmware "release" directory (the stub kit is test grade: AKAM_CHECK_ALLOW_TEST_KIT only for these tests)
FREL=$WORK/rel/firmware-v$V
cp -a "$OUT" "$FREL"
rm "$FREL/TEST_BUILD_NOT_PUBLISHABLE.txt"
resum "$FREL"
ssh-keygen -Y sign -f "$WORK/key" -n akam-release "$FREL/SHA256SUMS" >/dev/null 2>&1
check "firmware: without notes the run stops at the missing notes (commit checks passed)" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed firmware '$FREL' >'$WORK/c.out' 2>&1 &&
    grep -q 'come from commit' '$WORK/c.out' && grep -q 'notes.md not found' '$WORK/c.out'"
KSHA=$(awk -v f="$KITFILE" '$2 == f {print $1}' "$FREL/SHA256SUMS")
printf 'Release key %s, no kit sha\n' "$BLOB" >"$WORK/rel/firmware-v$V-notes.md"
check "firmware: notes must quote each kit sha256" fails_with 'does not quote the sha256' CS bash "$GATHER" --check-signed firmware "$FREL"
printf 'Release key %s, kit %s\n' "$BLOB" "$KSHA" >"$WORK/rel/firmware-v$V-notes.md"
check "firmware: a test-grade kit stops the run at verify_kit" fails_with 'verify_kit.sh failed' CS bash "$GATHER" --check-signed firmware "$FREL"
check "firmware: with the test-kit seam the release is signed and consistent" bash -c "
    AKAM_CHECK_ALLOW_TEST_KIT=1 AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed firmware '$FREL' >'$WORK/c.out' 2>&1 && grep -q 'is signed and consistent' '$WORK/c.out'"
check "firmware: a wrong expected commit is refused" fails_with 'was built from commit' \
    env AKAM_EXPECT_COMMIT=0000000000000000000000000000000000000000 AKAM_CHECK_ALLOW_TEST_KIT=1 AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed" bash "$GATHER" --check-signed firmware "$FREL"

echo "== gather: replacing an existing directory"
mkdir -p "$WORK/rep/sub"
check "a directory with a subdirectory is not replaced" fails_with 'subdirectory' GF --out "$WORK/rep" --grade test
rmdir "$WORK/rep/sub"
: >"$WORK/rep/SHA256SUMS.sig"
check "a directory holding a signature is not replaced" fails_with 'holds a signature' GF --out "$WORK/rep" --grade test
rm "$WORK/rep/SHA256SUMS.sig"
: >"$WORK/rep/akam-kit-old-model-0.0.1.tar.xz"
check "a directory of known release names is replaced" quiet bash "$GATHER" firmware "$V" --kit-dir "$KIT" --source "$WORK/src.tar.gz" --out "$WORK/rep" --grade test
printf 'not a tarball\n' >"$WORK/notatar.tar.gz"
check "an unreadable source tarball is refused" fails_with 'cannot list' \
    bash "$GATHER" firmware "$V" --kit-dir "$KIT" --source "$WORK/notatar.tar.gz" --out "$WORK/o_n" --grade test

echo "== make_release refusals (throw-away repository)"
unset ZIG AKAM_ALLOW_ANY_GO AKAM_STRIP AKAM_TARGETS AKAM_KIT_NO_BUILD AKAM_KIT_ALLOW_DIRTY AKAM_ALLOW_DIRTY AKAM_KIT_CHECK_TIME32 AK_RTSP_BIN FLASH_TOOL_BIN DROPBEAR_MULTI
R=$WORK/repo
mkdir -p "$R/tools/release/models" "$R/tools/assistant/internal/kit"
cp "$MAKEREL" "$GATHER" "$REPO/tools/release/verify_kit.sh" "$R/tools/release/"
mkdir -p "$R/tools/assistant/cmd/akam" "$R/docs"
printf "# Akam firmware <version>\n<kits>\n<changelog>\n" >"$R/tools/release/release_notes_firmware.md"
printf "# Akam assistant <version>\n<changelog>\n" >"$R/tools/release/release_notes_assistant.md"
printf '1.0.0\n' >"$R/tools/release/firmware_version"
printf 'supported_hardware_format=1\nmodel=test-model\nname=Test\nvalidated|x|-|-|-|-|-|-\n' >"$R/tools/release/models/test-model.txt"
printf '# Changelog\n\n## [firmware 1.0.0] - unreleased\n\n- first\n\n## [assistant 1.0.0] - unreleased\n\n- first\n' >"$R/CHANGELOG.md"
# shellcheck disable=SC2016  # literal text of the fixture script
printf '#!/bin/sh\nAKAM_WIZARD_VERSION=${AKAM_WIZARD_VERSION:-1.0.0}\n' >"$R/akam"
printf 'package main\n\nvar version = "1.0.0"\n' >"$R/tools/assistant/cmd/akam/main.go"
printf '# placeholder\n' >"$R/tools/release/allowed_signers"
cp "$R/tools/release/allowed_signers" "$R/tools/assistant/internal/kit/allowed_signers"
git -C "$R" init -q
git -C "$R" add -A
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -m init
M=$R/tools/release/make_release.sh
GC() { git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q "$@"; }
check "no family or version: usage" fails_with 'usage' bash "$M"
check "a bare version is refused (no single-tag mode any more)" fails_with 'usage' bash "$M" 1.0.0
check "v1.0.0 is not plain semver" fails_with 'plain semver' bash "$M" firmware v1.0.0
check "1.0 is not plain semver" fails_with 'plain semver' bash "$M" assistant 1.0
check "--test with --check-signed is refused" fails_with 'exclude each other' bash "$M" --test --check-signed firmware 1.0.0
check "a placeholder allowed_signers stops a release build (firmware)" fails_with 'no ssh-ed25519 key line' bash "$M" firmware 1.0.0 --out "$WORK/r1"
check "a placeholder allowed_signers stops a release build (assistant)" fails_with 'no ssh-ed25519 key line' bash "$M" assistant 1.0.0 --out "$WORK/r1"
check "a firmware version file that is not X.Y.Z stops a firmware build" fails_with "firmware_version is '1.0.0', not 1.0.1" bash "$M" firmware 1.0.1 --out "$WORK/r1a"
check "an assistant version that is not X.Y.Z stops an assistant build" fails_with "main.go is '1.0.0', not 1.0.1" bash "$M" assistant 1.0.1 --out "$WORK/r1a2"
check "build overrides are refused in release mode" fails_with 'AKAM_ALLOW_ANY_GO is set' env AKAM_ALLOW_ANY_GO=1 bash "$M" firmware 1.0.0 --out "$WORK/r1b"
check "a Zig other than the pinned one is refused in release mode" fails_with 'other than the pinned one' env ZIG=/nonexistent/zig bash "$M" firmware 1.0.0 --out "$WORK/r1e"
git -C "$R" tag firmware-v1.0.0
check "an existing firmware tag stops a firmware build" fails_with 'tag firmware-v1.0.0 already exists' bash "$M" firmware 1.0.0 --out "$WORK/r1c"
check "  ... but the assistant family has its own tags" fails_with 'no ssh-ed25519 key line' bash "$M" assistant 1.0.0 --out "$WORK/r1c2"
git -C "$R" tag -d firmware-v1.0.0 >/dev/null
git -C "$R" tag assistant-v1.0.0
check "an existing assistant tag stops an assistant build" fails_with 'tag assistant-v1.0.0 already exists' bash "$M" assistant 1.0.0 --out "$WORK/r1c3"
git -C "$R" tag -d assistant-v1.0.0 >/dev/null
mkdir -p "$WORK/r1d"
: >"$WORK/r1d/SHA256SUMS.sig"
check "an output directory holding a signature stops a release build" fails_with 'holds a signature' bash "$M" firmware 1.0.0 --out "$WORK/r1d"
printf 'x\n' >"$R/untracked"
check "a dirty tree stops a release build" fails_with 'not clean' bash "$M" firmware 1.0.0 --out "$WORK/r2"
rm "$R/untracked"
printf 'akam-release namespaces="akam-release" ssh-ed25519 AAAAfake\n' >>"$R/tools/release/allowed_signers"
GC -a -m key
check "check-signed wants the dist/release/<family>-vX.Y.Z directory name" fails_with 'dist/release/firmware-vX.Y.Z' bash "$M" --check-signed firmware 1.0.0 --out "$WORK/elsewhere"
check "an embedded copy that differs stops a release build" fails_with 'differs from tools/release/allowed_signers' bash "$M" firmware 1.0.0 --out "$WORK/r3"
cp "$R/tools/release/allowed_signers" "$R/tools/assistant/internal/kit/allowed_signers"
GC -a -m embed
printf '# Changelog\n' >"$R/CHANGELOG.md"
GC -a -m nochangelog
check "a CHANGELOG without the section of the release stops a release build" fails_with "no section '## \[firmware 1.0.0\]'" bash "$M" firmware 1.0.0 --out "$WORK/r3b"
git -C "$R" reset -q --hard HEAD~1
check "with the key in both files the key check passes (firmware: the run then fails in make_kit.sh: no repository files)" bash -c "
    ! bash '$M' firmware 1.0.0 --out '$WORK/r4' >'$WORK/m.out' 2>&1 && ! grep -qE 'no ssh-ed25519|differs from|not clean|already exists|set the version|no section' '$WORK/m.out' && grep -q 'make_kit' '$WORK/m.out'"
check "nothing was written for the refused builds" bash -c "[ ! -e '$WORK/r1' ] && [ ! -e '$WORK/r2' ] && [ ! -e '$WORK/r3' ]"

echo
echo "release_test: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
