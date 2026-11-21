#!/usr/bin/env bash
# Test of tools/ci/layering_lint.sh and of the Makefile guard against platform/model basename clashes: the lint passes on the tree and each kind of
# violation (a shared file including a platform header, a duplicated header basename on a build's include path, a model C file named like a platform
# C file) is caught. Nothing in the repository is modified: violations are made in copies or in a temporary model directory.
# Group "lint".
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd -P)
LINT=$REPO/tools/ci/layering_lint.sh
WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else ko "$d"; fi; }
fails() { ! "$@" >"$WORK/out.txt" 2>&1; }
outhas() { grep -q -- "$1" "$WORK/out.txt"; }

check "the lint passes on the tree" bash "$LINT"

# (a) a shared file that includes a platform header
cp -r "$REPO/src" "$WORK/src"
printf '#include "isp.h"\nint zz_layering(void) { return 0; }\n' >"$WORK/src/core/zz_layering.c"
check "(a) a src/ file including platforms/anyka-ak3918 isp.h is rejected" fails env LINT_SRC_DIR="$WORK/src" bash "$LINT"
check "(a) the message names the file" outhas "zz_layering.c"
rm "$WORK/src/core/zz_layering.c"
printf '#include "ptz.h"\nint zz_layering2(void) { return 0; }\n' >"$WORK/src/features/zz_layering2.c"
check "(a) a src/ file including a 2K model header is rejected" fails env LINT_SRC_DIR="$WORK/src" bash "$LINT"
rm "$WORK/src/features/zz_layering2.c"
check "(a) the copy without violations passes (the check is not tripping on the copy)" env LINT_SRC_DIR="$WORK/src" bash "$LINT"

# (b)/(c) a temporary model on the 2K platform whose files clash with the platform: made under firmware/ is not allowed (tracked tree), so the model
# directory is outside it and selected with MODEL=<relative path>; print-incdirs evaluates the guard without building anything.
PLAT=$REPO/platforms/anyka-ak3918av130
mkmodel() { # mkmodel <name>: a copy of the 2K model.mk and one file
    mkdir -p "$WORK/fw/$1"
    cp "$REPO/firmware/lsc-rotatable-2k-ak3918av130/model.mk" "$WORK/fw/$1/model.mk"
}
rel() { realpath --relative-to="$REPO/firmware" "$1"; }
mkmodel clash
: >"$WORK/fw/clash/ircut.c" # a platform source basename
check "(c) a model C file named like a platform C file stops the 2K make with an error" fails make -s -C "$PLAT" MODEL="$(rel "$WORK/fw/clash")" print-incdirs
check "(c) the error says basename clash and names the file" outhas "basename clash.*ircut.c"
mkmodel ok
check "(c) a model without a clash is accepted by the guard" make -s -C "$PLAT" MODEL="$(rel "$WORK/fw/ok")" print-sources >/dev/null
P1=$REPO/platforms/anyka-ak3918
mkdir -p "$WORK/fw/clash1080"
cp "$REPO/firmware/lsc-indoor-1080p-ak3918/model.mk" "$WORK/fw/clash1080/"
: >"$WORK/fw/clash1080/ircut.c"
check "(c) the 1080p make has the same guard" fails make -s -C "$P1" MODEL="$(rel "$WORK/fw/clash1080")" print-incdirs
check "(c) it names the file" outhas "basename clash.*ircut.c"

# (b) duplicated basenames on the include path of a build
mkmodel hdr
: >"$WORK/fw/hdr/md5.h" # a shared header basename (src/net/crypto/md5.h)
check "(b) a model header named like a shared header is rejected" fails env LINT_BUILDS="platforms/anyka-ak3918av130:$(rel "$WORK/fw/hdr")" bash "$LINT"
check "(b) the message names the header" outhas "md5.h"
mkmodel hdr2
: >"$WORK/fw/hdr2/isp.h" # a platform header basename (platforms/anyka-ak3918av130/isp.h)
check "(b) a model header named like a platform header is rejected" fails env LINT_BUILDS="platforms/anyka-ak3918av130:$(rel "$WORK/fw/hdr2")" bash "$LINT"

echo "layering_lint_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
