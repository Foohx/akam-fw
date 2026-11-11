#!/bin/bash
# Copies the camera-side scripts the assistant uploads from their source paths into the folder that
# is embedded in the binary (go generate ./internal/assets). A test fails when a copy drifts.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
dst=$here/scripts
mkdir -p "$dst"
for f in $(sed -e 's/#.*//' "$here/sources.txt"); do
    cp "$repo/$f" "$dst/$(basename "$f")"
done
