#!/usr/bin/env bash
# Download and verify the pinned build dependencies (see deps.lock):
#   - Zig toolchain          -> tools/.toolchain/zig-<ver>-<arch>-linux/  (arch: x86_64, aarch64)
#   - SmolRTSP checkout      -> src/smolrtsp
#   - its 4 header-only deps -> src/smolrtsp/build/_deps/<name>-src
#   - Dropbear archive       -> tools/.cache/ (verified; built by tools/dropbear/)
#   - Go modules of tools/assistant -> tools/.toolchain/go/mod (only when Go >= GO_VERSION is installed;
#     Go itself is never installed: the message says where to get it)
#   - static BusyBox (tests) -> tools/.toolchain/busybox-<ver>-x86_64/busybox (x86_64 hosts only)
# Idempotent. Every download is sha256-checked; a mismatch aborts.
#
#   bootstrap.sh [--print-plan] [--offline]
#     --print-plan  show what would be fetched for this host; download and write nothing
#     --offline     never touch the network: use tools/.cache (and an existing src/smolrtsp)
#                   only; fail clearly if something is missing (also: AKAM_OFFLINE=1)
# Transient failures (HTTP 5xx, 429, network errors) are retried with exponential backoff:
# AKAM_RETRY_MAX attempts (default 5), AKAM_RETRY_SLEEP initial delay in seconds (default 2).
# Other environment: DEPS_LOCK (alternative lock file), AKAM_CURL (curl command, tests),
# AKAM_HOST_ARCH (override uname -m, tests).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
lock="${DEPS_LOCK:-$here/deps.lock}"
# shellcheck source=tools/build/deps.lock
. "$lock"
# shellcheck source=tools/lib/host.sh
. "$root/tools/lib/host.sh"

die() { echo "bootstrap: error: $*" >&2; exit 1; }
log() { echo "bootstrap: $*"; }

plan=0
offline="${AKAM_OFFLINE:-0}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --print-plan) plan=1 ;;
        --offline) offline=1 ;;
        -h | --help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
    shift
done

cache="$root/tools/.cache"
toolchain="$root/tools/.toolchain"
smol="$root/src/smolrtsp"
curl_cmd="${AKAM_CURL:-curl}"
retry_max="${AKAM_RETRY_MAX:-5}"
retry_sleep="${AKAM_RETRY_SLEEP:-2}"

# --- host selection ---------------------------------------------------------
[ "$(uname -s)" = Linux ] || die "unsupported OS $(uname -s): only Linux (Arch, Debian/Ubuntu) is supported"
arch="$(host_arch)"
zig_url_v="ZIG_URL_linux_$arch"
zig_sha_v="ZIG_SHA256_linux_$arch"
ZIG_URL="${!zig_url_v:-}"
ZIG_SHA256="${!zig_sha_v:-}"
if [ -z "$ZIG_URL" ] || [ -z "$ZIG_SHA256" ]; then
    die "no pinned Zig for linux/$arch in $(basename "$lock") (supported: x86_64, aarch64)"
fi
zig_dir="$toolchain/zig-$ZIG_VERSION-$arch-linux"
zig_inner="zig-$arch-linux-$ZIG_VERSION"
bb_dir="$toolchain/busybox-$BUSYBOX_VERSION-x86_64"

# cache_state <file> <sha>: "cached" when tools/.cache holds a valid copy, else "missing".
cache_state() {
    local f="$cache/$1"
    if [ -f "$f" ] && [ "$(host_sha256 "$f")" = "$2" ]; then echo cached; else echo missing; fi
}

if [ "$plan" = 1 ]; then
    zstate="cache $(cache_state "$(basename "$ZIG_URL")" "$ZIG_SHA256")"
    [ -x "$zig_dir/zig" ] && zstate=installed
    sstate="to fetch"
    [ "$(git -C "$smol" rev-parse -q --verify HEAD 2>/dev/null || true)" = "$SMOLRTSP_COMMIT" ] && sstate=present
    echo "bootstrap plan (no download, nothing written)"
    echo "  host:       linux/$arch ($(host_os))"
    echo "  mode:       $([ "$offline" = 1 ] && echo offline || echo online)"
    echo "  zig:        $ZIG_VERSION"
    echo "    url:      $ZIG_URL"
    echo "    sha256:   $ZIG_SHA256"
    echo "    install:  $zig_dir ($zstate)"
    echo "  smolrtsp:   $SMOLRTSP_COMMIT ($sstate)"
    for name in slice99 metalang99 datatype99 interface99; do
        up="$(echo "$name" | tr '[:lower:]' '[:upper:]')"
        tag_v="${up}_TAG"
        sha_v="${up}_SHA256"
        echo "  $name:  ${!tag_v} (cache $(cache_state "$name-${!tag_v}.tar.gz" "${!sha_v}"))"
    done
    echo "  dropbear:   $DROPBEAR_VERSION (cache $(cache_state "$(basename "$DROPBEAR_URL")" "$DROPBEAR_SHA256"))"
    echo "  go modules: tools/assistant (GO_VERSION >= $GO_VERSION; host go: $(command -v go >/dev/null 2>&1 && GOTOOLCHAIN=local go env GOVERSION || echo none))"
    if [ "$arch" = x86_64 ]; then
        bstate="to fetch"
        [ -x "$bb_dir/busybox" ] && bstate=installed
        echo "  busybox:    $BUSYBOX_VERSION ($bstate)"
    else
        echo "  busybox:    skipped (pinned binary is x86_64 only)"
    fi
    exit 0
