#!/usr/bin/env bash
# Tests for tools/build/{check_host,bootstrap,env}.sh and host_arm_gdb: simulated Arch / Debian /
# Ubuntu (AKAM_OS_RELEASE), --print-plan, offline mode and retry logic with a fake curl.
# Nothing is downloaded and nothing system-wide is touched.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BASH_BIN=$(command -v bash)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }
check() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}
contains() { # name haystack needle
    case $2 in *"$3"*) ok "$1" ;; *) bad "$1 (missing '$3' in: $2)" ;; esac
}
lacks() { # name haystack needle
    case $2 in *"$3"*) bad "$1 (unexpected '$3' in: $2)" ;; *) ok "$1" ;; esac
}

osrel() { # lines... -> sets AKAM_OS_RELEASE
    printf '%s\n' "$@" >"$WORK/os-release"
    export AKAM_OS_RELEASE=$WORK/os-release
}

# ---------------------------------------------------------------- check_host
# A restricted PATH holding only the basics check_host.sh itself needs, plus stubs we choose.
mkstub() { # dir
    local d=$1 t
    rm -rf "$d"
    mkdir -p "$d"
    for t in dirname uname grep cat; do ln -s "$(command -v "$t")" "$d/$t"; done
}
stub() { # dir tool [script body]
    printf '#!/bin/sh\n%s\n' "${3:-exit 0}" >"$1/$2"
    chmod 755 "$1/$2"
}
run_host() { # dir args... -> stdout+stderr in $OUT, rc in $RC
    local d=$1
    shift
    OUT=$(AKAM_SBIN_DIRS="" PATH=$d "$BASH_BIN" "$REPO/tools/build/check_host.sh" "$@" 2>&1)
    RC=$?
}

S=$WORK/stub
mkstub "$S"

osrel 'ID=arch'
run_host "$S" --profile kit
check "arch: missing tools -> exit 1" 1 "$RC"
contains "arch: pacman command" "$OUT" "sudo pacman -S --needed"
contains "arch: xz package" "$OUT" "xz"
contains "arch: openssh package" "$OUT" "openssh"
lacks "arch: never apt" "$OUT" "apt-get"

osrel 'ID=debian' 'VERSION_ID="12"'
run_host "$S" --profile all
contains "debian: apt command" "$OUT" "sudo apt-get install -y"
contains "debian: xz-utils" "$OUT" "xz-utils"
contains "debian: openssh-client" "$OUT" "openssh-client"
contains "debian: gdb-multiarch" "$OUT" "gdb-multiarch"
contains "debian: dosfstools" "$OUT" "dosfstools"
contains "debian: squashfs-tools" "$OUT" "squashfs-tools"
contains "debian: gdb hard requirement note" "$OUT" "hard requirement"
lacks "debian: never pacman" "$OUT" "pacman"

osrel 'ID=ubuntu' 'ID_LIKE=debian' 'VERSION_ID="24.04"'
run_host "$S" --profile build
contains "ubuntu: apt command" "$OUT" "sudo apt-get install -y"
lacks "build profile: no shellcheck" "$OUT" "shellcheck"
run_host "$S" --profile kit
lacks "kit profile: no make" "$OUT" "make"
contains "kit profile: curl" "$OUT" "curl"

osrel 'ID=fedora'
run_host "$S" --profile kit
contains "unknown distro warning" "$OUT" "unsupported distribution"

# JSON mode is machine readable (validated with python3 when available).
osrel 'ID=ubuntu'
OUT=$(AKAM_SBIN_DIRS="" PATH=$S "$BASH_BIN" "$REPO/tools/build/check_host.sh" --json --profile kit 2>/dev/null)
RC=$?
check "json: exit 1 when missing" 1 "$RC"
contains "json: os" "$OUT" '"os":"ubuntu"'
contains "json: ok false" "$OUT" '"ok":false'
contains "json: install command" "$OUT" '"install_required":"sudo apt-get install -y '
contains "json: optional listed" "$OUT" '"name":"telnet","level":"optional"'
if command -v python3 >/dev/null 2>&1; then
    if printf '%s' "$OUT" | python3 -I -m json.tool >/dev/null 2>&1; then ok "json: valid"; else bad "json: valid"; fi
