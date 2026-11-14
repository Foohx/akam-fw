# shellcheck shell=bash
# Check groups shared by tools/ci/local_check.sh (what runs) and tools/ci/changed_groups.sh (what a diff
# touches), so local runs and GitHub CI agree. Source this file; it defines test_group only.
#   lint    always runs in CI (cheap): shellcheck, hygiene, tests/readme_links_test.sh
#   go      tests/assistant_test.sh (Go: fmt, vet, tests, six-target build, size budget, reproducibility)
#   native  tests that compile and run C code (tests/native/, src/) with the host compiler or pinned Zig
#   shell   every other tests/*_test.sh (wizard, camera-side ash, release scripts) and the wizard smoke runs
# Every test belongs to exactly one group, so the groups together are "all". The mapping is the explicit table
# tools/ci/test_groups.txt (no guessing from the test's text): a test missing from it is group "unknown", which
# makes local_check fail until the table is updated.

# test_group <tests/ directory> <basename of a *_test.sh>: prints lint, go, native, shell or unknown.
test_group() {
    local table g f
    table="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_groups.txt"
    while read -r g f; do
        case $g in '' | '#'*) continue ;; esac
        if [ "$f" = "$2" ]; then
            echo "$g"
            return 0
        fi
    done <"$table"
    echo unknown
}
