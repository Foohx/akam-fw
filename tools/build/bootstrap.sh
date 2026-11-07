#!/usr/bin/env bash
# Download and verify the pinned build dependencies (see deps.lock):
#   - Zig toolchain          -> tools/.toolchain/zig-<ver>-x86_64-linux/
#   - SmolRTSP checkout      -> src/smolrtsp
#   - its 4 header-only deps -> src/smolrtsp/build/_deps/<name>-src
# Idempotent. Every download is sha256-checked; a mismatch aborts.
# DEPS_LOCK may point to an alternative lock file (used for testing).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lock="${DEPS_LOCK:-$here/deps.lock}"
# shellcheck source=tools/build/deps.lock
. "$lock"

cache="$root/tools/.cache"
toolchain="$root/tools/.toolchain"
smol="$root/src/smolrtsp"
mkdir -p "$cache" "$toolchain"

die() { echo "bootstrap: error: $*" >&2; exit 1; }
log() { echo "bootstrap: $*"; }

# download <url> <sha256> <dest>: fetch into the cache unless already valid.
download() {
    local url="$1" sha="$2" dest="$3" got
    if [ -f "$dest" ]; then
        got="$(sha256sum "$dest" | cut -d' ' -f1)"
        [ "$got" = "$sha" ] && return 0
        log "cached $(basename "$dest") has wrong hash, re-downloading"
        rm -f "$dest"
    fi
    log "downloading $url"
    curl -fsSL --retry 3 -o "$dest.part" "$url" || die "download failed: $url"
    got="$(sha256sum "$dest.part" | cut -d' ' -f1)"
    if [ "$got" != "$sha" ]; then
        rm -f "$dest.part"
        die "sha256 mismatch for $url: expected $sha, got $got"
    fi
    mv "$dest.part" "$dest"
}

# --- Zig -------------------------------------------------------------------
zig_dir="$toolchain/zig-$ZIG_VERSION-x86_64-linux"
if [ -x "$zig_dir/zig" ]; then
    log "zig $ZIG_VERSION already installed"
else
    archive="$cache/$(basename "$ZIG_URL")"
    download "$ZIG_URL" "$ZIG_SHA256" "$archive"
    tmp="$(mktemp -d "$toolchain/.extract.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    tar -xJf "$archive" -C "$tmp"
    [ -x "$tmp/zig-x86_64-linux-$ZIG_VERSION/zig" ] || die "unexpected Zig archive layout"
    rm -rf "$zig_dir"
    mv "$tmp/zig-x86_64-linux-$ZIG_VERSION" "$zig_dir"
    log "installed zig $("$zig_dir/zig" version) in $zig_dir"
fi

# --- SmolRTSP --------------------------------------------------------------
if [ -d "$smol/.git" ]; then
    if [ -n "$(git -C "$smol" status --porcelain)" ]; then
        die "$smol has local modifications; refusing to touch it (clean or remove it first)"
    fi
else
    if [ -e "$smol" ] && [ -n "$(ls -A "$smol" 2>/dev/null)" ]; then
        die "$smol exists but is not a git checkout; remove it first"
    fi
    mkdir -p "$smol"
    git -C "$smol" init -q
    git -C "$smol" remote add origin "$SMOLRTSP_URL"
fi
head="$(git -C "$smol" rev-parse -q --verify HEAD 2>/dev/null || true)"
if [ "$head" != "$SMOLRTSP_COMMIT" ]; then
    log "fetching SmolRTSP $SMOLRTSP_COMMIT"
    git -C "$smol" fetch -q --depth 1 origin "$SMOLRTSP_COMMIT"
    git -C "$smol" checkout -q --detach "$SMOLRTSP_COMMIT"
fi
head="$(git -C "$smol" rev-parse HEAD)"
[ "$head" = "$SMOLRTSP_COMMIT" ] || die "SmolRTSP HEAD is $head, expected $SMOLRTSP_COMMIT"
log "SmolRTSP at $head"

# --- SmolRTSP header-only deps (CMake-less FetchContent equivalent) ---------
deps="$smol/build/_deps"
mkdir -p "$deps"
for name in slice99 metalang99 datatype99 interface99; do
    up="${name^^}"
    tag_v="${up}_TAG"; url_v="${up}_URL"; sha_v="${up}_SHA256"
    tag="${!tag_v}"; url="${!url_v}"; sha="${!sha_v}"
    dest="$deps/$name-src"
    if [ "$(cat "$dest/.camyn-dep" 2>/dev/null || true)" = "$tag $sha" ]; then
        log "$name $tag already in place"
        continue
    fi
    archive="$cache/$name-$tag.tar.gz"
    download "$url" "$sha" "$archive"
    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xzf "$archive" -C "$dest" --strip-components=1
    echo "$tag $sha" > "$dest/.camyn-dep"
    log "$name $tag extracted"
done

log "done"
