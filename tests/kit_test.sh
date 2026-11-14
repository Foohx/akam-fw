#!/usr/bin/env bash
# Tests for tools/release/{make_kit,sign_kit,verify_kit}.sh on stub binaries.
# Nothing here touches hardware, the network or the repository tree; keys are
# throw-away ones generated in a temporary directory.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
MAKE=$REPO/tools/release/make_kit.sh
SIGN=$REPO/tools/release/sign_kit.sh
VERIFY=$REPO/tools/release/verify_kit.sh

for t in tar xz sha256sum ssh-keygen; do
    command -v "$t" >/dev/null || { echo "missing tool: $t"; exit 1; }
done

WORK=$(mktemp -d)
# The repository tracks no symlink: build the fixture's lib/libfake.so link in a throw-away copy.
FIXTURE_SRC=$REPO/tests/fixtures/app_fake
FIXTURE=$WORK/app_fake
cp -a "$FIXTURE_SRC" "$FIXTURE"
ln -s libfake.so.1 "$FIXTURE/lib/libfake.so"
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
fails() { ! "$@"; }
# rc_is <expected> <cmd...>: run quietly, compare the exit status.
rc_is() {
    local want=$1 rc=0
    shift
    "$@" >"$WORK/last.out" 2>&1 || rc=$?
    [ "$rc" = "$want" ]
}

# --- stubs ----------------------------------------------------------------------------
S=$WORK/stub
mkdir -p "$S/smol/build/_deps" "$S/db"
printf '#!/bin/sh\necho stub ak_rtsp\n' >"$S/ak_rtsp"
printf '#!/bin/sh\necho stub flash_tool\n' >"$S/flash_tool"
printf '#!/bin/sh\necho stub dropbearmulti\n' >"$S/db/dropbearmulti"
printf 'Dropbear stub licence\n' >"$S/db/LICENSE.dropbear"
printf 'smolrtsp stub licence\n' >"$S/smol/LICENSE"
for d in slice99 metalang99 datatype99 interface99; do
    mkdir -p "$S/smol/build/_deps/$d-src"
    printf '%s stub licence\n' "$d" >"$S/smol/build/_deps/$d-src/LICENSE"
done
printf 'musl stub\n' >"$S/COPYRIGHT"
printf 'zig stub\n' >"$S/ZIGLICENSE"
printf '#!/bin/sh\nexit 0\n' >"$S/gate_ok"
printf '#!/bin/sh\necho "forbidden syscall 403" >&2\nexit 1\n' >"$S/gate_bad"
chmod 755 "$S"/ak_rtsp "$S"/flash_tool "$S/db/dropbearmulti" "$S"/gate_ok "$S"/gate_bad

export AKAM_KIT_NO_BUILD=1 AKAM_KIT_ALLOW_DIRTY=1 AKAM_KIT_VERSION=test1
export AK_RTSP_BIN=$S/ak_rtsp FLASH_TOOL_BIN=$S/flash_tool DROPBEAR_MULTI=$S/db/dropbearmulti
export SMOLRTSP_DIR=$S/smol MUSL_COPYRIGHT=$S/COPYRIGHT ZIG_LICENSE=$S/ZIGLICENSE
export AKAM_KIT_CHECK_TIME32=$S/gate_ok
export SOURCE_DATE_EPOCH=1700000000

echo "== make_kit"
check "builds a kit" rc_is 0 bash "$MAKE" --out "$WORK/o1"
TB=$WORK/o1/akam-kit-test1.tar.xz
check "tarball and SHA256SUMS written" test -s "$TB" -a -s "$WORK/o1/SHA256SUMS"
check "second build, same epoch: identical tarball sha256" bash -c "
    bash '$MAKE' --out '$WORK/o2' >/dev/null 2>&1 && cmp -s '$TB' '$WORK/o2/akam-kit-test1.tar.xz'"
check "different epoch: different tarball" bash -c "
    SOURCE_DATE_EPOCH=1700000001 bash '$MAKE' --out '$WORK/o3' >/dev/null 2>&1 && ! cmp -s '$TB' '$WORK/o3/akam-kit-test1.tar.xz'"
