#!/usr/bin/env bash
# Documentation checks: every relative link and #anchor in README.md and docs/*.md resolves, and
# every `./akam <command>` written in README.md exists in `./akam help`, and every `akam <command>` of the user pages exists in the Go help text.
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

# Every ./akam command in README.md and the user pages exists in `./akam help`.
HELP=$("$REPO/akam" help --no-color 2>&1 || true)
CMDS=$(sed -n '/^Commands:/,/^Options:/p' <<<"$HELP" | awk '/^  [a-z]/ { print $1 }')
[ -n "$CMDS" ] || bad "could not read the command list from ./akam help"
M=0
while IFS= read -r cmd; do
    [ -n "$cmd" ] || continue
    M=$((M + 1))
    # commands of the Go assistant only: the bash wizard has no `key` change, no configs, no API key and no text-port opt-in (documented as such)
    case $cmd in key | config | api-key | control-port) continue ;; esac
    grep -qxF -- "$cmd" <<<"$CMDS" || bad "'./akam $cmd' (README or a user page) is not in ./akam help"
done < <(grep -oh '\./akam [a-z][a-z-]*' README.md docs/using_akam.md docs/wizard_guide.md docs/onvif.md docs/compatible-cameras.md | awk '{ print $2 }' | sort -u | grep -vxF help || true)
echo "  checked $M distinct ./akam commands"

# Every `akam <command>` (the Go program, the name the user pages use) written in backticks exists in the Go help text
# (the commands, plus the 2K words of its settings block: track, wifi, settings).
GOHELP=tools/assistant/internal/cli/help.txt
GOCMDS=$({
    sed -n '/^Commands:/,/^Options:/p' "$GOHELP" | awk '/^  [a-z]/ { sub(/,$/, "", $1); print $1 }'
    awk '/^    (track|wifi|settings) / { print $1 }' "$GOHELP"
} | sort -u)
[ -n "$GOCMDS" ] || bad "could not read the command list from $GOHELP"
G=0
while IFS= read -r cmd; do
    [ -n "$cmd" ] || continue
    G=$((G + 1))
    grep -qxF -- "$cmd" <<<"$GOCMDS" || bad "'akam $cmd' (README or a user page) is not in $GOHELP"
done < <(grep -oh '`akam [a-z][a-z0-9-]*' README.md docs/install.md docs/using_akam.md docs/wizard_guide.md docs/onvif.md docs/compatible-cameras.md docs/assistant.md docs/mqtt.md docs/two_way_audio.md docs/troubleshooting.md | awk '{ print $2 }' | sort -u | grep -vxF help || true)
echo "  checked $G distinct akam commands against the Go help"

if [ "$FAIL" -ne 0 ]; then
    echo "readme_links_test: $FAIL failure(s)"
    exit 1
fi
echo "readme_links_test: OK"
