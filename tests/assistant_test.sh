#!/usr/bin/env bash
# Tests of the Go assistant (tools/assistant): gofmt, go vet, go test ./... (fake camera, golden plain
# output compared with the bash wizard, state round trip with bash), a reproducible build, the size
# budget, and the repository rules (no symlink, no secret, no forbidden file name).
# Nothing here touches a camera, the microphone, the speaker or ffmpeg. Every Go cache stays under
# tools/.toolchain/go/ (git-ignored); the build uses the module cache filled by tools/build/bootstrap.sh and needs no network.
# Skipped with a clear message when Go 1.26 or newer is missing.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
ASSIST=$REPO/tools/assistant
# shellcheck source=tools/lib/gotoolchain.sh
. "$REPO/tools/lib/gotoolchain.sh"
# The project-local official Go (tools/build/bootstrap.sh) when installed, else the go of PATH (with a note below).
akam_use_pinned_go "$REPO" || true

if ! command -v go >/dev/null 2>&1; then
    echo "SKIP: Go is not installed (the assistant needs Go 1.26 or newer: https://go.dev/dl/)"
    exit 0
fi
gover=$(go env GOVERSION 2>/dev/null | sed -n 's/^go\([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
read -r gomaj gomin <<<"$gover"
if [ -z "${gomaj:-}" ] || [ "$gomaj" -lt 1 ] || { [ "$gomaj" -eq 1 ] && [ "${gomin:-0}" -lt 26 ]; }; then
    echo "SKIP: Go $(go env GOVERSION) is too old (the assistant needs Go 1.26 or newer)"
    exit 0
fi

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

# Same environment as build.sh: caches under tools/.toolchain/go, modules from the local cache (bootstrap.sh fills it), no network.
TC=$REPO/tools/.toolchain/go
mkdir -p "$TC/path" "$TC/mod" "$TC/cache"
export GOPATH="$TC/path" GOMODCACHE="$TC/mod" GOCACHE="$TC/cache"
export GOFLAGS="-modcacherw -mod=readonly" CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off

echo "go: $(go version)"
# build.sh refuses a Go other than the one pinned in tools/build/deps.lock; this test only needs the
# toolchain to be recent enough, so it accepts any (the guard itself is tested below with a fake go).
PINNED=$(sed -n 's/^GO_VERSION=\([0-9][0-9.]*\).*/\1/p' "$REPO/tools/build/deps.lock" | head -n 1)
HAVEGO=$(go env GOVERSION); HAVEGO=${HAVEGO#go}; HAVEGO=${HAVEGO%%[ -]*}
if [ "$HAVEGO" != "$PINNED" ]; then
    echo "note: Go $HAVEGO is not the pinned Go $PINNED (run tools/build/bootstrap.sh to install it): sizes and hashes below are not release figures"
    export AKAM_ALLOW_ANY_GO=1
fi
FAKEGO=$(mktemp -d)
cd "$ASSIST" || exit 1

# gofmt on our own code 
unformatted=$(gofmt -l cmd internal 2>&1)
if [ -z "$unformatted" ]; then ok "gofmt: every file is formatted"; else ko "gofmt: $unformatted"; fi

if out=$(go vet -tags akamtest ./... 2>&1); then ok "go vet ./... (akamtest)"; else ko "go vet ./..."; echo "$out" | tail -n 20; fi

LOG=$(mktemp)
SELFSTATE=$(mktemp -d)
BRIDGE=$(mktemp -d)
REL1=$(mktemp -d)
REL2=$(mktemp -d)
trap 'rm -rf "$LOG" "$SELFSTATE" "$BRIDGE" "$FAKEGO" "$REL1" "$REL2"' EXIT
if go test -tags akamtest -count=1 ./... >"$LOG" 2>&1 && go test -count=1 ./internal/testhook >>"$LOG" 2>&1; then
    ok "go test ./... ($(grep -c '^ok' "$LOG") packages)"
else
    ko "go test ./..."
    tail -n 40 "$LOG"
fi
if grep -q '^SKIP\|--- SKIP' "$LOG"; then echo "note: some Go tests were skipped on this host"; fi

# The pinned toolchain is enforced: a different `go version` is refused unless AKAM_ALLOW_ANY_GO=1
# shellcheck disable=SC2016
printf '#!/bin/sh\nif [ "$1" = env ] && [ "$2" = GOVERSION ]; then echo go1.0.0; exit 0; fi\nexec "%s" "$@"\n' "$(command -v go)" >"$FAKEGO/go"
chmod +x "$FAKEGO/go"
out=$(PATH="$FAKEGO:$PATH" AKAM_GO_ROOT=/nonexistent env -u AKAM_ALLOW_ANY_GO "$ASSIST/build.sh" flat "$FAKEGO/out" 2>&1)
rc=$?
if [ "$rc" -ne 0 ] && [[ $out == *"pins Go $PINNED"* ]] && [ ! -e "$FAKEGO/out" ]; then ok "a Go other than the pinned one is refused with a clear message"; else ko "pinned Go guard: rc=$rc $out"; fi

# The six targets, release flags: sizes against the budget (12 MiB hard, 11 MiB soft), static/format check, flags
if out=$("$ASSIST/build.sh" all 2>&1); then ok "six targets built, all within the 12 MiB budget"; printf '        %s\n' "${out//$'\n'/$'\n'        }"; else ko "build all: $out"; fi
for t in linux/amd64 linux/arm64 darwin/amd64 darwin/arm64 windows/amd64 windows/arm64; do
    os=${t%/*}; arch=${t#*/}; f=$ASSIST/bin/$os-$arch/akam; [ "$os" = windows ] && f=$f.exe
    info=$(go version -m "$f" 2>&1)
    flags_ok=1
    [[ $info == *GOOS=$os* && $info == *GOARCH=$arch* && $info == *CGO_ENABLED=0* && $info == *-trimpath=true* && $info != *akamtest* ]] || flags_ok=0
    desc=$(file "$f" 2>/dev/null)
    fmt_ok=0
    case $os in
        linux) [[ $desc == *ELF* && $desc == *"statically linked"* ]] && fmt_ok=1 ;;
        darwin) [[ $desc == *Mach-O* ]] && fmt_ok=1 ;;
        windows) [[ $desc == *PE32+* ]] && fmt_ok=1 ;;
    esac
    # file(1) may be missing or old: fall back to the magic bytes
    if [ "$fmt_ok" = 0 ]; then
        magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
        case $os:$magic in linux:7f454c46 | windows:4d5a* | darwin:cffaedfe) fmt_ok=1 ;; esac
    fi
    if [ "$flags_ok" = 1 ] && [ "$fmt_ok" = 1 ]; then ok "$t: right executable format, CGO off, -trimpath, no test hooks"; else ko "$t: format/flags ($desc) $info"; fi