fi

mkdir -p "$cache" "$toolchain"

# retry <cmd...>: run it, retrying with exponential backoff (used for git).
retry() {
    local n=1 delay="$retry_sleep"
    until "$@"; do
        [ "$n" -lt "$retry_max" ] || return 1
        log "attempt $n/$retry_max failed, retrying in ${delay}s"
        sleep "$delay"
        n=$((n + 1))
        delay=$((delay * 2))
    done
}

# fetch <url> <dest>: curl with retries on transient errors only (5xx, 429, network).
# Other 4xx (404, 403...) fail immediately.
fetch() {
    local url="$1" dest="$2" n=1 delay="$retry_sleep" code rc
    while :; do
        rc=0
        code="$("$curl_cmd" -sSL --proto '=https' --proto-redir '=https' -o "$dest" \
            -w '%{http_code}' "$url")" || rc=$?
        if [ "$rc" = 0 ] && [ "$code" -ge 200 ] && [ "$code" -lt 300 ]; then
            return 0
        fi
        rm -f "$dest"
        if [ "$rc" = 0 ] && [ "$code" -ge 400 ] && [ "$code" -lt 500 ] && [ "$code" != 429 ]; then
            die "download failed: $url (HTTP $code)"
        fi
        [ "$n" -lt "$retry_max" ] || die "download failed after $retry_max attempts: $url (curl exit $rc, HTTP $code)"
        log "transient failure for $url (curl exit $rc, HTTP $code), retry $n/$retry_max in ${delay}s"
        sleep "$delay"
        n=$((n + 1))
        delay=$((delay * 2))
    done
}

# download <url> <sha256> <dest>: use the cache if valid, else fetch (never when offline).
download() {
    local url="$1" sha="$2" dest="$3" got
    if [ -f "$dest" ]; then
        got="$(host_sha256 "$dest")"
        [ "$got" = "$sha" ] && return 0
        [ "$offline" = 1 ] && die "offline: cached $(basename "$dest") has wrong hash"
        log "cached $(basename "$dest") has wrong hash, re-downloading"
        rm -f "$dest"
    fi
    [ "$offline" = 1 ] && die "offline: $(basename "$dest") is not in tools/.cache (run bootstrap.sh online once)"
    log "downloading $url"
    rm -f "$dest.part"
    fetch "$url" "$dest.part"
    got="$(host_sha256 "$dest.part")"
    if [ "$got" != "$sha" ]; then
        rm -f "$dest.part"
        die "sha256 mismatch for $url: expected $sha, got $got"
    fi
    mv "$dest.part" "$dest"
}

# --- Zig -------------------------------------------------------------------
if [ -x "$zig_dir/zig" ]; then
    log "zig $ZIG_VERSION already installed"
else
    archive="$cache/$(basename "$ZIG_URL")"
    download "$ZIG_URL" "$ZIG_SHA256" "$archive"
    tmp="$(mktemp -d "$toolchain/.extract.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    tar -xJf "$archive" -C "$tmp"
    [ -x "$tmp/$zig_inner/zig" ] || die "unexpected Zig archive layout"
    rm -rf "$zig_dir"
    mv "$tmp/$zig_inner" "$zig_dir"
    log "installed zig $("$zig_dir/zig" version) in $zig_dir"
fi

# --- SmolRTSP --------------------------------------------------------------
if [ -d "$smol/.git" ]; then
    if [ -n "$(git -C "$smol" status --porcelain)" ]; then
        die "$smol has local modifications; refusing to touch it (clean or remove it first)"
    fi
else
    [ "$offline" = 1 ] && die "offline: $smol is not checked out (run bootstrap.sh online once)"
    if [ -e "$smol" ] && [ -n "$(ls -A "$smol" 2>/dev/null)" ]; then
        die "$smol exists but is not a git checkout; remove it first"
    fi
    mkdir -p "$smol"
    git -C "$smol" init -q
    git -C "$smol" remote add origin "$SMOLRTSP_URL"
