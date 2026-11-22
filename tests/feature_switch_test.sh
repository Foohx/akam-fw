#!/usr/bin/env bash
# Feature switches (src/build/features.mk, firmware/<model>/model.mk) must gate something real.
#   1080p: AKAM_FEAT_ONVIF=0 drops every ONVIF object (the object list has none, onvif_off.c stands in) and the program still LINKS, without the
#          ONVIF code (no service string in the binary); the default (=1) object list has them all. The link needs the pinned Zig (skipped without).
#   2K:    PTZ / TRACK / SCENE / PATROL = 0 drop their objects (and the shared modules only they use) from the link list and link <feature>_off.c
#          instead (firmware/<model>/*_off.c: "not available" answers), so each variant LINKS. The ARM link of every variant (one at a time and all
#          four off) runs when the pinned uClibc toolchain, the vendor libraries and SmolRTSP are present. An unknown feature (WIFI5G, which gated
#          nothing, was removed) is a make error.
# Group "native" (uses the pinned Zig when present).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd -P)
P1=$REPO/platforms/anyka-ak3918
P2=$REPO/platforms/anyka-ak3918av130
WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else ko "$d"; fi; }
fails() { ! "$@" >"$WORK/out.txt" 2>&1; }
quiet() { "$@" >"$WORK/make.log" 2>&1; }
outhas() { grep -q -- "$1" "$WORK/out.txt"; }

# ---- 1080p: the object list
objs() { make -n -C "$P1" BUILD="$WORK/b$1" "${@:2}" ak_rtsp 2>&1 | grep -oE '[^ ]+\.o( |$)' | sort -u; }
objs on >"$WORK/on.txt"
objs off AKAM_FEAT_ONVIF=0 >"$WORK/off.txt"
check "1080p default: the ONVIF objects are in the link" bash -c "grep -q 'onvif_device.o' '$WORK/on.txt' && grep -q 'onvif_media.o' '$WORK/on.txt' && grep -q 'onvif_imaging.o' '$WORK/on.txt' && grep -q 'onvif_cfg.o' '$WORK/on.txt' && grep -q 'onvif_model.o' '$WORK/on.txt' && grep -q 'src/net/onvif/soap.o' '$WORK/on.txt' && ! grep -q 'onvif_off.o' '$WORK/on.txt'"
check "1080p ONVIF=0: none of the ONVIF objects is in the link" bash -c "! grep -qE 'onvif_(device|media|imaging|cfg|model)\.o|src/net/onvif/(soap|onvif|http_srv|wsd)\.o' '$WORK/off.txt'"
check "1080p ONVIF=0: onvif_off.o stands in" grep -q 'onvif_off.o' "$WORK/off.txt"

# shellcheck disable=SC2012
ZIG=$(ls -d "$REPO"/tools/.toolchain/zig-*-linux*/zig 2>/dev/null | head -1 || true)
if [ -z "$ZIG" ]; then ZIG=$(command -v zig || true); fi
if [ -n "$ZIG" ] && [ -d "$REPO/third_party/smolrtsp/build/_deps" ]; then
    PATH="$(dirname "$ZIG"):$PATH"
    export PATH
    check "1080p ONVIF=0 links" make -s -C "$P1" BUILD="$WORK/boff" AKAM_FEAT_ONVIF=0 ak_rtsp
    check "1080p ONVIF=0: the binary has no ONVIF service code" bash -c "! strings '$WORK/boff/ak_rtsp' | grep -q 'GetStreamUri'"
    check "1080p ONVIF=0: onvif.state still answers off" bash -c "strings -n 3 '$WORK/boff/ak_rtsp' | grep -qx 'off'"
else
    echo "  skip - no pinned Zig or SmolRTSP checkout: the ONVIF=0 link was not run"
fi

# ---- 2K
all=$(make -s -C "$P2" print-sources | sed "s|^$REPO/||")
none=$(make -s -C "$P2" AKAM_FEAT_PTZ=0 AKAM_FEAT_TRACK=0 AKAM_FEAT_SCENE=0 AKAM_FEAT_PATROL=0 print-sources | sed "s|^$REPO/||")
declare -A OBJS=(
    [PTZ]="ptz.c hal_ptz.c hal_ptz_drv.c ptz_desc.c onvif_ptz.c ptz_vel.c ptz_lease.c drift_match.c"
    [TRACK]="track.c track_run.c track_logic.c track_relay.c"
    [SCENE]="scene.c scene_check.c"
    [PATROL]="patrol.c patrol_run.c ptz_patrol.c")
