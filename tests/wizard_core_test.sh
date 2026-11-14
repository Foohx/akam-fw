#!/usr/bin/env bash
# shellcheck disable=SC2016
# Tests for ./akam (wizard core). No camera, no card, no network. Stubs: AKAM_CHECK_HOST,
# AKAM_VERIFY_KIT, AKAM_ASSEMBLE, unsquashfs and curl on PATH. The SD card tool is the REAL
# tools/sd/sd_card.sh: a thin wrapper (AKAM_SD_CLI) adds --dry-run --stage-dir so that no device
# is ever touched, on top of a fake lsblk/sysfs (same fakes as tests/sd_card_test.sh).
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
        --expect-model) echo "stub verify_kit expect-model=$2" >>"$STUB_LOG"; shift ;;
    esac
    shift
done
echo "stub verify_kit allow_unsigned=$unsigned" >>"$STUB_LOG"
if [ "${STUB_VK_RC:-0}" = 3 ] && [ "$unsigned" = 0 ]; then exit 3; fi
[ "${STUB_VK_RC:-0}" = 1 ] && exit 1
mkdir -p "$ext/bin"
echo "flash_tool test binary" >"$ext/bin/flash_tool"
printf 'kit_format=%s\nakam_version=1.0.0-test\nmodel=lsc-indoor-1080p-ak3918\n' "${STUB_KIT_FORMAT:-1}" >"$ext/MANIFEST.txt"
exit 0
EOF
cat >"$STUBS/assemble.sh" <<'EOF'
#!/usr/bin/env bash
echo "assemble $*" >>"$STUB_LOG"
[ "${STUB_ASM_RC:-0}" = 0 ] || exit "$STUB_ASM_RC"
out=""
while [ $# -gt 0 ]; do [ "$1" = --out ] && out=$2; shift; done
mkdir -p "$out"
head -c 5242880 /dev/zero >"$out/ak_rtsp_firmware_20260101_000000.squashfs"
printf 'compression: xz\nhardware verdict: stub\n' >"$out/build_report.txt"
EOF
# Fake lsblk / findmnt / sysfs: sda = system disk, sdb = a removable USB card reader (3.7 GB).
FD=$T/lsblk
SYS=$T/sys
mkdir -p "$FD" "$SYS/class/block/sda" "$SYS/class/block/sdb"
cat >"$STUBS/bin/lsblk" <<'EOF'
#!/usr/bin/env bash
last=${*: -1}
case " $* " in
    *MOUNTPOINTS*) exit 1 ;;
    *" -nspro "*) cat "$FAKE_LSBLK_DIR/inv_$(basename "$last").txt" 2>/dev/null || exit 1 ;;
    *" -dpP "*)
        if [ "${last#/dev/}" != "$last" ]; then grep -F "NAME=\"$last\"" "$FAKE_LSBLK_DIR/disks.txt"; else cat "$FAKE_LSBLK_DIR/disks.txt"; fi ;;
    *) cat "$FAKE_LSBLK_DIR/tree_$(basename "$last").txt" 2>/dev/null | sed 's/ MOUNTPOINTS="[^"]*"//'; [ -f "$FAKE_LSBLK_DIR/tree_$(basename "$last").txt" ] || exit 1 ;;
esac
EOF
printf '#!/usr/bin/env bash\nexit 1\n' >"$STUBS/bin/findmnt"
cat >"$FD/disks.txt" <<'EOF'
NAME="/dev/sda" TYPE="disk" RM="0" TRAN="sata" MODEL="Samsung\x20SSD"
NAME="/dev/sdb" TYPE="disk" RM="1" TRAN="usb" MODEL="Card\x20Reader"
EOF
printf 'NAME="/dev/sda" TYPE="disk" MOUNTPOINT=""\nNAME="/dev/sda1" TYPE="part" MOUNTPOINT="/"\n' >"$FD/tree_sda.txt"
printf 'NAME="/dev/sdb" TYPE="disk" MOUNTPOINT=""\nNAME="/dev/sdb1" TYPE="part" MOUNTPOINT="/run/media/u/AKAMSD"\n' >"$FD/tree_sdb.txt"
echo 976773168 >"$SYS/class/block/sda/size"
echo 7744512 >"$SYS/class/block/sdb/size"
export FAKE_LSBLK_DIR=$FD AKAM_SYSFS=$SYS

# Synthetic toolkit (the real tool pins the real one; the test pin is honoured in --dry-run only).
TK=$T/toolkit
mkdir -p "$TK/sd_card/custom/scripts" "$TK/sd_card/custom/configs" "$TK/sd_card/custom/state"
printf '#!/bin/sh\necho hack' >"$TK/sd_card/hack.sh"
printf 'TELNET=0\nTELNET_PORT=23\nOFFLINE=0\nONVIF=1\n' >"$TK/sd_card/custom/configs/hack.conf"
printf '#!/bin/sh\necho entry\n' >"$TK/sd_card/custom/scripts/entrypoint.sh"
: >"$TK/sd_card/custom/state/shadow.done"
TK_HASH=$( (cd "$TK/sd_card" && find . -type f | LC_ALL=C sort | LC_ALL=C xargs sha256sum | sha256sum) | cut -d' ' -f1)
export AKAM_TOOLKIT_DIR=$TK
CARD=$T/card
export CARD TK_HASH REAL_SD="$ROOT/tools/sd/sd_card.sh"

