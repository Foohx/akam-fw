#!/usr/bin/env bash
# Host test of the image orientation (flip / mirror) of the Rotatable 2K: the mappings, the zones frame, the tracker and
# directions, the scene reference conversion (tests/rot2k_orient_unit.c, under the sanitizers when the compiler has them), plus
# static checks that every consumer of the picture is wired to it. No hardware.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "skip: no host C compiler ($CC)"; exit 0; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rot2k_orient.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
FAILS=0
check() { if "${@:2}"; then echo "ok   - $1"; else echo "FAIL - $1"; FAILS=$((FAILS + 1)); fi; }

SAN=""
if echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=address,undefined -o "$WORK/probe" - 2>/dev/null; then SAN="-fsanitize=address,undefined"; fi
# shellcheck disable=SC2086
if "$CC" -Wall -Wextra -Werror -std=gnu99 -g $SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/orient_unit" "$HERE/rot2k_orient_unit.c" \
    "$PLATFORM_2K/orient.c" "$PLATFORM_2K/zlive.c" "$FW_2K/zones.c" "$FW_2K/detstate.c" "$FW_2K/store.c" "$SRC_FEATURES/track/track_logic.c" -lm; then
    "$WORK/orient_unit" "$WORK" >"$WORK/orient_unit.out" 2>&1 || true
    check "orientation unit test passes (state, direction signs, zone frame, live zone text, scene plane, tracker direction)" grep -q 'rot2k orient unit: 0 failed' "$WORK/orient_unit.out"
    grep 'FAIL' "$WORK/orient_unit.out" || true
else
    echo "FAIL - orientation unit test does not compile"
    FAILS=$((FAILS + 1))
fi

# The vendor call lives in cap.c only, with the arguments the lib validates (0 / 1 each, device 0).
check "ak_vpss_switch_flip_mirror is called in cap.c only" test "$(grep -l 'ak_vpss_switch_flip_mirror(' "$PLATFORM_2K"/*.c "$FW_2K"/*.c | sed 's|.*/||' | tr '\n' ' ')" = "cap.c "
check "the call is (0, flip, mirror) after a 0 / 1 check" grep -q 'rc = ak_vpss_switch_flip_mirror(0, flip, mirror);' "$PLATFORM_2K/cap.c"
check "the ISP mode switch replays the orientation" grep -q 'a2k_isp_flip_mirror(f, m)' "$PLATFORM_2K/cap.c"
# Consumers: each reads the orientation, none keeps a private copy of the frame rules.
check "the scene feed converts the live frame to the sensor frame and skips the settling window" bash -c "grep -q 'a2k_orient_plane' '$FW_2K/scene.c' && grep -q 'a2k_orient_settling' '$FW_2K/scene.c'"
check "the detection converts the zones to the displayed frame and refreshes them when the orientation changes" bash -c "grep -q 'a2k_zones_orient(&g_eff' '$PLATFORM_2K/detect.c' && grep -q 'a2k_orient_gen' '$PLATFORM_2K/detect.c'"
check "the tracker follows the orientation (listener, base flags kept)" bash -c "grep -q 'a2k_orient_listen(orient_changed)' '$FW_2K/track_run.c' && grep -q 'a2k_orient_invert_pan' '$FW_2K/track_run.c'"
check "the manual PTZ directions (buttons, ONVIF / ptz.vel / MQTT velocity) are mapped in ptz.c" test "$(grep -c 'a2k_orient_pan_sign\|a2k_orient_tilt_sign' "$FW_2K/ptz.c")" -ge 4
check "the zone text entities are converted in zlive.c (set and get)" test "$(grep -c 'a2k_zones_orient' "$PLATFORM_2K/zlive.c")" -ge 3
check "the settings rows image.flip / image.mirror are saved, image.ceiling is not" bash -c "grep 'row(\"image.flip\"' '$FW_2K/settings.c' | grep -q '\"0\", 1, RW' && grep 'row(\"image.mirror\"' '$FW_2K/settings.c' | grep -q '\"0\", 1, RW' && grep 'row(\"image.ceiling\"' '$FW_2K/settings.c' | grep -q '\"0\", 0, RW'"
check "the Home Assistant switches exist (image_flip, image_mirror, ceiling_mount)" bash -c "grep -q '\"image_flip\"' '$PLATFORM_2K/mqtt_cfg.c' && grep -q '\"image_mirror\"' '$PLATFORM_2K/mqtt_cfg.c' && grep -q '\"ceiling_mount\"' '$PLATFORM_2K/mqtt_cfg.c'"
check "patrol presets stay in motor counters (no orientation in patrol.c / ptz_patrol.c)" bash -c "! grep -q 'orient' '$FW_2K/patrol.c' && ! grep -q 'orient' '$SRC_FEATURES/ptz/ptz_patrol.c'"

if [ "$FAILS" -ne 0 ]; then
    echo "rot2k orient: $FAILS check(s) failed"
    exit 1
fi
echo "rot2k orient: all checks passed"