declare -A OFFC=([PTZ]=ptz_off.c [TRACK]=track_off.c [SCENE]=scene_off.c [PATROL]=patrol_off.c)
for f in PTZ TRACK SCENE PATROL; do
    one=$(make -s -C "$P2" "AKAM_FEAT_$f=0" print-sources | sed "s|^$REPO/||")
    check "2K $f=0 is accepted (no refusal, no AKAM_ALLOW_UNLINKABLE escape)" test -n "$one"
    for o in ${OBJS[$f]}; do
        check "2K default: $o is in the link" bash -c "echo '$all' | grep -q '/$o\$'"
        check "2K $f=0: $o is dropped" bash -c "! echo '$one' | grep -q '/$o\$'"
        check "2K all four off: $o is dropped" bash -c "! echo '$none' | grep -q '/$o\$'"
    done
    check "2K $f=0: ${OFFC[$f]} stands in" bash -c "echo '$one' | grep -q '/${OFFC[$f]}\$' && ! echo '$all' | grep -q '/${OFFC[$f]}\$'"
done
check "2K all four off: the rest of the program is still listed" bash -c "echo '$none' | grep -q '/main.c\$' && echo '$none' | grep -q '/settings.c\$'"
check "the AKAM_ALLOW_UNLINKABLE escape is gone from the 2K model" bash -c "! grep -rq AKAM_ALLOW_UNLINKABLE '$REPO/firmware' '$REPO/platforms' '$REPO/src'"

# The stand-ins behave (host build, no ARM toolchain): the parsers of an absent feature accept and ignore their akam2k.conf line (a refusal makes main.c exit with usage()),
# and pp_name_ok is the real rule (mode.apply validates a button mode name with it) - compared with the real ptz_patrol.c on a list of names.
if command -v "${HOST_CC:-cc}" >/dev/null 2>&1; then
    # shellcheck source=tests/lib/paths.sh
    . "$REPO/tests/lib/paths.sh"
    OFFDIR=$REPO/firmware/lsc-rotatable-2k-ak3918av130
    check "2K stand-ins: the host unit test builds (all four *_off.c together)" quiet "${HOST_CC:-cc}" -Wall -Wextra -Werror -std=gnu99 -DOFF -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/off_unit" \
        "$REPO/tests/rot2k_off_unit.c" "$OFFDIR/ptz_off.c" "$OFFDIR/scene_off.c" "$OFFDIR/track_off.c" "$OFFDIR/patrol_off.c"
    check "2K stand-ins: parsers accept and ignore their options, booleans answer false, pp_name_ok is the real rule" bash -c "'$WORK/off_unit' | grep -q 'rot2k off unit: 0 failed'"
    quiet "${HOST_CC:-cc}" -Wall -Wextra -Werror -std=gnu99 "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/real_unit" "$REPO/tests/rot2k_off_unit.c" "$REPO/src/features/ptz/ptz_patrol.c" || true
    check "2K patrol_off.c: pp_name_ok answers like the real ptz_patrol.c on every name of the list" bash -c "diff <('$WORK/off_unit' | grep '^name') <('$WORK/real_unit' | grep '^name')"
fi

# every variant links for ARM (needs the pinned uClibc toolchain, the vendor libraries of YOUR backup and the SmolRTSP checkout)
TCBIN=$REPO/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1/bin/arm-linux-gcc
VENDOR_LIB=$(make -s -C "$P2" -p -n 2>/dev/null | sed -n 's/^VENDOR := //p' | head -1)
if [ -x "$TCBIN" ] && [ -n "$VENDOR_LIB" ] && [ -d "$VENDOR_LIB" ] && [ -d "$REPO/third_party/smolrtsp/build/_deps" ]; then
    for v in "PTZ" "TRACK" "SCENE" "PATROL" "PTZ TRACK SCENE PATROL"; do
        args=""
        for f in $v; do args="$args AKAM_FEAT_$f=0"; done
        # shellcheck disable=SC2086
        check "2K links with: ${v// /=0 }=0" quiet make -s -C "$P2" OUT="$WORK/b2k_${v// /_}" $args poc3
    done
else
    echo "  skip - no uClibc toolchain, vendor libraries or SmolRTSP checkout: the 2K variant links were not run"
fi
check "an unknown feature (WIFI5G) is a make error" fails make -s -C "$P2" AKAM_FEAT_WIFI5G=1 print-sources
check "the unknown-feature error names it" outhas "unknown feature AKAM_FEAT_WIFI5G"

echo "feature_switch_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
