#!/usr/bin/env bash
# Tests for tools/release/{gather_release,make_release}.sh on synthetic fixtures: the kit is built from
# stub binaries (as in kit_test.sh), the assistant files and the source tarball are fake. Nothing here
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
git -C "$REPO" archive --format=tar --prefix="akam-$V/" HEAD tests/release_test.sh | gzip -n >"$WORK/src.tar.gz"
export AKAM_EXPECT_COMMIT=$HEADSHA

OUT=$WORK/out

echo "== gather (test grade, fixtures)"
check "gathers a test release" bash -c "bash '$GATHER' $V --kit-dir '$KIT' --assistant-dir '$ASS' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >'$WORK/g.out' 2>&1"
check "one combined SHA256SUMS, no .sig" bash -c "[ -f '$OUT/SHA256SUMS' ] && [ ! -e '$OUT/SHA256SUMS.sig' ] && [ \$(ls '$OUT' | grep -c '^SHA256SUMS') = 1 ]"
check "it lists kit, source, licence and every assistant file (17 lines with the marker)" bash -c "
    cd '$OUT' && [ \$(wc -l <SHA256SUMS) = 17 ] && grep -q ' TEST_BUILD_NOT_PUBLISHABLE.txt\$' SHA256SUMS && grep -q ' akam-kit-$V.tar.xz\$' SHA256SUMS && grep -q ' akam-$V-src.tar.gz\$' SHA256SUMS &&
    grep -q ' akam-windows-arm64.exe\$' SHA256SUMS && grep -q ' akam-darwin-amd64.tar.gz\$' SHA256SUMS && grep -q ' THIRD_PARTY_NOTICES.md\$' SHA256SUMS"
check "it is sorted by name and in sha256sum format" bash -c "
    cd '$OUT' && sed 's/^[0-9a-f]*  //' SHA256SUMS | LC_ALL=C sort -c && grep -qE '^[0-9a-f]{64}  [^ ]+\$' SHA256SUMS && sha256sum -c --quiet SHA256SUMS"
check "the stale assistant SHA256SUMS was not copied in" bash -c "! grep -q 'stale' '$OUT/SHA256SUMS'"
check "test marker written, and no OUT.tmp left" bash -c "[ -s '$OUT/TEST_BUILD_NOT_PUBLISHABLE.txt' ] && [ ! -e '$OUT.tmp' ]"
check "outer SHA256SUMS lets verify_kit accept the kit" quiet bash "$REPO/tools/release/verify_kit.sh" --allow-unsigned --allow-test-kit "$OUT/akam-kit-$V.tar.xz"
check "regathering into the same directory works" bash -c "bash '$GATHER' $V --kit-dir '$KIT' --assistant-dir '$ASS' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >/dev/null 2>&1"
cp "$OUT/SHA256SUMS" "$WORK/sums.first"
check "gathering is deterministic (same sums)" bash -c "bash '$GATHER' $V --kit-dir '$KIT' --assistant-dir '$ASS' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >/dev/null 2>&1 && cmp -s '$WORK/sums.first' '$OUT/SHA256SUMS'"

echo "== gather refusals"
check "release grade refuses a test-grade kit" fails_with 'verify_kit|kit_grade|refused' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/o_rel"
check "release grade refuses a version that is not X.Y.Z" fails_with 'plain X.Y.Z' \
    bash "$GATHER" "v$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/o_v"
check "wrong kit version: no tarball" fails_with 'not found' \
    bash "$GATHER" 1.2.3 --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/o_k" --grade test
cp -a "$ASS" "$WORK/ass_few"
rm "$WORK/ass_few/akam-darwin-arm64" "$WORK/ass_few/akam-darwin-arm64.tar.gz"
check "release grade needs all six targets" fails_with 'missing assistant file' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$WORK/ass_few" --source "$WORK/src.tar.gz" --out "$WORK/o_f"
for bad in app.squashfs mtd7.bin firmware.img mtdblock3 dump.dump; do
    cp -a "$ASS" "$WORK/ass_bad"
    : >"$WORK/ass_bad/$bad"
    check "assistant dir with $bad is refused" fails_with 'forbidden' \
        bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$WORK/ass_bad" --source "$WORK/src.tar.gz" --out "$WORK/o_b" --grade test
    rm -rf "$WORK/ass_bad"
