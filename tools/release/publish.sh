#!/usr/bin/env bash
# Publish a release built by make_release.sh to GitHub, in two steps (needs the `gh` CLI, logged in).
#
#   tools/release/publish.sh draft <release-dir> [--repo OWNER/NAME]
#   tools/release/publish.sh sign-and-publish [--key FILE] [--repo OWNER/NAME] <release-dir>...
#
# <release-dir> is dist/release/<family>-vX.Y.Z (family: firmware or assistant); the notes file
# <family>-vX.Y.Z-notes.md next to it is the release text. The tag is <family>-vX.Y.Z.
#
# draft: creates or updates the GitHub DRAFT release (title "Akam <family> X.Y.Z", the notes, every file of the
# directory except SHA256SUMS.sig). The tag is only named, not created, until the release is published; its target
# is the commit the release was built from, which must be the current origin/main, with a clean tree. Every
# uploaded asset is downloaded back and its sha256 compared with the local file. Idempotent: run it again after a
# rebuild, a changed file is replaced, an extra remote asset (and any signature) is removed. A published release is
# never touched. It never signs.
#
# sign-and-publish: for each directory, signs SHA256SUMS with the key (default ~/.ssh/akam_release; ssh-keygen
# asks for its passphrase, the key never leaves this computer and is never read by this script), verifies the
# signed directory (gather_release.sh --check-signed). When every directory is signed and consistent, it checks the
# draft's assets against the local files, uploads SHA256SUMS.sig, downloads everything back and compares, then
# publishes (the tag is created on the target commit): the assistant release becomes "Latest" (users download it),
# a firmware release is published with --latest=false. Firmware releases go first, so the assistant one stays the
# latest. The URLs are printed. Refuses a test build, a dirty tree, a commit that is not origin/main.
#
# Environment: AKAM_GH (the gh program, default gh), AKAM_REMOTE (default origin), AKAM_PUBLISH_NO_FETCH=1 (do not
# `git fetch` the remote first: tests).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
GH="${AKAM_GH:-gh}"
REMOTE="${AKAM_REMOTE:-origin}"
REPO="Foohx/akam-fw"
KEY="$HOME/.ssh/akam_release"
MARKER=TEST_BUILD_NOT_PUBLISHABLE.txt
die() { echo "publish: error: $*" >&2; exit 1; }
log() { echo "publish: $*"; }
usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//' >&2; exit 2; }

[ $# -ge 1 ] || usage
CMD=$1
shift
DIRS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --key) [ $# -ge 2 ] || usage; KEY=$2; shift ;;
        --repo) [ $# -ge 2 ] || usage; REPO=$2; shift ;;
        -h | --help) usage ;;
        -*) echo "publish: unknown option $1" >&2; usage ;;
        *) DIRS+=("${1%/}") ;;
    esac
    shift
done
case "$CMD" in
    draft) [ "${#DIRS[@]}" = 1 ] || { echo "publish: draft takes exactly one release directory" >&2; usage; } ;;
    sign-and-publish) [ "${#DIRS[@]}" -ge 1 ] || usage ;;
    -h | --help) usage ;;
    *) echo "publish: unknown command '$CMD'" >&2; usage ;;