done
if file "$ASSIST/bin/akam" 2>/dev/null | grep -q 'statically linked' || ldd "$ASSIST/bin/akam" 2>&1 | grep -q 'not a dynamic'; then ok "the host binary is statically linked"; else ko "the host binary is not static"; fi

# Two builds of every target give the same bytes (the Go build cache is reused: link output is compared)
if out=$("$ASSIST/build.sh" repro 2>&1); then ok "two builds of each of the six targets give the same sha256"; else ko "reproducible build: $out"; fi
if out=$("$ASSIST/build.sh" notices 2>&1); then ok "THIRD_PARTY_NOTICES.md lists every module linked into each target"; else ko "notices: $out"; fi
if out=$("$ASSIST/build.sh" vet-all 2>&1); then ok "go vet (akamtest) of the five other targets, test files included"; else ko "vet-all: $out"; fi

# WASAPI: a buffer whose address goes to COM must live on the heap (a stack move would leave the driver reading freed memory).
# The compiler's escape analysis must report no make/append in wasapi_windows.go as "does not escape".
if esc=$(GOOS=windows GOARCH=amd64 go build -gcflags=-m ./internal/audio 2>&1); then
    if grep -q 'wasapi_windows\.go:[0-9]*:[0-9]*: .*escapes to heap' <<<"$esc" && ! grep -E 'wasapi_windows\.go:[0-9]+:[0-9]+: .*(make|append).* does not escape' <<<"$esc" | grep -q .; then
        ok "wasapi_windows.go: no make/append buffer stays on the stack (escape analysis)"
    else ko "wasapi_windows.go: a make/append buffer does not escape (stack buffer passed to COM?) or the escape output is empty"; fi
else ko "windows escape-analysis build: $esc"; fi

