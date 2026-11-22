#!/usr/bin/env bash
# Docs link and path check: for every tracked Markdown file (docs/archive/ and CHANGELOG.md are history: links are checked, paths are not),
#  1. every relative link "](target)" resolves, and so does its #anchor when the target is Markdown;
#  2. every `repo/path` written in backticks that starts with a top-level repo directory (src/, firmware/, platforms/, tools/,
#     tests/, docs/, specs/, images/) names an existing file or directory (globs, <placeholders>, $vars, {a,b}, [ch] and git-ignored build outputs are skipped; tools/release/release_notes_*.md are templates).
# Exit 1 with one line per problem. Run from anywhere: tools/ci/docs_links_check.sh
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"

FAIL=0
bad() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

anchors() {
    awk '/^```/ { f = !f; next } !f && /^#+[ \t]/ { sub(/^#+[ \t]+/, ""); print }' "$1" |
        tr '[:upper:]' '[:lower:]' | sed -e 's/[^a-z0-9 _-]//g' -e 's/ /-/g'
}

N=0
M=0
while IFS= read -r f; do
    [ -f "$f" ] || continue
    case $f in tools/release/release_notes_*) continue ;; esac # release templates: links are docs-relative, rewritten at release time
    dir=$(dirname "$f")
    body=$(awk '/^```/ { c = !c; next } !c { print }' "$f")
    while IFS= read -r target; do
        [ -n "$target" ] || continue
        case $target in http://* | https://* | mailto:*) continue ;; esac
        path=${target%%#*}
        frag=
        case $target in *'#'*) frag=${target#*#} ;; esac
        if [ -z "$path" ]; then dest=$f; elif [ "$dir" = . ]; then dest=$path; else dest=$dir/$path; fi
        N=$((N + 1))
        if [ ! -e "$dest" ]; then bad "$f: broken link ($target)"; continue; fi
        if [ -n "$frag" ]; then
            case $dest in *.md) anchors "$dest" | grep -qxF -- "$frag" || bad "$f: missing anchor ($target)" ;; esac
        fi
    done < <(grep -o ']([^) ]*)' <<<"$body" | sed -e 's/^](//' -e 's/)$//' || true)

    case $f in docs/archive/* | CHANGELOG.md) continue ;; esac
    # shellcheck disable=SC2016  # the backticks are literal Markdown, not a command substitution
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        case $p in *'*'* | *'<'* | *'$'* | *'{'* | *'['* | *'...'* | *'~'*) continue ;; esac
        p=${p%%:*} # file:line
        p=${p%/}
        M=$((M + 1))
        [ -e "$p" ] || git check-ignore -q -- "$p" || git check-ignore -q -- "$p/" || bad "$f: path does not exist (\`$p\`)"
    done < <(grep -oE '`(src|firmware|platforms|tools|tests|docs|specs|images)/[^` ]*`' <<<"$body" | tr -d '`' || true)
done < <(git ls-files '*.md')

echo "  checked $N relative links and $M repo paths in Markdown"
if [ "$FAIL" -gt 0 ]; then
    echo "docs_links_check: $FAIL problem(s)"
    exit 1
fi
echo "docs_links_check: ok"