esac
command -v "$GH" >/dev/null 2>&1 || die "the gh program was not found (https://cli.github.com), or set AKAM_GH"
command -v git >/dev/null 2>&1 && command -v sha256sum >/dev/null 2>&1 || die "git and sha256sum are required"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Facts of one release directory (set by load_release): FAM VER TAG TITLE DIR NOTES COMMIT.
load_release() {
    DIR=$1
    [ -d "$DIR" ] || die "$DIR is not a directory"
    DIR="$(cd "$DIR" && pwd)"
    local base
    base="$(basename "$DIR")"
    case "$base" in
        firmware-v*) FAM=firmware ;;
        assistant-v*) FAM=assistant ;;
        *) die "$base: expected a directory named firmware-vX.Y.Z or assistant-vX.Y.Z" ;;
    esac
    VER="${base#"$FAM"-v}"
    printf '%s\n' "$VER" | grep -qE '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' || die "$base: the version must be plain X.Y.Z (a -test build is never published)"
    TAG="$FAM-v$VER"
    TITLE="Akam $FAM $VER"
    [ ! -e "$DIR/$MARKER" ] || die "$DIR is a TEST build ($MARKER): it can never be published"
    [ -f "$DIR/SHA256SUMS" ] || die "$DIR/SHA256SUMS not found"
    NOTES="$(dirname "$DIR")/$TAG-notes.md"
    [ -f "$NOTES" ] || die "$NOTES not found (make_release.sh writes it)"
    if grep -qE '<(sha256|key|date|version|kits|changelog)>' "$NOTES"; then die "$NOTES still has a placeholder"; fi
    COMMIT="$(sed -n 's/^<!-- akam-release-commit: \([0-9a-f]\{40\}\) -->$/\1/p' "$NOTES" | head -1)"
    [ -n "$COMMIT" ] || die "$NOTES does not record the commit the release was built from (rebuild with make_release.sh)"
    # Everything in the directory, one name per line (the signature is handled apart).
    ( cd "$DIR" && LC_ALL=C find . -maxdepth 1 -type f ! -name SHA256SUMS.sig -printf '%f\n' | LC_ALL=C sort ) >"$TMP/local.names"
    [ -s "$TMP/local.names" ] || die "$DIR is empty"
}

# The release must be built from the commit that origin/main points to now, from a clean tree.
check_commit() {
    [ -z "$(git -C "$ROOT" status --porcelain)" ] || die "the working tree is not clean"
    if [ "${AKAM_PUBLISH_NO_FETCH:-0}" != 1 ]; then
        git -C "$ROOT" fetch --quiet "$REMOTE" main || die "cannot fetch $REMOTE main"
    fi
    local main head
    main="$(git -C "$ROOT" rev-parse --verify -q "refs/remotes/$REMOTE/main" || true)"
    [ -n "$main" ] || die "no $REMOTE/main in this checkout: push the release commit first"
    head="$(git -C "$ROOT" rev-parse HEAD)"
    [ "$head" = "$main" ] || die "HEAD ($head) is not $REMOTE/main ($main): push the release commit and build from it"
    [ "$COMMIT" = "$main" ] || die "$DIR was built from $COMMIT, not from $REMOTE/main ($main): rebuild it (make_release.sh $FAM $VER)"
}

gh_() { "$GH" "$@"; }
remote_exists() { gh_ release view "$TAG" --repo "$REPO" --json isDraft --jq .isDraft >"$TMP/isdraft" 2>/dev/null; }
remote_names() { gh_ release view "$TAG" --repo "$REPO" --json assets --jq '.assets[].name' | LC_ALL=C sort; }

# Download every remote asset to $TMP/remote and fill $TMP/bad with the names that are missing or differ from the
# local files (names in $1: a file with one name per line).
compare_remote() {
    local names=$1 n
    rm -rf "$TMP/remote"
    mkdir -p "$TMP/remote"
    : >"$TMP/bad"
    remote_names >"$TMP/remote.names"
    if [ -s "$TMP/remote.names" ]; then
        gh_ release download "$TAG" --repo "$REPO" --dir "$TMP/remote" --clobber >/dev/null || die "cannot download the assets of $TAG back"
    fi
    while IFS= read -r n; do
        if ! grep -qxF "$n" "$TMP/remote.names" || [ ! -f "$TMP/remote/$n" ]; then
            echo "$n" >>"$TMP/bad"
        elif [ "$(sha256sum <"$TMP/remote/$n" | cut -d' ' -f1)" != "$(sha256sum <"$DIR/$n" | cut -d' ' -f1)" ]; then
            echo "$n" >>"$TMP/bad"
        fi
    done <"$names"
}

