# shellcheck shell=bash
# Pinned Go toolchain helpers (tools/build/deps.lock GO_VERSION). Source it, do not run it.
# The toolchain lives in tools/.toolchain/go-<version>-<os>-<arch>/ (filled by tools/build/bootstrap.sh).
# Test hooks: AKAM_GO_ROOT overrides the directory (a missing one means "use the go of PATH"),
# AKAM_HOST_ARCH overrides uname -m.

# akam_pinned_go_version REPO: the GO_VERSION of REPO/tools/build/deps.lock (DEPS_LOCK overrides the file, tests)
akam_pinned_go_version() {
    sed -n 's/^GO_VERSION=\([0-9][0-9.]*\).*/\1/p' "${DEPS_LOCK:-$1/tools/build/deps.lock}" | head -n 1
}

# akam_go_os / akam_go_arch: GOOS / GOARCH names of this host
akam_go_os() {
    case "$(uname -s)" in
        Linux) echo linux ;;
        Darwin) echo darwin ;;
        MINGW* | MSYS* | CYGWIN*) echo windows ;;
        *) uname -s | tr '[:upper:]' '[:lower:]' ;;
    esac
}
akam_go_arch() {
    case "${AKAM_HOST_ARCH:-$(uname -m)}" in
        x86_64 | amd64) echo amd64 ;;
        aarch64 | arm64) echo arm64 ;;
        *) echo "${AKAM_HOST_ARCH:-$(uname -m)}" ;;
    esac
}

# akam_go_dir REPO: where the pinned toolchain is (or will be) installed
akam_go_dir() {
    printf '%s\n' "${AKAM_GO_ROOT:-$1/tools/.toolchain/go-$(akam_pinned_go_version "$1")-$(akam_go_os)-$(akam_go_arch)}"
}

# akam_use_pinned_go REPO: when the project-local toolchain is installed, put it first (GOROOT and PATH of
# the calling shell only) and return 0; otherwise return 1 and leave the environment alone.
akam_use_pinned_go() {
    local d
    d=$(akam_go_dir "$1")
    [ -x "$d/bin/go" ] || return 1
    export GOROOT="$d"
    PATH="$d/bin:$PATH"
    export PATH
}
