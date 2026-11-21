#!/usr/bin/env bash
# Tests for tools/release/make_kit_2k.sh on stub binaries. Nothing here touches hardware, the network or the
# repository tree. AKAM_KIT2K_EMIT=DIR keeps the extracted kit there (used by the Go test that loads it).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
MAKE=$REPO/tools/release/make_kit_2k.sh
for t in tar xz sha256sum; do
    command -v "$t" >/dev/null || { echo "missing tool: $t"; exit 1; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/akam-kit2k-test.XXXXXX")
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
rc_is() {
    local want=$1 rc=0
    shift
    "$@" >"$WORK/last.out" 2>&1 || rc=$?
    [ "$rc" = "$want" ]
}
sha() { sha256sum "$1" | cut -d' ' -f1; }

# --- stubs: a fake 32-bit ARM ELF that names the uClibc interpreter ---------------------------------------------
S=$WORK/stub
mkdir -p "$S/app" "$S/db"
fake_elf() { # fake_elf FILE TAG
    { printf '\177ELF\001\001\001'; head -c 11 /dev/zero; printf '\050\000'; head -c 64 /dev/zero
      printf '/lib/ld-uClibc.so.0\000%s\n' "$2"; } >"$1"
    chmod 755 "$1"
}
fake_elf "$S/app/akam2k_rtsp" rtsp
fake_elf "$S/app/akam2k_sntp" sntp
printf 'symcheck=passed\nsha256=%s\nsntp_sha256=%s\nsource_sha256=x\nsntp_source_sha256=y\n' \
    "$(sha "$S/app/akam2k_rtsp")" "$(sha "$S/app/akam2k_sntp")" >"$S/app/BUILDINFO.txt"
printf '#!/bin/sh\necho stub dropbearmulti\n' >"$S/db/dropbearmulti"
chmod 755 "$S/db/dropbearmulti"
printf 'sha256_stripped=%s\nprograms=dropbear dropbearkey scp\ntime32_shim_sha256=z\n' "$(sha "$S/db/dropbearmulti")" >"$S/db/BUILDINFO.txt"
printf 'Dropbear stub licence\n' >"$S/db/LICENSE.dropbear"
printf '#!/bin/sh\necho stub busybox\n' >"$S/busybox"
chmod 755 "$S/busybox"
printf '#!/bin/sh\nexit 0\n' >"$S/gate_ok"
printf '#!/bin/sh\necho "forbidden syscall 403" >&2\nexit 1\n' >"$S/gate_bad"
chmod 755 "$S/gate_ok" "$S/gate_bad"

export AKAM_KIT_ALLOW_DIRTY=1 AKAM_KIT_VERSION=test1 SOURCE_DATE_EPOCH=1700000000
export AKAM_KIT_CHECK_TIME32=$S/gate_ok SKIP_SOURCE_PROOFS=1 AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY
AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY=$(sha "$S/busybox")
ARGS=(--app-dist "$S/app" --dropbear-dist "$S/db" --busybox "$S/busybox")
MODEL=lsc-rotatable-2k-ak3918av130

echo "== make_kit_2k"
check "builds a kit" rc_is 0 bash "$MAKE" "${ARGS[@]}" --out "$WORK/o1"
TB=$WORK/o1/akam-kit-$MODEL-test1.tar.xz
check "tarball and SHA256SUMS written" test -s "$TB" -a -s "$WORK/o1/SHA256SUMS"
check "SHA256SUMS names the tarball" bash -c "cd '$WORK/o1' && sha256sum -c SHA256SUMS >/dev/null"
check "second build, same epoch: identical tarball" bash -c "
    bash '$MAKE' ${ARGS[*]} --out '$WORK/o2' >/dev/null 2>&1 && cmp -s '$TB' '$WORK/o2/akam-kit-$MODEL-test1.tar.xz'"
check "different epoch: different tarball" bash -c "
    SOURCE_DATE_EPOCH=1700000001 bash '$MAKE' ${ARGS[*]} --out '$WORK/o3' >/dev/null 2>&1 && ! cmp -s '$TB' '$WORK/o3/akam-kit-$MODEL-test1.tar.xz'"
# K2: the disassembler of the build machine is not in the hashed MANIFEST, so another gdb gives the same tarball
mkdir -p "$WORK/fakebin"
printf '#!/bin/sh\necho "GNU gdb (fake) 99.9"\n' >"$WORK/fakebin/gdb"
chmod 755 "$WORK/fakebin/gdb"
check "another gdb on the build machine: identical tarball (K2)" bash -c "
    PATH='$WORK/fakebin':\$PATH bash '$MAKE' ${ARGS[*]} --out '$WORK/o_gdb' >/dev/null 2>&1 && cmp -s '$TB' '$WORK/o_gdb/akam-kit-$MODEL-test1.tar.xz' &&
    grep -q 'fake' '$WORK/o_gdb/akam-kit-$MODEL-test1.build-note.txt'"
tar -tvJf "$TB" >"$WORK/listing"
check "entries are owned by 0/0" bash -c "! awk '{print \$2}' '$WORK/listing' | grep -qv '^0/0\$'"
mkdir -p "$WORK/x"
tar -xJf "$TB" -C "$WORK/x"
K=$WORK/x/akam-kit-$MODEL-test1
check "MANIFEST: kind, model, version, one gate, test grade" bash -c "
    grep -qx kind=cardless-appfs '$K/MANIFEST.txt' && grep -qx model=$MODEL '$K/MANIFEST.txt' &&
    grep -qx akam_version=test1 '$K/MANIFEST.txt' && [ \"\$(grep -c '^time64_gate=' '$K/MANIFEST.txt')\" = 1 ] &&
    grep -qx time64_gate=pass '$K/MANIFEST.txt' && grep -qx kit_grade=test '$K/MANIFEST.txt'"
check "inner SHA256SUMS verifies every file but itself" bash -c "
    cd '$K' && sha256sum -c SHA256SUMS >/dev/null &&
    [ \"\$(wc -l <SHA256SUMS)\" = \"\$(find . -type f ! -name SHA256SUMS | wc -l)\" ]"
check "the file list is the one the Go installer expects" bash -c "
    cd '$K' && find . -type f | LC_ALL=C sort | tr '\n' ' ' | grep -q \
'^./MANIFEST.txt ./SHA256SUMS ./appfs/bin/akam2k_rtsp ./appfs/bin/akam2k_rtsp.BUILDINFO ./appfs/bin/akam2k_sntp ./appfs/bin/busybox ./appfs/bin/dropbearmulti ./appfs/bin/dropbearmulti.BUILDINFO ./appfs/overlay/akam_api_key.sh ./appfs/overlay/akam_flash_boot.sh ./appfs/overlay/akam_hostname.sh ./appfs/overlay/akam_net.sh ./appfs/overlay/akam_ntp.sh ./appfs/overlay/common.sh ./appfs/overlay/decloud.sh ./appfs/overlay/own_app_run.sh ./appfs/overlay/own_app_stub.sh ./appfs/overlay/start_ssh.sh ./card/akam/enable_local.sh ./card/akam/restore_param.sh ./card/akam_boot.sh ./card/yy_debug.ini ./licenses/LICENSE.akam ./licenses/LICENSE.dropbear ./licenses/SOURCES.txt ./licenses/THIRD_PARTY_NOTICES.md ./supported_hardware.txt '"
check "card yy_debug.ini is mode=debug, never factory" bash -c "
    grep -qx mode=debug '$K/card/yy_debug.ini' && ! grep -q factory '$K/card/yy_debug.ini'"
check "no image, dump, key or factory file" bash -c "! find '$K' -iname '*.squashfs' -o -iname 'mtd*' -o -iname 'fac_av130*' -o -name '*_host_key*' | grep -q ."
check "modes are 644 or 755 only (the verifier requires it)" bash -c "
    ! find '$K' -type f ! -perm 644 ! -perm 755 | grep -q ."
if [ -n "${AKAM_KIT2K_EMIT:-}" ]; then
    rm -rf "$AKAM_KIT2K_EMIT"
    cp -a "$K" "$AKAM_KIT2K_EMIT"
    cp "$TB" "$WORK/o1/SHA256SUMS" "$AKAM_KIT2K_EMIT/../" 2>/dev/null || true
fi

echo "== refusals"
check "refuses when the time64 gate fails, writes no tarball" bash -c "
    ! AKAM_KIT_CHECK_TIME32='$S/gate_bad' bash '$MAKE' ${ARGS[*]} --out '$WORK/o4' >'$WORK/gate.out' 2>&1 &&
    grep -q 'time64 gate FAILED' '$WORK/gate.out' && ! ls '$WORK/o4'/*.tar.xz 2>/dev/null"
check "refuses the 1080p model (native-app)" bash -c "
    ! bash '$MAKE' ${ARGS[*]} --model lsc-indoor-1080p-ak3918 --out '$WORK/o5' >'$WORK/m.out' 2>&1 && grep -q 'not a cardless-appfs model' '$WORK/m.out'"
check "refuses an unknown model" bash -c "! bash '$MAKE' ${ARGS[*]} --model nope-nope-1 --out '$WORK/o5' >/dev/null 2>&1"
check "refuses a busybox that is not the pinned one" bash -c "
    printf 'x' >'$WORK/bb2'; ! env -u AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY bash '$MAKE' ${ARGS[*]} --busybox '$WORK/bb2' --out '$WORK/o5' >'$WORK/b.out' 2>&1 &&
    grep -q 'not the pinned' '$WORK/b.out'"
check "refuses an app that does not match its BUILDINFO" bash -c "
    cp -a '$S/app' '$WORK/app2' && printf 'tamper' >>'$WORK/app2/akam2k_rtsp' &&
    ! bash '$MAKE' --app-dist '$WORK/app2' --dropbear-dist '$S/db' --busybox '$S/busybox' --out '$WORK/o5' >'$WORK/a.out' 2>&1 &&
    grep -q 'does not match its BUILDINFO' '$WORK/a.out'"
check "refuses an app that is not ARM" bash -c "
    cp -a '$S/app' '$WORK/app3' && printf 'not an elf /lib/ld-uClibc.so.0' >'$WORK/app3/akam2k_sntp' &&
    sed -i \"s/^sntp_sha256=.*/sntp_sha256=\$(sha256sum '$WORK/app3/akam2k_sntp' | cut -d' ' -f1)/\" '$WORK/app3/BUILDINFO.txt' &&
    ! bash '$MAKE' --app-dist '$WORK/app3' --dropbear-dist '$S/db' --busybox '$S/busybox' --out '$WORK/o5' >'$WORK/a.out' 2>&1 &&
    grep -q 'not an ARM executable' '$WORK/a.out'"

echo
echo "kit2k_test: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
