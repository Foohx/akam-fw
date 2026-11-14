#!/usr/bin/env bash
# Tests for tools/release/publish.sh with a fake `gh` on PATH (it records its calls and keeps the "GitHub" side in a
# directory). Nothing here touches the network or a real repository: the scripts run in a throw-away git
# repository, the kit is built from stub binaries, the signing key is a throw-away one WITHOUT a passphrase.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
REL=$REPO/tools/release

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
fails_with() {
    local pat=$1 rc=0
    shift
    "$@" >"$WORK/last.out" 2>&1 || rc=$?
    [ "$rc" != 0 ] && grep -qE -e "$pat" "$WORK/last.out"
}
quiet() { "$@" >/dev/null 2>&1; }

# --- the fake gh --------------------------------------------------------------------------------
mkdir -p "$WORK/bin"
cat >"$WORK/bin/gh" <<'FAKE'
#!/usr/bin/env bash
# fake gh: the "GitHub" is $GH_FAKE_STATE/rel/<tag>/{draft,title,notes,target,latest,assets/}
S=${GH_FAKE_STATE:?}
echo "gh $*" >>"$S/calls.log"
[ "${1:-}" = release ] || { echo "fake gh: only 'release' is supported" >&2; exit 2; }
sub=$2
tag=$3
shift 3
files=() json="" jq="" dir=""
clobber=0
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) shift ;;
        --json) json=$2; shift ;;
        --jq) jq=$2; shift ;;
        --dir) dir=$2; shift ;;
        --title) echo "$2" >"$S/rel/$tag/title" 2>/dev/null || true; T=$2; shift ;;
        --notes-file) N=$2; shift ;;
        --target) G=$2; shift ;;
        --draft) D=true ;;
        --draft=false) D=false ;;
        --latest) L=true ;;
        --latest=false) L=false ;;
        --clobber) clobber=1 ;;
        --yes) ;;
        -*) echo "fake gh: unknown flag $1" >&2; exit 2 ;;
        *) files+=("$1") ;;
    esac
    shift