else
    echo "skip: python3 absent, JSON syntax not validated"
fi

# Every tool present: exit 0, ok true, no install command. A plain (non-multiarch) gdb is rejected.
for t in curl tar xz sha256sum mksquashfs unsquashfs ssh ssh-keygen lsblk wipefs sfdisk blockdev \
    mkfs.vfat partprobe make git bzip2 file readelf cmake python3 telnet shellcheck; do
    stub "$S" "$t"
done
stub "$S" gdb 'case "$*" in *"set architecture arm"*) exit 1;; esac; exit 0'
osrel 'ID=debian'
run_host "$S" --json
check "plain gdb without ARM: exit 1" 1 "$RC"
contains "plain gdb rejected -> gdb-multiarch" "$OUT" 'gdb-multiarch'
stub "$S" gdb-multiarch
run_host "$S" --json
check "gdb-multiarch accepted: exit 0" 0 "$RC"
contains "all present: ok true" "$OUT" '"ok":true'
contains "all present: empty install" "$OUT" '"install_required":""'
rm -f "$S/telnet" "$S/cmake"
run_host "$S"
check "optional missing: still exit 0" 0 "$RC"
contains "optional missing is reported" "$OUT" "optional tools not found: telnet cmake"
contains "optional install command" "$OUT" "Optional install:   sudo apt-get install -y telnet cmake"
osrel 'ID=arch'
run_host "$S"
contains "arch optional: inetutils" "$OUT" "sudo pacman -S --needed inetutils cmake"

# check_time32.sh needs an ARM-capable gdb (no objdump fallback): clear error otherwise.
T=$WORK/t32
mkstub "$T"
stub "$T" readelf
stub "$T" awk
stub "$T" gdb 'exit 1'
OUT=$(PATH=$T "$BASH_BIN" "$REPO/tools/build/check_time32.sh" /bin/true 2>&1)
check "check_time32 without ARM gdb: exit 2" 2 "$?"
contains "check_time32 without ARM gdb: hint" "$OUT" "no ARM-capable gdb"

# ---------------------------------------------------------------- bootstrap --print-plan
plan() { OUT=$(AKAM_HOST_ARCH=$1 "$REPO/tools/build/bootstrap.sh" --print-plan 2>&1); RC=$?; }
# shellcheck source=tools/build/deps.lock
. "$REPO/tools/build/deps.lock"
check "x86_64 zig hash is the historical one" \
    24aeeec8af16c381934a6cd7d95c807a8cb2cf7df9fa40d359aa884195c4716c "$ZIG_SHA256_linux_x86_64"
plan x86_64
check "plan x86_64 exit" 0 "$RC"
contains "plan x86_64 url" "$OUT" "zig-x86_64-linux-0.14.1.tar.xz"
contains "plan x86_64 sha" "$OUT" "$ZIG_SHA256_linux_x86_64"
contains "plan x86_64 install dir" "$OUT" "zig-0.14.1-x86_64-linux"
plan aarch64
check "plan aarch64 exit" 0 "$RC"
contains "plan aarch64 url" "$OUT" "zig-aarch64-linux-0.14.1.tar.xz"
contains "plan aarch64 sha" "$OUT" "$ZIG_SHA256_linux_aarch64"
contains "plan aarch64 busybox skipped" "$OUT" "busybox:    skipped"
plan arm64
contains "arm64 is aarch64" "$OUT" "linux/aarch64"
plan riscv64
check "plan unknown arch fails" 1 "$RC"
contains "plan unknown arch message" "$OUT" "no pinned Zig for linux/riscv64"

# ---------------------------------------------------------------- bootstrap in a sandbox
ROOT=$WORK/root
FAKE=$WORK/fake
mkdir -p "$ROOT/tools/build" "$ROOT/tools/lib" "$ROOT/tools/assistant" "$FAKE/files" "$WORK/art"
cp "$REPO/tools/build/bootstrap.sh" "$ROOT/tools/build/"
cp "$REPO/tools/lib/host.sh" "$REPO/tools/lib/gotoolchain.sh" "$ROOT/tools/lib/"
printf 'module fake\n\ngo 1.26.0\n' >"$ROOT/tools/assistant/go.mod"

