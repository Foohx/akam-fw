#!/usr/bin/env bash
# Regenerates the golden files of the plain-mode tests from the BASH wizard (the reference):
#   tools/assistant/testdata/gen_golden.sh
# One file per scenario in tools/assistant/testdata/golden/: the exit status, stdout and stderr of
# ./akam with --no-color, no terminal and an empty state directory (its path is shown as STATE).
# The Go test (internal/cli/golden_test.go) runs the same scenarios and compares byte for byte.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=$HERE/golden
mkdir -p "$OUT"
# scenarios: name|arguments, one per line, in scenarios.txt (read by golden_test.go too)
while IFS='|' read -r name args; do
    [ -n "$name" ] || continue
    st=$(mktemp -d)
    rc=0
    # shellcheck disable=SC2086  # the scenario words are split on purpose
    bash "$REPO/akam" --no-color --state-dir "$st" $args >"$st.out" 2>"$st.err" </dev/null || rc=$?
    {
        echo "rc=$rc"
        echo "--- stdout ---"
        sed "s#$st#STATE#g" "$st.out"
        echo "--- stderr ---"
        sed "s#$st#STATE#g" "$st.err"
    } >"$OUT/$name.golden"
    rm -rf "$st" "$st.out" "$st.err"
done <"$HERE/scenarios.txt"
echo "golden files written to $OUT"
