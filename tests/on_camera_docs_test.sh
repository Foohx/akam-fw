#!/usr/bin/env bash
# on-camera docs cannot rot: every Akam path named in backticks in docs/porting/<model>/on-camera.md
# must exist in what builds that model's image.
#   - a stock path is written `/path` (stock) and is not checked;
#   - runtime paths (/tmp /etc/config /config /dev /sys /proc, /mnt/sdcard/akam/<non-script>) are not checked.
# 1080p: /usr/<path> must be in tools/release/image_files.txt (image path column), or be a build output
#        (bin/akamd, bin/dropbearmulti, local/akam/LICENSE.*, local/akam/MANIFEST.txt).
# 2K:    /appfs/... names must appear in image/build_appfs.sh; /mnt/sdcard/<x>.sh|.ini and
#        /mnt/sdcard/akam/<x>.sh must exist under boot/card, boot/rescue or the shared script templates.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
cd "$REPO"

FAIL=0
N=0
bad() { FAIL=$((FAIL + 1)); echo "  FAIL - $1"; }

# paths_of <doc>: absolute camera paths in backticks that are not followed by (stock)
# shellcheck disable=SC2016  # literal Markdown backticks, not command substitution
paths_of() {
    { grep -oE '`/[A-Za-z0-9_./*+-]+`( \(stock)?' "$1" || true; } | { grep -v '(stock$' || true; } |
        sed -e 's/^`//' -e 's/`.*$//' | { grep -vxE '/(tmp|usr|appfs|config|etc/config|mnt/sdcard)' || true; } | LC_ALL=C sort -u
}

check_1080p() {
    local doc=docs/porting/lsc-1080p/on-camera.md list=tools/release/image_files.txt p rel
    [ -f "$doc" ] || { bad "$doc missing"; return; }
    while IFS= read -r p; do
        case $p in
            /usr/*) ;;
            /tmp/* | /etc/config/* | /dev/* | /proc/* | /sys/*) continue ;;
            *) bad "$doc: $p is neither a runtime path nor under /usr (mark a stock file with (stock))"; continue ;;
        esac
        N=$((N + 1))
        rel=${p#/usr/}
        case $rel in
            bin/akamd | bin/dropbearmulti | local/akam/LICENSE.* | local/akam/MANIFEST.txt) continue ;;
        esac
        awk -v r="$rel" '($1 == "file" || $1 == "link") && $4 == r { f = 1 } END { exit !f }' "$list" ||
            bad "$doc: $p (image path $rel) is not in $list"
    done < <(paths_of "$doc")
}

check_2k() {
    local doc=docs/porting/lsc-rotatable-2k/on-camera.md fw=firmware/lsc-rotatable-2k-ak3918av130 p b
    [ -f "$doc" ] || { bad "$doc missing"; return; }
    while IFS= read -r p; do
        b=${p##*/}
        case $p in
            /tmp/* | /config/* | /dev/* | /proc/* | /sys/* | /etc/*) ;;
            /appfs/*)
                N=$((N + 1))
                case $b in akam | bin) continue ;; esac
                grep -qF -- "$b" "$fw/image/build_appfs.sh" || [ -f "$fw/boot/flash/$b" ] ||
                    bad "$doc: $p is not named in image/build_appfs.sh" ;;
            /mnt/sdcard/akam/*)
                case $b in
                    akam_ptz)
                        N=$((N + 1))
                        [ -f "$fw/helpers/ptz/akam_ptz.c" ] || bad "$doc: $p has no source ptz/akam_ptz.c" ;;
                    *.sh)
                        N=$((N + 1))
                        [ -f "$fw/boot/card/akam/$b" ] || [ -f "firmware/lsc-indoor-1080p-ak3918/overlay/$b" ] || [ -f "src/scripts/$b" ] ||
                            bad "$doc: $p is not in $fw/boot/card/akam or the shared templates" ;;
                esac ;;
            /mnt/sdcard/*)
                case $b in
                    *.sh | *.ini | telnetd)
                        N=$((N + 1))
                        [ -f "$fw/boot/card/$b" ] || [ -f "$fw/boot/rescue/$b" ] ||
                            bad "$doc: $p is not in $fw/boot/card or $fw/boot/rescue" ;;
                esac ;;
            *) bad "$doc: $p is neither a runtime path nor under /appfs or /mnt/sdcard (mark a stock file with (stock))" ;;
        esac
    done < <(paths_of "$doc")
}

check_1080p
check_2k
echo "  checked $N Akam paths"

# the parser itself works: the marked stock path is skipped, the others are kept
# shellcheck disable=SC2016  # literal Markdown backticks, not command substitution
sample=$(printf '%s\n' '`/usr/sbin/akam_nope.sh` and `/usr/sbin/akam_run.sh`' '`/usr/bin/x` (stock)')
echo "$sample" >"${TMPDIR:-.}/.oncam_sample.$$"
got=$(paths_of "${TMPDIR:-.}/.oncam_sample.$$" | tr '\n' ' ')
rm -f "${TMPDIR:-.}/.oncam_sample.$$"
[ "$got" = "/usr/sbin/akam_nope.sh /usr/sbin/akam_run.sh " ] || bad "paths_of parsing is wrong: '$got'"
[ "$N" -ge 20 ] || bad "only $N paths checked, the docs lost their paths"

if [ "$FAIL" -ne 0 ]; then
    echo "on_camera_docs_test: $FAIL failure(s)"
    exit 1
fi
echo "on_camera_docs_test: OK"
