#!/usr/bin/env bash
# Builds the assistant: static (CGO_ENABLED=0), modules from tools/.toolchain/go/mod (filled by
# `bash tools/build/bootstrap.sh`, checksum-verified by go.sum), reproducible flags.
#
#   tools/assistant/build.sh               this computer's OS/arch  -> tools/assistant/bin/akam
#   tools/assistant/build.sh all           the six targets -> bin/<os>-<arch>/akam[.exe] (and bin/akam for this host)
#   tools/assistant/build.sh flat DIR      the six targets -> DIR/akam-<os>-<arch>[.exe] (used by tools/release/build_assistant.sh)
#   tools/assistant/build.sh test          go vet + go test ./... (what tests/assistant_test.sh runs)
#   tools/assistant/build.sh vet-all [--link]  go vet (-tags akamtest) for the five non-host targets; --link also builds their test binaries (go test -c)
#   tools/assistant/build.sh sizes [--by-package [os/arch]]
#                                          sizes of the built binaries against the budget (hard 12 MiB, soft 11 MiB);
#                                          --by-package: size per Go package of an unstripped build
#   tools/assistant/build.sh repro [os/arch ...]   builds twice and compares the sha256 (default: all six)
#   tools/assistant/build.sh notices       THIRD_PARTY_NOTICES.md lists every module linked into each target
#
# The Go used is the project-local official toolchain tools/.toolchain/go-<GO_VERSION>-<os>-<arch>/ when
# bootstrap.sh installed it (GOROOT and PATH are set for this script only), else the go of PATH.
# Everything Go writes (module cache, build cache, GOPATH) stays under tools/.toolchain/go/ (git-ignored):
# nothing goes to ~/go or ~/.cache. VERSION=x.y.z sets the version shown by `akam version`.
# Release-grade actions (all, flat, repro) refuse any Go other than GO_VERSION of tools/build/deps.lock;
# AKAM_ALLOW_ANY_GO=1 overrides (the result is then not a release). `host` only warns.
# AKAM_TARGETS="linux/amd64 windows/arm64" restricts `all`, `flat` and `repro` to some targets.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck source=tools/lib/gotoolchain.sh
. "$REPO/tools/lib/gotoolchain.sh"
BUDGET_BYTES=$((12 * 1024 * 1024))
SOFT_BYTES=$((11 * 1024 * 1024))
VERSION=${VERSION:-1.0.0-dev}
ALL_TARGETS="linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64 windows/arm64"
TARGETS=${AKAM_TARGETS:-$ALL_TARGETS}

akam_use_pinned_go "$REPO" || true
if ! command -v go >/dev/null 2>&1; then
    echo "build.sh: Go is not installed (run: bash tools/build/bootstrap.sh, which installs the Go of GO_VERSION in tools/build/deps.lock)" >&2
    exit 127
fi
TC="$REPO/tools/.toolchain/go"
mkdir -p "$TC/path" "$TC/mod" "$TC/cache"
export GOPATH="$TC/path" GOMODCACHE="$TC/mod" GOCACHE="$TC/cache"
export GOFLAGS="-modcacherw -mod=readonly" CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off
cd "$HERE"
if ! go list -deps ./cmd/akam >/dev/null 2>&1; then
    echo "build.sh: the Go modules are not in tools/.toolchain/go/mod yet; run: bash tools/build/bootstrap.sh" >&2
    exit 1
fi

