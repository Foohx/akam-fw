#!/usr/bin/env bash
# 2K golden check (opt-in, not part of local_check by default): builds the Rotatable 2K own app from clean with the
# pinned uClibc toolchain (make poc3-dist) and compares, with tests/golden/rot2k_app.golden:
#   - the stripped sha256 of akam2k_rtsp and of the three helper binaries (sntp, mic, led);
#   - the sha256 of `objdump -d -j .text` of akam2k_rtsp (addresses included: proves identical code layout).
# Used to prove that a pure file move or rename does not change the 2K binary (reorganisation steps 1 to 6).
#
#   tools/ci/repro_check_rot2k.sh              build once, compare with the golden (exit 1 on any difference)
#   tools/ci/repro_check_rot2k.sh --twice      build twice from clean and require both builds identical (stability)
#   tools/ci/repro_check_rot2k.sh --update     write the golden from a (twice-checked) build
# Needs: tools/.toolchain (platforms/anyka-ak3918av130/toolchain/fetch_toolchain.sh), dist/rot2k-app/vendor (platforms/anyka-ak3918av130/toolchain/prepare_vendor.sh), third_party/smolrtsp, and
# CAMERA_LIBC=<libuClibc-1.0.31.so from your own camera backup> (default: .local/rootfs/lib/libuClibc-1.0.31.so).
set -euo pipefail
shopt -s inherit_errexit
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
APP=$REPO/platforms/anyka-ak3918av130
OUT=$REPO/build/lsc-rotatable-2k-ak3918av130   # per-model build directory; poc3-dist also publishes a copy to dist/rot2k-app
GOLDEN=$REPO/tests/golden/rot2k_app.golden
TC=$REPO/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1
SMOLRTSP=${SMOLRTSP:-$REPO/third_party/smolrtsp}
CAMERA_LIBC=${CAMERA_LIBC:-$REPO/.local/rootfs/lib/libuClibc-1.0.31.so}
mode=once
case "${1:-}" in --twice) mode=twice ;; --update) mode=update ;; "") ;; *) echo "usage: $0 [--twice|--update]" >&2; exit 2 ;; esac
[ -f "$CAMERA_LIBC" ] || { echo "repro_check_rot2k: set CAMERA_LIBC=<libuClibc-1.0.31.so> (not found: $CAMERA_LIBC)" >&2; exit 2; }

build_and_hash() {
    rm -rf "$OUT"
    make -C "$APP" poc3-dist SMOLRTSP="$SMOLRTSP" CAMERA_LIBC="$CAMERA_LIBC" >&2
    for b in rtsp sntp mic led; do echo "$b=$(sha256sum "$OUT/akam2k_$b" | cut -d' ' -f1)"; done
    # Drop the "file format" header line (it names the file); everything else, addresses included, is the layout.
    echo "rtsp_text=$("$TC/bin/arm-linux-objdump" -d -j .text "$OUT/akam2k_rtsp" | grep -v 'file format' | sha256sum | cut -d' ' -f1)"
}

a=$(build_and_hash)
if [ "$mode" = twice ] || [ "$mode" = update ]; then
    b=$(build_and_hash)
    [ "$a" = "$b" ] || { echo "repro_check_rot2k: TWO CLEAN BUILDS DIFFER" >&2; diff <(echo "$a") <(echo "$b") >&2 || true; exit 1; }
    echo "repro_check_rot2k: two clean builds identical" >&2
fi
if [ "$mode" = update ]; then
    {
        echo "# Golden of the Rotatable 2K own app (tools/ci/repro_check_rot2k.sh): stripped sha256 of the four binaries and sha256 of"
        echo "# the .text disassembly of akam2k_rtsp, built from clean with the pinned toolchain (uClibc armv5 2018.11). Changes only"
        echo "# with an intentional 2K code or build change; regenerate with: tools/ci/repro_check_rot2k.sh --update"
        echo "$a"
    } >"$GOLDEN"
    echo "repro_check_rot2k: golden written to $GOLDEN"
    exit 0
fi
want=$(grep -v '^#' "$GOLDEN" | grep .)
echo "golden:"; echo "$want"; echo "built:"; echo "$a"
[ "$a" = "$want" ] || { echo "repro_check_rot2k: DIFFERS" >&2; diff <(echo "$want") <(echo "$a") >&2 || true; exit 1; }
echo "repro_check_rot2k: identical"