# The wizard calls this instead of sd_card.sh: same CLI, but prepare/wipe never touch a device
# (--dry-run --stage-dir) and verify reads the staged tree instead of the card.
cat >"$STUBS/sd.sh" <<'EOF'
#!/usr/bin/env bash
cmd=${1:-}
[ $# -eq 0 ] || shift
echo "sd $cmd $*" >>"$STUB_LOG"
if [ -n "${AKAM_WIFI_PASS:-}${AKAM_WIFI_SSID:-}" ]; then echo "sd env: ssid=${AKAM_WIFI_SSID:+set} pass=${AKAM_WIFI_PASS:+set}" >>"$STUB_LOG"; fi
mode=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do [ "${args[$i]}" = --mode ] && mode=${args[$((i + 1))]}; done
extra=()
case $cmd in
    prepare) rm -rf "${CARD:?}/$mode"; extra=(--dry-run --stage-dir "$CARD/$mode") ;;
    wipe) extra=(--dry-run) ;;
    verify)
        out=()
        for ((i = 0; i < ${#args[@]}; i++)); do
            if [ "${args[$i]}" = --device ]; then i=$((i + 1)); else out+=("${args[$i]}"); fi
        done
        args=("${out[@]}")
        extra=(--stage-dir "$CARD/$mode")
        ;;
esac
AKAM_TEST_DRYRUN_ONLY=1 AKAM_TEST_PIN_TREE_SHA=$TK_HASH exec bash "$REAL_SD" "$cmd" ${args[@]+"${args[@]}"} ${extra[@]+"${extra[@]}"}
EOF
cat >"$STUBS/bin/curl" <<'EOF'
#!/usr/bin/env bash
# stub curl: records the call; STUB_CURL_MODE=notfound (HTTP 404 -> exit 22), neterr (exit 6) or ok (copies from STUB_CURL_SRC)
out=""; url=""; proto=""
while [ $# -gt 0 ]; do
    case $1 in
        -o) out=$2; shift ;;
        --proto) proto=$2; shift ;;
        https://*) url=$1 ;;
    esac
    shift
done
echo "curl proto=$proto $url" >>"$STUB_LOG"
case $url in
    https://api.github.com/*releases\?*) # the release list: page N is $STUB_CURL_SRC/api_releases_N.json
        [ "${STUB_CURL_MODE:-notfound}" = ok ] || exit 22
        page=${url##*page=}
        [ -f "$STUB_CURL_SRC/api_releases_$page.json" ] || exit 22
        cp "$STUB_CURL_SRC/api_releases_$page.json" "$out"
        exit 0
        ;;
esac
case ${STUB_CURL_MODE:-notfound} in
    notfound) exit 22 ;;
    neterr) exit 6 ;;
    ok) [ -f "$STUB_CURL_SRC/$(basename "$url")" ] || exit 22; cp "$STUB_CURL_SRC/$(basename "$url")" "$out" ;;
esac
EOF
cat >"$STUBS/bin/unsquashfs" <<'EOF'
#!/usr/bin/env bash
case $1 in
    -s) printf 'Found a valid SQUASHFS 4:0 superblock.\nCompression %s\nBlock size 131072\n' "${STUB_COMP:-xz}" ;;
    -l) printf 'squashfs-root\nsquashfs-root/bin\nsquashfs-root/sbin\nsquashfs-root/local\n' ;;
