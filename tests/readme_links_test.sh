#!/usr/bin/env bash
# Documentation checks: every relative link and #anchor in README.md and docs/*.md resolves, and
# every `./akam <command>` written in README.md exists in `./akam help`.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
cd "$REPO"

FAIL=0
bad() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

# Heading anchors of a Markdown file (headings outside code fences), one per line.
anchors() {
    awk '/^```/ { f = !f; next } !f && /^#+[ \t]/ { sub(/^#+[ \t]+/, ""); print }' "$1" |
        tr '[:upper:]' '[:lower:]' | sed -e 's/[^a-z0-9 _-]//g' -e 's/ /-/g'
}

# Inline link targets "](target)" outside code fences, one per line.
links() {
    awk '/^```/ { f = !f; next } !f { print }' "$1" | grep -o ']([^) ]*)' | sed -e 's/^](//' -e 's/)$//' || true
}

N=0
for f in README.md docs/*.md; do
    dir=$(dirname "$f")
    while IFS= read -r target; do
        [ -n "$target" ] || continue
        case $target in
            http://* | https://* | mailto:*) continue ;;
        esac
        path=${target%%#*}
        frag=
        case $target in *'#'*) frag=${target#*#} ;; esac
        if [ -z "$path" ]; then
            dest=$f
        elif [ "$dir" = . ]; then
            dest=$path
        else
            dest=$dir/$path
        fi
        N=$((N + 1))
        if [ ! -e "$dest" ]; then
            bad "$f: broken link ($target)"
            continue
        fi
        if [ -n "$frag" ]; then
            case $dest in
                *.md) anchors "$dest" | grep -qxF -- "$frag" || bad "$f: missing anchor ($target)" ;;
            esac
        fi
    done < <(links "$f")
done
echo "  checked $N relative links"

# Every ./akam command in README.md exists in `./akam help`.
HELP=$("$REPO/akam" help --no-color 2>&1 || true)
CMDS=$(sed -n '/^Commands:/,/^Options:/p' <<<"$HELP" | awk '/^  [a-z]/ { print $1 }')
[ -n "$CMDS" ] || bad "could not read the command list from ./akam help"
M=0
while IFS= read -r cmd; do
    [ -n "$cmd" ] || continue
    M=$((M + 1))
    grep -qxF -- "$cmd" <<<"$CMDS" || bad "README.md: './akam $cmd' is not in ./akam help"
done < <(grep -o '\./akam [a-z][a-z-]*' README.md | awk '{ print $2 }' | sort -u | grep -vxF help || true)
echo "  checked $M distinct ./akam commands"

if [ "$FAIL" -ne 0 ]; then
    echo "readme_links_test: $FAIL failure(s)"
    exit 1
fi
echo "readme_links_test: OK"
