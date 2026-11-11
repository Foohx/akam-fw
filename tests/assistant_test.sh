#!/usr/bin/env bash
# Tests of the Go assistant (tools/assistant): gofmt, go vet, go test ./... (fake camera, golden plain
# output compared with the bash wizard, state round trip with bash), a reproducible build, the size
# budget, and the repository rules (no symlink, no secret, no forbidden file name).
# Nothing here touches a camera, the microphone, the speaker or ffmpeg. Every Go cache stays under
# tools/.toolchain/go/ (git-ignored); the build uses the vendored modules and needs no network.
# Skipped with a clear message when Go 1.26 or newer is missing.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
ASSIST=$REPO/tools/assistant

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

# Same environment as build.sh: caches under tools/.toolchain/go, vendored modules, no network.
TC=$REPO/tools/.toolchain/go
mkdir -p "$TC/path" "$TC/mod" "$TC/cache"
export GOPATH="$TC/path" GOMODCACHE="$TC/mod" GOCACHE="$TC/cache"
export GOFLAGS="-modcacherw -mod=vendor" CGO_ENABLED=0 GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off

echo "go: $(go version)"
cd "$ASSIST" || exit 1

# gofmt on our own code (not the vendored modules)
unformatted=$(gofmt -l cmd internal 2>&1)
if [ -z "$unformatted" ]; then ok "gofmt: every file is formatted"; else ko "gofmt: $unformatted"; fi

if out=$(go vet ./... 2>&1); then ok "go vet ./..."; else ko "go vet ./..."; echo "$out" | tail -n 20; fi

LOG=$(mktemp)
SELFSTATE=$(mktemp -d)
BRIDGE=$(mktemp -d)
trap 'rm -rf "$LOG" "$SELFSTATE" "$BRIDGE"' EXIT
if go test -count=1 ./... >"$LOG" 2>&1; then
    ok "go test ./... ($(grep -c '^ok' "$LOG") packages)"
else
    ko "go test ./..."
    tail -n 40 "$LOG"
fi
if grep -q '^SKIP\|--- SKIP' "$LOG"; then echo "note: some Go tests were skipped on this host"; fi

if out=$("$ASSIST/build.sh" repro 2>&1); then ok "two builds give the same sha256"; else ko "reproducible build: $out"; fi
if out=$("$ASSIST/build.sh" host 2>&1); then ok "static build within the 12 MiB budget"; printf '        %s\n' "${out//$'\n'/$'\n'        }"; else ko "build: $out"; fi
if file "$ASSIST/bin/akam" 2>/dev/null | grep -q 'statically linked'; then ok "the binary is statically linked"; else
    if ldd "$ASSIST/bin/akam" 2>&1 | grep -q 'not a dynamic'; then ok "the binary is statically linked"; else ko "the binary is not static"; fi
fi
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
if [ -z "$(find tools/assistant -type l 2>/dev/null)" ]; then ok "no symlink under tools/assistant"; else ko "symlink under tools/assistant"; fi
if grep -rIl --exclude-dir=vendor -e 'BEGIN OPENSSH PRIVATE KEY' -e 'BEGIN RSA PRIVATE KEY' tools/assistant >/dev/null 2>&1; then
    ko "a private key is stored under tools/assistant"
else ok "no private key under tools/assistant"; fi
if [ -f tools/assistant/go.mod ] && git ls-files --error-unmatch tools/assistant/go.mod >/dev/null 2>&1 || ! git rev-parse --git-dir >/dev/null 2>&1; then
    ok "go.mod is tracked"
else ko "go.mod is not tracked (the .gitignore rule *.mod must not hide it)"; fi

echo "assistant tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