done
cp -a "$ASS" "$WORK/ass_x"
: >"$WORK/ass_x/notes.txt"
check "an unexpected assistant file is refused" fails_with 'unexpected file' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$WORK/ass_x" --source "$WORK/src.tar.gz" --out "$WORK/o_x" --grade test
mkdir -p "$WORK/srcfx/akam-$V/pkg/testdata"
: >"$WORK/srcfx/akam-$V/pkg/testdata/tiny.sqfs"
tar -C "$WORK/srcfx" -czf "$WORK/src_fx.tar.gz" "akam-$V"
check "a tiny test fixture under testdata/ in the source is accepted" quiet bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src_fx.tar.gz" --out "$WORK/o_fx" --grade test
mkdir -p "$WORK/srcbad/akam-$V"
: >"$WORK/srcbad/akam-$V/app_dump.squashfs"
tar -C "$WORK/srcbad" -czf "$WORK/src_bad.tar.gz" "akam-$V"
check "a source tarball holding an image is refused" fails_with 'forbidden entry' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src_bad.tar.gz" --out "$WORK/o_s" --grade test
mkdir -p "$WORK/precious"
printf 'keep\n' >"$WORK/precious/file"
check "an unrelated non-empty output directory is not cleared" fails_with 'not empty' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/precious" --grade test
check "  ... and is left alone" test -f "$WORK/precious/file"

echo "== check-signed"
ssh-keygen -q -t ed25519 -N '' -C test -f "$WORK/key" >/dev/null
printf 'akam-release namespaces="akam-release" %s\n' "$(cut -d' ' -f1,2 "$WORK/key.pub")" >"$WORK/allowed"
printf '# placeholder only\n' >"$WORK/allowed_empty"
BLOB=$(cut -d' ' -f2 "$WORK/key.pub")
check "a test build is refused by --check-signed" fails_with 'TEST build' bash "$GATHER" --check-signed "$OUT"
check "sign_kit refuses a test build directory" fails_with 'TEST build' bash "$SIGN" "$WORK/key" "$OUT"
# A "release" directory: the test one without its marker, binaries that embed the key, sums regenerated.
REL=$WORK/rel/v$V
mkdir -p "$WORK/rel"
cp -a "$OUT" "$REL"
rm "$REL/TEST_BUILD_NOT_PUBLISHABLE.txt" "$REL/SHA256SUMS"
for f in "$REL"/akam-linux-amd64 "$REL"/akam-linux-arm64 "$REL"/akam-darwin-amd64 "$REL"/akam-darwin-arm64 "$REL"/akam-windows-amd64.exe "$REL"/akam-windows-arm64.exe; do
    printf '%s\n' "$BLOB" >>"$f"
