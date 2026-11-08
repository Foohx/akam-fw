#!/usr/bin/env bash
# shellcheck disable=SC2016
# Tests for ./akam (wizard core). No camera, no card, no network: every external tool is a stub
# (AKAM_CHECK_HOST, AKAM_VERIFY_KIT, AKAM_ASSEMBLE, AKAM_SD_CLI, unsquashfs on PATH).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AKAM="$ROOT/akam"
T=$(mktemp -d)
trap 'if [ -n "${KEEPOUT:-}" ]; then cp -r "$T"/*.out "$KEEPOUT"/ 2>/dev/null; fi; chmod -R u+rwX "$T" 2>/dev/null; rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { # name, command...
    local n=$1
    shift
    if "$@"; then ok "$n"; else ko "$n"; fi
}
has() { grep -qF -- "$2" "$1"; }
hasnt() { ! grep -qF -- "$2" "$1"; }

SENTINEL="WifiP4ss_SENTINEL_91x"

# ---- stubs
STUBS=$T/stubs
mkdir -p "$STUBS/bin"
cat >"$STUBS/check_host.sh" <<'EOF'
#!/usr/bin/env bash
echo "stub check_host $*"
exit "${STUB_CH_RC:-0}"
EOF
cat >"$STUBS/verify_kit.sh" <<'EOF'
#!/usr/bin/env bash
# stub: --extract-to DIR KIT ; STUB_VK_RC=3 means unsigned unless --allow-unsigned is given
ext=""; unsigned=0
while [ $# -gt 1 ]; do
    case $1 in
        --extract-to) ext=$2; shift ;;
        --allow-unsigned) unsigned=1 ;;
    esac
    shift
done
echo "stub verify_kit allow_unsigned=$unsigned" >>"$STUB_LOG"
if [ "${STUB_VK_RC:-0}" = 3 ] && [ "$unsigned" = 0 ]; then exit 3; fi
[ "${STUB_VK_RC:-0}" = 1 ] && exit 1
mkdir -p "$ext"
printf 'kit_format=%s\nakam_version=1.0.0-test\n' "${STUB_KIT_FORMAT:-1}" >"$ext/MANIFEST.txt"
exit 0
EOF
cat >"$STUBS/assemble.sh" <<'EOF'
#!/usr/bin/env bash
echo "assemble $*" >>"$STUB_LOG"
[ "${STUB_ASM_RC:-0}" = 0 ] || exit "$STUB_ASM_RC"
out=""
while [ $# -gt 0 ]; do [ "$1" = --out ] && out=$2; shift; done
mkdir -p "$out"
head -c 5242880 /dev/zero >"$out/ak_rtsp_firmware_test.squashfs"
printf 'compression: xz\nhardware verdict: stub\n' >"$out/build_report.txt"
EOF
cat >"$STUBS/sd.sh" <<'EOF'
#!/usr/bin/env bash
echo "sd $* PASS=${AKAM_SD_PASS:-}" >>"$STUB_LOG"
[ "$1" = list-candidates ] && echo "/dev/sdz|8G|FakeCard"
exit 0
EOF
cat >"$STUBS/bin/unsquashfs" <<'EOF'
#!/usr/bin/env bash
case $1 in
    -s) printf 'Found a valid SQUASHFS 4:0 superblock.\nCompression %s\nBlock size 131072\n' "${STUB_COMP:-xz}" ;;
    -l) printf 'squashfs-root\nsquashfs-root/bin\nsquashfs-root/sbin\nsquashfs-root/local\n' ;;
