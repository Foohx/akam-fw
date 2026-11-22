#!/usr/bin/env bash
# Test of tools/ci/docs_links_check.sh: it passes on the tree, and fails on a broken relative link and on a backticked repo path that does not
# exist. The violations are made in a temporary git repository; nothing in the repository is modified.
# Group "lint".
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd -P)
CHECK=$REPO/tools/ci/docs_links_check.sh
WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

if bash "$CHECK" >"$WORK/out.txt" 2>&1; then ok "the check passes on the tree"; else ko "the check passes on the tree"; cat "$WORK/out.txt"; fi

R=$WORK/r
mkdir -p "$R/tools/ci" "$R/docs" "$R/src"
cp "$CHECK" "$R/tools/ci/"
touch "$R/src/a.c"
git -C "$R" init -q .
try() { # try <markdown body>: run the copy of the check on a docs/x.md with that body
    printf '# T\n\n%s\n' "$1" >"$R/docs/x.md"
    git -C "$R" add -A
    bash "$R/tools/ci/docs_links_check.sh" >"$WORK/out.txt" 2>&1
}

if try 'See [a](missing.md).'; then ko "a broken link fails"
elif grep -q 'broken link (missing.md)' "$WORK/out.txt"; then ok "a broken link fails"
else ko "a broken link fails"; fi
# shellcheck disable=SC2016  # backticks are literal Markdown here
if try 'See `src/gone.c`.'; then ko "a missing path fails"
elif grep -q 'path does not exist' "$WORK/out.txt"; then ok "a missing path fails"
else ko "a missing path fails"; fi
# shellcheck disable=SC2016  # backticks are literal Markdown here
if try 'See `src/a.c` and [t](x.md#t).'; then ok "a good path and anchor pass"; else ko "a good path and anchor pass"; cat "$WORK/out.txt"; fi

echo "docs_links_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
