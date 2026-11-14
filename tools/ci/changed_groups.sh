#!/usr/bin/env bash
# Which check groups does a set of changed files require? (GitHub CI: job "changes"; see .github/workflows/ci.yml.)
# Usage: tools/ci/changed_groups.sh [--all] [BASE]     changed files = git diff --name-only BASE...HEAD
#        git diff --name-only ... | tools/ci/changed_groups.sh -      changed files from stdin
# Prints "shell=true|false", "native=...", "go=..." (one per line; GITHUB_OUTPUT format).
#   --all, no BASE, or an unknown/unreachable BASE: everything runs (never skip checks on doubt).
# Rules, per changed path:
#   docs only (*.md, docs/, images/, specs/, .github/ISSUE_TEMPLATE/, .github/FUNDING.yml)  -> nothing (lint still runs)
#   src/**, tests/native/**                                  -> native
#   tests/<name>_test.sh                                     -> the group of that test (groups.sh)
#   tools/assistant/**, tools/release/**                     -> go (tools/release/*.sh also shell)
#   tools/lib, tools/build, tools/ci (shared by every group)  -> everything
#   akam, other tools/**/*.sh                                -> shell
#   anything else (deps.lock, tools/lib, workflows, fixtures, Go files elsewhere...) -> everything
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck source=tools/ci/groups.sh
. "$HERE/groups.sh"

shell=false
native=false
go=false
all() { shell=true native=true go=true; }

files=""
if [ "${1:-}" = - ]; then
    files=$(cat)
else
    [ "${1:-}" = --all ] && { all; shift; }
    base=${1:-}
    if [ "$shell$native$go" != truetruetrue ]; then
        if [ -z "$base" ] || [[ $base =~ ^0+$ ]] || ! files=$(git -C "$REPO" diff --name-only "$base...HEAD" 2>/dev/null); then
            all
            files=""
        fi
    fi
fi

while IFS= read -r f; do
    [ -n "$f" ] || continue
    case $f in
        *.md | docs/* | images/* | specs/* | .github/ISSUE_TEMPLATE/* | .github/FUNDING.yml) ;;
        src/* | tests/native/*.c | tests/native/*.h) native=true ;;
        tests/*_test.sh)
            case $(test_group "$REPO/tests" "${f#tests/}") in
                native) native=true ;;
                go) go=true ;;
                shell) shell=true ;;
            esac
            ;;
        tools/lib/* | tools/build/* | tools/ci/*) all ;; # shared by every group
        tools/assistant/*) go=true ;;
        tools/release/*.sh) go=true shell=true ;;
        tools/release/*) go=true ;;
        akam | tools/*.sh) shell=true ;;
        *) all ;;
    esac
done <<<"$files"

echo "shell=$shell"
echo "native=$native"
echo "go=$go"
