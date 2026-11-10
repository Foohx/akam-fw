#!/usr/bin/env bash
# Host test of tools/hw_tests/stock_capture_pack.sh on a fake toolkit card folder.
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
PACK=$REPO/tools/hw_tests/stock_capture_pack.sh
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL - $1"; }

CARD=$WORK/card
mkdir -p "$CARD/custom"
: >"$CARD/hack.sh"
printf 'dummy hook\n' >"$WORK/hook.so"

if bash "$PACK" "$CARD" --hook "$WORK/hook.so" >"$WORK/out.txt" 2>&1; then ok "pack succeeds on a toolkit card"; else ko "pack failed"; cat "$WORK/out.txt"; fi
for f in stock_hook.so stock_capture.sh SHA256SUMS; do
    if [ -f "$CARD/stock_capture/$f" ]; then ok "$f copied"; else ko "$f missing"; fi
done
if (cd "$CARD/stock_capture" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then ok "SHA256SUMS verifies"; else ko "SHA256SUMS does not verify"; fi
top=$(find "$CARD" -mindepth 1 -maxdepth 1 | sort | tr '\n' ' ')
if [ "$top" = "$CARD/custom $CARD/hack.sh $CARD/stock_capture " ]; then ok "nothing else on the card was touched"; else ko "card content changed: $top"; fi

mkdir "$WORK/notcard"
if bash "$PACK" "$WORK/notcard" --hook "$WORK/hook.so" >/dev/null 2>&1; then ko "a folder that is not a toolkit card is accepted"; else ok "a folder that is not a toolkit card is refused"; fi
: >"$CARD/firmware.bin"
if bash "$PACK" "$CARD" --hook "$WORK/hook.so" >/dev/null 2>&1; then ko "a top-level .bin is accepted"; else ok "a top-level .bin on the card is refused"; fi

rm -f "$CARD/firmware.bin"; : >"$CARD/UPDATE.BIN"
if bash "$PACK" "$CARD" --hook "$WORK/hook.so" >/dev/null 2>&1; then ko "an upper-case .BIN (FAT) is accepted"; else ok "an upper-case .BIN on the card is refused"; fi

echo
echo "stock_capture_pack_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