# Fixture artifacts.
mkdir -p "$WORK/art/zig-x86_64-linux-0.14.1"
printf '#!/bin/sh\necho 0.14.1\n' >"$WORK/art/zig-x86_64-linux-0.14.1/zig"
chmod 755 "$WORK/art/zig-x86_64-linux-0.14.1/zig"
tar -cJf "$FAKE/files/zig-x86_64-linux-0.14.1.tar.xz" -C "$WORK/art" zig-x86_64-linux-0.14.1
for n in slice99 metalang99 datatype99 interface99; do
    mkdir -p "$WORK/art/$n-1"
    echo "$n" >"$WORK/art/$n-1/README"
    tar -czf "$FAKE/files/$n.tar.gz" -C "$WORK/art" "$n-1"
done
echo dropbear >"$FAKE/files/dropbear.tar.bz2"
# A fake official Go archive: top-level go/, bin/go answers `env GOVERSION` and records `mod download`.
mkdir -p "$WORK/art/go/bin"
cat >"$WORK/art/go/bin/go" <<'GOEOF'
#!/bin/sh
case "$1 $2" in
    "env GOVERSION") echo go1.27.4 ;;
    "mod download") echo "GOROOT=$GOROOT TOOLCHAIN=$GOTOOLCHAIN" >"$FAKE_DIR/moddl" ;;
esac
exit 0
GOEOF
chmod 755 "$WORK/art/go/bin/go"
tar -czf "$FAKE/files/go1.27.4.linux-amd64.tar.gz" -C "$WORK/art" go
printf '#!/bin/sh\necho busybox\n' >"$FAKE/files/busybox"
sha() { sha256sum "$1" | cut -d' ' -f1; }
# A local git repository stands in for SmolRTSP.
SMOL=$WORK/smol.git
mkdir -p "$SMOL"
git -C "$SMOL" init -q
echo x >"$SMOL/f"; echo build/ >"$SMOL/.gitignore"
git -C "$SMOL" add f .gitignore
git -C "$SMOL" -c user.name=t -c user.email=t@t commit -q -m init
SMOL_SHA=$(git -C "$SMOL" rev-parse HEAD)
cat >"$WORK/deps.lock" <<EOF
ZIG_VERSION=0.14.1
ZIG_URL_linux_x86_64=https://fake.invalid/zig-x86_64-linux-0.14.1.tar.xz
ZIG_SHA256_linux_x86_64=$(sha "$FAKE/files/zig-x86_64-linux-0.14.1.tar.xz")
GO_VERSION=1.27.4
GO_URL_linux_amd64=https://fake.invalid/go1.27.4.linux-amd64.tar.gz
GO_SHA256_linux_amd64=$(sha "$FAKE/files/go1.27.4.linux-amd64.tar.gz")
SMOLRTSP_URL=$SMOL
SMOLRTSP_COMMIT=$SMOL_SHA
SLICE99_TAG=v1
SLICE99_URL=https://fake.invalid/slice99.tar.gz
SLICE99_SHA256=$(sha "$FAKE/files/slice99.tar.gz")
METALANG99_TAG=v1
METALANG99_URL=https://fake.invalid/metalang99.tar.gz
METALANG99_SHA256=$(sha "$FAKE/files/metalang99.tar.gz")
DATATYPE99_TAG=v1
DATATYPE99_URL=https://fake.invalid/datatype99.tar.gz
DATATYPE99_SHA256=$(sha "$FAKE/files/datatype99.tar.gz")
INTERFACE99_TAG=v1
INTERFACE99_URL=https://fake.invalid/interface99.tar.gz
INTERFACE99_SHA256=$(sha "$FAKE/files/interface99.tar.gz")
DROPBEAR_VERSION=1
DROPBEAR_URL=https://fake.invalid/dropbear.tar.bz2
DROPBEAR_SHA256=$(sha "$FAKE/files/dropbear.tar.bz2")
BUSYBOX_VERSION=1
BUSYBOX_URL=https://fake.invalid/busybox
BUSYBOX_SHA256=$(sha "$FAKE/files/busybox")
EOF

