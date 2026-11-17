#!/usr/bin/env bash
# ORACLE of the 1080p LED engine (convergence P6): the engine of src/ak_rtsp/leds.c (light_logic + led_pat rules, default patterns)
# against tests/native/leds_legacy.c, a verbatim copy of the engine it replaced, on scripted and pseudo-random traces, each on a fake
# sysfs directory. Same write sequences, states, network checks, IR hook calls and saved file, or the test fails. Also the rules,
# patterns, the saved pattern lines, the night rule and the transient blip of the new engine (tests/native/leds_rules_test.c).
# No camera. Under ASan + UBSan.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

INC=(-I"$REPO/src/ak_rtsp" -I"$REPO/tests/native" -I"$REPO/src/common/net" -I"$REPO/src/common/logic")
ENG=("$REPO/src/ak_rtsp/leds.c" "$REPO/src/common/logic/led_pat.c" "$REPO/src/common/logic/light_logic.c")

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=write "${INC[@]}" \
    -o "$WORK/leds_oracle_test" "$REPO/tests/native/leds_oracle_test.c" "$REPO/tests/native/leds_legacy.c" "${ENG[@]}"
"$WORK/leds_oracle_test"

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread "${INC[@]}" \
    -o "$WORK/leds_rules_test" "$REPO/tests/native/leds_rules_test.c" "${ENG[@]}"
"$WORK/leds_rules_test"
