#!/bin/sh
# source_hash.sh -- sha256 of the sources of akamd. The file list is GENERATED from the build, not kept by hand: the platform Makefile's
# `print-sources` (every source of POC3_ORDER: platform, model and shared src/ files) plus the closure of their quoted includes over the build's
# include path (`print-incdirs`, tools/build/include_closure.sh), plus the whole platform and model directories (*.c, *.h, Makefile, model.mk, probes)
# and the shared build rules src/build/*.mk. The hash covers each file's repository-relative path and its bytes. SmolRTSP (third party, fetched) is
# not part of it. Used by `make poc3-dist` (BUILDINFO source_sha256) and by build_card.sh / build_appfs.sh / make_kit_2k.sh (refuse a stale
# binary). Host only: needs make and POSIX sh, no compiler.
#   source_hash.sh            akamd                  source_hash.sh sntp   akam-sntp (helpers/sntp)
#   source_hash.sh audio      akam-mic (helpers/mic)     source_hash.sh led    akam-led
#   source_hash.sh --list     the file list of akamd (what tests/rot2k_source_hash_test.sh checks)
set -eu
here=$(cd "$(dirname "$0")" && pwd -P)
fw=$(cd "$here/.." && pwd -P)
root=$(cd "$here/../../.." && pwd -P)
plat=$root/platforms/anyka-ak3918av130
# the Makefile derives MODEL from the directory name of the firmware this script lives in
model=$(basename "$fw")
mk() { make -s -C "$plat" MODEL="$model" "$@"; }
hash_files() { # stdin: absolute paths; prints the sha256 of "relative path, bytes" of each, sorted and de-duplicated
    LC_ALL=C sort -u | while IFS= read -r f; do
        printf '%s\n' "${f#"$root"/}"
        cat "$f"
    done | sha256sum | cut -d' ' -f1
}
closure() { # closure of the sources given as arguments over the build's include path
    incargs=""
    for d in $(mk print-incdirs); do incargs="$incargs -I $d"; done
    # shellcheck disable=SC2086
    sh "$root/tools/build/include_closure.sh" $incargs "$@"
}
inputs() { # absolute paths of every input of the akamd hash
    # shellcheck disable=SC2046
    closure $(mk print-sources)
    printf '%s\n' "$plat"/*.c "$plat"/*.h "$plat"/Makefile "$plat"/probes/*.c "$fw"/*.c "$fw"/*.h "$fw"/model.mk "$root"/src/build/*.mk "$fw/helpers/ptz/akam_ptz_safe.h"
}
case "${1:-}" in
    sntp) printf '%s\n' "$fw"/helpers/sntp/*.c "$fw"/helpers/sntp/*.h | hash_files ;;
    audio) printf '%s\n' "$fw"/helpers/mic/*.c "$fw"/helpers/mic/*.h "$root"/src/hal/akpcm.h | hash_files ;;
    led)
        # shellcheck disable=SC2046
        closure $(mk print-led-sources) | hash_files
        ;;
    --list) inputs | LC_ALL=C sort -u ;;
    "") inputs | hash_files ;;
    *) echo "usage: source_hash.sh [sntp|audio|led|--list]" >&2; exit 2 ;;
esac
