#!/usr/bin/env bash
# Regenerates the committed test images of this directory from a synthetic tree built here (no vendor or
# camera data). Needs mksquashfs 4.x, python3 and mkfifo. Usage: bash gen.sh   (run from this directory)
# The images only have to stay valid and stable: the expected listings (*.listing) are written by
# `go test -run TestCommittedImages -update-golden` after the reader was compared with unsquashfs.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/sqfs-gen.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
T=$WORK/tree
mkdir -p "$T/bin" "$T/data" "$T/many" "$T/sticky" "$T/empty_dir"
: >"$T/empty"
printf 'x' >"$T/one"
python3 - "$T" <<'PY'
import sys, hashlib
t = sys.argv[1]
def text(n):
    out = b""
    i = 0
    while len(out) < n:
        out += b"line %d of a synthetic test file\n" % i
        i += 1
    return out[:n]
def noise(n):  # deterministic, hard to compress: a sha256 chain
    out, h = b"", b"seed"
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return out[:n]
open(t + "/bin/tool", "wb").write(text(1000))
open(t + "/data/exact", "wb").write(text(131072))
open(t + "/data/over", "wb").write(text(131073))
open(t + "/data/zeros", "wb").write(bytes(300000))
open(t + "/data/noise", "wb").write(noise(20000))
for i in range(300):
    open(t + "/many/file_with_a_rather_long_name_%04d.txt" % i, "wb").write(b"file %d\n" % i)
PY
chmod 755 "$T/bin/tool"
chmod 1777 "$T/sticky"
ln -s bin/tool "$T/link"
ln -s nowhere "$T/dangling"
ln "$T/bin/tool" "$T/bin/tool_hardlink"
mkfifo "$T/pipe"
COMMON=(-noappend -no-xattrs -quiet -processors 1 -all-time 1700000000 -mkfs-time 1700000000 -force-uid 1000 -force-gid 1000
    -p "bin/tool m 755 1000 100" -p "dev_null c 666 0 0 1 3" -p "dev_sda b 660 0 6 8 0")
mksquashfs "$T" "$HERE/tiny_xz.sqfs" "${COMMON[@]}" -comp xz -b 131072
mksquashfs "$T" "$HERE/tiny_gzip_4k.sqfs" "${COMMON[@]}" -comp gzip -b 4096
mksquashfs "$T" "$HERE/tiny_stored.sqfs" "${COMMON[@]}" -comp gzip -b 4096 -no-compression -no-exports -e data/exact data/over
ls -l "$HERE"/*.sqfs
