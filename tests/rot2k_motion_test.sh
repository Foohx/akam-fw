#!/usr/bin/env bash
# Host test of the motion detection of the Rotatable 2K (no hardware): the grid logic, frame differencing, the vendor statistics in the four orientations, the masks (head moving, orientation
# change, day / night, restart, size change), and the motion-aware lost-target search (tests/rot2k_motion_unit.c, under the sanitizers when the compiler has them).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "skip: no host C compiler ($CC)"; exit 0; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rot2k_motion.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
FAILS=0
check() { if "${@:2}"; then echo "ok   - $1"; else echo "FAIL - $1"; FAILS=$((FAILS + 1)); fi; }

SAN=""
if echo 'int main(void){return 0;}' | "$CC" -x c -fsanitize=address,undefined -o "$WORK/probe" - 2>/dev/null; then SAN="-fsanitize=address,undefined"; fi
# shellcheck disable=SC2086
if "$CC" -Wall -Wextra -Werror -std=gnu99 -g $SAN -pthread "${INC_2K[@]}" "${SHARED_INC[@]}" -o "$WORK/motion_unit" "$HERE/rot2k_motion_unit.c" \
    "$FW_2K/motion.c" "$SRC_FEATURES/motion/motion_grid.c" "$SRC_FEATURES/track/track_logic.c" "$PLATFORM_2K/orient.c" -lm; then
    "$WORK/motion_unit" >"$WORK/motion_unit.out" 2>&1 || true
    check "motion unit test passes (grid, direction hysteresis, masks, diff source, vendor statistics in four orientations, the search)" grep -q 'rot2k motion unit: 0 failed' "$WORK/motion_unit.out"
    grep 'FAIL' "$WORK/motion_unit.out" || true
else
    echo "FAIL - motion unit test does not compile"
    FAILS=$((FAILS + 1))
fi

# the wiring is present (static checks): the frame callback chain, the rows, the conf keys
check "main.c runs the motion step in the detection frame callback and chains the scene check" grep -q 'a2k_motion_set_chain' "$PLATFORM_2K/main.c"
check "settings rows motion.enabled motion.sensitivity motion.active motion.dir motion.x motion.y motion.source track.search exist" bash -c "for k in motion.enabled motion.sensitivity motion.active motion.dir motion.x motion.y motion.source track.search; do grep -q \"row(\\\"\$k\\\"\" '$FW_2K/settings.c' || exit 1; done"
check "the supervisor passes the motion and search conf keys" bash -c "for k in motion motion_sensitivity motion_source motion_md_frame track_search track_pass_s track_pass_home_s track_search_cooldown_s track_reacq_s; do grep -q \"^            \$k)\" '$FW_2K/boot/card/akam/own_app_run.sh' || exit 1; done"

if [ "$FAILS" -ne 0 ]; then
    echo "motion test: $FAILS failed"
    exit 1
fi
echo "motion test: 0 failed"