esac
EOF
chmod +x "$STUBS"/*.sh "$STUBS/bin/unsquashfs"
cat >"$STUBS/sudo" <<'EOF'
#!/usr/bin/env bash
echo "SUDO RAN" >>"$STUB_LOG"
EOF
chmod +x "$STUBS/sudo"

export STUB_LOG=$T/stub.log
export PATH="$STUBS/bin:$PATH"
export AKAM_CHECK_HOST=$STUBS/check_host.sh AKAM_VERIFY_KIT=$STUBS/verify_kit.sh
export AKAM_ASSEMBLE=$STUBS/assemble.sh AKAM_SD_CLI=$STUBS/sd.sh
unset AKAM_SD_PASS AKAM_SD_SSID AKAM_STATE_DIR NO_COLOR AKAM_FORCE_TTY

# ---- fixtures
KIT=$T/akam-kit-1.0.0-test.tar.xz
echo "kit bytes" >"$KIT"
BK=$T/backup
mkdir -p "$BK"
for i in 0 1 2 3 4 5 6; do echo "part $i" >"$BK/mtdblock$i.bin"; done
head -c 5242880 /dev/urandom >"$BK/mtdblock7.bin"
(cd "$BK" && sha256sum mtdblock*.bin >SHA256SUMS)
APP_SHA=$(sha256sum "$BK/mtdblock7.bin" | cut -d' ' -f1)
HWV=$T/hw_validated.txt
printf 'supported_hardware_format=1\nvalidated|Test cam|SOC|k|8 partitions|squashfs xz block=131072 bcj=none|%s|1.30.1\n' "$APP_SHA" >"$HWV"
PUB=$T/akam.pub
echo "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAI$(printf 'A%.0s' $(seq 1 47)) test" >"$PUB"
PRIV=$T/akam_priv.pub
printf -- '-----BEGIN OPENSSH PRIVATE KEY-----\nxxxx\n' >"$PRIV"

new_state() { rm -rf "$T/st"; mkdir -p "$T/st"; : >"$STUB_LOG"; }
# a ARGS... : run ./akam without a terminal (stdin closed)
a() { "$AKAM" --state-dir "$T/st" "$@" </dev/null; }
# at INPUT ARGS... : run with a pretend terminal and INPUT on stdin
at() {
    local input=$1
    shift
    printf '%s\n' "$input" | AKAM_FORCE_TTY=1 "$AKAM" --state-dir "$T/st" "$@"
}
state_get() { grep -m1 "^$1=" "$T/st/state" | cut -d= -f2-; }

# ---------------------------------------------------------------- help and usage
out=$("$AKAM" help </dev/null)
check "help lists the subcommands" bash -c '[[ "$1" == *doctor* && "$1" == *get-kit* && "$1" == *prepare-flash-sd* && "$1" == *--dry-run* ]]' _ "$out"
"$AKAM" frobnicate </dev/null >/dev/null 2>&1
check "unknown command exits 2" test $? -eq 2
"$AKAM" --kit </dev/null >/dev/null 2>&1
check "option without value exits 2" test $? -eq 2

# ---------------------------------------------------------------- full dry run
new_state
a --dry-run >"$T/dry.out" 2>&1
check "full dry-run succeeds without camera or card" test $? -eq 0
check "dry-run shows WOULD run lines" has "$T/dry.out" "WOULD run:"
check "dry-run walks all nine steps" has "$T/dry.out" "Step 9/9"
check "dry-run asks nothing real (typed prompts only shown)" has "$T/dry.out" "WOULD ask: Erase"
check "dry-run executed no stub tool" bash -c '! grep -q -e "^assemble" -e "^sd prepare" -e "^sd verify" -e "SUDO" "$1"' _ "$STUB_LOG"
check "dry-run never writes the real state" bash -c '! grep -q step_ "$1" 2>/dev/null' _ "$T/st/state"
check "dry-run keeps its own state file" test -s "$T/st/state.dry"
check "dry-run output has no colour escapes" bash -c '! grep -q $'"'"'\033'"'"' "$1"' _ "$T/dry.out"

# ---------------------------------------------------------------- status
new_state
a status >"$T/s.out" 2>&1
check "status shows todo and blocked" bash -c 'grep -q "\[todo" "$1" && grep -q "\[blocked" "$1"' _ "$T/s.out"
check "status shows a why/risk line per step" bash -c '[ "$(grep -c "Why:" "$1")" -eq 9 ]' _ "$T/s.out"
a >"$T/m.out" 2>&1
check "menu without a terminal prints status only" has "$T/m.out" "No terminal attached"

# ---------------------------------------------------------------- doctor
new_state
a doctor >"$T/d.out" 2>&1
check "doctor passes with the stub" test $? -eq 0
check "doctor records the step" test -n "$(state_get step_doctor_done)"
check "doctor used the kit profile" has "$T/d.out" "--profile kit"
STUB_CH_RC=1 a doctor >"$T/d2.out" 2>&1
check "doctor failure is reported with what to do" bash -c 'grep -q "never runs sudo" "$1"' _ "$T/d2.out"

# ---------------------------------------------------------------- get-kit
new_state
a get-kit --kit "$KIT" >"$T/g.out" 2>&1
check "get-kit is blocked before doctor" test $? -ne 0
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >"$T/g.out" 2>&1
check "get-kit with a local kit succeeds" test $? -eq 0
check "kit sha is recorded" test "$(state_get kit_sha)" = "$(sha256sum "$KIT" | cut -d' ' -f1)"
a get-kit --kit "$KIT" --allow-unsigned >/dev/null 2>&1
check "get-kit twice is fine" test $? -eq 0
STUB_VK_RC=1 a get-kit --kit "$KIT" >"$T/g1.out" 2>&1
check "failed verification stops get-kit" test $? -ne 0
new_state
a doctor >/dev/null 2>&1
STUB_VK_RC=3 a get-kit --kit "$KIT" >"$T/g3.out" 2>&1
check "unsigned kit without a terminal is refused" test $? -ne 0
check "unsigned kit leaves no state" test -z "$(state_get step_get_kit_done)"
STUB_VK_RC=3 at "USE UNSIGNED KIT" get-kit --kit "$KIT" >"$T/g4.out" 2>&1
check "unsigned kit accepted after the typed phrase" test $? -eq 0
new_state
a doctor >/dev/null 2>&1
STUB_KIT_FORMAT=9 a get-kit --kit "$KIT" >"$T/g5.out" 2>&1
check "unsupported kit_format is refused" test $? -ne 0
check "kit_format refusal explains itself" has "$T/g5.out" "does not support"
a get-kit >"$T/g6.out" 2>&1
check "development checkout cannot download without --version" has "$T/g6.out" "--version"

# ---------------------------------------------------------------- backup import
new_state
a doctor >/dev/null 2>&1
a backup --backup-dir "$BK" >"$T/b.out" 2>&1
check "backup import without a terminal is refused (second-copy confirmation)" test $? -ne 0
at "y" backup --backup-dir "$BK" >"$T/b2.out" 2>&1
check "backup import accepted after the second-copy confirmation" test $? -eq 0
check "backup reminder mentions a second medium" has "$T/b2.out" "SECOND MEDIUM"
check "backup sha recorded" test "$(state_get backup_sha)" = "$APP_SHA"
mkdir -p "$T/bk8"
cp "$BK"/mtdblock*.bin "$T/bk8/"
echo x >"$T/bk8/mtdblock8.bin"
at "y" backup --backup-dir "$T/bk8" >"$T/b3.out" 2>&1
check "a backup with a ninth partition (other layout) is refused" test $? -ne 0
mkdir -p "$T/bkshort"
cp "$BK"/mtdblock*.bin "$T/bkshort/"
echo short >"$T/bkshort/mtdblock7.bin"
at "y" backup --backup-dir "$T/bkshort" >"$T/b4.out" 2>&1
check "an APP dump of the wrong size is refused" test $? -ne 0

# ---------------------------------------------------------------- image
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
OUT1=$T/img1
AKAM_HW_LIST="$ROOT/tools/release/supported_hardware.txt" a image --out "$OUT1" >"$T/i1.out" 2>&1
check "untested firmware without a terminal is refused" test $? -ne 0
check "assemble not run for refused image" bash -c '! grep -q "^assemble" "$1"' _ "$STUB_LOG"
AKAM_HW_LIST="$ROOT/tools/release/supported_hardware.txt" at "nope" image --out "$OUT1" >"$T/i2.out" 2>&1
check "wrong typed phrase is refused" test $? -ne 0
check "untested warning is explicit" has "$T/i2.out" "NOT on the validated list"
check "compression of the stock APP is shown" has "$T/i2.out" "Stock APP compression: xz"
AKAM_HW_LIST="$ROOT/tools/release/supported_hardware.txt" at "I UNDERSTAND THIS IS UNTESTED" image --out "$OUT1" >"$T/i3.out" 2>&1
check "untested firmware accepted after the exact phrase" test $? -eq 0
check "assemble got --i-understand-untested" has "$STUB_LOG" "--i-understand-untested"
check "image state recorded" test "$(state_get image_sha)" = "$(sha256sum "$OUT1/ak_rtsp_firmware_test.squashfs" | cut -d' ' -f1)"
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
AKAM_HW_LIST=$HWV a image --out "$T/img2" >"$T/i4.out" 2>&1
check "validated firmware needs no confirmation" test $? -eq 0
check "validated verdict displayed" has "$T/i4.out" "Hardware verdict: validated"
check "validated build passes no untested flag" bash -c '! grep -q -e "--i-understand-untested" "$1"' _ "$STUB_LOG"
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
STUB_COMP=lzo AKAM_HW_LIST=$HWV a image --out "$T/img3" >"$T/i5.out" 2>&1
check "unsupported stock compression (lzo) is refused" test $? -ne 0
check "lzo refusal says only xz and gzip are supported" has "$T/i5.out" "only xz and gzip"

# ---------------------------------------------------------------- resume after interruption
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
STUB_ASM_RC=1 AKAM_HW_LIST=$HWV a image --out "$T/img4" >"$T/r1.out" 2>&1
check "interrupted image build fails" test $? -ne 0
a status >"$T/r2.out" 2>&1
check "after the interruption earlier steps are still done" bash -c 'grep -q "1. \[done" "$1" && grep -q "2. \[done" "$1" && grep -q "6. \[todo" "$1"' _ "$T/r2.out"
AKAM_HW_LIST=$HWV a image --out "$T/img4" >"$T/r3.out" 2>&1
check "the next run resumes and finishes the image" test $? -eq 0
check "image step now done" test -n "$(state_get step_image_done)"

# ---------------------------------------------------------------- stale state
echo "tampered" >>"$KIT"
a status >"$T/st1.out" 2>&1
check "changed kit makes get-kit stale" has "$T/st1.out" "the kit file changed"
check "changed kit makes the image stale too" has "$T/st1.out" "the kit changed since the image was built"
check "a stale step is shown as todo" bash -c 'grep -q "2. \[todo" "$1"' _ "$T/st1.out"
a get-kit --kit "$KIT" >/dev/null 2>&1
check "re-verifying the kit refreshes get-kit" test "$(state_get kit_sha)" = "$(sha256sum "$KIT" | cut -d' ' -f1)"
a status >"$T/st2.out" 2>&1
check "image still stale until rebuilt (kit differs)" has "$T/st2.out" "the kit changed since the image was built"
AKAM_HW_LIST=$HWV a image --out "$T/img5" >/dev/null 2>&1
a status >"$T/st3.out" 2>&1
check "rebuilding clears the stale image" bash -c '! grep -q "Inputs changed" "$1"' _ "$T/st3.out"
echo "different" >"$BK/mtdblock7.bin"
a status >"$T/st4.out" 2>&1
check "changed APP dump makes the backup stale" has "$T/st4.out" "the APP dump (mtdblock7.bin) changed"

# ---------------------------------------------------------------- prepare SD cards, secrets, TTY
head -c 5242880 /dev/urandom >"$BK/mtdblock7.bin"
(cd "$BK" && sha256sum mtdblock*.bin >SHA256SUMS)
APP_SHA=$(sha256sum "$BK/mtdblock7.bin" | cut -d' ' -f1)
printf 'supported_hardware_format=1\nvalidated|Test cam|SOC|k|8 partitions|squashfs xz block=131072 bcj=none|%s|1.30.1\n' "$APP_SHA" >"$HWV"
new_state
a doctor >/dev/null 2>&1
AKAM_SD_PASS=$SENTINEL a prepare-backup-sd --device /dev/sdz --ssid MyNet >"$T/p1.out" 2>&1
check "erasing a card without a terminal is refused" test $? -ne 0
check "the SD tool was not called" bash -c '! grep -q "^sd " "$1"' _ "$STUB_LOG"
AKAM_SD_PASS=$SENTINEL at "ERASE sdy" prepare-backup-sd --device /dev/sdz --ssid MyNet >"$T/p2.out" 2>&1
check "wrong ERASE phrase is refused" test $? -ne 0
AKAM_SD_PASS=$SENTINEL at "ERASE sdz" prepare-backup-sd --device /dev/sdz --ssid MyNet >"$T/p3.out" 2>&1
check "backup card prepared after the typed ERASE phrase" test $? -eq 0
check "SD tool received prepare --mode backup" has "$STUB_LOG" "sd prepare --mode backup --device /dev/sdz"
check "the SD tool got the password through the environment" has "$STUB_LOG" "PASS=$SENTINEL"
check "no secret in the state file" bash -c '! grep -rq "$2" "$1"' _ "$T/st/state" "$SENTINEL"
check "no secret in akam.log" bash -c '! grep -rq "$2" "$1"' _ "$T/st/akam.log" "$SENTINEL"
check "no secret on the terminal output" bash -c '! grep -rq "$2" "$1"' _ "$T/p3.out" "$SENTINEL"
check "log records commands" has "$T/st/akam.log" "CMD"
check "state dir is private" bash -c '[ "$(stat -c %a "$1")" = 700 ]' _ "$T/st"
# password typed at the prompt (no env): also never stored
new_state
a doctor >/dev/null 2>&1
printf 'ERASE sdz\n%s\n' "$SENTINEL" >"$T/in.txt"
# order of prompts: ERASE comes after SSID and password prompts
printf 'MyNet\n%s\nERASE sdz\n' "$SENTINEL" | AKAM_FORCE_TTY=1 "$AKAM" --state-dir "$T/st" prepare-backup-sd --device /dev/sdz >"$T/p4.out" 2>&1
check "password typed at the prompt reaches the SD tool" has "$STUB_LOG" "PASS=$SENTINEL"
check "typed password not in state or log" bash -c '! grep -rq "$2" "$1"' _ "$T/st" "$SENTINEL"
check "typed password not echoed" bash -c '! grep -q "$2" "$1"' _ "$T/p4.out" "$SENTINEL"

# flash card: needs an image
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
AKAM_HW_LIST=$HWV a image --out "$T/img6" >/dev/null 2>&1
: >"$STUB_LOG"
at "ERASE sdz" prepare-flash-sd --device /dev/sdz --pubkey "$PRIV" --ssid N >"$T/f1.out" 2>&1
check "a private key is never accepted for the card" test $? -ne 0
check "private key refusal is explicit" has "$T/f1.out" "PRIVATE key"
AKAM_SD_PASS=$SENTINEL a prepare-flash-sd --device /dev/sdz --pubkey "$PUB" --ssid N >"$T/f2.out" 2>&1
check "flash card without a terminal is refused" test $? -ne 0
check "SD tool untouched when refused" bash -c '! grep -q "^sd " "$1"' _ "$STUB_LOG"
AKAM_SD_PASS=$SENTINEL at "ERASE sdz" prepare-flash-sd --device /dev/sdz --pubkey "$PUB" --ssid N >"$T/f3.out" 2>&1
check "flash card prepared" test $? -eq 0
check "SD tool got first-boot mode, key and image" bash -c 'grep -q "prepare --mode first-boot --device /dev/sdz --pubkey" "$1" && grep -q "ak_rtsp_firmware_test.squashfs" "$1"' _ "$STUB_LOG"
check "flash card recorded for this image" test "$(state_get flash_sd_image_sha)" = "$(state_get image_sha)"
echo changed >>"$T/img6/ak_rtsp_firmware_test.squashfs"
a status >"$T/f4.out" 2>&1
check "a changed image makes the prepared flash card stale" has "$T/f4.out" "the image file changed after it was built"

# ---------------------------------------------------------------- sudo is never run
check "run_cmd refuses sudo and prints the command" bash -c '
    ROOT=$1; export PATH="$2:$PATH"
    . "$ROOT/tools/lib/host.sh"; . "$ROOT/tools/wizard/lib/log.sh"; . "$ROOT/tools/wizard/lib/ui.sh"
    . "$ROOT/tools/wizard/lib/state.sh"; . "$ROOT/tools/wizard/lib/steps_host.sh"
    out=$(run_cmd sudo pacman -S foo 2>&1); rc=$?
    [ "$rc" -ne 0 ] && [[ "$out" == *"sudo pacman -S foo"* ]]' _ "$ROOT" "$STUBS"
check "sudo stub never ran" bash -c '! grep -q SUDO "$1"' _ "$STUB_LOG"

# ---------------------------------------------------------------- state lib guards
check "state_set refuses a registered secret and a bad key" bash -c '
    ROOT=$1; . "$ROOT/tools/wizard/lib/log.sh"; . "$ROOT/tools/wizard/lib/state.sh"
    state_init "$2/sl" 0; log_add_secret "hunter2hunter2"
    ! state_set k "x hunter2hunter2 y" 2>/dev/null && ! state_set "Bad-Key" v 2>/dev/null && state_set ok_key v' _ "$ROOT" "$T"

echo
echo "wizard_core_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