# Fake curl: serves $FAKE/files/<basename>. Attempt N answers with line N of $FAKE/seq (an HTTP
# code, "000" = network error) and 200 once the sequence is exhausted. Counts calls in $FAKE/count.
cat >"$FAKE/curl" <<'EOF'
#!/bin/sh
out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out=$2; shift ;;
        -w) shift ;;
        https://*) url=$1 ;;
    esac
    shift
done
n=$(cat "$FAKE_DIR/count" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" >"$FAKE_DIR/count"
code=$(sed -n "${n}p" "$FAKE_DIR/seq" 2>/dev/null)
[ -n "$code" ] || code=200
case "$code" in
    200) cp "$FAKE_DIR/files/$(basename "$url")" "$out"; printf 200 ;;
    000) printf 000; exit 6 ;;
    *) : >"$out"; printf '%s' "$code" ;;
esac
EOF
chmod 755 "$FAKE/curl"

boot() { # args... ; env: SEQ (newline-separated codes)
    rm -f "$FAKE/count"
    printf '%b' "${SEQ:-}" >"$FAKE/seq"
    OUT=$(FAKE_DIR=$FAKE AKAM_CURL=$FAKE/curl DEPS_LOCK=$WORK/deps.lock AKAM_HOST_ARCH=x86_64 \
        AKAM_RETRY_SLEEP=0 AKAM_RETRY_MAX="${MAXTRY:-5}" "$ROOT/tools/build/bootstrap.sh" "$@" 2>&1)
    RC=$?
    CALLS=$(cat "$FAKE/count" 2>/dev/null || echo 0)
}

# 404 is permanent: no retry.
SEQ='404\n' boot
check "404: fails" 1 "$RC"
check "404: single attempt" 1 "$CALLS"
contains "404: message" "$OUT" "HTTP 404"

# Retries exhausted.
SEQ='503\n503\n503\n' MAXTRY=3 boot
check "5xx forever: fails" 1 "$RC"
check "5xx forever: 3 attempts" 3 "$CALLS"
contains "5xx forever: message" "$OUT" "after 3 attempts"

# Transient 502 / 503 / network error then success.
SEQ='502\n503\n000\n' boot
check "transient errors: succeeds" 0 "$RC"
contains "transient: retry logged" "$OUT" "retry 3/5"
if [ "$CALLS" -ge 9 ]; then ok "transient: all artifacts downloaded ($CALLS curl calls)"; else bad "transient: curl calls $CALLS"; fi
if [ -x "$ROOT/tools/.toolchain/zig-0.14.1-x86_64-linux/zig" ]; then ok "zig installed in per-arch dir"; else bad "zig installed in per-arch dir"; fi
check "smolrtsp checked out" "$SMOL_SHA" "$(git -C "$ROOT/src/smolrtsp" rev-parse HEAD)"

# Idempotent: nothing to fetch the second time.
SEQ='' boot
check "second run: ok" 0 "$RC"
check "second run: no download" 0 "$CALLS"