tar -tvJf "$TB" >"$WORK/listing"
check "archive entries are owned by 0/0" bash -c "! awk '{print \$2}' '$WORK/listing' | grep -qv '^0/0\$'"
mkdir -p "$WORK/x"
tar -xJf "$TB" -C "$WORK/x"
K=$WORK/x/akam-kit-test1
check "MANIFEST has kit_format, version, gate and date" bash -c "
    grep -qx kit_format=1 '$K/MANIFEST.txt' && grep -qx akam_version=test1 '$K/MANIFEST.txt' &&
    grep -qx time64_gate=pass '$K/MANIFEST.txt' && grep -qx 'built_utc=2023-11-14T22:13:20Z' '$K/MANIFEST.txt'"
check "MANIFEST lists flash_tool, ak_rtsp and dropbearmulti with modes" bash -c "
    grep -qE '^file [0-9a-f]{64} 755 bin/flash_tool\$' '$K/MANIFEST.txt' &&
    grep -qE '^file [0-9a-f]{64} 755 bin/ak_rtsp\$' '$K/MANIFEST.txt' &&
    grep -qE '^file [0-9a-f]{64} 644 overlay/local/_ht_process.ini\$' '$K/MANIFEST.txt' &&
    grep -qx 'link dropbearmulti 777 overlay/bin/dropbear' '$K/MANIFEST.txt'"
