#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/audio_out.c (speaker output: device sequence, crash guard,
# amplifier policy, software volume, beep, 8091 keys, playback thread) against a fake
# PCM device and a fake clock. No camera, no sleeping on the amp timers.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/audio_out_test" \
    "$REPO/tests/native/audio_out_test.c" "$REPO/platforms/anyka-ak3918/audio_out.c" "$REPO/src/features/audio/audio_jitter.c"
"$WORK/audio_out_test"