do_draft() {
    local n state
    check_commit
    if remote_exists; then
        state="$(cat "$TMP/isdraft")"
        [ "$state" = true ] || die "$TAG is already published: never change a published release, use a new version"
        log "updating the existing draft $TAG"
        gh_ release edit "$TAG" --repo "$REPO" --title "$TITLE" --notes-file "$NOTES" --target "$COMMIT" >/dev/null
    else
        log "creating the draft $TAG (target $COMMIT)"
        gh_ release create "$TAG" --repo "$REPO" --draft --title "$TITLE" --notes-file "$NOTES" --target "$COMMIT" >/dev/null
    fi
    # Remove what must not be there: the signature (the draft carries none until sign-and-publish) and unknown assets.
    remote_names >"$TMP/remote.names"
    while IFS= read -r n; do
        [ -n "$n" ] || continue
        if ! grep -qxF "$n" "$TMP/local.names"; then
            log "removing the remote asset $n (not in $DIR)"
            gh_ release delete-asset "$TAG" "$n" --repo "$REPO" --yes >/dev/null
        fi
    done <"$TMP/remote.names"
    compare_remote "$TMP/local.names"
    if [ -s "$TMP/bad" ]; then
        while IFS= read -r n; do
            log "uploading $n"
            gh_ release upload "$TAG" "$DIR/$n" --repo "$REPO" --clobber >/dev/null
        done <"$TMP/bad"
        compare_remote "$TMP/local.names"
        [ ! -s "$TMP/bad" ] || die "the uploaded assets do not match the local files: $(tr '\n' ' ' <"$TMP/bad")"
    fi
    log "ok: $(wc -l <"$TMP/local.names" | tr -d ' ') assets of $TAG match the local sha256"
    echo "draft: $(gh_ release view "$TAG" --repo "$REPO" --json url --jq .url)"
}

# --- sign-and-publish ---------------------------------------------------------------------------------
SIGNED=()
do_sign() {
    check_commit
    remote_exists || die "no draft $TAG on $REPO: run  tools/release/publish.sh draft $DIR  first"
    [ "$(cat "$TMP/isdraft")" = true ] || die "$TAG is already published"
    log "signing $DIR with $KEY (ssh-keygen asks for the passphrase)"
    bash "$HERE/sign_kit.sh" "$KEY" "$DIR"
    bash "$HERE/gather_release.sh" --check-signed "$FAM" "$DIR" || die "$DIR does not pass --check-signed after signing"
}

do_publish() {
    local n latest
    check_commit
    compare_remote "$TMP/local.names"
    [ ! -s "$TMP/bad" ] || die "the draft $TAG differs from $DIR ($(tr '\n' ' ' <"$TMP/bad")): run  tools/release/publish.sh draft $DIR  again"
    [ -f "$DIR/SHA256SUMS.sig" ] || die "$DIR/SHA256SUMS.sig not found"
    gh_ release upload "$TAG" "$DIR/SHA256SUMS.sig" --repo "$REPO" --clobber >/dev/null
    { cat "$TMP/local.names"; echo SHA256SUMS.sig; } | LC_ALL=C sort >"$TMP/all.names"
    compare_remote "$TMP/all.names"
    [ ! -s "$TMP/bad" ] || die "the uploaded assets do not match the local files: $(tr '\n' ' ' <"$TMP/bad")"
    [ "$(wc -l <"$TMP/remote.names" | tr -d ' ')" = "$(wc -l <"$TMP/all.names" | tr -d ' ')" ] || die "the draft holds assets that are not in $DIR"
    log "ok: the assets of $TAG (with the signature) match the local sha256"
    if [ "$FAM" = assistant ]; then latest=--latest; else latest=--latest=false; fi
    gh_ release edit "$TAG" --repo "$REPO" --draft=false "$latest" >/dev/null
    log "published $TAG"
    echo "published: $(gh_ release view "$TAG" --repo "$REPO" --json url --jq .url)"
}

case "$CMD" in
    draft)
        load_release "${DIRS[0]}"
        do_draft
        ;;
    sign-and-publish)
        # Firmware first, so that the assistant release (published last) stays "Latest".
        ORDER=()
        for d in "${DIRS[@]}"; do case "$(basename "$d")" in firmware-v*) ORDER+=("$d") ;; esac; done
        for d in "${DIRS[@]}"; do case "$(basename "$d")" in firmware-v*) ;; *) ORDER+=("$d") ;; esac; done
        for d in "${ORDER[@]}"; do
            load_release "$d"
            do_sign
            SIGNED+=("$d")
        done
        for d in "${SIGNED[@]}"; do
            load_release "$d"
            do_publish
        done
        ;;
esac