check "kit holds only our top-level directories" bash -c "
    [ \"\$(LC_ALL=C ls '$K' | tr '\n' ' ')\" = 'MANIFEST.txt SHA256SUMS SOURCES.txt bin licenses overlay patches scripts supported_hardware.txt ' ]"
check "no image, squashfs or dump in the kit" bash -c "! find '$K' -iname '*.squashfs' -o -iname '*.img' -o -iname 'app_*' | grep -q ."
check "wrapper, akam_*.sh, hwcheck and flash scripts present" bash -c "
    cd '$K' && test -x overlay/sbin/ak_rtsp_wrapper.sh && test -x overlay/sbin/akam_ssh.sh &&
    test -x overlay/sbin/akam_lockdown.sh && test -x overlay/sbin/akam_rtsp_auth.sh && test -x overlay/sbin/akam_hostname.sh && test -x overlay/sbin/akam_debug.sh && test -x overlay/sbin/akam_onvif.sh && test -x overlay/sbin/akam_net.sh && test -x overlay/sbin/akam_ntp.sh && test -x overlay/sbin/akam_mqtt.sh && test -x scripts/hwcheck/akam_hwcheck.sh &&
    test -x scripts/flash/flash_guard.sh && test -x patches/patch_service_sh.sh"

echo "== licences cover THIRD_PARTY_NOTICES.md"
check "every LICENSE.<x> named in the notices is in the kit" bash -c "
    for n in \$(grep -o 'LICENSE\\.[a-z0-9]*' '$REPO/THIRD_PARTY_NOTICES.md' | sort -u); do
        test -s '$K/licenses/'\$n || { echo missing \$n; exit 1; }
    done"
check "notices file itself is shipped, byte for byte" cmp -s "$REPO/THIRD_PARTY_NOTICES.md" "$K/licenses/THIRD_PARTY_NOTICES.md"

echo "== time64 gate"
check "make_kit refuses when the gate fails" bash -c "
    ! AKAM_KIT_CHECK_TIME32='$S/gate_bad' bash '$MAKE' --out '$WORK/o4' >'$WORK/gate.out' 2>&1"
check "  ... says why and writes no tarball" bash -c "grep -q 'time64 gate FAILED' '$WORK/gate.out' && ! ls '$WORK/o4'/*.tar.xz 2>/dev/null"
check "make_kit refuses a missing licence source" bash -c "
    ! ZIG_LICENSE='$WORK/nope' bash '$MAKE' --out '$WORK/o5' >/dev/null 2>&1"

echo "== sign_kit / verify_kit"
ssh-keygen -q -t ed25519 -N '' -C test -f "$WORK/key" >/dev/null
ssh-keygen -q -t ed25519 -N '' -C other -f "$WORK/key2" >/dev/null
printf 'akam-release namespaces="akam-release" %s\n' "$(cut -d' ' -f1,2 "$WORK/key.pub")" >"$WORK/allowed"
printf '# placeholder only\n' >"$WORK/allowed_empty"
check "sign_kit refuses a key inside the repository" bash -c "
    cp '$WORK/key' '$REPO/.kit_test_key' 2>/dev/null; rc=0
    bash '$SIGN' '$REPO/.kit_test_key' '$WORK/o1' >/dev/null 2>&1 || rc=\$?; rm -f '$REPO/.kit_test_key'; [ \$rc -ne 0 ]"
check "unsigned kit: exit 3 with a WARNING" bash -c "
    rc=0; bash '$VERIFY' --allow-test-kit --allowed-signers '$WORK/allowed' '$TB' >'$WORK/v.out' 2>&1 || rc=\$?; [ \$rc = 3 ] && grep -q 'WARNING: no signature' '$WORK/v.out'"
check "unsigned kit with --allow-unsigned: exit 0, says UNSIGNED" bash -c "
    bash '$VERIFY' --allow-test-kit --allow-unsigned '$TB' >'$WORK/v.out' 2>&1 && grep -q 'UNSIGNED' '$WORK/v.out'"
check "sign_kit signs" rc_is 0 env AKAM_ALLOWED_SIGNERS="$WORK/allowed" bash "$SIGN" "$WORK/key" "$WORK/o1"
check "signed kit verifies" rc_is 0 bash "$VERIFY" --allow-test-kit --allowed-signers "$WORK/allowed" "$TB"
check "  ... and says VERIFIED and signed" grep -q 'VERIFIED and signed' "$WORK/last.out"
check "--expect-sha256 right prefix ok" rc_is 0 bash "$VERIFY" --allow-test-kit --allowed-signers "$WORK/allowed" \
    --expect-sha256 "$(sha256sum "$TB" | cut -c1-12)" "$TB"
check "--expect-sha256 wrong prefix fails" rc_is 1 bash "$VERIFY" --allow-test-kit --allowed-signers "$WORK/allowed" \
    --expect-sha256 000000000000 "$TB"
check "signature from another key: fail" bash -c "
    mkdir '$WORK/o6' && cp '$TB' '$WORK/o1/SHA256SUMS' '$WORK/o6/' && cd '$WORK/o6' &&
    ssh-keygen -Y sign -f '$WORK/key2' -n akam-release SHA256SUMS >/dev/null 2>&1 &&
    rc=0; bash '$VERIFY' --allow-test-kit --allowed-signers '$WORK/allowed' akam-kit-test1.tar.xz >'$WORK/v.out' 2>&1 || rc=\$?
    [ \$rc = 1 ] && grep -q 'INVALID signature' '$WORK/v.out'"
check "signature over modified SHA256SUMS: fail" bash -c "
    mkdir '$WORK/o7' && cp '$TB' '$WORK/o1/SHA256SUMS' '$WORK/o1/SHA256SUMS.sig' '$WORK/o7/' &&
    printf '\n' >>'$WORK/o7/SHA256SUMS' &&
    rc=0; bash '$VERIFY' --allow-test-kit --allowed-signers '$WORK/allowed' '$WORK/o7/akam-kit-test1.tar.xz' >/dev/null 2>&1 || rc=\$?; [ \$rc = 1 ]"
check "signature with a placeholder allowed_signers: fail" rc_is 1 bash "$VERIFY" --allow-test-kit --allowed-signers "$WORK/allowed_empty" "$TB"
check "--allow-unsigned does not excuse an invalid signature" bash -c "
    ! bash '$VERIFY' --allow-test-kit --allow-unsigned --allowed-signers '$WORK/o6/none' '$WORK/o6/akam-kit-test1.tar.xz' >/dev/null 2>&1"

echo "== tampering"
# repack <dir-with-akam-kit-test1> <out.tar.xz>: same layout as make_kit.
repack() { (cd "$1" && tar --sort=name --mtime=@1700000000 --owner=0 --group=0 --numeric-owner -cf - akam-kit-test1 | xz -T1 >"$2"); }
# tamper <name> <edit-command...>: copy the extracted kit, edit it, repack, re-sum
# (no signature: --allow-unsigned), so only the kit's own checks can catch it.
tamper() {
    local name=$1
    shift
    rm -rf "$WORK/t_$name"
    mkdir -p "$WORK/t_$name/out"
    cp -a "$WORK/x/akam-kit-test1" "$WORK/t_$name/"
    (cd "$WORK/t_$name/akam-kit-test1" && "$@")
    repack "$WORK/t_$name" "$WORK/t_$name/out/akam-kit-test1.tar.xz"
    (cd "$WORK/t_$name/out" && sha256sum akam-kit-test1.tar.xz >SHA256SUMS)
}
vt() { rc_is 1 bash "$VERIFY" --allow-test-kit --allow-unsigned "$WORK/t_$1/out/akam-kit-test1.tar.xz"; }
tamper control true
check "control repack verifies" rc_is 0 bash "$VERIFY" --allow-test-kit --allow-unsigned "$WORK/t_control/out/akam-kit-test1.tar.xz"
tamper byte bash -c "printf 'x' >>bin/ak_rtsp"
check "one byte added to bin/ak_rtsp: fail" vt byte
tamper licence bash -c "printf 'x' >>licenses/LICENSE.musl"
check "one byte added to a licence: fail" vt licence
tamper manifest bash -c "sed -i 's/^akam_version=test1/akam_version=test2/' MANIFEST.txt"
check "one edit in MANIFEST.txt: fail" vt manifest
tamper manhash bash -c "sed -i '0,/^file [0-9a-f]/s/^file [0-9a-f]/file z/' MANIFEST.txt"
check "edited hash in MANIFEST.txt: fail" vt manhash
tamper mode bash -c "chmod 777 bin/ak_rtsp"
check "mode changed: fail" vt mode
tamper extra bash -c "echo hi >extra.txt"
check "extra file: fail" vt extra
tamper missing bash -c "rm licenses/LICENSE.zig"
check "missing file: fail" vt missing
tamper link bash -c "ln -sf ../../bin/ak_rtsp overlay/bin/dropbear"
check "symlink retargeted: fail" vt link
tamper fmt bash -c "sed -i 's/^kit_format=1/kit_format=2/' MANIFEST.txt && sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||') >SHA256SUMS"
check "unsupported kit_format: fail" vt fmt
check "  ... says so" grep -q 'kit_format 2 is not supported' "$WORK/last.out"
tamper gate bash -c "sed -i 's/^time64_gate=pass/time64_gate=skipped/' MANIFEST.txt && sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||') >SHA256SUMS"
check "time64_gate not pass: fail" vt gate
check "sha256 of the tarball not in SHA256SUMS: fail" bash -c "
    cp '$WORK/t_byte/out/akam-kit-test1.tar.xz' '$WORK/o7/other.tar.xz'; ! bash '$VERIFY' --allow-test-kit --allow-unsigned '$WORK/o7/other.tar.xz' >/dev/null 2>&1"

echo "== hostile archives"
mkdir -p "$WORK/h/akam-kit-test1/overlay/bin"
ln -s /etc "$WORK/h/akam-kit-test1/evil"
(cd "$WORK/h" && tar -cf - akam-kit-test1 | xz >evil_symlink.tar.xz)
(cd "$WORK/h" && sha256sum evil_symlink.tar.xz >SHA256SUMS)
check "archive with an unexpected symlink: fail" rc_is 1 bash "$VERIFY" --allow-test-kit --allow-unsigned "$WORK/h/evil_symlink.tar.xz"
check "  ... rejected before extraction" grep -q 'unexpected archive entry' "$WORK/last.out"
rm "$WORK/h/akam-kit-test1/evil"
echo x >"$WORK/h/outside"
(cd "$WORK/h/akam-kit-test1" && tar --transform 's|^outside|../outside|' -cf - ../outside 2>/dev/null | xz >../evil_dotdot.tar.xz) || true
(cd "$WORK/h" && sha256sum evil_dotdot.tar.xz >SHA256SUMS)
check "archive with '..' path: fail" rc_is 1 bash "$VERIFY" --allow-test-kit --allow-unsigned "$WORK/h/evil_dotdot.tar.xz"

echo "== directory mode"
check "extracted kit directory verifies" rc_is 0 bash "$VERIFY" --allow-test-kit "$K"
check "  ... and says it is not authenticated" grep -q 'not authenticated' "$WORK/last.out"
cp -a "$K" "$WORK/kd"
printf 'x' >>"$WORK/kd/bin/flash_tool"
check "directory with a modified file: fail" rc_is 1 bash "$VERIFY" --allow-test-kit "$WORK/kd"
check "--extract-to keeps the verified tree" bash -c "
    bash '$VERIFY' --allow-test-kit --allow-unsigned --extract-to '$WORK/keep' '$WORK/t_control/out/akam-kit-test1.tar.xz' >/dev/null 2>&1 &&
    test -s '$WORK/keep/MANIFEST.txt'"

echo "== supported hardware (spec addition)"
SH=$REPO/tools/release/supported_hardware.txt
check "supported_hardware.txt is shipped byte for byte" cmp -s "$SH" "$K/supported_hardware.txt"
check "MANIFEST records its sha256 and counts" bash -c "
    grep -qx 'supported_hardware_sha256=$(sha256sum "$SH" | cut -d' ' -f1)' '$K/MANIFEST.txt' &&
    grep -q '^supported_hardware=supported_hardware.txt (1 validated, 0 reported, 2 untested records)' '$K/MANIFEST.txt' &&
    grep -qE '^file [0-9a-f]{64} 644 supported_hardware.txt\$' '$K/MANIFEST.txt'"
check "validated record carries the stock APP fingerprint" bash -c "
    grep -F '14a2e43f9169575ff181d4027b78da652fb343714b4045b8b1f129b8a0d85294' '$SH' | grep -q '^validated|.*3215672.3.*D26228)|AK3918AV130|4.4.302-cip94 #1 Fri Nov 14 19:06:55 CST 2025|8 partitions; mtd7=APP 00500000|squashfs xz block=131072 bcj=none|.*|1.30.1\$'"
check "3215672.2 and any other target are untested" bash -c "
    grep -q '^untested|.*3215672.2.*B26101.*C26101' '$SH' && grep -q '^untested|any other' '$SH'"
check "every record has 8 fields" bash -c "
    grep -vE '^(#|supported_hardware_format=|\$)' '$SH' | awk -F'|' 'NF != 8 { bad = 1 } END { exit bad }'"
check "no record other than validated/reported/untested" bash -c "
    ! grep -vE '^(#|supported_hardware_format=|validated\\||reported\\||untested\\||\$)' '$SH' | grep -q ."
check "MANIFEST without kit_grade=release: verify refuses without --allow-test-kit" bash -c "
    rc=0; bash '$VERIFY' --allow-unsigned '$WORK/t_control/out/akam-kit-test1.tar.xz' >'$WORK/g.out' 2>&1 || rc=\$?; [ \$rc = 1 ] && grep -q 'not a release kit' '$WORK/g.out'"
check "stub-built kit is graded test" grep -qx kit_grade=test "$K/MANIFEST.txt"
# regenerate the inner SHA256SUMS after an edit (so only the semantic checks can catch it).
resum() { # rewrite SHA256SUMS of the current directory
    local t
    t=$(mktemp)
    find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | xargs sha256sum >"$t" && cat "$t" >SHA256SUMS
    rm -f "$t"
}
tamper hwedit bash -c "
    sed -i '/^validated|/d' supported_hardware.txt &&
    sed -i \"s/^\\(file \\)[0-9a-f]* \\(.* supported_hardware.txt\\)\$/\\1\$(sha256sum supported_hardware.txt | cut -d' ' -f1) \\2/\" MANIFEST.txt &&
    sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\\./||') >SHA256SUMS"
check "supported_hardware.txt edited behind the MANIFEST: fail" vt hwedit
check "  ... on the hardware checksum" grep -q 'supported_hardware_sha256' "$WORK/last.out"

echo "== hostile archives (more)"
tamper hardlink bash -c "ln bin/ak_rtsp bin/hl"
check "hard link entry: fail" vt hardlink
check "  ... rejected from the listing, before extraction" grep -q 'unexpected archive entry' "$WORK/last.out"
tamper fifo bash -c "mkfifo bin/pipe"
check "fifo entry: fail" vt fifo
check "  ... rejected from the listing" grep -q 'unexpected archive entry' "$WORK/last.out"
tamper setuid bash -c "chmod 4755 bin/ak_rtsp"
check "setuid file: fail" vt setuid
check "  ... rejected from the listing" grep -q 'unexpected file mode' "$WORK/last.out"
tamper spacename bash -c "echo x >'sp ace'"
check "file name with a space: fail" vt spacename
check "  ... rejected from the listing" grep -q 'character outside' "$WORK/last.out"
tamper dirmode bash -c "chmod 700 licenses"
check "directory mode 700: fail" vt dirmode
rm -rf "$WORK/t_dup" && mkdir -p "$WORK/t_dup/out" && cp -a "$K" "$WORK/t_dup/"
(cd "$WORK/t_dup" && tar --owner=0 --group=0 -cf d.tar akam-kit-test1 && tar --owner=0 --group=0 -rf d.tar akam-kit-test1/MANIFEST.txt && xz -c d.tar >out/akam-kit-test1.tar.xz && rm d.tar &&
    cd out && sha256sum akam-kit-test1.tar.xz >SHA256SUMS)
check "duplicate archive entry: fail" vt dup
check "  ... says so" grep -q 'duplicate entries' "$WORK/last.out"
tamper evillink bash -c "ln -s /etc overlay/bin/evil && printf 'link /etc 777 overlay/bin/evil\n' >>MANIFEST.txt && sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\\./||') >SHA256SUMS"
check "MANIFEST listing an extra symlink: fail (tarball)" vt evillink
rm -rf "$WORK/kd2" && cp -a "$K" "$WORK/kd2" && ln -s /etc "$WORK/kd2/overlay/bin/evil" && printf 'link /etc 777 overlay/bin/evil\n' >>"$WORK/kd2/MANIFEST.txt" &&
    (cd "$WORK/kd2" && resum)
check "MANIFEST listing an extra symlink: fail (directory mode)" rc_is 1 bash "$VERIFY" --allow-test-kit "$WORK/kd2"
check "  ... says so" grep -q 'unexpected symlink' "$WORK/last.out"
tamper twogates bash -c "printf 'time64_gate=fail\n' >>MANIFEST.txt && sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\\./||') >SHA256SUMS"
check "two time64_gate lines: fail" vt twogates
tamper badmode bash -c "chmod 4755 bin/flash_tool && sed -i 's|^\\(file [0-9a-f]* \\)755 bin/flash_tool|\\14755 bin/flash_tool|' MANIFEST.txt && sha256sum \$(find . -type f ! -name SHA256SUMS | sort | sed 's|^\\./||') >SHA256SUMS"
check "setuid mode recorded in MANIFEST: fail" vt badmode

echo "== verify options"
check "--sig naming a missing file: fail (not 'unsigned')" bash -c "
    rc=0; bash '$VERIFY' --allow-test-kit --allow-unsigned --sig '$WORK/nope.sig' '$TB' >'$WORK/s.out' 2>&1 || rc=\$?; [ \$rc = 1 ] && grep -q 'no such file' '$WORK/s.out'"
check "--expect-sha256 '' is refused" rc_is 2 bash "$VERIFY" --allow-test-kit --allow-unsigned --expect-sha256 '' "$TB"
check "--expect-sha256 with 4 digits is refused" rc_is 2 bash "$VERIFY" --allow-test-kit --allow-unsigned --expect-sha256 abcd "$TB"
check "--expect-sha256 upper-case prefix accepted" rc_is 0 bash "$VERIFY" --allow-test-kit --allowed-signers "$WORK/allowed" \
    --expect-sha256 "$(sha256sum "$TB" | cut -c1-12 | tr a-f A-F)" "$TB"
check "version with '.' and '+' verifies" bash -c "
    AKAM_KIT_VERSION='1.0+rc.1' bash '$MAKE' --out '$WORK/o8' >/dev/null 2>&1 &&
    bash '$VERIFY' --allow-test-kit --allow-unsigned '$WORK/o8/akam-kit-1.0+rc.1.tar.xz' >/dev/null 2>&1"
check "signature present, signer principal not allowed: fail" bash -c "
    printf 'someone-else namespaces=\"akam-release\" %s\n' \"\$(cut -d' ' -f1,2 '$WORK/key.pub')\" >'$WORK/allowed_other'
    ! bash '$VERIFY' --allow-test-kit --allowed-signers '$WORK/allowed_other' '$TB' >/dev/null 2>&1"
check "signature present, key allowed only for another namespace: fail" bash -c "
    printf 'akam-release namespaces=\"other\" %s\n' \"\$(cut -d' ' -f1,2 '$WORK/key.pub')\" >'$WORK/allowed_ns'
    ! bash '$VERIFY' --allow-test-kit --allowed-signers '$WORK/allowed_ns' '$TB' >/dev/null 2>&1"

echo "== make_kit content guard and drift"
mkdir -p "$WORK/badlic"
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\nxx\n' >"$WORK/badlic/COPYRIGHT"
check "a private key in a shipped file stops make_kit" bash -c "
    ! MUSL_COPYRIGHT='$WORK/badlic/COPYRIGHT' bash '$MAKE' --out '$WORK/o9' >'$WORK/bad.out' 2>&1 && grep -q 'forbidden content' '$WORK/bad.out'"

# image_files.txt is the single list; build_firmware.sh must install exactly the same files.
check "fixture APP tree exists" test -d "$FIXTURE/sbin"
DRIFT=$WORK/drift
mkdir -p "$DRIFT"
check "build_firmware.sh builds an image with the same stubs" bash -c "
    cd '$WORK' && env APP_EXTRACTED='$FIXTURE' FW_DIR='$DRIFT' AKAM_ALLOW_UNCHECKED_TIME64=1 bash '$REPO/src/ak_rtsp/build_firmware.sh' >'$WORK/drift.log' 2>&1"
P=$DRIFT/app_patched
drift_check() {
    local kind a b c d f rc=0 want="$WORK/drift.want" got="$WORK/drift.got"
    : >"$want"
    while read -r kind a b c d; do
        case "$kind" in
            '' | '#'*) continue ;;
            file)
                [ "$c" != - ] || continue
                echo "$c" >>"$want"
                [ "$(stat -c %a "$P/$c")" = "$a" ] || { echo "mode of $c differs from image_files.txt ($a)"; rc=1; }
                cmp -s "$P/$c" "$REPO/$d" || { echo "content of $c differs from $d"; rc=1; }
                if [ "$(stat -c %a "$K/$b")" != "$a" ] || ! cmp -s "$K/$b" "$P/$c"; then echo "kit $b differs from image $c"; rc=1; fi
                ;;
            link)
                echo "$c" >>"$want"
                { [ "$(readlink "$P/$c")" = "$a" ] && [ "$(readlink "$K/$b")" = "$a" ]; } || { echo "link $c differs"; rc=1; }
                ;;
        esac
    done <"$REPO/tools/release/image_files.txt"
    # binaries and licences: built from variables, compared by name
    for f in bin/ak_rtsp bin/dropbearmulti; do
        echo "$f" >>"$want"
        { cmp -s "$P/$f" "$K/$f" && [ "$(stat -c %a "$P/$f")" = "$(stat -c %a "$K/$f")" ]; } || { echo "$f differs between image and kit"; rc=1; }
    done
    for f in "$K"/licenses/*; do
        f=$(basename "$f")
        case "$f" in THIRD_PARTY_NOTICES.md | LICENSE.*) ;; *) continue ;; esac
        echo "local/akam/$f" >>"$want"
        { cmp -s "$P/local/akam/$f" "$K/licenses/$f" && [ "$(stat -c %a "$P/local/akam/$f")" = 644 ]; } || { echo "licence $f differs between image and kit"; rc=1; }
    done
    # every Akam-owned path of the image (new or changed against the stock tree) is in the list
    printf '%s\n' sbin/service.sh sbin/ethernet.sh sbin/drop_caches.sh sbin/main.sh local/akam/MANIFEST.txt >>"$want"
    (cd "$P" && find . \( -type f -o -type l \) | sed 's|^\./||' | LC_ALL=C sort) |
        while IFS= read -r f; do
            if [ ! -e "$FIXTURE/$f" ] && [ ! -L "$FIXTURE/$f" ]; then echo "$f"; elif [ -L "$P/$f" ]; then [ "$(readlink "$P/$f")" = "$(readlink "$FIXTURE/$f")" ] || echo "$f"; elif ! cmp -s "$P/$f" "$FIXTURE/$f"; then echo "$f"; fi
        done >"$got"
    LC_ALL=C sort -u "$want" >"$want.s"
    if ! diff "$want.s" "$got" >"$WORK/drift.diff"; then echo "image and image_files.txt disagree:"; cat "$WORK/drift.diff"; rc=1; fi
    return $rc
}
check "image content/modes/links == image_files.txt == kit overlay (no drift)" drift_check

echo
echo "kit_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
