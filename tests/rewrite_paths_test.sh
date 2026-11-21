#!/usr/bin/env bash
# tools/dev/rewrite_paths.sh and tools/dev/path-map.txt: the table is well formed (2 fields, every old path exists in this
# checkout until the move happens, no duplicate old path), and the script rewrites a sample patch, in a/ b/ headers,
# $VAR/ prefixes and plain text, only at path boundaries, and by default (dry run) does not touch the file.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tests/lib/paths.sh
. "$HERE/lib/paths.sh"
RW=$REPO/tools/dev/rewrite_paths.sh
MAP=$REPO/tools/dev/path-map.txt
mkdir -p "${TMPDIR:-$HOME/.cache/akam-tmp}"
WORK=$(mktemp -d "${TMPDIR:-$HOME/.cache/akam-tmp}/rwp.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0
check() { if "${@:2}"; then echo "  ok   - $1"; else echo "FAIL: $1"; fails=$((fails + 1)); fi; }

bad=$(awk -F'\t' '!/^#/ && NF && NF != 2' "$MAP" | wc -l)
check "every table line has exactly two tab separated fields" test "$bad" -eq 0
dups=$(grep -v '^#' "$MAP" | grep . | cut -f1 | sort | uniq -d | wc -l)
check "no old path is listed twice" test "$dups" -eq 0
# Every old path must still exist (before its step) or have moved to its new path (after it): the table stays valid for late
# branches through the whole reorganisation. A directory rule ("old/ new/") is checked on the new directory.
missing=0
while IFS=$'\t' read -r old new; do
    [ -e "$REPO/$old" ] || [ -e "$REPO/$new" ] || { echo "  neither old nor new path exists: $old -> $new"; missing=$((missing + 1)); }
done < <(grep -v '^#' "$MAP" | grep .)
check "every old path exists or was moved to its new path" test "$missing" -eq 0

cat >"$WORK/in.patch" <<'EOP'
diff --git a/src/common/net/md5.c b/src/common/net/md5.c
--- a/src/common/net/md5.c
+++ b/src/common/net/md5.c
+cc "$REPO/src/ak_rtsp/rtsp.c" ${REPO}/src/smolrtsp/include/x.h src/ak_rtsp/rtsp.cx xsrc/ak_rtsp/rtsp.c
EOP
cp "$WORK/in.patch" "$WORK/orig.patch"
"$RW" "$WORK/in.patch" >/dev/null 2>&1
check "dry run leaves the file untouched" cmp -s "$WORK/in.patch" "$WORK/orig.patch"
"$RW" --stdout "$WORK/in.patch" >"$WORK/out.patch"
check "a/ b/ headers rewritten" grep -q '^diff --git a/src/net/crypto/md5.c b/src/net/crypto/md5.c$' "$WORK/out.patch"
# shellcheck disable=SC2016 # the single quotes keep the literal ${REPO}
check "VAR/ prefixes rewritten" grep -q 'platforms/anyka-ak3918/rtsp_session.c" ${REPO}/third_party/smolrtsp/include/x.h' "$WORK/out.patch"
check "longer names and glued prefixes are left alone" grep -q ' src/ak_rtsp/rtsp.cx xsrc/ak_rtsp/rtsp.c$' "$WORK/out.patch"
"$RW" --apply "$WORK/in.patch" >/dev/null 2>&1
check "--apply rewrites in place" cmp -s "$WORK/in.patch" "$WORK/out.patch"
if [ "$fails" -ne 0 ]; then echo "rewrite_paths_test: $fails failed"; exit 1; fi
echo "rewrite_paths_test: all passed"
