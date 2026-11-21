#!/usr/bin/env bash
# Test of firmware/lsc-rotatable-2k-ak3918av130/image/source_hash.sh, the stale-binary guard of build_card.sh / build_appfs.sh: its file list is
# generated from the build (Makefile print-sources + include closure), so a source or header the 2K app compiles can never be left out of the hash.
#   1. every source of POC3_ORDER (make print-sources) is in the list;
#   2. every header the pinned cross compiler reports (gcc -MM over the same sources and include path) that lives in this repository is in the
#      list (skipped without tools/.toolchain);
#   3. changing any one of the files the old hand list missed (track_cfgkv.h, led_pat.c/.h, api_backend.h, src/core/control, src/core/version),
#      a header of every src/ subdirectory, the model and the build rules changes the hash (mutation on a copy, nothing is modified in place).
# Group "rot2k".
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd -P)
FWN=lsc-rotatable-2k-ak3918av130
SH=$REPO/firmware/$FWN/image/source_hash.sh
PLAT=$REPO/platforms/anyka-ak3918av130
WORK=$(mktemp -d)
PASS=0
FAIL=0
trap 'rm -rf "$WORK"' EXIT
ok() { PASS=$((PASS + 1)); echo "  ok   - $1"; }
ko() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }
check() { local d=$1; shift; if "$@"; then ok "$d"; else ko "$d"; fi; }

sh "$SH" --list >"$WORK/list.txt"
make -s -C "$PLAT" print-sources >"$WORK/sources.txt"
make -s -C "$PLAT" print-incdirs >"$WORK/incdirs.txt"
all_exist() { local f; while read -r f; do [ -f "$f" ] || return 1; done <"$1"; }
missing_from_list() { grep -vxFf "$WORK/list.txt" "$1" || true; }
check "the hash list is large and every entry exists" bash -c "[ \$(wc -l <'$WORK/list.txt') -gt 100 ]"
check "every entry of the hash list exists" all_exist "$WORK/list.txt"
check "every compiled source of POC3_ORDER is hashed" test -z "$(missing_from_list "$WORK/sources.txt")"
for f in src/features/track/track_cfgkv.h src/features/light/led_pat.c src/features/light/led_pat.h src/api/api_backend.h; do
    check "$f is hashed" grep -qxF "$REPO/$f" "$WORK/list.txt"
done
# src/core/control and src/core/version are linked by the 1080p only today: they are hashed exactly when POC3_ORDER names them
# (a module added there is then covered without touching the hash script).
for d in src/core/control src/core/version; do
    if grep -q "^$REPO/$d/" "$WORK/sources.txt"; then
        check "the compiled files of $d are hashed" test -z "$(grep "^$REPO/$d/" "$WORK/sources.txt" | grep -vxFf "$WORK/list.txt" || true)"
    else
        echo "  note - $d is not part of the 2K build, so it is not hashed"
    fi
done

# 2. oracle: the cross compiler's own dependency scan
GCC=$REPO/tools/.toolchain/armv5-eabi--uclibc--stable-2018.11-1/bin/arm-linux-gcc
if [ -x "$GCC" ]; then
    : >"$WORK/gccdeps.txt"
    incs=()
    while read -r d; do incs+=(-I"$d"); done <"$WORK/incdirs.txt"
    while read -r s; do
        "$GCC" -std=gnu99 -MM -MG "${incs[@]}" -I"$REPO/third_party/smolrtsp/include" "$s" 2>/dev/null |
            sed 's/[ \\]\{1,\}/\n/g' | grep -E '\.(h|c)$' >>"$WORK/gccdeps.txt" || true
    done <"$WORK/sources.txt"
    : >"$WORK/gccabs.txt"
    while read -r h; do
        case $h in
            /*) if [ -f "$h" ]; then echo "$h" >>"$WORK/gccabs.txt"; fi ;;
            *) while read -r d; do if [ -f "$d/$h" ]; then echo "$d/$h" >>"$WORK/gccabs.txt"; break; fi; done <"$WORK/incdirs.txt" ;;
        esac
    done < <(sort -u "$WORK/gccdeps.txt")
    sort -u "$WORK/gccabs.txt" | grep -v -e '/third_party/' -e '/tools/.toolchain/' >"$WORK/gccrepo.txt" || true
    check "the compiler's own header scan finds files (the oracle works)" bash -c "[ \$(wc -l <'$WORK/gccrepo.txt') -gt 80 ]"
    check "every file the compiler reads is hashed" test -z "$(missing_from_list "$WORK/gccrepo.txt")"
else
    echo "  skip - no tools/.toolchain: header oracle not run"
fi

# 3. mutation: copy what the hash reads, change one file at a time
COPY=$WORK/copy
mkdir -p "$COPY/tools/build" "$COPY/firmware/$FWN/image" "$COPY/platforms"
cp "$REPO/tools/build/include_closure.sh" "$COPY/tools/build/"
cp "$SH" "$COPY/firmware/$FWN/image/"
cp -r "$REPO/src" "$COPY/src"
cp -r "$PLAT" "$COPY/platforms/"
(cd "$REPO/firmware/$FWN" && find . -maxdepth 3 \( -name '*.c' -o -name '*.h' -o -name '*.mk' \) -not -path './boot/*' -not -path './tools/*' -print0 | xargs -0 cp --parents -t "$COPY/firmware/$FWN")
CSH=$COPY/firmware/$FWN/image/source_hash.sh
base=$(sh "$CSH")
check "the copy hashes like the original (path based, not location based)" test "$base" = "$(sh "$SH")"
mutate() { # mutate <path under the copy>: the hash must change, then the file is restored
    local f=$COPY/$1 h
    cp "$f" "$WORK/keep"
    case $f in *.mk | */Makefile) printf '\n# mutated\n' >>"$f" ;; *) printf '\n/* mutated */\n' >>"$f" ;; esac
    h=$(sh "$CSH")
    cp "$WORK/keep" "$f"
    [ "$h" != "$base" ]
}
targets=(src/features/track/track_cfgkv.h src/features/light/led_pat.c src/features/light/led_pat.h src/api/api_backend.h src/build/sources.mk platforms/anyka-ak3918av130/Makefile "firmware/$FWN/model.mk" "firmware/$FWN/ptz.h" "firmware/$FWN/helpers/ptz/akam_ptz_safe.h")
# one header of every src/ directory the 2K build reads from (the first one that is in the hash list)
for d in api core features/audio features/button features/light features/night features/ptz features/scene features/track hal net/crypto net/http net/mqtt net/onvif net/rtsp; do
    h=$(grep "^$REPO/src/$d/.*\.h\$" "$WORK/list.txt" | head -1 || true)
    [ -z "$h" ] || targets+=("${h#"$REPO"/}")
done
for rel in "${targets[@]}"; do
    check "changing $rel changes the hash" mutate "$rel"
done
# a new header that is only included by another header: found through the closure
printf '#define NEW_DEP 1\n' >"$COPY/src/api/zz_new_dep.h"
printf '\n#include "zz_new_dep.h"\n' >>"$COPY/src/api/api_backend.h"
nb=$(sh "$CSH")
check "a newly included header changes the hash" test "$nb" != "$base"
printf '#define NEW_DEP 2\n' >"$COPY/src/api/zz_new_dep.h"
check "and so does a later change to that header" test "$(sh "$CSH")" != "$nb"

echo "rot2k_source_hash_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
