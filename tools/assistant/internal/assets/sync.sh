#!/bin/bash
# Copies the camera-side scripts the assistant uploads from their source paths into the folder that
# is embedded in the binary (go generate ./internal/assets), with an .embed suffix so that they are
# not mistaken for scripts of this repository. A test fails when a copy drifts.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
dst=$here/scripts
rm -rf "$dst"
mkdir -p "$dst"
while read -r f; do
    [ -n "$f" ] || continue
    cp "$repo/$f" "$dst/$(basename "$f").embed"
done < "$here/sources.txt"