fi
head="$(git -C "$smol" rev-parse -q --verify HEAD 2>/dev/null || true)"
if [ "$head" != "$SMOLRTSP_COMMIT" ]; then
    [ "$offline" = 1 ] && die "offline: SmolRTSP $SMOLRTSP_COMMIT is not checked out in src/smolrtsp"
    log "fetching SmolRTSP $SMOLRTSP_COMMIT"
    retry git -C "$smol" fetch -q --depth 1 origin "$SMOLRTSP_COMMIT" \
        || die "git fetch failed for $SMOLRTSP_URL"
    git -C "$smol" checkout -q --detach "$SMOLRTSP_COMMIT"
fi
head="$(git -C "$smol" rev-parse HEAD)"
[ "$head" = "$SMOLRTSP_COMMIT" ] || die "SmolRTSP HEAD is $head, expected $SMOLRTSP_COMMIT"
log "SmolRTSP at $head"

# --- SmolRTSP header-only deps (CMake-less FetchContent equivalent) ---------
deps="$smol/build/_deps"
mkdir -p "$deps"
for name in slice99 metalang99 datatype99 interface99; do
    up="$(echo "$name" | tr '[:lower:]' '[:upper:]')"
    tag_v="${up}_TAG"
    url_v="${up}_URL"
    sha_v="${up}_SHA256"
    tag="${!tag_v}"
    url="${!url_v}"
    sha="${!sha_v}"
    dest="$deps/$name-src"
    if [ "$(cat "$dest/.akam-dep" 2>/dev/null || true)" = "$tag $sha" ]; then
        log "$name $tag already in place"
        continue
    fi
    archive="$cache/$name-$tag.tar.gz"
    download "$url" "$sha" "$archive"
    rm -rf "$dest"
    mkdir -p "$dest"
    tar -xzf "$archive" -C "$dest" --strip-components=1
    echo "$tag $sha" > "$dest/.akam-dep"
    log "$name $tag extracted"
done

# --- Dropbear source archive (built by tools/dropbear/build_dropbear.sh) -----
download "$DROPBEAR_URL" "$DROPBEAR_SHA256" "$cache/$(basename "$DROPBEAR_URL")"
log "dropbear $DROPBEAR_VERSION archive verified"

# --- Static BusyBox for host-side tests (x86_64 binary: x86_64 hosts only) ---
if [ "$arch" = x86_64 ]; then
    mkdir -p "$bb_dir"
    download "$BUSYBOX_URL" "$BUSYBOX_SHA256" "$bb_dir/busybox"
    chmod 755 "$bb_dir/busybox"
    log "busybox $BUSYBOX_VERSION at $bb_dir/busybox"
else
    log "busybox skipped on $arch (pinned binary is x86_64 only; camera-side ash tests will skip)"
fi

# --- Go modules of the native assistant (tools/assistant; optional: needs Go) ---
# Fetched into tools/.toolchain/go/mod, verified against go.sum. Never touches ~/go.
GO_VERSION="${GO_VERSION:-1.26.0}"; GO_NOTE="${GO_NOTE:-needs Go >= 1.26 (https://go.dev/dl/)}"
go_min="${GO_VERSION%.*}"
have_go=""
if command -v go >/dev/null 2>&1; then
    have_go="$(GOTOOLCHAIN=local go env GOVERSION | sed 's/^go//;s/[ -].*//')"
fi
version_ge() { # version_ge A B: major.minor of A >= major.minor of B
    local a="${1%.*}" b="${2%.*}"
    [ "$(printf '%s\n%s\n' "$a" "$b" | sort -V | head -n1)" = "$b" ]
}
if [ ! -f "$root/tools/assistant/go.mod" ]; then
    log "tools/assistant not present: no Go modules to fetch"
elif [ -z "$have_go" ]; then
    log "Go not found: skipping the assistant modules ($GO_NOTE)"
elif ! version_ge "$have_go" "$go_min"; then
    log "Go $have_go is older than $go_min: skipping the assistant modules ($GO_NOTE)"
elif [ "$offline" = 1 ]; then
    log "offline: assistant Go modules not fetched (run bootstrap.sh online once)"
else
    mkdir -p "$toolchain/go/path" "$toolchain/go/mod" "$toolchain/go/cache"
    if (cd "$root/tools/assistant" && GOPATH="$toolchain/go/path" GOMODCACHE="$toolchain/go/mod" \
        GOCACHE="$toolchain/go/cache" GOFLAGS=-modcacherw GOTOOLCHAIN=local GOTELEMETRY=off \
        GOPROXY="${AKAM_GOPROXY:-https://proxy.golang.org}" go mod download); then
        log "assistant Go modules ready in $toolchain/go/mod (go $have_go)"
    else
        die "go mod download failed (network? proxy: set AKAM_GOPROXY)"
    fi
fi

log "done"