# The official Go is installed project-local, used for the module download, and the plan shows it.
GOD=$ROOT/tools/.toolchain/go-1.27.4-linux-amd64
if [ -x "$GOD/bin/go" ]; then ok "go installed in tools/.toolchain/go-<ver>-<os>-<arch>"; else bad "go installed in tools/.toolchain/go-<ver>-<os>-<arch>"; fi
contains "go modules fetched with the project-local go" "$(cat "$FAKE/moddl" 2>/dev/null)" "GOROOT=$GOD TOOLCHAIN=local"
if [ -d "$ROOT/go" ] || [ -n "$(find "$ROOT/tools/.toolchain" -maxdepth 1 -name '.extract.*')" ]; then bad "go install leaves no stray directory"; else ok "go install leaves no stray directory"; fi
boot --print-plan
contains "plan shows the Go version" "$OUT" "go:         1.27.4"
contains "plan shows the Go url" "$OUT" "https://fake.invalid/go1.27.4.linux-amd64.tar.gz"
contains "plan shows the Go sha256" "$OUT" "$(sha "$FAKE/files/go1.27.4.linux-amd64.tar.gz")"
contains "plan shows the Go install dir" "$OUT" "install:  $GOD (installed)"
# A wrong archive hash aborts and installs nothing.
rm -rf "$GOD" "$ROOT/tools/.cache/go1.27.4.linux-amd64.tar.gz"
cp "$FAKE/files/go1.27.4.linux-amd64.tar.gz" "$WORK/go.good"
echo corrupt >"$FAKE/files/go1.27.4.linux-amd64.tar.gz"
SEQ='' boot
check "go sha mismatch: fails" 1 "$RC"
contains "go sha mismatch: message" "$OUT" "sha256 mismatch"
if [ -e "$GOD" ]; then bad "go sha mismatch: nothing installed"; else ok "go sha mismatch: nothing installed"; fi
cp "$WORK/go.good" "$FAKE/files/go1.27.4.linux-amd64.tar.gz"
SEQ='' boot
check "go reinstalled after the fix" 0 "$RC"
# Offline: the cached archive is enough; no curl call.
rm -rf "$GOD"
SEQ='' boot --offline
check "offline: go installed from the cache" 0 "$RC"
check "offline: go, no curl call" 0 "$CALLS"
if [ -x "$GOD/bin/go" ]; then ok "offline: go present"; else bad "offline: go present"; fi
# An archive that is not the pinned version is refused.
rm -rf "$GOD"
sed -i 's/go1.27.4/go1.27.9/' "$WORK/art/go/bin/go"
tar -czf "$FAKE/files/go1.27.4.linux-amd64.tar.gz" -C "$WORK/art" go
sed -i "s/^GO_SHA256_linux_amd64=.*/GO_SHA256_linux_amd64=$(sha "$FAKE/files/go1.27.4.linux-amd64.tar.gz")/" "$WORK/deps.lock"
SEQ='' boot
check "wrong Go version in the archive: fails" 1 "$RC"
contains "wrong Go version: message" "$OUT" "is not Go 1.27.4"
if [ -e "$GOD" ]; then bad "wrong Go version: nothing installed"; else ok "wrong Go version: nothing installed"; fi
sed -i 's/go1.27.9/go1.27.4/' "$WORK/art/go/bin/go"
tar -czf "$FAKE/files/go1.27.4.linux-amd64.tar.gz" -C "$WORK/art" go
sed -i "s/^GO_SHA256_linux_amd64=.*/GO_SHA256_linux_amd64=$(sha "$FAKE/files/go1.27.4.linux-amd64.tar.gz")/" "$WORK/deps.lock"
SEQ='' boot
check "go back to the right archive" 0 "$RC"

# Sha mismatch from the server aborts.
rm -rf "$ROOT/tools/.cache" "$ROOT/tools/.toolchain/zig-0.14.1-x86_64-linux"
cp "$FAKE/files/zig-x86_64-linux-0.14.1.tar.xz" "$WORK/zig.good"
echo corrupt >"$FAKE/files/zig-x86_64-linux-0.14.1.tar.xz"
SEQ='' boot
check "sha mismatch: fails" 1 "$RC"
contains "sha mismatch: message" "$OUT" "sha256 mismatch"
cp "$WORK/zig.good" "$FAKE/files/zig-x86_64-linux-0.14.1.tar.xz"

# Offline mode: empty cache fails cleanly, populated cache works without curl.
SEQ='' boot --offline
check "offline, empty cache: fails" 1 "$RC"
contains "offline, empty cache: message" "$OUT" "offline:"
check "offline, empty cache: no curl call" 0 "$CALLS"
rm -rf "$ROOT/src/smolrtsp/build"
SEQ='' boot
check "online repopulate" 0 "$RC"
rm -rf "$ROOT/tools/.toolchain/zig-0.14.1-x86_64-linux" "$ROOT/src/smolrtsp/build"
SEQ='' boot --offline
check "offline, full cache: ok" 0 "$RC"
check "offline, full cache: no curl call" 0 "$CALLS"
if [ -x "$ROOT/tools/.toolchain/zig-0.14.1-x86_64-linux/zig" ]; then ok "offline: zig reinstalled from cache"; else bad "offline: zig reinstalled from cache"; fi
# AKAM_OFFLINE=1 is equivalent.
rm -rf "$ROOT/tools/.toolchain/zig-0.14.1-x86_64-linux"
OUT=$(AKAM_OFFLINE=1 FAKE_DIR=$FAKE AKAM_CURL=/nonexistent DEPS_LOCK=$WORK/deps.lock AKAM_HOST_ARCH=x86_64 \
    "$ROOT/tools/build/bootstrap.sh" 2>&1)
