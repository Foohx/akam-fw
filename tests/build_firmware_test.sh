#!/usr/bin/env bash
# End-to-end test of src/ak_rtsp/build_firmware.sh on the synthetic fixture,
# with stub ak_rtsp / dropbearmulti binaries. Nothing here touches hardware,
# the repository tree, or a real APP dump. Sizes in the reports are fixture
# sizes, NOT representative of the real free space.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BUILD=$REPO/src/ak_rtsp/build_firmware.sh
FIXTURE=$REPO/tests/fixtures/app_fake
TARGET=5242880

for t in mksquashfs unsquashfs sha256sum; do
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

# Stub binaries (text, so nothing binary is stored in the repository).
mkdir -p "$WORK/stub"
printf '#!/bin/sh\necho stub ak_rtsp\n' >"$WORK/stub/ak_rtsp"
printf '#!/bin/sh\necho stub dropbearmulti\n' >"$WORK/stub/dropbearmulti"
printf 'Dropbear stub licence\n' >"$WORK/stub/LICENSE.dropbear"
chmod 755 "$WORK/stub/ak_rtsp" "$WORK/stub/dropbearmulti"
export AK_RTSP_BIN=$WORK/stub/ak_rtsp DROPBEAR_MULTI=$WORK/stub/dropbearmulti

# run_build <outdir> [VAR=val ...]: build from another cwd, as the flasher does.
run_build() {
    local out=$1
    shift
    mkdir -p "$out"
    (cd "$WORK" && env APP_EXTRACTED="${APP:-$FIXTURE}" FW_DIR="$out" "$@" bash "$BUILD") >"$out.log" 2>&1
}

# with_app <tree> <outdir> [VAR=val ...]: like run_build with another APP tree.
with_app() { APP=$1 run_build "${@:2}"; }

echo "== 1. nominal build (gzip)"
O=$WORK/o1
check "build exits 0" run_build "$O" SOURCE_DATE_EPOCH=1700000000
IMG=$(find "$O" -maxdepth 1 -name 'ak_rtsp_firmware_*.squashfs' | head -1)
check "image found by flasher pattern" test -n "$IMG"
check "image is exactly $TARGET bytes" test "$(stat -c %s "$IMG")" = "$TARGET"
check "name matches ak_rtsp_firmware_YYYYMMDD_HHMMSS.squashfs" bash -c "basename '$IMG' | grep -Eq '^ak_rtsp_firmware_[0-9]{8}_[0-9]{6}\.squashfs\$'"
check "sha256 file valid" bash -c "cd '$O' && sha256sum -c '$(basename "$IMG").sha256' >/dev/null"
check "unsquashfs -s: gzip" bash -c "unsquashfs -s '$IMG' | grep -qi '^Compression gzip'"
check "unsquashfs -s: block 131072" bash -c "unsquashfs -s '$IMG' | grep -q '^Block size 131072'"
check "report says fixture/free space" grep -q 'free in mtd7' "$O/build_report.txt"
echo "--- resource report (fixture; not representative of the real APP) ---"
cat "$O/build_report.txt"
echo "---"
unsquashfs -no-xattrs -d "$WORK/x1" "$IMG" >/dev/null 2>&1 || true
X=$WORK/x1
check "bin/anyka_ipc removed" test ! -e "$X/bin/anyka_ipc"
check "bin/ak_rtsp present, 755" test "$(stat -c %a "$X/bin/ak_rtsp")" = 755
check "bin/dropbearmulti present, 755" test "$(stat -c %a "$X/bin/dropbearmulti")" = 755
check "bin/dropbear -> dropbearmulti" test "$(readlink "$X/bin/dropbear")" = dropbearmulti
check "bin/dropbearkey -> dropbearmulti" test "$(readlink "$X/bin/dropbearkey")" = dropbearmulti
check "sbin/camyn_ssh.sh 755" test "$(stat -c %a "$X/sbin/camyn_ssh.sh")" = 755
check "sbin/camyn_lockdown.sh 755" test "$(stat -c %a "$X/sbin/camyn_lockdown.sh")" = 755
check "sbin/ak_rtsp_wrapper.sh 755" test "$(stat -c %a "$X/sbin/ak_rtsp_wrapper.sh")" = 755
check "LICENSE.dropbear shipped" test -s "$X/local/camyn/LICENSE.dropbear"
check "MANIFEST has ak_rtsp sha256" grep -q "^ak_rtsp_sha256=$(sha256sum "$WORK/stub/ak_rtsp" | cut -d' ' -f1)" "$X/local/camyn/MANIFEST.txt"
check "fixture symlink preserved" test "$(readlink "$X/lib/libfake.so")" = libfake.so.1
check "ini has a single ak_rtsp_wrapper.sh entry" bash -c "[ \$(grep -c '^\[process_' '$X/local/_ht_process.ini') = 1 ] && grep -q 'ak_rtsp_wrapper.sh' '$X/local/_ht_process.ini'"
check "service.sh: no active telnetd/ftpd launch" bash -c "! grep -v '^[[:space:]]*:[[:space:]]*# CAMYN-DISABLED' '$X/sbin/service.sh' | grep -vE '^[[:space:]]*#' | grep -Eq 'telnetd|ftpd'"
check "service.sh: disabled markers present" grep -q 'CAMYN-DISABLED:' "$X/sbin/service.sh"
check "service.sh: valid shell" sh -n "$X/sbin/service.sh"
check "service.sh: SD hook guarded" grep -q 'CAMYN_MAINTENANCE' "$X/sbin/service.sh"
check "no private key / host key in image" bash -c "! grep -rlE 'PRIVATE KEY' '$X' && [ -z \"\$(find '$X' -name '*_host_key*' -o -name 'authorized_keys*')\" ]"
check "fixture tree not modified" test ! -e "$FIXTURE/sbin/camyn_ssh.sh"
check "no temp dirs left in /tmp by the build" bash -c "! ls /tmp/rt_check >/dev/null 2>&1"