esac
EOF
chmod +x "$STUBS"/*.sh "$STUBS"/bin/*
cat >"$STUBS/sudo" <<'EOF'
#!/usr/bin/env bash
echo "SUDO RAN" >>"$STUB_LOG"
EOF
chmod +x "$STUBS/sudo"

export STUB_LOG=$T/stub.log
export PATH="$STUBS/bin:$PATH"
export AKAM_CHECK_HOST=$STUBS/check_host.sh AKAM_VERIFY_KIT=$STUBS/verify_kit.sh
export AKAM_ASSEMBLE=$STUBS/assemble.sh AKAM_SD_CLI=$STUBS/sd.sh
unset AKAM_SD_PASS AKAM_SD_SSID AKAM_WIFI_PASS AKAM_WIFI_SSID AKAM_STATE_DIR NO_COLOR AKAM_FORCE_TTY

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
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopq akam-test' >"$PUB"
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
check "dry-run asks nothing real (confirmations only shown)" has "$T/dry.out" "WOULD ask: Erase"
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
STUB_VK_RC=3 at "y" get-kit --kit "$KIT" >"$T/g4.out" 2>&1
check "unsigned kit accepted after an explicit yes (red box)" test $? -eq 0
new_state
a doctor >/dev/null 2>&1
STUB_KIT_FORMAT=9 a get-kit --kit "$KIT" >"$T/g5.out" 2>&1
check "unsupported kit_format is refused" test $? -ne 0
check "kit_format refusal explains itself" has "$T/g5.out" "does not support"
a get-kit >"$T/g6.out" 2>&1
check "development checkout cannot download without --version" has "$T/g6.out" "--version"

# ---------------------------------------------------------------- get-kit download
new_state
a doctor >/dev/null 2>&1
a get-kit --version 1.0.0-test >"$T/dl1.out" 2>&1
check "no release published: get-kit fails" test $? -ne 0
check "  ... says the release was not found" has "$T/dl1.out" "was not found"
check "  ... and suggests --kit FILE" has "$T/dl1.out" "--kit FILE"
check "  ... downloaded over https only from the project releases" has "$STUB_LOG" "curl proto==https https://github.com/Foohx/akam-fw/releases/download/firmware-v1.0.0-test/akam-kit-lsc-indoor-1080p-ak3918-1.0.0-test.tar.xz"
check "  ... nothing was verified or run" bash -c '! grep -q "stub verify_kit" "$1"' _ "$STUB_LOG"
check "  ... and nothing recorded" test -z "$(state_get step_get_kit_done)"
STUB_CURL_MODE=neterr a get-kit --version 1.0.0-test >"$T/dl2.out" 2>&1
check "network error is reported as such" has "$T/dl2.out" "curl exit 6"
a get-kit --version '../evil' >"$T/dl3.out" 2>&1
check "a path-like version is refused" has "$T/dl3.out" "not a valid version"
a get-kit >"$T/dl4.out" 2>&1
check "a development checkout without --version suggests --kit FILE" has "$T/dl4.out" "--kit FILE"
SRC=$T/relsrc
mkdir -p "$SRC"
echo "kit bytes" >"$SRC/akam-kit-lsc-indoor-1080p-ak3918-1.0.0-test.tar.xz"
echo "sums" >"$SRC/SHA256SUMS"
echo "sig" >"$SRC/SHA256SUMS.sig"
: >"$STUB_LOG"
STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC a get-kit --version 1.0.0-test >"$T/dl5.out" 2>&1
check "download + verification succeeds with a signed release" test $? -eq 0
check "the downloaded kit went through verify_kit (checksum + signature)" has "$STUB_LOG" "stub verify_kit allow_unsigned=0"
check "verify_kit ran after the downloads" bash -c '[ "$(grep -n "curl" "$1" | tail -1 | cut -d: -f1)" -lt "$(grep -n "stub verify_kit" "$1" | head -1 | cut -d: -f1)" ]' _ "$STUB_LOG"
check "the kit path recorded is the downloaded file" has "$T/st/state" "kit_path=$T/st/downloads/1.0.0-test/akam-kit-lsc-indoor-1080p-ak3918-1.0.0-test.tar.xz"
check "the kit is verified for the model (the only one of the registry)" has "$STUB_LOG" "stub verify_kit expect-model=lsc-indoor-1080p-ak3918"
check "the kit model is recorded" test "$(state_get kit_model)" = lsc-indoor-1080p-ak3918
# no --version: the newest firmware release with a kit for the model is read from the (stubbed) GitHub API
K=akam-kit-lsc-indoor-1080p-ak3918
asset_list() { printf '{"name":"%s"},{"name":"SHA256SUMS"},{"name":"SHA256SUMS.sig"}' "$1"; }
cat >"$SRC/api_releases_1.json" <<APIEOF
[
 {"tag_name":"firmware-v1.1.0","draft":true,"prerelease":false,"assets":[$(asset_list $K-1.1.0.tar.xz)]},
 {"tag_name":"firmware-v1.0.5","draft":false,"prerelease":true,"assets":[$(asset_list $K-1.0.5.tar.xz)]},
 {"tag_name":"assistant-v9.0.0","draft":false,"prerelease":false,"assets":[{"name":"akam-linux-amd64"}]},
 {"tag_name":"firmware-v1.0.3","draft":false,"prerelease":false,"assets":[$(asset_list akam-kit-other-model-1.0.3.tar.xz)]},
 {"tag_name":"firmware-v1.0.2-rc1","draft":false,"prerelease":false,"assets":[$(asset_list $K-1.0.2-rc1.tar.xz)]},
 {"tag_name":"firmware-v1.0.1","draft":false,"prerelease":false,"assets":[$(asset_list $K-1.0.1.tar.xz)]},
 {"tag_name":"firmware-v1.0.0","draft":false,"prerelease":false,"assets":[$(asset_list $K-1.0.0.tar.xz)]}
]
APIEOF
echo "kit bytes 101" >"$SRC/$K-1.0.1.tar.xz"
new_state
a doctor >/dev/null 2>&1
if command -v python3 >/dev/null 2>&1; then
    STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC a get-kit >"$T/dl5c.out" 2>&1
    check "no --version: the newest usable release for the model is chosen (no draft, prerelease, other model, odd tag)" has "$T/dl5c.out" "Newest firmware release with a kit for lsc-indoor-1080p-ak3918: 1.0.1"
    check "  ... and that release is the one downloaded, over https" has "$STUB_LOG" "curl proto==https https://github.com/Foohx/akam-fw/releases/download/firmware-v1.0.1/$K-1.0.1.tar.xz"
    check "  ... the list was asked from the GitHub API over https" has "$STUB_LOG" "https://api.github.com/repos/Foohx/akam-fw/releases?per_page=50&page=1"
    check "  ... the kit went through verify_kit" test "$(state_get kit_path)" = "$T/st/downloads/1.0.1/$K-1.0.1.tar.xz"
    new_state
    a doctor >/dev/null 2>&1
    STUB_CURL_MODE=neterr a get-kit >"$T/dl5d.out" 2>&1
    check "no --version and GitHub unreachable: refused, asks for --version" has "$T/dl5d.out" "--version"
else
    echo "  skip - python3 not installed, release list not parsed" # (the wizard falls back to --version / a prompt)
fi
new_state
a doctor >/dev/null 2>&1
state_set_test() { printf '%s=%s\n' "$1" "$2" >>"$T/st/state"; }
state_set_test camera_model lsc-indoor-1080p-ak3918
STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC a get-kit --model other-model --version 1.0.0-test >"$T/dl5e.out" 2>&1
check "--model that differs from the recorded camera model is refused" test $? -ne 0
check "  ... with a clear message" has "$T/dl5e.out" "recorded as the model lsc-indoor-1080p-ak3918, but --model asks for other-model"
new_state
a doctor >/dev/null 2>&1
STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC a get-kit --model other-model --version 1.0.0-test >"$T/dl5b.out" 2>&1
check "--model asks for another model's kit file: a release without it says so" has "$T/dl5b.out" "no kit for the model other-model"
new_state
a doctor >/dev/null 2>&1
rm -f "$SRC/SHA256SUMS.sig"
STUB_VK_RC=3 STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC a get-kit --version 1.0.0-test >"$T/dl6.out" 2>&1
check "an unsigned release is refused without a terminal" test $? -ne 0
check "  ... with a warning about the missing signature" has "$T/dl6.out" "no signature file"
STUB_VK_RC=3 STUB_CURL_MODE=ok STUB_CURL_SRC=$SRC at "y" get-kit --version 1.0.0-test >"$T/dl7.out" 2>&1
check "an unsigned release is accepted after an explicit yes" test $? -eq 0
check "the unsigned decision is remembered" test "$(state_get kit_unsigned_ok)" = 1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
: >"$STUB_LOG"
AKAM_HW_LIST=$HWV a image --out "$T/img7" >/dev/null 2>&1
check "the image step reuses it (assemble gets --allow-unsigned)" has "$STUB_LOG" "--allow-unsigned"

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
AKAM_HW_LIST="$ROOT/tools/release/models/lsc-indoor-1080p-ak3918.txt" a image --out "$OUT1" >"$T/i1.out" 2>&1
check "untested firmware without a terminal is refused" test $? -ne 0
check "assemble not run for refused image" bash -c '! grep -q "^assemble" "$1"' _ "$STUB_LOG"
AKAM_HW_LIST="$ROOT/tools/release/models/lsc-indoor-1080p-ak3918.txt" at "n" image --out "$OUT1" >"$T/i2.out" 2>&1
check "answering no to untested firmware is refused" test $? -ne 0
check "untested warning is explicit" has "$T/i2.out" "NOT on the validated list"
check "compression of the stock APP is shown" has "$T/i2.out" "Stock APP compression: xz"
AKAM_HW_LIST="$ROOT/tools/release/models/lsc-indoor-1080p-ak3918.txt" at "y" image --out "$OUT1" >"$T/i3.out" 2>&1
check "untested firmware accepted after an explicit yes" test $? -eq 0
check "assemble got --i-understand-untested" has "$STUB_LOG" "--i-understand-untested"
check "image state recorded" test "$(state_get image_sha)" = "$(sha256sum "$OUT1/ak_rtsp_firmware_20260101_000000.squashfs" | cut -d' ' -f1)"
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
AKAM_WIFI_PASS=$SENTINEL a prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p1.out" 2>&1
check "erasing a card without a terminal is refused" test $? -ne 0
check "the SD tool was not called" bash -c '! grep -q "^sd " "$1"' _ "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "y" prepare-backup-sd --device /dev/sdc --ssid MyNet >"$T/p1b.out" 2>&1
check "a device that is not a listed card is refused" test $? -ne 0
check "  ... with an explanation" has "$T/p1b.out" "not in the list of usable cards"
AKAM_WIFI_PASS=$SENTINEL at "y" prepare-backup-sd --device /dev/sda --ssid MyNet >"$T/p1c.out" 2>&1
check "the system disk is refused (never listed)" test $? -ne 0
check "no prepare was attempted on refused devices" bash -c '! grep -q "^sd prepare" "$1"' _ "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "n" prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p2.out" 2>&1
check "answering no to the erase question is refused" test $? -ne 0
check "the SD tool did not prepare after a no" bash -c '! grep -q "^sd prepare" "$1"' _ "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "y" prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p3.out" 2>&1
check "backup card prepared after an explicit yes" test $? -eq 0
check "SD tool received prepare --mode backup --device --toolkit" has "$STUB_LOG" "sd prepare --mode backup --device /dev/sdb --yes-i-confirmed /dev/sdb --toolkit $TK"
check "SD tool received verify --mode backup" has "$STUB_LOG" "sd verify --mode backup --device /dev/sdb --toolkit $TK"
check "list-candidates --human and the TSV form were both used" bash -c 'grep -q "^sd list-candidates --human" "$1" && grep -q "^sd list-candidates $" "$1"' _ "$STUB_LOG"
check "the SD tool got the Wi-Fi settings through the environment" has "$STUB_LOG" "sd env: ssid=set pass=set"
check "the real tool staged wifi.conf with the password (delivery proven)" has "$CARD/backup/custom/configs/wifi.conf" "$SENTINEL"
check "the real tool verified the staged backup card" has "$T/p3.out" "VERIFY: OK"
check "no secret in the state file" bash -c '! grep -rq "$2" "$1"' _ "$T/st/state" "$SENTINEL"
check "no secret in akam.log" bash -c '! grep -rq "$2" "$1"' _ "$T/st/akam.log" "$SENTINEL"
check "no secret on the terminal output" bash -c '! grep -rq "$2" "$1"' _ "$T/p3.out" "$SENTINEL"
check "no secret in the stub log (command lines)" bash -c '! grep -rq "$2" "$1"' _ "$STUB_LOG" "$SENTINEL"
check "log records commands" has "$T/st/akam.log" "CMD"
check "state dir is private (700)" bash -c '[ "$(stat -c %a "$1")" = 700 ]' _ "$T/st"
check "state file is private (600)" bash -c '[ "$(stat -c %a "$1")" = 600 ]' _ "$T/st/state"
check "log file is private (600)" bash -c '[ "$(stat -c %a "$1")" = 600 ]' _ "$T/st/akam.log"
# the erase confirmation: a red box naming the device, model and size, ONE yes/no, default No
: >"$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "" prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p5.out" 2>&1
check "erase: an empty answer means No (default No): nothing prepared" bash -c '[ "$1" -ne 0 ] && ! grep -q "^sd prepare" "$2"' _ $? "$STUB_LOG"
check "erase: the warning names the device, its model and size and says what happens" bash -c 'grep -q "Everything on /dev/sdb will be ERASED" "$1" && grep -q "Model: Card Reader" "$1" && grep -q "Size: 3.9 GB" "$1" && grep -q "Erase /dev/sdb now? \[y/N\]" "$1"' _ "$T/p5.out"
check "erase: nothing has to be typed any more (no phrase is asked)" bash -c '! grep -q "type ERASE" "$1"' _ "$T/p5.out"
printf '' | AKAM_WIFI_PASS=$SENTINEL AKAM_FORCE_TTY=1 "$AKAM" --state-dir "$T/st" prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p6.out" 2>&1
check "erase: the end of the input (no answer) is No" bash -c '[ "$1" -ne 0 ] && ! grep -q "^sd prepare" "$2"' _ $? "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL a prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p7.out" 2>&1
check "erase: without a terminal it refuses (no yes can be given)" bash -c '[ "$1" -ne 0 ] && ! grep -q "^sd prepare" "$2"' _ $? "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "yes" prepare-backup-sd --device /dev/sdb --ssid MyNet >"$T/p8.out" 2>&1
check "erase: 'yes' is accepted like 'y'" test $? -eq 0
new_state
a doctor >/dev/null 2>&1
# password typed at the prompt (no env): also never stored
new_state
a doctor >/dev/null 2>&1
printf 'MyNet\n%s\ny\n' "$SENTINEL" | AKAM_FORCE_TTY=1 "$AKAM" --state-dir "$T/st" prepare-backup-sd --device /dev/sdb >"$T/p4.out" 2>&1
check "password typed at the prompt reaches the SD tool" has "$CARD/backup/custom/configs/wifi.conf" "$SENTINEL"
check "typed password not in state or log" bash -c '! grep -rq "$2" "$1"' _ "$T/st" "$SENTINEL"
check "typed password not echoed" bash -c '! grep -q "$2" "$1"' _ "$T/p4.out" "$SENTINEL"
check "typed password not in the stub log" bash -c '! grep -q "$2" "$1"' _ "$STUB_LOG" "$SENTINEL"
# a Wi-Fi password in the environment must not reach other tools started by the wizard
cat >"$STUBS/env_probe.sh" <<'EOF'
#!/usr/bin/env bash
echo "probe sees pass=${AKAM_WIFI_PASS:-}${AKAM_SD_PASS:-}" >>"$STUB_LOG"
exit 0
EOF
chmod +x "$STUBS/env_probe.sh"
new_state
AKAM_CHECK_HOST=$STUBS/env_probe.sh AKAM_SD_PASS=$SENTINEL a doctor >/dev/null 2>&1
check "the Wi-Fi password is removed from the environment of other tools" has "$STUB_LOG" "probe sees pass="
check "  ... (nothing after pass=)" bash -c '! grep -q "probe sees pass=.\+" "$1"' _ "$STUB_LOG"

# flash card: needs an image
new_state
a doctor >/dev/null 2>&1
a get-kit --kit "$KIT" >/dev/null 2>&1
at "y" backup --backup-dir "$BK" >/dev/null 2>&1
AKAM_HW_LIST=$HWV a image --out "$T/img6" >/dev/null 2>&1
: >"$STUB_LOG"
at "y" prepare-flash-sd --device /dev/sdb --pubkey "$PRIV" --ssid N >"$T/f1.out" 2>&1
check "a private key is never accepted for the card" test $? -ne 0
check "private key refusal is explicit" has "$T/f1.out" "PRIVATE key"
AKAM_WIFI_PASS=$SENTINEL a prepare-flash-sd --device /dev/sdb --pubkey "$PUB" --ssid N >"$T/f2.out" 2>&1
check "flash card without a terminal is refused" test $? -ne 0
check "SD tool untouched when refused" bash -c '! grep -q "^sd " "$1"' _ "$STUB_LOG"
AKAM_WIFI_PASS=$SENTINEL at "y" prepare-flash-sd --device /dev/sdb --pubkey "$PUB" --ssid N >"$T/f3.out" 2>&1
check "flash card prepared" test $? -eq 0
check "first-boot prepare got the public key and toolkit" has "$STUB_LOG" "sd prepare --mode first-boot --device /dev/sdb --yes-i-confirmed /dev/sdb --pubkey $PUB --toolkit $TK"
check "flash-files prepare got image, flash_tool and backup dir" bash -c 'grep -q "sd prepare --mode flash-files --device /dev/sdb --image .*ak_rtsp_firmware_20260101_000000.squashfs --flash-tool .*/kit/bin/flash_tool --backup-dir " "$1"' _ "$STUB_LOG"
check "first-boot prepare came before flash-files, verifies came last" bash -c '
    l=$(grep -n -e "^sd prepare --mode first-boot" -e "^sd prepare --mode flash-files" -e "^sd verify --mode first-boot" -e "^sd verify --mode flash-files" "$1" | sed "s/^[0-9]*://" | cut -d" " -f1-4 | tr "\n" ";")
    [ "$l" = "sd prepare --mode first-boot;sd prepare --mode flash-files;sd verify --mode first-boot;sd verify --mode flash-files;" ]' _ "$STUB_LOG"