# The exact Go release pinned in tools/build/deps.lock (GO_VERSION).
pinned_go() { akam_pinned_go_version "$REPO"; }
have_go() { local v; v=$(go env GOVERSION); v=${v#go}; echo "${v%%[ -]*}"; }
check_go() { # strict (1: refuse, 0: warn)
    local want have
    want=$(pinned_go)
    have=$(have_go)
    [ "$have" = "$want" ] && return 0
    if [ "${AKAM_ALLOW_ANY_GO:-}" = 1 ]; then
        echo "build.sh: warning: Go $have, but the pinned toolchain is Go $want (AKAM_ALLOW_ANY_GO=1: not a release build)" >&2
        return 0
    fi
    if [ "$1" = 1 ]; then
        echo "build.sh: refusing to build: this is Go $have but tools/build/deps.lock pins Go $want." >&2
        echo "build.sh: run bash tools/build/bootstrap.sh (installs Go $want in tools/.toolchain/) or set AKAM_ALLOW_ANY_GO=1 for a non-release build." >&2
        exit 1
    fi
    echo "build.sh: warning: Go $have, the pinned toolchain is Go $want (sizes and hashes may differ)" >&2
}

exe() { if [ "$1" = windows ]; then echo .exe; fi; }

build_one() { # GOOS GOARCH OUT
    mkdir -p "$(dirname "$3")"
    # nethttpomithttp2: no bundled HTTP/2 client (-360 KiB measured); downloads use HTTP/1.1. Never -tags akamtest here.
    GOOS=$1 GOARCH=$2 go build -trimpath -buildvcs=false -tags nethttpomithttp2 \
        -ldflags "-s -w -X main.version=$VERSION" -o "$3" ./cmd/akam
}

mib() { awk -v s="$1" 'BEGIN{printf "%.2f", s/1048576}'; }

size_line() { # label file ; returns 1 above the hard budget
    local sz verdict=ok
    sz=$(wc -c <"$2" | tr -d ' ')
    [ "$sz" -le "$SOFT_BYTES" ] || verdict="WARN (soft limit 11 MiB)"
    [ "$sz" -le "$BUDGET_BYTES" ] || verdict=OVER
    printf '%-40s %9d bytes  %5s MiB  (budget 12 MiB: %s)\n' "$1" "$sz" "$(mib "$sz")" "$verdict"
    [ "$sz" -le "$BUDGET_BYTES" ]
}

sizes() {
    local f n=0 rc=0
    for f in "$HERE"/bin/akam "$HERE"/bin/*-*/akam "$HERE"/bin/*-*/akam.exe; do
        [ -f "$f" ] || continue
        n=$((n + 1))
        size_line "${f#"$REPO"/}" "$f" || rc=1
    done
    [ "$n" -gt 0 ] || { echo "no binary built yet" >&2; return 1; }
    return $rc
}

# Size per Go package from the symbol table of an UNSTRIPPED build (symbol sizes only: the sum is below the file size).
by_package() { # GOOS GOARCH
    local tmp
    tmp=$(mktemp -d)
    GOOS=$1 GOARCH=$2 go build -trimpath -buildvcs=false -tags nethttpomithttp2 -ldflags "-X main.version=$VERSION" -o "$tmp/akam" ./cmd/akam
    echo "size by package, $1/$2, unstripped build (symbol sizes only):"
    go tool nm -size "$tmp/akam" | awk '
        { sz = $2; name = $4
          if (sz !~ /^[0-9]+$/ || name == "" || $3 ~ /^[Bb]$/) next   # bss takes no room in the file
          if (name ~ /^(type:|go:)/) { pkg = "(type:/go: metadata)" }
          else {
              n = split(name, parts, "/"); last = parts[n]; d = index(last, ".")
              if (d == 0) { pkg = "(unnamed)" }
              else { pkg = substr(name, 1, length(name) - length(last)) substr(last, 1, d - 1) }
          }
          tot[pkg] += sz; all += sz }
        END { for (p in tot) printf "%9d %s\n", tot[p], p; printf "%9d %s\n", all, "TOTAL (symbols)" }' |
        sort -rn | head -n 25 | awk '{ printf "  %8.1f KiB  %s\n", $1/1024, $2 }'
    rm -rf "$tmp"
}

notices() {
    local t mods path ver rc=0 missing
    for t in $ALL_TARGETS; do
        mods=$(GOOS=${t%/*} GOARCH=${t#*/} go list -deps -f '{{with .Module}}{{if not .Main}}{{.Path}} {{.Version}}{{end}}{{end}}' ./cmd/akam | sort -u)
        missing=0
        while read -r path ver; do
            [ -n "$path" ] || continue
            if ! grep -F "$path" "$REPO/THIRD_PARTY_NOTICES.md" | grep -qF "$ver"; then
                echo "notices: $t links $path $ver, which THIRD_PARTY_NOTICES.md does not list (with that version)" >&2
                missing=1
            fi
        done <<<"$mods"
        echo "notices: $t: $(printf '%s\n' "$mods" | grep -c .) modules linked, $([ "$missing" = 0 ] && echo 'all listed' || echo 'SOME MISSING')"
        [ "$missing" = 0 ] || rc=1
    done
    return $rc
}

vet_all() { # [--link]
    local t tmp host
    host="$(go env GOOS)/$(go env GOARCH)"
    tmp=$(mktemp -d)
    for t in $ALL_TARGETS; do
        [ "$t" = "$host" ] && continue
        echo "vet-all: $t"
        GOOS=${t%/*} GOARCH=${t#*/} go vet -tags akamtest ./...
        # vet type-checks the test files too; --link also builds (links) every test binary (slow: about a minute)
        if [ "$1" = --link ]; then
            GOOS=${t%/*} GOARCH=${t#*/} go test -tags akamtest -c -o "$tmp/" ./... >/dev/null
            rm -rf "${tmp:?}"/*
        fi
    done
    rm -rf "$tmp"
}

repro_one() { # os/arch ; the second build uses a fresh build cache when AKAM_REPRO_FRESH=1
    local t=$1 tmp rc=0
    tmp=$(mktemp -d)
    build_one "${t%/*}" "${t#*/}" "$tmp/a"
    if [ "${AKAM_REPRO_FRESH:-}" = 1 ]; then
        GOCACHE="$tmp/cache" build_one "${t%/*}" "${t#*/}" "$tmp/b"
    else
        build_one "${t%/*}" "${t#*/}" "$tmp/b"
    fi
    if cmp -s "$tmp/a" "$tmp/b"; then
        echo "reproducible: $t $(sha256sum "$tmp/a" | cut -d' ' -f1)"
    else
        echo "NOT reproducible: $t: two builds differ" >&2
        rc=1
    fi
    chmod -R u+w "$tmp" 2>/dev/null || true
    rm -rf "$tmp"
    return $rc
}

case ${1:-host} in
    host)
        check_go 0
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$HERE/bin/akam"
        sizes
        ;;
    all)
        check_go 1
        for t in $TARGETS; do build_one "${t%/*}" "${t#*/}" "$HERE/bin/${t%/*}-${t#*/}/akam$(exe "${t%/*}")"; done
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$HERE/bin/akam"
        sizes
        ;;
    flat)
        [ $# -eq 2 ] || { echo "usage: build.sh flat DIR" >&2; exit 2; }
        check_go 1
        for t in $TARGETS; do build_one "${t%/*}" "${t#*/}" "$2/akam-${t%/*}-${t#*/}$(exe "${t%/*}")"; done
        ;;
    test)
        check_go 0
        go vet -tags akamtest ./...
        go test -tags akamtest -count=1 ./...
        # the downloads (kit, toolkit) must work with the release tag that drops the bundled HTTP/2 client
        go test -tags "akamtest nethttpomithttp2" -count=1 ./internal/kit ./internal/sdcard
        go test -count=1 ./internal/testhook   # release build: the test hooks are ignored
        ;;
    vet-all) check_go 0; vet_all "${2:-}" ;;
    notices) notices ;;
    sizes)
        if [ "${2:-}" = --by-package ]; then
            t=${3:-$(go env GOOS)/$(go env GOARCH)}
            by_package "${t%/*}" "${t#*/}"
        else
            sizes
        fi
        ;;
    repro)
        check_go 1
        shift
        rc=0
        list=("$@")
        [ ${#list[@]} -gt 0 ] || read -ra list <<<"$TARGETS"
        for t in "${list[@]}"; do repro_one "$t" || rc=1; done
        exit $rc
        ;;
    *)
        echo "usage: build.sh [host | all | flat DIR | test | vet-all | sizes [--by-package [os/arch]] | repro [os/arch ...] | notices]" >&2
        exit 2
        ;;
esac
