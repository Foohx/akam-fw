# shellcheck shell=bash
# Check groups shared by tools/ci/local_check.sh (what runs) and tools/ci/changed_groups.sh (what a diff
# touches), so local runs and GitHub CI agree. Source this file; it defines test_group only.
#   lint    always runs in CI (cheap): shellcheck, hygiene, tests/readme_links_test.sh
#   go      tests/assistant_test.sh (Go: fmt, vet, tests, six-target build, size budget, reproducibility)
#   native  tests that compile and run C code (tests/native/, src/) with the host compiler or pinned Zig
#   shell   every other tests/*_test.sh (wizard, camera-side ash, release scripts) and the wizard smoke runs
# Every test belongs to exactly one group, so the groups together are "all".

# test_group <tests/ directory> <basename of a *_test.sh>: prints the group of that test.
test_group() {
    case $2 in
        readme_links_test.sh) echo lint ;;
        assistant_test.sh) echo go ;;
        *)
            if grep -q -E 'tests/native|/src/|SRC=' "$1/$2" 2>/dev/null; then echo native; else echo shell; fi
            ;;
    esac
}