check "the real tool staged the authorized key" has "$CARD/first-boot/akam/authorized_keys" "ssh-ed25519"
check "the real tool staged the image (name pattern checked by the tool)" test -f "$CARD/flash-files/custom_firmware/ak_rtsp_firmware_20260101_000000.squashfs"
check "the real tool staged flash_tool from the kit" has "$CARD/flash-files/flash_tool" "flash_tool test binary"
check "the real tool staged the stock APP backup" test -f "$CARD/flash-files/original_firmware/mtdblock7.bin"
check "both verifies passed" bash -c '[ "$(grep -c "VERIFY: OK" "$1")" -ge 2 ]' _ "$T/f3.out"
check "no secret in the flash card run" bash -c '! grep -rq "$2" "$1" "$3" "$4"' _ "$T/f3.out" "$SENTINEL" "$T/st/state" "$T/st/akam.log"
check "flash card recorded for this image" test "$(state_get flash_sd_image_sha)" = "$(state_get image_sha)"
check "flash card device recorded (for the wipe offer)" test "$(state_get flash_sd_device)" = /dev/sdb
# ---- SSH key choice: the question "Which SSH key should open this camera?" (shared key, a key for this
# camera only, or an existing ed25519 key of ~/.ssh). A stub ssh-keygen stands in for the real one: it needs
# a terminal for the passphrase, and nothing here may touch a real key.
KGBIN=$T/kgbin
mkdir -p "$KGBIN"
cat >"$KGBIN/ssh-keygen" <<EOF
#!/usr/bin/env bash
echo "ssh-keygen \$*" >>"$T/keygen.log"
f="" c=""
while [ \$# -gt 0 ]; do
    case \$1 in -f) f=\$2; shift ;; -C) c=\$2; shift ;; esac
    shift
