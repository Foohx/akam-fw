#!/usr/bin/env bash
# rewrite_paths.sh -- rewrite old repo paths to the new ones of the reorganisation (tools/dev/path-map.txt) in text files:
# a patch (git format-patch / git diff output, "a/" and "b/" prefixes included), a script, a doc, a test. For a feature branch
# that was cut before the move and is landed after it.
#
#   tools/dev/rewrite_paths.sh FILE...            dry run (default): per file, the lines that WOULD change, as a diff
#   tools/dev/rewrite_paths.sh --apply FILE...    rewrite the files in place
#   tools/dev/rewrite_paths.sh --stdout FILE...   print the rewritten text (use "-" or no FILE for stdin)
#   tools/dev/rewrite_paths.sh --map FILE ...     use another table than tools/dev/path-map.txt
# Exit 0 always for a successful run; 2 on usage errors. A path is rewritten only at a path boundary (not inside a longer
# name); a map line ending in "/" is a directory prefix, the others are whole files; the longest old path wins. Lines of the
# table marked "# UNDECIDED" are not in the table, so those paths are left alone and must be fixed by hand.
# Typical use on a late branch:  git format-patch dev..mybranch -o P; tools/dev/rewrite_paths.sh --apply P/*.patch; git am P/*.patch
# Also rewrites $VAR/ and ${VAR}/ prefixed paths ($REPO/src/common/net/md5.c). A directory that the move SPLITS over several
# targets (src/common/net, src/ak_rtsp) is not rewritten when named alone: fix those by hand. Only touches the files named on the
# command line. Needs GNU sed.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MAP=$HERE/path-map.txt
mode=dry
files=()
while [ $# -gt 0 ]; do
    case "$1" in
        --apply) mode=apply ;;
        --stdout) mode=stdout ;;
        --dry-run) mode=dry ;;
        --map) [ $# -ge 2 ] || { echo "rewrite_paths: --map needs a file" >&2; exit 2; }; MAP=$2; shift ;;
        -h | --help) sed -n "2,17p" "$0"; exit 0 ;;
        -) files+=(-) ;;
        -*) echo "rewrite_paths: unknown option $1" >&2; exit 2 ;;
        *) files+=("$1") ;;
    esac
    shift
done
[ -f "$MAP" ] || { echo "rewrite_paths: map not found: $MAP" >&2; exit 2; }
[ ${#files[@]} -gt 0 ] || files=(-)
if [ "$mode" != stdout ]; then
    for f in "${files[@]}"; do [ "$f" != - ] || { echo "rewrite_paths: stdin needs --stdout" >&2; exit 2; }; done
fi
TMPBASE=${TMPDIR:-$HOME/.cache/akam-tmp}
mkdir -p "$TMPBASE"
WORK=$(mktemp -d "$TMPBASE/rewrite_paths.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Build the sed program: longest old path first. Characters special in a basic/extended regex or in the s### delimiter are escaped.
esc() { printf '%s' "$1" | sed -e 's/[][\\.*^$+?(){}|#&]/\\&/g'; }
escrepl() { printf '%s' "$1" | sed -e 's/[\\#&]/\\&/g'; }
awk -F'\t' '!/^#/ && NF==2 {print length($1) "\t" $1 "\t" $2}' "$MAP" | LC_ALL=C sort -t"$(printf '\t')" -k1,1nr -k2,2 | cut -f2- >"$WORK/rules.tsv"
: >"$WORK/prog.sed"
while IFS=$'\t' read -r old new; do
    o=$(esc "$old"); n=$(escrepl "$new")
    case "$old" in
        */) tail=''  ;;
        *) tail='([^A-Za-z0-9_-]|$)' ;;
    esac
    # generic boundary, then the "a/" and "b/" prefixes of diff headers
    if [ -n "$tail" ]; then
        printf 's#(^|[^A-Za-z0-9_./-]|[$][{(]?[A-Za-z_][A-Za-z0-9_]*[})]?/)%s%s#\\1%s\\2#g\n' "$o" "$tail" "$n" >>"$WORK/prog.sed"
        printf 's#(^|[[:space:]])([ab]/)%s%s#\\1\\2%s\\3#g\n' "$o" "$tail" "$n" >>"$WORK/prog.sed"
    else
        printf 's#(^|[^A-Za-z0-9_./-]|[$][{(]?[A-Za-z_][A-Za-z0-9_]*[})]?/)%s#\\1%s#g\n' "$o" "$n" >>"$WORK/prog.sed"
        printf 's#(^|[[:space:]])([ab]/)%s#\\1\\2%s#g\n' "$o" "$n" >>"$WORK/prog.sed"
    fi
done <"$WORK/rules.tsv"
# The file rules consume the character after the name, so two paths separated by one space need a second pass.
cat "$WORK/prog.sed" "$WORK/prog.sed" >"$WORK/prog2.sed"

rewrite() { sed -E -f "$WORK/prog2.sed"; }

total=0
for f in "${files[@]}"; do
    if [ "$f" = - ]; then rewrite; continue; fi
    [ -f "$f" ] || { echo "rewrite_paths: not a file: $f" >&2; exit 2; }
    rewrite <"$f" >"$WORK/out"
    case "$mode" in
        stdout) cat "$WORK/out" ;;
        apply)
            if ! cmp -s "$f" "$WORK/out"; then cat "$WORK/out" >"$f"; echo "rewrote $f"; total=$((total + 1)); fi ;;
        dry)
            if ! cmp -s "$f" "$WORK/out"; then
                n=$(diff "$f" "$WORK/out" | grep -c '^>' || true)
                echo "== $f: $n line(s) would change"
                diff "$f" "$WORK/out" | grep -E '^[<>]' || true
                total=$((total + 1))
            fi ;;
    esac
done
[ "$mode" = stdout ] || echo "rewrite_paths: $total file(s) $([ "$mode" = apply ] && echo rewritten || echo would change) (mode: $mode)" >&2