# The release script refuses a dirty tree and an unpinned Go
if [ -n "$(git -C "$REPO" status --porcelain 2>/dev/null)" ]; then
    out=$(env -u AKAM_ALLOW_DIRTY AKAM_TARGETS=linux/amd64 "$REPO/tools/release/build_assistant.sh" --out "$REL2" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && [[ $out == *"tree is not clean"* ]]; then ok "the release script refuses a dirty tree"; else ko "dirty tree not refused: rc=$rc $out"; fi
fi
out=$(PATH="$FAKEGO:$PATH" AKAM_GO_ROOT=/nonexistent env -u AKAM_ALLOW_ANY_GO AKAM_ALLOW_DIRTY=1 "$REPO/tools/release/build_assistant.sh" --out "$REL2" 2>&1); rc=$?
if [ "$rc" -ne 0 ] && [[ $out == *"pins Go $PINNED"* ]]; then ok "the release script refuses a Go other than the pinned one"; else ko "release script, unpinned Go: rc=$rc $out"; fi

# The release script, twice, on this tree (two targets only: all six are built by the checks above and by the maintainer)
export AKAM_ALLOW_DIRTY=1 AKAM_TARGETS="linux/amd64 windows/arm64" AKAM_VERSION=0.0.0-test SOURCE_DATE_EPOCH=1700000000
if "$REPO/tools/release/build_assistant.sh" --out "$REL1" >/dev/null 2>&1 && "$REPO/tools/release/build_assistant.sh" --out "$REL2" >/dev/null 2>&1 \
    && cmp -s "$REL1/SHA256SUMS" "$REL2/SHA256SUMS" && (cd "$REL1" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
    ok "the release script run twice gives identical files and a valid SHA256SUMS"
else ko "release script (twice, compared)"; fi
listing=$(tar tvzf "$REL1/akam-linux-amd64.tar.gz" --numeric-owner 2>/dev/null)
if [[ $listing =~ -rwxr-xr-x[[:space:]]+0/0[[:space:]].*akam-linux-amd64/akam ]] && [ "$(grep -c ' 0/0 ' <<<"$listing")" -eq "$(grep -c . <<<"$listing")" ] \
    && ! grep -aqF -e "$REPO" "$REL1/akam-linux-amd64" "$REL1/akam-linux-amd64.tar.gz" "$REL1/akam-windows-arm64.exe" \
    && [ ! -s "$REL1/SHA256SUMS.sig" ]; then ok "archives: exec bit kept, owner 0/0, no checkout path in any file"; else ko "archive content"; fi
unset AKAM_ALLOW_DIRTY AKAM_TARGETS AKAM_VERSION SOURCE_DATE_EPOCH

if v=$("$ASSIST/bin/akam" version 2>&1) && [ "${v%% *}" = akam ]; then ok "akam version: $v"; else ko "akam version: $v"; fi
if out=$("$ASSIST/bin/akam" self-test --state-dir "$SELFSTATE" 2>&1 </dev/null); then ok "akam self-test"; else ko "akam self-test: $out"; fi

# The AKAM_USE_BIN bridge of the bash ./akam hands the same arguments to the binary
printf '#!/bin/sh\necho "native:$*"\n' >"$BRIDGE/akam-fake"
chmod +x "$BRIDGE/akam-fake"
out=$(AKAM_USE_BIN=1 AKAM_BIN="$BRIDGE/akam-fake" bash "$REPO/akam" --camera salon status </dev/null 2>&1)
if [ "$out" = "native:--camera salon status" ]; then ok "AKAM_USE_BIN=1 ./akam execs the binary with the same arguments"; else ko "bridge: $out"; fi
out=$(AKAM_USE_BIN=1 AKAM_BIN="$BRIDGE/none" bash "$REPO/akam" status </dev/null 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && [[ $out == *"native binary is not built"* ]]; then ok "AKAM_USE_BIN=1 without a binary fails clearly"; else ko "bridge, no binary: rc=$rc $out"; fi
out=$(AKAM_USE_BIN='' bash "$REPO/akam" --no-color --state-dir "$BRIDGE/st" status </dev/null 2>&1)
if [[ $out == *"Akam setup progress"* ]]; then ok "without AKAM_USE_BIN the bash wizard runs as before"; else ko "bash status: $out"; fi
out=$(AKAM_USE_BIN=1 AKAM_BIN="$ASSIST/bin/akam" bash "$REPO/akam" version </dev/null 2>&1)
if [[ $out == akam\ * ]]; then ok "the bridge runs the real binary: $out"; else ko "bridge, real binary: $out"; fi

# Repository rules for what was added under tools/assistant
cd "$REPO" || exit 1
# guard logic stays pure and shared: no OS-specific (//go:build or _GOOS.go) file calls Evaluate(
evalbad=$(grep -rlE 'Evaluate\(' tools/assistant --include='*.go' 2>/dev/null | while read -r gf; do
    if grep -qE '^//go:build .*(linux|windows|darwin)' "$gf" || [[ $gf =~ _(linux|windows|darwin)(_test)?\.go$ ]]; then echo "$gf"; fi
done)
if [ -z "$evalbad" ]; then ok "no OS-specific file contains Evaluate("; else ko "OS-specific file(s) call Evaluate(: $evalbad"; fi
ign=1
for p in tools/assistant/bin/akam tools/assistant/bin/windows-amd64/akam.exe dist/assistant/SHA256SUMS dist/assistant/akam-linux-amd64.tar.gz; do
    git check-ignore -q "$p" 2>/dev/null || { ign=0; echo "not ignored: $p"; }
done
if [ "$ign" = 1 ]; then ok "build outputs (bin/, dist/) are git-ignored"; else ko "bin/ or dist/ outputs are not git-ignored"; fi
if [ -z "$(find tools/assistant -type l 2>/dev/null)" ]; then ok "no symlink under tools/assistant"; else ko "symlink under tools/assistant"; fi
if grep -rIl -e 'BEGIN OPENSSH PRIVATE KEY' -e 'BEGIN RSA PRIVATE KEY' tools/assistant >/dev/null 2>&1; then
    ko "a private key is stored under tools/assistant"
else ok "no private key under tools/assistant"; fi
if [ -f tools/assistant/go.mod ] && git ls-files --error-unmatch tools/assistant/go.mod >/dev/null 2>&1 || ! git rev-parse --git-dir >/dev/null 2>&1; then
    ok "go.mod is tracked"
else ko "go.mod is not tracked (the .gitignore rule *.mod must not hide it)"; fi

echo "assistant tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
