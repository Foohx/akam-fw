#!/usr/bin/env bash
# Which check groups does a set of changed files require? (GitHub CI: job "changes"; see .github/workflows/ci.yml.)
# Usage: tools/ci/changed_groups.sh [--all] [BASE]     changed files = git diff --name-only --no-renames BASE...HEAD
#        git diff --name-only --no-renames ... | tools/ci/changed_groups.sh -      changed files from stdin
# Prints "shell=true|false", "native=...", "go=..." (one per line; GITHUB_OUTPUT format).
#   --all, no BASE, or an unknown/unreachable BASE: everything runs (never skip checks on doubt).
# Rules, per changed path (when unsure, more groups run, never fewer):
#   docs only (README.md, other *.md, docs/, images/, specs/, .github/ISSUE_TEMPLATE/, .github/FUNDING.yml) -> nothing
#       (lint still runs); but THIRD_PARTY_NOTICES.md, LICENSE, CHANGELOG.md and tools/release/*.md are embedded in
#       or checked by release code and tests -> go + shell
#   a file listed in tools/assistant/internal/assets/sources.txt (embedded in the assistant) -> go, plus its own rule
#   src/**, tests/native/**                                  -> native + shell (shell tests build against src/)
#   tests/<name>_test.sh                                     -> the group of that test (tools/ci/test_groups.txt)
#   tools/lib, tools/build, tools/ci (shared by every group) -> everything
#   tools/assistant/**                                       -> go
#   tools/release/models/*.txt, tools/release/image_files.txt -> go + shell
#   tools/release/*.sh                                       -> go + shell
#   tools/release/** (other)                                 -> go
#   akam, other tools/**/*.sh                                -> shell + native
#   anything else (deps.lock, workflows, fixtures, tests helpers, Go files elsewhere...) -> everything
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
        if [ -z "$base" ] || [[ $base =~ ^0+$ ]] || ! files=$(git -C "$REPO" diff --name-only --no-renames "$base...HEAD" 2>/dev/null); then
            all
            files=""
        fi
    fi
fi

# Files embedded in the assistant binary (tools/assistant/internal/assets/sources.txt).
embedded=" $(grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$REPO/tools/assistant/internal/assets/sources.txt" 2>/dev/null | tr '\n' ' ')"

while IFS= read -r f; do
    [ -n "$f" ] || continue
    case $embedded in *" $f "*) go=true ;; esac # then its own rule below
    case $f in
        THIRD_PARTY_NOTICES.md | LICENSE | CHANGELOG.md | tools/release/*.md) go=true shell=true ;;
        *.md | docs/* | images/* | specs/* | .github/ISSUE_TEMPLATE/* | .github/FUNDING.yml) ;;
        src/* | tests/native/*) native=true shell=true ;;
        tests/*_test.sh)
            case $(test_group "$REPO/tests" "${f#tests/}") in
                native) native=true ;;
                go) go=true ;;
                shell) shell=true ;;
                lint) ;;
                *) all ;; # unknown (new, unlisted test): local_check fails it; run everything
            esac
            ;;
        tools/lib/* | tools/build/* | tools/ci/*) all ;; # shared by every group
        tools/assistant/*) go=true ;;
        tools/release/models/*.txt | tools/release/image_files.txt) go=true shell=true ;;
        tools/release/*.sh) go=true shell=true ;;
        tools/release/*) go=true ;;
        akam | tools/*.sh) shell=true native=true ;;
        *) all ;;
    esac
done <<<"$files"

echo "shell=$shell"
echo "native=$native"
echo "go=$go"