echo "== 2. reproducible content"
run_build "$WORK/o2" SOURCE_DATE_EPOCH=1700000000 || true
s1=$(grep '^squashfs sha256' "$O/build_report.txt" | awk '{print $3}')
s2=$(grep '^squashfs sha256' "$WORK/o2/build_report.txt" | awk '{print $3}')
check "same SOURCE_DATE_EPOCH -> same squashfs sha256" test -n "$s1" -a "$s1" = "$s2"

echo "== 3. xz"
check "xz build exits 0" run_build "$WORK/o3" CAMYN_SQUASHFS_COMP=xz
IMGX=$(find "$WORK/o3" -name 'ak_rtsp_firmware_*.squashfs' | head -1)
check "xz image is exactly $TARGET bytes" test "$(stat -c %s "$IMGX")" = "$TARGET"
check "xz recorded" bash -c "unsquashfs -s '$IMGX' | grep -qi '^Compression xz'"

echo "== 4. size guard (oversize, nothing padded)"
cp -a "$FIXTURE" "$WORK/big"
head -c 5500000 /dev/urandom >"$WORK/big/lib/blob.bin"
if APP=$WORK/big run_build "$WORK/o4"; then ko "oversize image refused"; else ok "oversize image refused"; fi
check "oversize: message" grep -q 'larger than' "$WORK/o4.log"
check "oversize: no padded image written" test -z "$(find "$WORK/o4" -name 'ak_rtsp_firmware_*' | head -1)"
head -c 5150000 /dev/urandom >"$WORK/big/lib/blob.bin"
if APP=$WORK/big run_build "$WORK/o4b"; then ko "tight image (< 1 block free) refused"; else ok "tight image (< 1 block free) refused"; fi
check "tight accepted with CAMYN_ALLOW_TIGHT=1" with_app "$WORK/big" "$WORK/o4c" CAMYN_ALLOW_TIGHT=1

echo "== 5. refusals"
mkdir -p "$WORK/nosbin"
cp -a "$FIXTURE/bin" "$FIXTURE/local" "$WORK/nosbin/"
if APP=$WORK/nosbin run_build "$WORK/o5a"; then ko "tree without sbin/ refused"; else ok "tree without sbin/ refused"; fi
cp -a "$FIXTURE" "$WORK/key"
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\nxx\n-----END OPENSSH PRIVATE KEY-----\n' >"$WORK/key/lib/oops.txt"
if APP=$WORK/key run_build "$WORK/o5b"; then ko "planted private key refused"; else ok "planted private key refused"; fi
check "private key: message" grep -q 'private key' "$WORK/o5b.log"
if AK_RTSP_BIN=$WORK/nonexistent run_build "$WORK/o5c"; then ko "missing ak_rtsp refused"; else ok "missing ak_rtsp refused"; fi
check "missing ak_rtsp: clear message" grep -q 'ak_rtsp binary not found' "$WORK/o5c.log"

echo "== 6. service.sh without telnet/ftp lines"
mkdir -p "$WORK/nosvc"
cp -a "$FIXTURE/." "$WORK/nosvc/"
printf '#!/bin/sh\necho nothing to see\n' >"$WORK/nosvc/sbin/service.sh"
if APP=$WORK/nosvc run_build "$WORK/o6a"; then ko "unpatchable service.sh is fatal"; else ok "unpatchable service.sh is fatal"; fi
check "unpatchable build with override" with_app "$WORK/nosvc" "$WORK/o6b" CAMYN_ALLOW_UNPATCHED_SERVICE=1
check "override: loud warning in report" grep -q 'service.sh NOT patched' "$WORK/o6b/build_report.txt"

echo "== 7. stale app_patched/ is not reused by default"
mkdir -p "$WORK/o7/app_patched/sbin"
echo stale >"$WORK/o7/app_patched/sbin/stale.sh"
run_build "$WORK/o7" || true
check "stale file gone" test ! -e "$WORK/o7/app_patched/sbin/stale.sh"
check "patch applied" grep -q 'CAMYN-DISABLED' "$WORK/o7/app_patched/sbin/service.sh"

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