done
echo "FAKE PRIVATE" >"\$f"
chmod 600 "\$f"
echo "\$(cut -d' ' -f1,2 "$PUB") \$c" >"\$f.pub"
EOF
chmod +x "$KGBIN/ssh-keygen"
: >"$T/keygen.log"
state_unset_key() { grep -v "^$1=" "$T/st/state" >"$T/st/state.n"; mv "$T/st/state.n" "$T/st/state"; }
KH=$T/home_keys
mkdir -p "$KH/.ssh"
cp "$PUB" "$KH/.ssh/id_work.pub"
echo 'ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQ rsa-ignored' >"$KH/.ssh/id_rsa.pub"
echo 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ other' >"$KH/.ssh/id_other.pub"
state_unset_key ssh_pubkey
: >"$STUB_LOG"
HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'3\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k1.out" 2>&1
check "ssh key: the question offers the shared key, a key for this camera only, then the existing ed25519 keys" bash -c 'grep -q "Which SSH key should open this camera?" "$1" && grep -q "1) Shared key ~/.ssh/akam (all your cameras)" "$1" && grep -q "2) A key for this camera only (~/.ssh/akam-default)" "$1" && grep -q "3) .*id_other.pub" "$1" && grep -q "4) .*id_work.pub" "$1"' _ "$T/k1.out"
check "ssh key: an rsa key is not offered" hasnt "$T/k1.out" id_rsa.pub
check "ssh key: the chosen key goes to the card and is remembered in the state" bash -c 'grep -q "sd prepare --mode first-boot --device /dev/sdb --yes-i-confirmed /dev/sdb --pubkey '"$KH"'/.ssh/id_other.pub\|sd prepare --mode first-boot --device /dev/sdb --yes-i-confirmed /dev/sdb --pubkey '"$KH"'/.ssh/id_work.pub" "$1" && grep -q "^ssh_pubkey='"$KH"'/.ssh/id_" "$2"' _ "$STUB_LOG" "$T/st/state"
REMEMBERED=$(state_get ssh_pubkey)
: >"$STUB_LOG"
HOME=$KH AKAM_WIFI_PASS=$SENTINEL at 'y' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k2.out" 2>&1
check "ssh key: the choice is remembered (not asked again)" bash -c '! grep -q "Which SSH key" "$1" && grep -q "chosen before" "$1" && grep -q "pubkey $2" "$3"' _ "$T/k2.out" "$REMEMBERED" "$STUB_LOG"
state_unset_key ssh_pubkey
PATH=$KGBIN:$PATH HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'1\nn' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k3.out" 2>&1
check "ssh key: a key that does not exist is only created after a yes (here no: stops)" bash -c '[ "$1" -ne 0 ] && grep -q "no SSH key" "$2"' _ $? "$T/k3.out"
check "ssh key: ... and nothing was created" bash -c '[ ! -s "$1" ] && [ ! -e "$2/.ssh/akam" ]' _ "$T/keygen.log" "$KH"
HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'9\n3\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k4.out" 2>&1
check "ssh key: a number out of range is asked again" has "$T/k4.out" "Please type a number between 1 and 4"
state_unset_key ssh_pubkey
# default (Enter) = the shared key, created when missing; the question names the comment akam
PATH=$KGBIN:$PATH HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'\ny\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k6.out" 2>&1
check "ssh key: Enter picks the shared key and creates ~/.ssh/akam (comment akam)" bash -c 'grep -q "ssh-keygen -t ed25519 -f $2/.ssh/akam -C akam" "$1" && [ "$(grep -m1 ^ssh_pubkey= "$3" | cut -d= -f2)" = "$2/.ssh/akam.pub" ]' _ "$T/keygen.log" "$KH" "$T/st/state"
check "ssh key: the shared key went to the card" has "$STUB_LOG" "--pubkey $KH/.ssh/akam.pub"
# a key for this camera only: ~/.ssh/akam-<camera>, comment akam-<camera>
state_unset_key ssh_pubkey
rm -f "$KH/.ssh/akam" "$KH/.ssh/akam.pub"
: >"$T/keygen.log"
PATH=$KGBIN:$PATH HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'2\ny\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k7.out" 2>&1
check "ssh key: choice 2 creates ~/.ssh/akam-default (comment akam-default), not the shared key" bash -c 'grep -q "ssh-keygen -t ed25519 -f $2/.ssh/akam-default -C akam-default" "$1" && [ ! -e "$2/.ssh/akam.pub" ] && [ "$(grep -m1 ^ssh_pubkey= "$3" | cut -d= -f2)" = "$2/.ssh/akam-default.pub" ]' _ "$T/keygen.log" "$KH" "$T/st/state"
# an existing key of the shared name is used, never recreated or overwritten
state_unset_key ssh_pubkey
: >"$T/keygen.log"
PATH=$KGBIN:$PATH HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'2\ny\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k8.out" 2>&1
check "ssh key: an existing ~/.ssh/akam-default is asked about, then used as it is (no ssh-keygen)" bash -c '[ ! -s "$1" ] && grep -q "A key for the name default already exists: ~/.ssh/akam-default (created " "$2" && grep -q "Using ~/.ssh/akam-default.pub" "$2"' _ "$T/keygen.log" "$T/k8.out"
# answering No offers another name, which is made (and the old file is left alone)
state_unset_key ssh_pubkey
: >"$T/keygen.log"
PATH=$KGBIN:$PATH HOME=$KH AKAM_WIFI_PASS=$SENTINEL at $'2\nn\nsalon9\ny\ny' prepare-flash-sd --device /dev/sdb --ssid N >"$T/k9.out" 2>&1
check "ssh key: refusing the existing key offers another name (akam-salon9 is made)" bash -c 'grep -q "ssh-keygen -t ed25519 -f $2/.ssh/akam-salon9 -C akam-salon9" "$1" && [ "$(grep -m1 ^ssh_pubkey= "$3" | cut -d= -f2)" = "$2/.ssh/akam-salon9.pub" ]' _ "$T/keygen.log" "$KH" "$T/st/state"
rm -f "$KH/.ssh/akam-salon9" "$KH/.ssh/akam-salon9.pub"
# --pubkey: a file that does not end in .pub is refused, a relative path becomes absolute
state_unset_key ssh_pubkey
cp "$PUB" "$KH/nosuffix"
AKAM_WIFI_PASS=$SENTINEL at 'y' prepare-flash-sd --device /dev/sdb --pubkey "$KH/nosuffix" --ssid N >"$T/k10.out" 2>&1
check "ssh key: --pubkey must end in .pub" bash -c '[ "$1" -ne 0 ] || exit 1; grep -q "does not end in .pub" "$2"' _ $? "$T/k10.out"
(cd "$KH/.ssh" && AKAM_WIFI_PASS=$SENTINEL at 'y' prepare-flash-sd --device /dev/sdb --pubkey id_work.pub --ssid N >"$T/k11.out" 2>&1)
check "ssh key: a relative --pubkey is saved as an absolute path" bash -c '[ "$(grep -m1 ^ssh_pubkey= "$1" | cut -d= -f2)" = "$2/.ssh/id_work.pub" ]' _ "$T/st/state" "$KH"
rm -f "$KH/.ssh/akam-default" "$KH/.ssh/akam-default.pub"
state_unset_key ssh_pubkey
HOME=$KH AKAM_WIFI_PASS=$SENTINEL at 'y' prepare-flash-sd --device /dev/sdb --pubkey "$PUB" --ssid N >"$T/k5.out" 2>&1
check "ssh key: --pubkey wins over everything and needs no question" bash -c '! grep -q "Which SSH key" "$1"' _ "$T/k5.out"
echo changed >>"$T/img6/ak_rtsp_firmware_20260101_000000.squashfs"
a status >"$T/f4.out" 2>&1
check "a changed image makes the prepared flash card stale" has "$T/f4.out" "the image file changed after it was built"
check "  ... and the flash card step with it" has "$T/f4.out" "the card was made from an image that is out of date"