check "AKAM_OFFLINE=1" 0 "$?"

# aarch64: busybox skipped, aarch64 archive required (not in the fixture lock -> clean error).
OUT=$(AKAM_HOST_ARCH=aarch64 DEPS_LOCK=$WORK/deps.lock "$ROOT/tools/build/bootstrap.sh" 2>&1)
check "aarch64 without pin: fails" 1 "$?"
contains "aarch64 without pin: message" "$OUT" "no pinned Zig for linux/aarch64"

# ---------------------------------------------------------------- env.sh
EROOT=$WORK/envroot
mkdir -p "$EROOT/tools/build" "$EROOT/tools/lib"
cp "$REPO/tools/build/env.sh" "$REPO/tools/build/deps.lock" "$EROOT/tools/build/"
cp "$REPO/tools/lib/host.sh" "$EROOT/tools/lib/"
BBV=$(sed -n 's/^BUSYBOX_VERSION=//p' "$REPO/tools/build/deps.lock")
mkdir -p "$EROOT/tools/.toolchain/busybox-$BBV-x86_64"
printf '#!/bin/sh\nexit 0\n' >"$EROOT/tools/.toolchain/busybox-$BBV-x86_64/busybox"
chmod 755 "$EROOT/tools/.toolchain/busybox-$BBV-x86_64/busybox"
# shellcheck disable=SC2016  # the snippet runs in the child shell
OUT=$(AKAM_HOST_ARCH=x86_64 bash -c '. "$1"; rc=$?; echo "rc=$rc bb=${AKAM_BUSYBOX:-} zig=${ZIG:-}"' _ "$EROOT/tools/build/env.sh" 2>&1)
contains "env.sh without zig: still exports AKAM_BUSYBOX" "$OUT" "bb=$EROOT/tools/.toolchain/busybox-$BBV-x86_64/busybox"
contains "env.sh without zig: returns 1" "$OUT" "rc=1"
contains "env.sh without zig: reports it" "$OUT" "pinned Zig not found"
ZV=$(sed -n 's/^ZIG_VERSION=//p' "$REPO/tools/build/deps.lock")
mkdir -p "$EROOT/tools/.toolchain/zig-$ZV-x86_64-linux"
printf '#!/bin/sh\nexit 0\n' >"$EROOT/tools/.toolchain/zig-$ZV-x86_64-linux/zig"
chmod 755 "$EROOT/tools/.toolchain/zig-$ZV-x86_64-linux/zig"
# shellcheck disable=SC2016
OUT=$(AKAM_HOST_ARCH=x86_64 bash -c '. "$1"; rc=$?; echo "rc=$rc cc=${AKAM_CC:-}"' _ "$EROOT/tools/build/env.sh" 2>&1)
contains "env.sh with zig: rc 0" "$OUT" "rc=0"
contains "env.sh with zig: AKAM_CC" "$OUT" "cc=$EROOT/tools/.toolchain/zig-$ZV-x86_64-linux/zig cc -target arm-linux-musleabi"
# Per-arch directory: an aarch64 host does not pick the x86_64 zig.
# shellcheck disable=SC2016
OUT=$(AKAM_HOST_ARCH=aarch64 bash -c '. "$1"; rc=$?; echo "rc=$rc"' _ "$EROOT/tools/build/env.sh" 2>&1)
contains "env.sh aarch64 host looks for aarch64 zig" "$OUT" "zig-$ZV-aarch64-linux"

echo "--"
echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
