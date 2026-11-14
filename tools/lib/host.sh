# shellcheck shell=bash
# Akam host helper library (Linux: Arch and Debian/Ubuntu). Source it, do not run it:
#   . tools/lib/host.sh
# All functions are prefixed host_ and have no side effect at source time.
# Test hooks (tests only): AKAM_OS_RELEASE overrides /etc/os-release,
# AKAM_HOST_ARCH overrides uname -m, AKAM_FORCE_TTY=1 pretends a terminal is attached.

# host_os_release_file: path of the os-release file to read.
host_os_release_file() {
    printf '%s\n' "${AKAM_OS_RELEASE:-/etc/os-release}"
}

# host_os: print arch | debian | ubuntu | unknown. Derived from ID, then ID_LIKE.
host_os() {
    local f id="" like="" line
    f=$(host_os_release_file)
    if [ -r "$f" ]; then
        while IFS= read -r line; do
            case $line in
                ID=*) id=${line#ID=} ;;
                ID_LIKE=*) like=${line#ID_LIKE=} ;;
            esac
        done <"$f"
    fi
    id=${id//\"/}
    like=${like//\"/}
    case $id in
        arch | archarm | manjaro | endeavouros) echo arch; return 0 ;;
        ubuntu) echo ubuntu; return 0 ;;
        debian) echo debian; return 0 ;;
    esac
    case " $like " in
        *" arch "*) echo arch ;;
        *" ubuntu "*) echo ubuntu ;;
        *" debian "*) echo debian ;;
        *) echo unknown ;;
    esac
}

# host_arch: print x86_64 | aarch64 | the raw uname -m value. AKAM_HOST_ARCH overrides (tests).
host_arch() {
    local m=${AKAM_HOST_ARCH:-$(uname -m)}
    case $m in
        x86_64 | amd64) echo x86_64 ;;
        aarch64 | arm64) echo aarch64 ;;
        *) echo "$m" ;;
    esac
}

# host_arm_gdb: print the first gdb that can disassemble ARM (gdb-multiarch, then gdb).
# Debian/Ubuntu's plain `gdb` is native-only; Arch's gdb handles ARM. Returns 1 if none.
host_arm_gdb() {
    local g
    for g in gdb-multiarch gdb; do
        if host_have "$g" && "$g" -batch -ex 'set architecture arm' >/dev/null 2>&1; then
            echo "$g"
            return 0
        fi
    done
    return 1
}

# host_pkg_install_hint PKG...: print the install command for the detected OS.
# Package names are passed through unchanged. Prints a generic hint on unknown.
host_pkg_install_hint() {
    case $(host_os) in
        arch) echo "sudo pacman -S --needed $*" ;;
        debian | ubuntu) echo "sudo apt-get install $*" ;;
        *) echo "install with your package manager: $*" ;;
    esac
}

# host_have CMD: true when CMD is on PATH.
host_have() {
    command -v "$1" >/dev/null 2>&1
}

# host_sha256 FILE: print only the hex digest.
host_sha256() {
    if host_have sha256sum; then
        sha256sum -- "$1" | cut -d' ' -f1
    elif host_have shasum; then
        shasum -a 256 -- "$1" | cut -d' ' -f1
    else
        echo "host_sha256: no sha256sum available" >&2
        return 127
    fi
}

# host_md5 FILE: print only the hex digest.
host_md5() {
    if host_have md5sum; then
        md5sum -- "$1" | cut -d' ' -f1
    else
        echo "host_md5: no md5sum available" >&2
        return 127
    fi
}

# host_is_tty: true when stdin and stderr are terminals (AKAM_FORCE_TTY=1 for tests).
host_is_tty() {
    [ "${AKAM_FORCE_TTY:-}" = 1 ] && return 0
    [ -t 0 ] && [ -t 2 ]
}

# host_confirm PROMPT: ask y/N on a terminal. Returns 0 only on "y"/"yes".
# Refuses (returns 2, message on stderr) without a TTY: never auto-confirms.
host_confirm() {
    local ans
    if ! host_is_tty; then
        echo "refusing to ask '$1' without a terminal" >&2
        return 2
    fi
    printf '%s [y/N] ' "$1" >&2
    IFS= read -r ans || return 1
    case $ans in y | Y | yes | YES | Yes) return 0 ;; *) return 1 ;; esac
}

# host_confirm_typed PROMPT EXPECTED: require the exact string EXPECTED (e.g. YES).
# Same TTY rule as host_confirm: returns 2 without a terminal.
host_confirm_typed() {
    local ans
    if ! host_is_tty; then
        echo "refusing to ask '$1' without a terminal" >&2
        return 2
    fi
    printf '%s (type %s to continue) ' "$1" "$2" >&2
    IFS= read -r ans || return 1
    [ "$ans" = "$2" ]
}

# host_color_ok: true when coloured output is allowed on stdout (TTY, no NO_COLOR, TERM not dumb).
host_color_ok() {
    [ -z "${NO_COLOR:-}" ] && [ -t 1 ] && [ "${TERM:-dumb}" != dumb ]
}

# host_say STYLE TEXT...: print TEXT on stdout; STYLE is red|green|yellow|bold|plain.
# Escapes are emitted only when host_color_ok, so logs and pipes stay clean.
host_say() {
    local style=$1 code=""
    shift
    if host_color_ok; then
        case $style in
            red) code=31 ;;
            green) code=32 ;;
            yellow) code=33 ;;
            bold) code=1 ;;
        esac
    fi
    if [ -n "$code" ]; then
        printf '\033[%sm%s\033[0m\n' "$code" "$*"
    else
        printf '%s\n' "$*"
    fi
}