# ---------------------------------------------------------------- wipe-sd (cards holding the Wi-Fi password)
: >"$STUB_LOG"
a wipe-sd --device /dev/sdb >"$T/w1.out" 2>&1
check "wipe without a terminal is refused" test $? -ne 0
check "  ... and the SD tool was not asked to wipe" bash -c '! grep -q "^sd wipe" "$1"' _ "$STUB_LOG"
at "n" wipe-sd --device /dev/sdb >"$T/w2.out" 2>&1
check "wipe answered no is refused" test $? -ne 0
at "y" wipe-sd --device /dev/sdb >"$T/w3.out" 2>&1
check "wipe after an explicit yes calls the SD tool with --yes-i-confirmed for that device" has "$STUB_LOG" "sd wipe --device /dev/sdb --yes-i-confirmed /dev/sdb"
check "wiping a recorded card forgets it" test -z "$(state_get flash_sd_device)"
a wipe-sd --device /dev/sda --dry-run >"$T/w4.out" 2>&1
check "wipe rehearsal only shows the command" has "$T/w4.out" "WOULD run:"

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

# ---------------------------------------------------------------- entering a step from the plain menu asks first
new_state
printf 'default\n' >"$T/st/current_camera"
printf '\nn\nq\n' | AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st" >"$T/m1.out" 2>&1
check "menu: a step shows what it does, what it touches and asks 'Start this step? [Y/n]'" bash -c 'grep -q "What it does:" "$1" && grep -q "What it touches:" "$1" && grep -q "Start this step? \[Y/n\]" "$1"' _ "$T/m1.out"
check "menu: answering n returns to the menu and the step is not run" bash -c 'grep -q "Not started" "$1" && ! grep -q "stub check_host" "$1" && ! grep -q "^step_doctor_done=" "$2/state"' _ "$T/m1.out" "$T/st"
printf '\n\nq\n' | AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st" >"$T/m2.out" 2>&1
check "menu: Enter at the question means yes: the step runs" bash -c 'grep -q "^step_doctor_done=" "$1/state"' _ "$T/st"
printf '2\nn\nq\n' | AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st" >"$T/m3.out" 2>&1
check "menu: a step chosen by number asks as well" bash -c 'grep -q "Start this step?" "$1" && ! grep -q "^step_get_kit_done=" "$2/state"' _ "$T/m3.out" "$T/st"
new_state
a step doctor >"$T/m4.out" 2>&1
check "a step started by name (./akam step NAME) is not asked again" bash -c '! grep -q "Start this step?" "$1"' _ "$T/m4.out"