done
(cd "$REL" && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS)
check "sign_kit signs the combined SHA256SUMS" quiet env AKAM_ALLOWED_SIGNERS="$WORK/allowed" bash "$SIGN" "$WORK/key" "$REL"
CS() { env AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed" "$@"; }
check "placeholder allowed_signers is refused" fails_with 'no ssh-ed25519 key line' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed_empty" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed "$REL"
check "embedded copy that differs is refused" fails_with 'embedded copy' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed "$REL"
check "signature by another key is refused" bash -c "
    ssh-keygen -q -t ed25519 -N '' -C other -f '$WORK/key2' >/dev/null &&
    printf 'akam-release namespaces=\"akam-release\" %s\n' \"\$(cut -d' ' -f1,2 '$WORK/key2.pub')\" >'$WORK/allowed2' &&
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed2' AKAM_EMBEDDED_SIGNERS='$WORK/allowed2' bash '$GATHER' --check-signed '$REL' >'$WORK/c.out' 2>&1 &&
    grep -q 'does not verify' '$WORK/c.out'"
# Everything is right except the notes file and the stub kit being test grade: the run must pass the key check
# and every check before the notes, and stop at the missing notes.
check "six binaries with the key pass the key, commit and source checks (the glob bug)" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed '$REL' >'$WORK/c.out' 2>&1 &&
    grep -q 'six binaries embed the release key' '$WORK/c.out' && grep -q 'come from commit' '$WORK/c.out' && grep -q 'notes.md not found' '$WORK/c.out'"
KSHA=$(awk '$2 == "akam-kit-'$V'.tar.xz" {print $1}' "$REL/SHA256SUMS")
printf 'Kit <sha256> and <key>\n' >"$WORK/rel/v$V-notes.md"
check "a notes file with placeholders is refused" fails_with 'placeholder' CS bash "$GATHER" --check-signed "$REL"
printf 'Kit %s key %s\n' "$KSHA" "$BLOB" >"$WORK/rel/v$V-notes.md"
check "filled notes pass; the run then stops at verify_kit (the stub kit is test grade)" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed '$REL' >'$WORK/c.out' 2>&1 &&
    grep -q 'is filled' '$WORK/c.out' && grep -q 'verify_kit.sh failed' '$WORK/c.out'"
check "a wrong expected commit is refused" fails_with 'was built from commit' \
    env AKAM_EXPECT_COMMIT=0000000000000000000000000000000000000000 AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed" bash "$GATHER" --check-signed "$REL"
mkdir -p "$WORK/relnk"
cp -a "$REL" "$WORK/relnk/v$V"
printf 'fake no key\n' >"$WORK/relnk/v$V/akam-darwin-arm64"
(cd "$WORK/relnk/v$V" && rm SHA256SUMS SHA256SUMS.sig && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS)
ssh-keygen -Y sign -f "$WORK/key" -n akam-release "$WORK/relnk/v$V/SHA256SUMS" >/dev/null 2>&1
check "a binary built before the key was committed is refused" fails_with 'akam-darwin-arm64 does not embed' CS bash "$GATHER" --check-signed "$WORK/relnk/v$V"
check "a file changed after signing fails the check" bash -c "
    cp -a '$REL' '$WORK/rel3' && printf x >>'$WORK/rel3/LICENSE' &&
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed '$WORK/rel3' >'$WORK/c.out' 2>&1 &&
    grep -q 'SHA256SUMS check failed' '$WORK/c.out'"

echo "== gather: replacing an existing directory"
mkdir -p "$WORK/rep/sub"
check "a directory with a subdirectory is not replaced" fails_with 'subdirectory' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/rep" --grade test
rmdir "$WORK/rep/sub"
: >"$WORK/rep/SHA256SUMS.sig"
check "a directory holding a signature is not replaced" fails_with 'holds a signature' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/rep" --grade test
rm "$WORK/rep/SHA256SUMS.sig"
: >"$WORK/rep/akam-linux-amd64"
check "a directory of known release names is replaced" quiet bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/src.tar.gz" --out "$WORK/rep" --grade test
printf 'not a tarball\n' >"$WORK/notatar.tar.gz"
check "an unreadable source tarball is refused" fails_with 'cannot list' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$ASS" --source "$WORK/notatar.tar.gz" --out "$WORK/o_n" --grade test
cp -a "$ASS" "$WORK/ass_sq"
: >"$WORK/ass_sq/rootfs.sqfs"
check "assistant dir with a .sqfs is refused" fails_with 'forbidden' \
    bash "$GATHER" "$V" --kit-dir "$KIT" --assistant-dir "$WORK/ass_sq" --source "$WORK/src.tar.gz" --out "$WORK/o_q" --grade test

echo "== make_release refusals (throw-away repository)"
unset ZIG AKAM_ALLOW_ANY_GO AKAM_STRIP AKAM_TARGETS AKAM_KIT_NO_BUILD AKAM_KIT_ALLOW_DIRTY AKAM_ALLOW_DIRTY AKAM_KIT_CHECK_TIME32 AK_RTSP_BIN FLASH_TOOL_BIN DROPBEAR_MULTI
R=$WORK/repo
mkdir -p "$R/tools/release" "$R/tools/assistant/internal/kit"
cp "$MAKEREL" "$GATHER" "$REPO/tools/release/verify_kit.sh" "$R/tools/release/"
mkdir -p "$R/tools/assistant/cmd/akam" "$R/docs"
printf "# Akam <version>\n" >"$R/docs/release_notes_draft.md"
# shellcheck disable=SC2016  # literal text of the fixture script
printf '#!/bin/sh\nAKAM_WIZARD_VERSION=${AKAM_WIZARD_VERSION:-1.0.0}\n' >"$R/akam"
printf 'package main\n\nvar version = "1.0.0"\n' >"$R/tools/assistant/cmd/akam/main.go"
printf '# placeholder\n' >"$R/tools/release/allowed_signers"
cp "$R/tools/release/allowed_signers" "$R/tools/assistant/internal/kit/allowed_signers"
git -C "$R" init -q
git -C "$R" add -A
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -m init
M=$R/tools/release/make_release.sh
check "no version: usage" fails_with 'usage' bash "$M"
check "v1.0.0 is not plain semver" fails_with 'plain semver' bash "$M" v1.0.0
check "1.0 is not plain semver" fails_with 'plain semver' bash "$M" 1.0
check "--test with --check-signed is refused" fails_with 'exclude each other' bash "$M" --test --check-signed 1.0.0
check "a placeholder allowed_signers stops a release build" fails_with 'no ssh-ed25519 key line' bash "$M" 1.0.0 --out "$WORK/r1"
check "a wizard default version that is not X.Y.Z stops a release build" fails_with 'set the version in the release commit' bash "$M" 1.0.1 --out "$WORK/r1a"
check "build overrides are refused in release mode" fails_with 'AKAM_ALLOW_ANY_GO is set' env AKAM_ALLOW_ANY_GO=1 bash "$M" 1.0.0 --out "$WORK/r1b"
check "a Zig other than the pinned one is refused in release mode" fails_with 'other than the pinned one' env ZIG=/nonexistent/zig bash "$M" 1.0.0 --out "$WORK/r1e"
git -C "$R" tag v1.0.0
check "an existing tag stops a release build" fails_with 'tag v1.0.0 already exists' bash "$M" 1.0.0 --out "$WORK/r1c"
git -C "$R" tag -d v1.0.0 >/dev/null
mkdir -p "$WORK/r1d"
: >"$WORK/r1d/SHA256SUMS.sig"
check "an output directory holding a signature stops a release build" fails_with 'holds a signature' bash "$M" 1.0.0 --out "$WORK/r1d"
printf 'x\n' >"$R/untracked"
check "a dirty tree stops a release build" fails_with 'not clean' bash "$M" 1.0.0 --out "$WORK/r2"
rm "$R/untracked"
printf 'akam-release namespaces="akam-release" ssh-ed25519 AAAAfake\n' >>"$R/tools/release/allowed_signers"
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -a -m key
check "check-signed wants the dist/release/vX.Y.Z directory name" fails_with 'dist/release/vX.Y.Z' bash "$M" --check-signed 1.0.0 --out "$WORK/elsewhere"
check "an embedded copy that differs stops a release build" fails_with 'differs from tools/release/allowed_signers' bash "$M" 1.0.0 --out "$WORK/r3"
cp "$R/tools/release/allowed_signers" "$R/tools/assistant/internal/kit/allowed_signers"
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -a -m embed
check "with the key in both files the key check passes (the run then fails in make_kit.sh: no repository files)" bash -c "
    ! bash '$M' 1.0.0 --out '$WORK/r4' >'$WORK/m.out' 2>&1 && ! grep -qE 'no ssh-ed25519|differs from|not clean|already exists|set the version' '$WORK/m.out' && grep -q 'make_kit' '$WORK/m.out'"
check "nothing was written for the refused builds" bash -c "[ ! -e '$WORK/r1' ] && [ ! -e '$WORK/r2' ] && [ ! -e '$WORK/r3' ]"

echo
echo "release_test: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
