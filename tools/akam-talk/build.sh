#!/usr/bin/env bash
# Build akam-talk (the PC-side client of the audio backchannel, ./akam talk and ./akam say).
#
#   tools/akam-talk/build.sh                 native build with the host C compiler -> OUT/akam-talk
#   tools/akam-talk/build.sh --cross         every supported OS/CPU with the pinned Zig -> OUT/<target>/
#   tools/akam-talk/build.sh --out DIR       output directory (default: dist/akam-talk)
#
# The sources are tools/akam-talk/akam_talk.c plus src/ak_rtsp/md5.c and g711.c (the camera's own
# code). The cross build needs the pinned toolchain (tools/build/bootstrap.sh, then ". tools/build/env.sh");
# targets: linux x86_64 and aarch64 (static musl), macOS x86_64 and arm64, Windows x86_64.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/dist/akam-talk"
CROSS=0
while [ $# -gt 0 ]; do
    case $1 in
        --cross) CROSS=1 ;;
        --out) [ $# -ge 2 ] || { echo "build.sh: --out needs a directory" >&2; exit 2; }; OUT=$2; shift ;;
        -h | --help) sed -n '2,/^set -euo/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "build.sh: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

SRCS=("$HERE/akam_talk.c" "$ROOT/src/ak_rtsp/md5.c" "$ROOT/src/ak_rtsp/g711.c")
CFLAGS=(-std=gnu99 -O2 -Wall -Wextra -I"$ROOT/src/ak_rtsp")
mkdir -p "$OUT"

if [ "$CROSS" = 0 ]; then
    "${CC:-cc}" "${CFLAGS[@]}" -o "$OUT/akam-talk" "${SRCS[@]}"
    echo "built $OUT/akam-talk"
    exit 0
fi

# shellcheck source=tools/build/env.sh
. "$ROOT/tools/build/env.sh" || { echo "build.sh: pinned Zig missing: run tools/build/bootstrap.sh" >&2; exit 1; }
for t in x86_64-linux-musl aarch64-linux-musl x86_64-macos aarch64-macos x86_64-windows-gnu; do
    exe=akam-talk
    extra=()
    case $t in
        *windows*) exe=akam-talk.exe; extra=(-lws2_32) ;;
        *linux*) extra=(-static -s) ;;
    esac
    mkdir -p "$OUT/$t"
    "$ZIG" cc -target "$t" "${CFLAGS[@]}" -o "$OUT/$t/$exe" "${SRCS[@]}" "${extra[@]}"
    find "$OUT/$t" -name '*.pdb' -delete
    echo "built $OUT/$t/$exe"
done