# ---------------------------------------------------------------- several cameras (names, one state each)
HC=$T/home_cam
mkdir -p "$HC"
new_state
printf 'step_doctor_done=2026-01-01T00:00:00Z\ncamera_ip=192.0.2.10\n' >"$T/st/state"   # the state of earlier versions = camera "default"
HOME=$HC a --camera salon doctor >"$T/c1.out" 2>&1
check "camera: --camera NAME keeps its own state under cameras/NAME" bash -c '[ -f "$1/cameras/salon/state" ] && grep -q "^step_doctor_done=" "$1/cameras/salon/state" && [ -f "$1/cameras/salon/akam.log" ]' _ "$T/st"
check "camera: the old state is untouched (it is the camera called default)" bash -c 'grep -q "^camera_ip=192.0.2.10$" "$1/state" && [ "$(grep -c . "$1/state")" -ge 2 ]' _ "$T/st"
HOME=$HC a --camera salon status >"$T/c2.out" 2>&1
check "camera: status shows the camera name" has "$T/c2.out" "camera: salon"
HOME=$HC a status >"$T/c3.out" 2>&1
check "camera: the camera used last is remembered" bash -c 'grep -q "camera: salon" "$1" && [ "$(cat "$2/current_camera")" = salon ]' _ "$T/c3.out" "$T/st"
HOME=$HC a --camera default status >"$T/c4.out" 2>&1
check "camera: --camera default selects the legacy state" has "$T/c4.out" "camera: default"
HOME=$HC a cameras >"$T/c5.out" 2>&1
check "camera: cameras lists default and salon with their progress and marks the current one" bash -c 'grep -q "^  \* default .*1/9 steps done  192.0.2.10" "$1" && grep -q "^    salon .*1/9 steps done" "$1"' _ "$T/c5.out"
for bad in Salon 'my room' -x '' abcdefghijklmnopqrstu 'a_b' 'trail-'; do
    HOME=$HC "$AKAM" --state-dir "$T/st" --camera "$bad" status </dev/null >"$T/c6.out" 2>&1
    check "camera: the name '$bad' is refused (exit 2)" test $? -eq 2
