#!/usr/bin/env bash
# Local equivalent of CI (no CI minutes): shellcheck, all tests/*_test.sh, tracked-file hygiene.
# Usage: tools/ci/local_check.sh [--only GROUP[,GROUP...]]
#   (from anywhere; run tools/build/bootstrap.sh first so camera-side tests get the pinned BusyBox)
# Groups (default and "all": every group, in this order; GitHub CI runs the SAME code per group,
# see .github/workflows/ci.yml, so each check runs once per pipeline):
#   lint    shellcheck, tracked-file hygiene (forbidden files, symlinks, private IPs), tests/readme_links_test.sh
#   shell   wizard smoke runs and every tests/*_test.sh that is neither native, go nor lint
#   native  tests/*_test.sh that build and run C code of src/ and tests/native/ (host compiler or pinned Zig)
#   go      tests/assistant_test.sh (gofmt, vet, go test, six-target build + size budget, reproducibility)
#   race    opt-in, not part of "all": CGO_ENABLED=1 go test -race -tags akamtest ./... in tools/assistant
#           (CI runs --only go,race; needs a C compiler for cgo)
# Every test belongs to exactly one group (test_group in tools/ci/groups.sh, also used by changed_groups.sh),
# so the groups together are "all".
# Exit 0 only when every step passes. Prints "LOCAL CHECK: OK (N tests, M skipped)".
# A test is counted as skipped when it exits 0 and prints a line starting with "skip:" or "SKIP:".
# Data files (next to this script):
#   - tools/ci/shellcheck_legacy.txt: tracked scripts allowed to have findings (upstream or known)
#   - tools/ci/tracked_exceptions.txt: tracked paths allowed despite the forbidden-file patterns
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO" || exit 2

# shellcheck source=tools/lib/host.sh
. "$REPO/tools/lib/host.sh"

# ---------------------------------------------------------------- group selection
ALL_GROUPS="lint shell native go"
ONLY=""
while [ "$#" -gt 0 ]; do
    case $1 in
        --only)
            [ "$#" -ge 2 ] || { echo "local_check: --only needs a group list" >&2; exit 2; }
            ONLY=$2
            shift
            ;;
        --only=*) ONLY=${1#--only=} ;;
        -h | --help) sed -n '2,/^set -uo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "local_check: unknown argument: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
SEL=" "
if [ -z "$ONLY" ] || [ "$ONLY" = all ]; then
    SEL=" $ALL_GROUPS "
else
    for g in ${ONLY//,/ }; do
        case " $ALL_GROUPS race " in
            *" $g "*) SEL="$SEL$g " ;;
            *) echo "local_check: unknown group: $g (lint, shell, native, go, race, all)" >&2; exit 2 ;;
        esac
    done
fi
want() { case "$SEL" in *" $1 "*) return 0 ;; esac; return 1; }

# shellcheck source=tools/ci/groups.sh
. "$HERE/groups.sh"

FAILED=0
note_fail() {
    FAILED=$((FAILED + 1))
    host_say red "FAIL: $1"
}

# Pinned toolchain (BusyBox for camera-side tests, Zig). Optional: tests skip without it.
if [ -f "$REPO/tools/build/env.sh" ]; then
    # shellcheck source=tools/build/env.sh
    . "$REPO/tools/build/env.sh" >/dev/null 2>&1 || true
fi

# Lines of a data file without comments and blanks.
data_lines() {
    [ -f "$1" ] && grep -v -e '^[[:space:]]*#' -e '^[[:space:]]*$' "$1"
    return 0
}

in_list() { # needle, list file
    data_lines "$2" | grep -qxF -- "$1"
}

