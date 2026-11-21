#!/usr/bin/env bash
# Feature switches (src/build/features.mk, firmware/<model>/model.mk) must gate something real.
#   1080p: AKAM_FEAT_ONVIF=0 drops every ONVIF object (the object list has none, onvif_off.c stands in) and the program still LINKS, without the
#          ONVIF code (no service string in the binary); the default (=1) object list has them all. The link needs the pinned Zig (skipped without).
#   2K:    PTZ / TRACK / SCENE / PATROL = 0 drop their objects from the link list, but the model code calls them unconditionally, so the build refuses
#          the switch with a clear error (until the 2K model directory is split); AKAM_ALLOW_UNLINKABLE=1 shows the dropped list. An unknown
#          feature (WIFI5G, which gated nothing, was removed) is a make error.
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
for f in PTZ TRACK SCENE PATROL; do
    check "2K $f=0 is refused with a clear error" fails make -s -C "$P2" "AKAM_FEAT_$f=0" print-sources
    check "2K $f=0: the message names the switch" outhas "AKAM_FEAT_$f := 0 does not link"
done
none=$(make -s -C "$P2" AKAM_ALLOW_UNLINKABLE=1 AKAM_FEAT_PTZ=0 AKAM_FEAT_TRACK=0 AKAM_FEAT_SCENE=0 AKAM_FEAT_PATROL=0 print-sources | sed "s|^$REPO/||")
for o in ptz.c hal_ptz.c hal_ptz_drv.c ptz_desc.c track.c track_run.c scene.c patrol.c patrol_run.c onvif_ptz.c ptz_patrol.c scene_check.c drift_match.c ptz_vel.c ptz_lease.c track_logic.c track_relay.c; do
    check "2K default: $o is in the link" bash -c "echo '$all' | grep -q '/$o\$'"
    check "2K all four off: $o is dropped" bash -c "! echo '$none' | grep -q '/$o\$'"
done
check "2K all four off: the rest of the program is still listed" bash -c "echo '$none' | grep -q '/main.c\$' && echo '$none' | grep -q '/settings.c\$'"
check "an unknown feature (WIFI5G) is a make error" fails make -s -C "$P2" AKAM_FEAT_WIFI5G=1 print-sources
check "the unknown-feature error names it" outhas "unknown feature AKAM_FEAT_WIFI5G"

echo "feature_switch_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