done
HOME=$HC "$AKAM" --state-dir "$T/st" --camera abcdefghijklmnopqrst status </dev/null >"$T/c6.out" 2>&1
check "camera: a name of exactly 20 characters is accepted" test $? -eq 0
HOME=$HC a --camera salon get-kit --kit "$KIT" >"$T/c7.out" 2>&1
check "camera: the kit is shared (BASE/kit), not copied per camera" bash -c '[ -d "$1/kit" ] && [ ! -d "$1/cameras/salon/kit" ]' _ "$T/st"
HOME=$HC at "y" --camera salon backup --backup-dir "$BK" >/dev/null 2>&1
HOME=$HC AKAM_HW_LIST=$HWV a --camera salon image >"$T/c8.out" 2>&1
check "camera: the image folder carries the camera name" bash -c 'grep -q "^assemble .*--out '"$HC"'/akam-image-salon-[0-9]*-[0-9]*" "$1"' _ "$STUB_LOG"
check "camera: the other camera's progress is not changed" bash -c '! grep -q "^step_image_done" "$1/state"' _ "$T/st"
HOME=$HC "$AKAM" --state-dir "$T/st" --camera salon --dry-run status </dev/null >/dev/null 2>&1
printf 'default\n' >"$T/st/current_camera"
HOME=$HC "$AKAM" --state-dir "$T/st" --camera salon --dry-run status </dev/null >/dev/null 2>&1
check "camera: a rehearsal never changes the remembered camera" test "$(cat "$T/st/current_camera")" = default
# first run of the menu: asks for the name before creating any state
rm -rf "$T/st2"
mkdir -p "$T/st2"
printf 'bad name\nsalon\nq\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st2" >"$T/c9.out" 2>&1
check "camera: the first menu run asks for a name, refuses a bad one and asks again" bash -c 'grep -q "Camera name \[default\]" "$1" && grep -q "is not valid" "$1"' _ "$T/c9.out"
check "camera: ...then works in cameras/<name> and remembers it" bash -c '[ -f "$1/cameras/salon/state" ] && [ "$(cat "$1/current_camera")" = salon ] && [ ! -e "$1/state" ]' _ "$T/st2"
printf 'q\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st2" >"$T/c10.out" 2>&1
check "camera: the second menu run does not ask again" bash -c '! grep -q "Camera name" "$1" && grep -q "camera: salon" "$1"' _ "$T/c10.out"
rm -rf "$T/st3"
mkdir -p "$T/st3"
printf '\nq\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st3" >"$T/c11.out" 2>&1
check "camera: Enter at the name question means default" bash -c 'grep -q "camera: default" "$1" && [ -f "$2/state" ]' _ "$T/c11.out" "$T/st3"
rm -rf "$T/st4"
mkdir -p "$T/st4"
HOME=$HC "$AKAM" --plain --state-dir "$T/st4" </dev/null >"$T/c12.out" 2>&1
check "camera: nothing is asked without a terminal" bash -c '! grep -q "Camera name" "$1"' _ "$T/c12.out"

# migration: the progress of an earlier version is one camera; the first menu run asks what to call it
rm -rf "$T/st5"
mkdir -p "$T/st5/kit"
printf 'step_doctor_done=2026-01-01T00:00:00Z\ncamera_ip=192.0.2.2\n' >"$T/st5/state"
printf 'old log\n' >"$T/st5/akam.log"
printf 'akam-02\nq\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st5" >"$T/g1.out" 2>&1
check "migration: the first menu run asks what to call the existing progress" has "$T/g1.out" "progress saved by an earlier version belongs to one camera"
check "migration: the existing state moves to cameras/akam-02 (address and steps kept)" bash -c 'grep -q "^camera_ip=192.0.2.2$" "$1/cameras/akam-02/state" && grep -q "^step_doctor_done=" "$1/cameras/akam-02/state" && grep -q "old log" "$1/cameras/akam-02/akam.log" && [ ! -e "$1/state" ]' _ "$T/st5"
check "migration: the shared kit folder stays at the base and the name is remembered" bash -c '[ -d "$1/kit" ] && [ "$(cat "$1/current_camera")" = akam-02 ]' _ "$T/st5"
check "migration: the menu now shows the new name" has "$T/g1.out" "camera: akam-02"
check "migration: an untouched backup copy of the old state is kept" bash -c 'grep -q "^camera_ip=192.0.2.2$" "$1/state.before-rename.bak" && grep -q "^step_doctor_done=" "$1/state.before-rename.bak"' _ "$T/st5"
rm -rf "$T/st6"
mkdir -p "$T/st6"
printf 'step_doctor_done=2026-01-01T00:00:00Z\n' >"$T/st6/state"
printf '\nq\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st6" >"$T/g2.out" 2>&1
check "migration: Enter keeps the name default and leaves the files where they are" bash -c 'grep -q "^step_doctor_done=" "$1/state" && [ "$(cat "$1/current_camera")" = default ] && [ ! -d "$1/cameras" ]' _ "$T/st6"
printf 'q\n' | HOME=$HC AKAM_FORCE_TTY=1 "$AKAM" --plain --state-dir "$T/st6" >"$T/g3.out" 2>&1
check "migration: it is asked only once" bash -c '! grep -q "earlier version" "$1"' _ "$T/g3.out"
HOME=$HC "$AKAM" --state-dir "$T/st6" rename-camera akam-01 </dev/null >"$T/g4.out" 2>&1
check "rename-camera: moves the progress to the new name and remembers it" bash -c '[ "$1" -eq 0 ] && grep -q "^step_doctor_done=" "$2/cameras/akam-01/state" && [ ! -e "$2/state" ] && [ "$(cat "$2/current_camera")" = akam-01 ]' _ $? "$T/st6"
HOME=$HC "$AKAM" --state-dir "$T/st6" rename-camera Bad_Name </dev/null >"$T/g5.out" 2>&1
check "rename-camera: a bad name is refused" bash -c '[ "$1" -ne 0 ] && [ -f "$2/cameras/akam-01/state" ]' _ $? "$T/st6"
HOME=$HC "$AKAM" --state-dir "$T/st5" --camera akam-01 doctor </dev/null >/dev/null 2>&1
HOME=$HC "$AKAM" --state-dir "$T/st5" --camera akam-01 rename-camera akam-02 </dev/null >"$T/g6.out" 2>&1
check "rename-camera: refuses a name that is already used" bash -c '[ "$1" -ne 0 ] && grep -q "already exists" "$2"' _ $? "$T/g6.out"

echo
echo "wizard_core_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
