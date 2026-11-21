#!/usr/bin/env bash
# Host test of platforms/anyka-ak3918/swdog.c (software stall watchdog): pure decision logic on an injected clock.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tests/lib/paths.sh
. "$REPO/tests/lib/paths.sh"
CC=${HOST_CC:-cc}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$CC" -std=gnu99 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
 -I"$REPO/platforms/anyka-ak3918" -I"$REPO/firmware/lsc-indoor-1080p-ak3918" "${SHARED_INC[@]}" \
    -o "$WORK/swdog_test" \
    "$REPO/tests/native/swdog_test.c" "$REPO/platforms/anyka-ak3918/swdog.c"
"$WORK/swdog_test"
