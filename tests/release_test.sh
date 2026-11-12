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
SRCDIR=$WORK/srcsrc/akam-$V
mkdir -p "$SRCDIR"
printf 'readme\n' >"$SRCDIR/README.md"
tar -C "$WORK/srcsrc" -czf "$WORK/src.tar.gz" "akam-$V"

OUT=$WORK/out

echo "== gather (test grade, fixtures)"
check "gathers a test release" bash -c "bash '$GATHER' $V --kit-dir '$KIT' --assistant-dir '$ASS' --source '$WORK/src.tar.gz' --out '$OUT' --grade test >'$WORK/g.out' 2>&1"
check "one combined SHA256SUMS, no .sig" bash -c "[ -f '$OUT/SHA256SUMS' ] && [ ! -e '$OUT/SHA256SUMS.sig' ] && [ \$(ls '$OUT' | grep -c '^SHA256SUMS') = 1 ]"
check "it lists kit, source, licence and every assistant file (16 lines)" bash -c "
    cd '$OUT' && [ \$(wc -l <SHA256SUMS) = 16 ] && grep -q ' akam-kit-$V.tar.xz\$' SHA256SUMS && grep -q ' akam-$V-src.tar.gz\$' SHA256SUMS &&
    grep -q ' akam-windows-arm64.exe\$' SHA256SUMS && grep -q ' akam-darwin-amd64.tar.gz\$' SHA256SUMS && grep -q ' THIRD_PARTY_NOTICES.md\$' SHA256SUMS"
check "it is sorted by name and in sha256sum format" bash -c "
    cd '$OUT' && sed 's/^[0-9a-f]*  //' SHA256SUMS | LC_ALL=C sort -c && grep -qE '^[0-9a-f]{64}  [^ ]+\$' SHA256SUMS && sha256sum -c --quiet SHA256SUMS"
check "the stale assistant SHA256SUMS was not copied in" bash -c "! grep -q 'stale' '$OUT/SHA256SUMS'"
check "test marker written" test -s "$OUT/TEST_BUILD_NOT_PUBLISHABLE.txt"
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
check "a test build is refused by --check-signed" fails_with 'TEST build' bash "$GATHER" --check-signed "$OUT"
check "sign_kit signs the combined SHA256SUMS" quiet bash "$SIGN" "$WORK/key" "$OUT"
check "still refused after signing (test marker)" fails_with 'TEST build' bash "$GATHER" --check-signed "$OUT"
rm "$OUT/TEST_BUILD_NOT_PUBLISHABLE.txt"
check "placeholder allowed_signers is refused" fails_with 'no key line' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed_empty" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed "$OUT"
check "embedded copy that differs is refused" fails_with 'embedded copy' \
    env AKAM_ALLOWED_SIGNERS="$WORK/allowed" AKAM_EMBEDDED_SIGNERS="$WORK/allowed_empty" bash "$GATHER" --check-signed "$OUT"
check "signature by another key is refused" bash -c "
    ssh-keygen -q -t ed25519 -N '' -C other -f '$WORK/key2' >/dev/null &&
    printf 'akam-release namespaces=\"akam-release\" %s\n' \"\$(cut -d' ' -f1,2 '$WORK/key2.pub')\" >'$WORK/allowed2' &&
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed2' AKAM_EMBEDDED_SIGNERS='$WORK/allowed2' bash '$GATHER' --check-signed '$OUT' >'$WORK/c.out' 2>&1 &&
    grep -q 'does not verify' '$WORK/c.out'"
# Signature and checksums are fine here; the binaries are fakes without the key, so the run must stop there.
check "signature and sums pass, binaries without the key are refused" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed '$OUT' >'$WORK/c.out' 2>&1 &&
    grep -q 'signature verifies' '$WORK/c.out' && grep -q 'every SHA256SUMS line matches' '$WORK/c.out' && grep -q 'does not embed the release key' '$WORK/c.out'"
BLOB=$(cut -d' ' -f2 "$WORK/key.pub")
for f in "$OUT"/akam-linux-amd64 "$OUT"/akam-linux-arm64 "$OUT"/akam-darwin-amd64 "$OUT"/akam-darwin-arm64 "$OUT"/akam-windows-amd64.exe "$OUT"/akam-windows-arm64.exe; do
    printf '%s\n' "$BLOB" >>"$f"
done
check "a file changed after signing fails the check" bash -c "
    ! AKAM_ALLOWED_SIGNERS='$WORK/allowed' AKAM_EMBEDDED_SIGNERS='$WORK/allowed' bash '$GATHER' --check-signed '$OUT' >'$WORK/c.out' 2>&1 &&
    grep -q 'SHA256SUMS check failed' '$WORK/c.out'"

echo "== make_release refusals (throw-away repository)"
R=$WORK/repo
mkdir -p "$R/tools/release" "$R/tools/assistant/internal/kit"
cp "$MAKEREL" "$GATHER" "$REPO/tools/release/verify_kit.sh" "$R/tools/release/"
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
check "a placeholder allowed_signers stops a release build" fails_with 'no key line' bash "$M" 1.0.0 --out "$WORK/r1"
printf 'x\n' >"$R/untracked"
check "a dirty tree stops a release build" fails_with 'not clean' bash "$M" 1.0.0 --out "$WORK/r2"
rm "$R/untracked"
printf 'akam-release namespaces="akam-release" ssh-ed25519 AAAAfake\n' >>"$R/tools/release/allowed_signers"
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -a -m key
check "an embedded copy that differs stops a release build" fails_with 'differs from tools/release/allowed_signers' bash "$M" 1.0.0 --out "$WORK/r3"
cp "$R/tools/release/allowed_signers" "$R/tools/assistant/internal/kit/allowed_signers"
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -a -m embed
check "with the key in both files the key check passes (the run then fails in make_kit.sh: no repository files)" bash -c "
    ! bash '$M' 1.0.0 --out '$WORK/r4' >'$WORK/m.out' 2>&1 && ! grep -qE 'no key line|differs from|not clean' '$WORK/m.out' && grep -q 'make_kit' '$WORK/m.out'"
check "nothing was written for the refused builds" bash -c "[ ! -e '$WORK/r1' ] && [ ! -e '$WORK/r2' ] && [ ! -e '$WORK/r3' ]"

echo
echo "release_test: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
