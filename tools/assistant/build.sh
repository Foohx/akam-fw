#!/usr/bin/env bash
# Builds the assistant: static (CGO_ENABLED=0), from the vendored modules, reproducible flags.
#
#   tools/assistant/build.sh             this computer's OS/arch  -> tools/assistant/bin/akam
#   tools/assistant/build.sh all         linux/amd64 and linux/arm64 -> bin/linux-<arch>/akam (and bin/akam)
#   tools/assistant/build.sh test        go vet + go test ./... (what tests/assistant_test.sh runs)
#   tools/assistant/build.sh sizes       sizes of the built binaries against the budget (12 MiB)
#   tools/assistant/build.sh repro       builds twice and compares the sha256
#
# Everything Go writes (module cache, build cache, GOPATH) stays under tools/.toolchain/go/ (git-ignored):
# nothing goes to ~/go or ~/.cache. VERSION=x.y.z sets the version shown by `akam version`.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUDGET_BYTES=$((12 * 1024 * 1024))
VERSION=${VERSION:-1.0.0-dev}

if ! command -v go >/dev/null 2>&1; then
    echo "build.sh: Go is not installed (need Go 1.26 or newer: https://go.dev/dl/)" >&2
    exit 127
fi
TC="$REPO/tools/.toolchain/go"
mkdir -p "$TC/path" "$TC/mod" "$TC/cache"
export GOPATH="$TC/path" GOMODCACHE="$TC/mod" GOCACHE="$TC/cache"
export GOFLAGS="-modcacherw -mod=vendor" CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off
cd "$HERE"

build_one() { # GOOS GOARCH OUT
    mkdir -p "$(dirname "$3")"
    GOOS=$1 GOARCH=$2 go build -trimpath -buildvcs=false \
        -ldflags "-s -w -X main.version=$VERSION" -o "$3" ./cmd/akam
}

sizes() {
    local f n=0
    for f in "$HERE"/bin/akam "$HERE"/bin/linux-*/akam; do
        [ -f "$f" ] || continue
        n=$((n + 1))
        local sz
        sz=$(wc -c <"$f" | tr -d ' ')
        printf '%-45s %9d bytes  %5.2f MiB  (budget 12 MiB: %s)\n' "${f#"$REPO"/}" "$sz" "$(awk -v s="$sz" 'BEGIN{printf "%.2f", s/1048576}')" \
            "$([ "$sz" -le "$BUDGET_BYTES" ] && echo ok || echo OVER)"
        [ "$sz" -le "$BUDGET_BYTES" ] || return 1
    done
    [ "$n" -gt 0 ] || { echo "no binary built yet" >&2; return 1; }
}

case ${1:-host} in
    host)
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$HERE/bin/akam"
        sizes
        ;;
    all)
        build_one linux amd64 "$HERE/bin/linux-amd64/akam"
        build_one linux arm64 "$HERE/bin/linux-arm64/akam"
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$HERE/bin/akam"
        sizes
        ;;
    test)
        go vet ./...
        go test -count=1 ./...
        ;;
    sizes) sizes ;;
    repro)
        tmp=$(mktemp -d)
        trap 'rm -rf "$tmp"' EXIT
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$tmp/a"
        build_one "$(go env GOOS)" "$(go env GOARCH)" "$tmp/b"
        if cmp -s "$tmp/a" "$tmp/b"; then
            echo "reproducible: $(sha256sum "$tmp/a" | cut -d' ' -f1)"
        else
            echo "NOT reproducible: two builds differ" >&2
            exit 1
        fi
        ;;
    *)
        echo "usage: build.sh [host | all | test | sizes | repro]" >&2
        exit 2
        ;;
esac