# ---------------------------------------------------------------- 1. shellcheck
if want lint; then
    host_say bold "== shellcheck"
    if ! host_have shellcheck; then
        note_fail "shellcheck not installed: $(host_pkg_install_hint shellcheck)"
    else
        sc_n=0
        sc_legacy=0
        while IFS= read -r f; do
            case $f in tests/fixtures/*) continue ;; esac
            if in_list "$f" "$HERE/shellcheck_legacy.txt"; then
                sc_legacy=$((sc_legacy + 1))
                continue
            fi
            sc_n=$((sc_n + 1))
            if ! shellcheck -x "$f"; then
                note_fail "shellcheck findings in $f"
            fi
        done < <(git ls-files '*.sh')
        echo "shellcheck: $sc_n scripts checked, $sc_legacy legacy-allowlisted"
    fi
fi

# ---------------------------------------------------------------- 2. tests
host_say bold "== tests"
if [ -n "${AKAM_BUSYBOX:-}" ]; then
    echo "camera-side tests use pinned BusyBox: $AKAM_BUSYBOX"
else
    echo "pinned BusyBox absent (run tools/build/bootstrap.sh): camera-side tests may skip"
fi
T_RUN=0
T_PASS=0
T_SKIP=0
T_FAIL=0
LOGDIR=$(mktemp -d)
trap 'rm -rf "$LOGDIR"' EXIT
for t in "$REPO"/tests/*_test.sh; do
    [ -f "$t" ] || continue
    name=$(basename "$t")
    want "$(test_group "$REPO/tests" "$name")" || continue
    T_RUN=$((T_RUN + 1))
    bash "$t" >"$LOGDIR/$name.log" 2>&1 </dev/null
    rc=$?
    if [ "$rc" -ne 0 ]; then
        T_FAIL=$((T_FAIL + 1))
        host_say red "FAIL  $name (exit $rc)"
        tail -n 25 "$LOGDIR/$name.log" | sed 's/^/      | /'
        note_fail "test $name"
    elif grep -q -e '^skip:' -e '^SKIP:' "$LOGDIR/$name.log"; then
        T_SKIP=$((T_SKIP + 1))
        host_say yellow "SKIP  $name ($(grep -m1 -e '^skip:' -e '^SKIP:' "$LOGDIR/$name.log"))"
    else
        T_PASS=$((T_PASS + 1))
        host_say green "PASS  $name"
    fi
done
echo "tests: $T_PASS passed, $T_FAIL failed, $T_SKIP skipped (of $T_RUN)"
if [ "$T_RUN" -eq 0 ] && [ "$SEL" != " race " ]; then
    note_fail "no tests found"
fi

# ---------------------------------------------------------------- 3. wizard smoke runs
if want shell; then
    host_say bold "== wizard smoke"
    SMOKE_STATE=$(mktemp -d)
    for smoke in "doctor" "--dry-run status"; do
        # shellcheck disable=SC2086  # $smoke is a fixed word list
        if bash "$REPO/akam" --no-color --state-dir "$SMOKE_STATE" $smoke >"$LOGDIR/smoke.log" 2>&1 </dev/null; then
            host_say green "PASS  ./akam $smoke"
        else
            tail -n 15 "$LOGDIR/smoke.log" | sed 's/^/      | /'
            note_fail "./akam $smoke"
        fi
    done
    rm -rf "$SMOKE_STATE"
fi

# ---------------------------------------------------------------- 4. tracked-file hygiene
if want lint; then
    host_say bold "== tracked files"
    bad=0
    while IFS= read -r f; do
        case $f in tests/fixtures/*) continue ;; esac
        base=${f##*/}
        hit=""
        case $base in
            *.bin | *.squashfs | *.img | *.dump | *.pem | *.key | id_rsa | id_ed25519 | id_ecdsa | id_dsa)
                hit="binary, dump or key" ;;
            CLAUDE.md | AGENTS.md) hit="assistant instruction file" ;;
        esac
        [ -z "$hit" ] && continue
        if in_list "$f" "$HERE/tracked_exceptions.txt"; then
            echo "allowed by tracked_exceptions.txt: $f"
            continue
        fi
        bad=$((bad + 1))
        note_fail "forbidden tracked file ($hit): $f"
    done < <(git ls-files)
    echo "tracked files: $bad forbidden"

    # No symlink may ever be tracked (git mode 120000). Tests that need one create it in a temp dir.
    links=0
    while IFS= read -r f; do
        links=$((links + 1))
        note_fail "tracked symlink (git mode 120000), never commit symlinks; create it at test time in a temp dir: $f"
    done < <(git ls-files -s | awk '$1 == "120000" { sub(/^[^\t]*\t/, ""); print }')
    echo "tracked symlinks: $links"

    # No private 192.168 address (RFC 1918 class C) in tracked files: examples use RFC 5737 documentation ranges
    # (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24). Exceptions: tools/ci/ip_allowlist.txt.
    ipbad=0
    while IFS= read -r f; do
        in_list "$f" "$HERE/ip_allowlist.txt" && continue
        ipbad=$((ipbad + 1))
        note_fail "private 192.168 address in tracked file, use an RFC 5737 documentation range (192.0.2.x, 198.51.100.x, 203.0.113.x) or list the file in tools/ci/ip_allowlist.txt: $f"
    done < <(git grep -l -E '192[.]168[.]')
    echo "tracked private-address files: $ipbad"
fi

# ---------------------------------------------------------------- 5. Go race detector (opt-in group "race")
if want race; then
    host_say bold "== go test -race"
    if [ -f "$REPO/tools/lib/gotoolchain.sh" ]; then
        # shellcheck source=tools/lib/gotoolchain.sh
        . "$REPO/tools/lib/gotoolchain.sh"
        akam_use_pinned_go "$REPO" || true
    fi
    TC=$REPO/tools/.toolchain/go
    mkdir -p "$TC/path" "$TC/mod" "$TC/cache"
    if ! host_have go; then
        note_fail "go not found (run tools/build/bootstrap.sh)"
    elif (cd "$REPO/tools/assistant" && GOPATH="$TC/path" GOMODCACHE="$TC/mod" GOCACHE="$TC/cache" \
        GOFLAGS="-modcacherw -mod=readonly" GOTOOLCHAIN=local GOPROXY=off GOTELEMETRY=off \
        CGO_ENABLED=1 go test -race -tags akamtest -count=1 ./...); then
        host_say green "PASS  go test -race"
    else
        note_fail "go test -race"
    fi
fi

# ---------------------------------------------------------------- summary
echo
if [ "$FAILED" -eq 0 ]; then
    host_say green "LOCAL CHECK: OK ($T_RUN tests, $T_SKIP skipped)${ONLY:+ [groups: $ONLY]}"
    exit 0
fi
host_say red "LOCAL CHECK: FAILED ($FAILED problem(s); $T_RUN tests, $T_SKIP skipped)${ONLY:+ [groups: $ONLY]}"
exit 1
