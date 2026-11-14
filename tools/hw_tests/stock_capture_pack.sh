#!/usr/bin/env bash
# Host side: put the stock-capture kit on the mounted toolkit microSD.
#
#   tools/hw_tests/stock_capture_pack.sh CARD_MOUNT_DIR [--hook FILE]
#
# Builds src/ak_rtsp/stock_hook.so with the pinned Zig (run tools/build/bootstrap.sh once),
# or takes a prebuilt one with --hook, and copies it with stock_capture.sh to
# CARD_MOUNT_DIR/stock_capture/, plus a SHA256SUMS file the camera script checks.
# Nothing else on the card is touched. See docs/manual/stock-capture.md.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

CARD=
HOOK=
while [ $# -gt 0 ]; do
    case $1 in
        --hook) HOOK=${2:?--hook needs a file}; shift ;;
        -h | --help) sed -n '2,10p' "$0"; exit 0 ;;
        *) if [ -z "$CARD" ]; then CARD=$1; else echo "unexpected argument: $1" >&2; exit 2; fi ;;
    esac
    shift
done
[ -n "$CARD" ] || { sed -n '2,10p' "$0" >&2; exit 2; }
[ -d "$CARD" ] || { echo "error: $CARD is not a directory (mount the card first)" >&2; exit 1; }
if [ ! -d "$CARD/custom" ] || [ ! -f "$CARD/hack.sh" ]; then
    echo "error: $CARD does not look like a toolkit card (no hack.sh and custom/); prepare it with ./akam prepare-backup-sd" >&2
    exit 1
fi
if find "$CARD" -maxdepth 1 -iname '*.bin' | grep -q .; then
    echo "error: a .bin file sits at the top of the card; the stock firmware would try to flash it. Move it into a folder first." >&2
    exit 1
fi

if [ -z "$HOOK" ]; then
    # shellcheck source=tools/build/env.sh
    . "$REPO/tools/build/env.sh" || { echo "error: run tools/build/bootstrap.sh first (pinned Zig missing)" >&2; exit 1; }
    make -C "$REPO/src/ak_rtsp" stock_hook.so >&2
    HOOK=$REPO/src/ak_rtsp/stock_hook.so
fi
[ -f "$HOOK" ] || { echo "error: hook not found: $HOOK" >&2; exit 1; }

DEST=$CARD/stock_capture
mkdir -p "$DEST"
cp "$HOOK" "$DEST/stock_hook.so"
cp "$HERE/stock_capture.sh" "$DEST/stock_capture.sh"
( cd "$DEST" && sha256sum stock_hook.so stock_capture.sh >SHA256SUMS )
sync
echo "Kit written to $DEST:"
ls -l "$DEST"