done
R=$S/rel/$tag
case "$sub" in
    view)
        [ -d "$R" ] || { echo "release not found" >&2; exit 1; }
        case "$jq" in
            .isDraft) cat "$R/draft" ;;
            '.assets[].name') ls "$R/assets" ;;
            .url) echo "https://example.invalid/releases/$tag" ;;
            *) echo "fake gh: unknown jq $jq" >&2; exit 2 ;;
        esac
        ;;
    create)
        [ ! -d "$R" ] || { echo "already exists" >&2; exit 1; }
        mkdir -p "$R/assets"
        echo "${D:-false}" >"$R/draft"
        echo "${T:-}" >"$R/title"
        cp "$N" "$R/notes"
        echo "${G:-}" >"$R/target"
        ;;
    edit)
        [ -d "$R" ] || { echo "release not found" >&2; exit 1; }
        [ -z "${T:-}" ] || echo "$T" >"$R/title"
        [ -z "${N:-}" ] || cp "$N" "$R/notes"
        [ -z "${G:-}" ] || echo "$G" >"$R/target"
        [ -z "${D:-}" ] || echo "$D" >"$R/draft"
        [ -z "${L:-}" ] || echo "$L" >"$R/latest"
        ;;
    upload)
        [ -d "$R" ] || { echo "release not found" >&2; exit 1; }
        for f in "${files[@]}"; do
            b=$(basename "$f")
            if [ -e "$R/assets/$b" ] && [ "$clobber" = 0 ]; then echo "asset exists" >&2; exit 1; fi
            cp "$f" "$R/assets/$b"
            [ "${GH_FAKE_CORRUPT:-}" != "$b" ] || printf x >>"$R/assets/$b"
        done
        ;;
    delete-asset) rm -f "$R/assets/${files[0]}" ;;
    download)
        [ -d "$R" ] || { echo "release not found" >&2; exit 1; }
        cp "$R"/assets/* "$dir"/ 2>/dev/null || true
        ;;
    *) echo "fake gh: unknown subcommand $sub" >&2; exit 2 ;;
esac
FAKE
chmod +x "$WORK/bin/gh"
export AKAM_GH="$WORK/bin/gh" GH_FAKE_STATE="$WORK/gh"
mkdir -p "$GH_FAKE_STATE/rel"
calls() { cat "$GH_FAKE_STATE/calls.log" 2>/dev/null || true; }
reset_gh() { rm -rf "$GH_FAKE_STATE"; mkdir -p "$GH_FAKE_STATE/rel"; }

# --- a stub firmware kit (test grade) and a throw-away repository -------------------------------------
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
V=1.0.0
MODEL=lsc-indoor-1080p-ak3918
KIT=$WORK/kit
(
    export AKAM_KIT_NO_BUILD=1 AKAM_KIT_ALLOW_DIRTY=1 AKAM_KIT_VERSION=$V SOURCE_DATE_EPOCH=1700000000
    export AK_RTSP_BIN=$S/ak_rtsp FLASH_TOOL_BIN=$S/flash_tool DROPBEAR_MULTI=$S/db/dropbearmulti
    export SMOLRTSP_DIR=$S/smol MUSL_COPYRIGHT=$S/COPYRIGHT ZIG_LICENSE=$S/ZIGLICENSE AKAM_KIT_CHECK_TIME32=$S/gate_ok
    bash "$REL/make_kit.sh" --out "$KIT" >"$WORK/kit.log" 2>&1
) || { cat "$WORK/kit.log"; echo "cannot build the stub kit"; exit 1; }
git -C "$REPO" archive --format=tar --prefix="akam-firmware-$V/" HEAD tests/release_test.sh | gzip -n >"$WORK/src.tar.gz"
export AKAM_EXPECT_COMMIT
AKAM_EXPECT_COMMIT=$(git -C "$REPO" rev-parse HEAD)
export AKAM_CHECK_ALLOW_TEST_KIT=1 AKAM_PUBLISH_NO_FETCH=1

ssh-keygen -q -t ed25519 -N '' -C test -f "$WORK/key" >/dev/null
ssh-keygen -q -t ed25519 -N '' -C other -f "$WORK/key2" >/dev/null
printf 'akam-release namespaces="akam-release" %s\n' "$(cut -d' ' -f1,2 "$WORK/key.pub")" >"$WORK/allowed"
BLOB=$(cut -d' ' -f2 "$WORK/key.pub")

R=$WORK/repo
mkdir -p "$R/tools/release" "$R/tools/assistant/internal/kit"
cp "$REL/publish.sh" "$REL/gather_release.sh" "$REL/sign_kit.sh" "$REL/verify_kit.sh" "$R/tools/release/"
cp "$WORK/allowed" "$R/tools/release/allowed_signers"
cp "$WORK/allowed" "$R/tools/assistant/internal/kit/allowed_signers"
printf 'LICENSE\n' >"$R/LICENSE"
printf 'notices\n' >"$R/THIRD_PARTY_NOTICES.md"
git -C "$R" init -q
git -C "$R" add -A
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -m init
git -C "$R" update-ref refs/remotes/origin/main HEAD
HEADR=$(git -C "$R" rev-parse HEAD)
P=$R/tools/release/publish.sh
resum() { (cd "$1" && rm -f SHA256SUMS SHA256SUMS.sig && LC_ALL=C find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -printf '%f\n' | LC_ALL=C sort | xargs sha256sum >SHA256SUMS); }

# release directories as make_release.sh leaves them: gathered, no signature, notes next to them
DIST=$WORK/dist
mkdir -p "$DIST"
bash "$REL/gather_release.sh" firmware "$V" --kit-dir "$KIT" --source "$WORK/src.tar.gz" --out "$DIST/firmware-v$V" --grade test >/dev/null
rm "$DIST/firmware-v$V/TEST_BUILD_NOT_PUBLISHABLE.txt"
resum "$DIST/firmware-v$V"
ASS=$WORK/ass
mkdir -p "$ASS"
for t in linux-amd64 linux-arm64 darwin-amd64 darwin-arm64 windows-amd64 windows-arm64; do
    case $t in
        windows-*) printf 'fake %s %s\n' "$t" "$BLOB" >"$ASS/akam-$t.exe"; printf 'fake zip %s\n' "$t" >"$ASS/akam-$t.zip" ;;
        *) printf 'fake %s %s\n' "$t" "$BLOB" >"$ASS/akam-$t"; printf 'fake tgz %s\n' "$t" >"$ASS/akam-$t.tar.gz" ;;
    esac
done
printf 'licence\n' >"$ASS/LICENSE"
printf 'notices\n' >"$ASS/THIRD_PARTY_NOTICES.md"
bash "$REL/gather_release.sh" assistant "$V" --assistant-dir "$ASS" --out "$DIST/assistant-v$V" --grade test >/dev/null
rm "$DIST/assistant-v$V/TEST_BUILD_NOT_PUBLISHABLE.txt"
resum "$DIST/assistant-v$V"
KSHA=$(awk -v f="akam-kit-$MODEL-$V.tar.xz" '$2 == f {print $1}' "$DIST/firmware-v$V/SHA256SUMS")
write_notes() { printf 'Release notes. Key %s. Kit %s\n\n<!-- akam-release-commit: %s -->\n' "$BLOB" "$KSHA" "$1" >"$DIST/$2-v$V-notes.md"; }
write_notes "$HEADR" firmware
write_notes "$HEADR" assistant
FW=$DIST/firmware-v$V
AS=$DIST/assistant-v$V
export AKAM_ALLOWED_SIGNERS=$WORK/allowed AKAM_EMBEDDED_SIGNERS=$WORK/allowed

echo "== draft: refusals"
check "no gh program: refused" fails_with 'gh program was not found' env AKAM_GH=/nonexistent/gh bash "$P" draft "$FW"
check "draft takes exactly one directory" fails_with 'exactly one' bash "$P" draft "$FW" "$AS"
check "an unknown command is refused" fails_with 'unknown command' bash "$P" frobnicate "$FW"
check "a directory not named after a family is refused" fails_with 'firmware-vX.Y.Z or assistant-vX.Y.Z' bash "$P" draft "$DIST"
mkdir -p "$DIST/firmware-v$V-test"
check "a -test directory is refused (not plain X.Y.Z)" fails_with 'plain X.Y.Z' bash "$P" draft "$DIST/firmware-v$V-test"
mkdir -p "$DIST/assistant-v2.0.0"
printf 'x\n' >"$DIST/assistant-v2.0.0/SHA256SUMS"
: >"$DIST/assistant-v2.0.0/TEST_BUILD_NOT_PUBLISHABLE.txt"
check "a TEST build directory is refused" fails_with 'TEST build' bash "$P" draft "$DIST/assistant-v2.0.0"
printf 'x\n' >"$R/untracked"
check "a dirty tree is refused" fails_with 'not clean' bash "$P" draft "$FW"
rm "$R/untracked"
write_notes 0000000000000000000000000000000000000000 firmware
check "a release built from another commit than origin/main is refused" fails_with 'not from origin/main' bash "$P" draft "$FW"
write_notes "$HEADR" firmware
printf 'x\n' >"$R/more"
git -C "$R" add more
git -C "$R" -c user.name=t -c user.email=t@example.invalid commit -q -m more
check "HEAD ahead of origin/main (not pushed) is refused" fails_with 'is not origin/main' bash "$P" draft "$FW"
git -C "$R" reset -q --hard HEAD~1
check "no gh call was made by the refusals" bash -c "[ ! -s '$GH_FAKE_STATE/calls.log' ]"

echo "== draft"
check "creates the firmware draft" quiet bash "$P" draft "$FW"
TAGF=firmware-v$V
RF=$GH_FAKE_STATE/rel/$TAGF
check "  draft, title, target = the origin/main commit, notes" bash -c "
    [ \"\$(cat '$RF/draft')\" = true ] && [ \"\$(cat '$RF/title')\" = 'Akam firmware $V' ] && [ \"\$(cat '$RF/target')\" = '$HEADR' ] && cmp -s '$RF/notes' '$DIST/$TAGF-notes.md'"
check "  gh was called with create --draft --target" bash -c "grep -q 'release create $TAGF .*--draft .*--target $HEADR' '$GH_FAKE_STATE/calls.log'"
check "  every file except the signature is uploaded, with the same sha256" bash -c "
    cd '$FW' && for f in \$(ls); do [ \"\$f\" = SHA256SUMS.sig ] && continue; cmp -s \"\$f\" '$RF/assets/'\$f || exit 1; done && [ \$(ls '$RF/assets' | wc -l) = \$(ls | wc -l) ]"
check "  no tag is created and nothing is published" bash -c "! grep -q 'draft=false' '$GH_FAKE_STATE/calls.log' && ! grep -q ' tag ' '$GH_FAKE_STATE/calls.log'"
: >"$GH_FAKE_STATE/calls.log"
check "running it again is idempotent: edit, no upload" bash -c "bash '$P' draft '$FW' >/dev/null 2>&1 && ! grep -q 'release upload' '$GH_FAKE_STATE/calls.log' && grep -q 'release edit $TAGF' '$GH_FAKE_STATE/calls.log' && ! grep -q 'release create' '$GH_FAKE_STATE/calls.log'"
printf 'changed\n' >>"$RF/assets/LICENSE"
printf 'stray\n' >"$RF/assets/stray.txt"
printf 'old sig\n' >"$RF/assets/SHA256SUMS.sig"
: >"$GH_FAKE_STATE/calls.log"
check "a changed remote asset is replaced, a stray one and a stale signature are removed" bash -c "
    bash '$P' draft '$FW' >/dev/null 2>&1 && cmp -s '$FW/LICENSE' '$RF/assets/LICENSE' && [ ! -e '$RF/assets/stray.txt' ] && [ ! -e '$RF/assets/SHA256SUMS.sig' ] &&
    [ \$(grep -c 'release upload' '$GH_FAKE_STATE/calls.log') = 1 ]"
reset_gh
check "an upload that arrives corrupted is detected" fails_with 'do not match the local files: .*LICENSE' env GH_FAKE_CORRUPT=LICENSE bash "$P" draft "$FW"
reset_gh
mkdir -p "$GH_FAKE_STATE/rel/$TAGF/assets"
echo false >"$GH_FAKE_STATE/rel/$TAGF/draft"
check "an already published release is never touched" fails_with 'already published' bash "$P" draft "$FW"
check "  (no edit or upload was attempted)" bash -c "! grep -qE 'release (edit|upload|create)' '$GH_FAKE_STATE/calls.log'"
reset_gh

echo "== sign-and-publish"
check "without a draft: refused, asks for the draft first" fails_with 'publish.sh draft' bash "$P" sign-and-publish --key "$WORK/key" "$FW"
bash "$P" draft "$FW" >/dev/null 2>&1
bash "$P" draft "$AS" >/dev/null 2>&1
check "a key that is not the release key: refused before anything is published" fails_with 'does not verify|wrong key' bash "$P" sign-and-publish --key "$WORK/key2" "$FW" "$AS"
check "  (nothing published, no signature uploaded)" bash -c "! grep -q 'draft=false' '$GH_FAKE_STATE/calls.log' && [ ! -e '$GH_FAKE_STATE/rel/$TAGF/assets/SHA256SUMS.sig' ]"
check "a missing key file is refused" fails_with 'not found' bash "$P" sign-and-publish --key "$WORK/nokey" "$FW"
printf 'tampered\n' >>"$GH_FAKE_STATE/rel/assistant-v$V/assets/LICENSE"
check "a draft that differs from the local files is refused (run draft again)" fails_with 'differs from' bash "$P" sign-and-publish --key "$WORK/key" "$AS"
check "  (nothing published)" bash -c "! grep -q 'draft=false' '$GH_FAKE_STATE/calls.log'"
bash "$P" draft "$AS" >/dev/null 2>&1
: >"$GH_FAKE_STATE/calls.log"
check "signs both releases and publishes them" bash -c "bash '$P' sign-and-publish --key '$WORK/key' '$AS' '$FW' >'$WORK/sp.out' 2>&1"
check "  both directories hold a verifying signature" bash -c "
    for d in '$FW' '$AS'; do ssh-keygen -Y verify -f '$WORK/allowed' -I akam-release -n akam-release -s \$d/SHA256SUMS.sig <\$d/SHA256SUMS >/dev/null 2>&1 || exit 1; done"
check "  the signature was uploaded to both drafts, identical to the local one" bash -c "
    cmp -s '$FW/SHA256SUMS.sig' '$GH_FAKE_STATE/rel/firmware-v$V/assets/SHA256SUMS.sig' && cmp -s '$AS/SHA256SUMS.sig' '$GH_FAKE_STATE/rel/assistant-v$V/assets/SHA256SUMS.sig'"
check "  both are published; the assistant is Latest, the firmware is not" bash -c "
    [ \"\$(cat '$GH_FAKE_STATE/rel/firmware-v$V/draft')\" = false ] && [ \"\$(cat '$GH_FAKE_STATE/rel/assistant-v$V/draft')\" = false ] &&
    [ \"\$(cat '$GH_FAKE_STATE/rel/firmware-v$V/latest')\" = false ] && [ \"\$(cat '$GH_FAKE_STATE/rel/assistant-v$V/latest')\" = true ] &&
    grep -q 'release edit firmware-v$V .*--draft=false --latest=false' '$GH_FAKE_STATE/calls.log' && grep -q 'release edit assistant-v$V .*--draft=false --latest\$' '$GH_FAKE_STATE/calls.log'"
check "  the firmware is published before the assistant (so the assistant stays Latest)" bash -c "
    f=\$(grep -n 'draft=false' '$GH_FAKE_STATE/calls.log' | grep firmware | cut -d: -f1); a=\$(grep -n 'draft=false' '$GH_FAKE_STATE/calls.log' | grep assistant | cut -d: -f1); [ \$f -lt \$a ]"
check "  the URLs are printed" bash -c "grep -q 'published: https://example.invalid/releases/firmware-v$V' '$WORK/sp.out' && grep -q 'published: https://example.invalid/releases/assistant-v$V' '$WORK/sp.out'"
check "a published release is refused by draft and by sign-and-publish" bash -c "
    ! bash '$P' draft '$FW' >/dev/null 2>&1 && ! bash '$P' sign-and-publish --key '$WORK/key' '$FW' >/dev/null 2>&1"
check "the private key was never copied into the repository or the release directories" bash -c "! grep -rl 'BEGIN OPENSSH PRIVATE KEY' '$R' '$DIST' >/dev/null 2>&1"

echo
echo "publish_test: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
