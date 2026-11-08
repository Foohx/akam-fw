#!/usr/bin/env bash
# Check that the host tools needed by Akam are installed, and print the exact command that
# installs the missing ones for this distribution. NEVER runs sudo or installs anything.
#
#   tools/build/check_host.sh [--profile kit|build|all] [--json] [--quiet]
#
# Profiles (each one includes the previous):
#   kit    tools to use a prebuilt kit: curl, tar, xz, squashfs-tools, ssh, SD-card tools
#   build  + tools to build from sources: make, git, bzip2, file, binutils, an ARM-capable gdb
#   all    + shellcheck (default; what tools/ci/local_check.sh needs)
# Distributions: Arch (pacman), Debian and Ubuntu (apt-get). Others: tools are still checked,
# the install hint is generic.
#
# --json prints one JSON object on stdout (for the wizard):
#   {"os","arch","profile","ok","missing_required":[..],"missing_optional":[..],
#    "install_required":"<cmd or empty>","install_optional":"<cmd or empty>",
#    "tools":[{"name","level","profile","found","package"}]}
# Exit: 0 all required tools present, 1 some missing, 2 usage error.
# Test hooks: AKAM_OS_RELEASE (see tools/lib/host.sh), AKAM_HOST_ARCH, AKAM_SBIN_DIRS
# (directories searched in addition to PATH, default "/usr/sbin /sbin").
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/lib/host.sh
. "$here/../lib/host.sh"

profile=all
json=0
quiet=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --json) json=1 ;;
        --quiet) quiet=1 ;;
        --profile)
            [ "$#" -ge 2 ] || { echo "check_host: --profile needs a value" >&2; exit 2; }
            profile="$2"
            shift
            ;;
        -h | --help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; exit 0 ;;
        *) echo "check_host: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
case "$profile" in
    kit) maxrank=1 ;;
    build) maxrank=2 ;;
    all) maxrank=3 ;;
    *) echo "check_host: unknown profile: $profile (kit|build|all)" >&2; exit 2 ;;
esac

os="$(host_os)"
arch="$(host_arch)"

# name|profile|level|arch package|debian/ubuntu package
# sbin tools (util-linux, dosfstools, parted) live in /usr/sbin, which is not in a normal
# user's PATH on Debian: they are looked up there too (the scripts run them through sudo).
tools_table='
curl|kit|req|curl|curl
tar|kit|req|tar|tar
xz|kit|req|xz|xz-utils
sha256sum|kit|req|coreutils|coreutils
mksquashfs|kit|req|squashfs-tools|squashfs-tools
unsquashfs|kit|req|squashfs-tools|squashfs-tools
ssh|kit|req|openssh|openssh-client
ssh-keygen|kit|req|openssh|openssh-client
lsblk|kit|req|util-linux|util-linux
wipefs|kit|req|util-linux|util-linux
sfdisk|kit|req|util-linux|util-linux
blockdev|kit|req|util-linux|util-linux
mkfs.vfat|kit|req|dosfstools|dosfstools
partprobe|kit|req|parted|parted
python3|kit|opt|python|python3
telnet|kit|opt|inetutils|telnet
make|build|req|make|make
git|build|req|git|git
bzip2|build|req|bzip2|bzip2
file|build|req|file|file
readelf|build|req|binutils|binutils
gdb|build|req|gdb|gdb-multiarch
cmake|build|opt|cmake|cmake
shellcheck|all|req|shellcheck|shellcheck
'

have_tool() {
    case "$1" in
        gdb) host_arm_gdb >/dev/null ;; # a gdb that can disassemble ARM (Debian: gdb-multiarch)
        *)
            local d
            host_have "$1" && return 0
            for d in ${AKAM_SBIN_DIRS-/usr/sbin /sbin}; do
                [ -x "$d/$1" ] && return 0
            done
            return 1
            ;;
    esac
}

pkg_for() { # arch_pkg deb_pkg
    case "$os" in
        arch) echo "$1" ;;
        debian | ubuntu) echo "$2" ;;
        *) echo "$2" ;;
    esac
}

add_unique() { # list item -> list (space separated)
    case " $1 " in *" $2 "*) echo "$1" ;; *) echo "${1:+$1 }$2" ;; esac
}

install_cmd() { # packages...
    [ "$#" -gt 0 ] || return 0
    case "$os" in
        arch) echo "sudo pacman -S --needed $*" ;;
        debian | ubuntu) echo "sudo apt-get install -y $*" ;;
        *) echo "install with your package manager: $*" ;;
    esac
}

missing_req=""
missing_opt=""
pkgs_req=""
pkgs_opt=""
json_tools=""
human=""
while IFS='|' read -r name prof level apkg dpkg; do
    [ -n "$name" ] || continue
    case "$prof" in kit) rank=1 ;; build) rank=2 ;; *) rank=3 ;; esac
    [ "$rank" -le "$maxrank" ] || continue
    pkg="$(pkg_for "$apkg" "$dpkg")"
    if have_tool "$name"; then
        found=true
        human="$human$(printf 'ok       %s\n' "$name")"$'\n'
    else
        found=false
        if [ "$level" = req ]; then
            missing_req="$(add_unique "$missing_req" "$name")"
            pkgs_req="$(add_unique "$pkgs_req" "$pkg")"
            human="$human$(printf 'MISSING  %s (package: %s)\n' "$name" "$pkg")"$'\n'
        else
            missing_opt="$(add_unique "$missing_opt" "$name")"
            pkgs_opt="$(add_unique "$pkgs_opt" "$pkg")"
            human="$human$(printf 'optional %s not found (package: %s)\n' "$name" "$pkg")"$'\n'
        fi
    fi
    lvl=required
    [ "$level" = req ] || lvl=optional
    json_tools="$json_tools${json_tools:+,}{\"name\":\"$name\",\"level\":\"$lvl\",\"profile\":\"$prof\",\"found\":$found,\"package\":\"$pkg\"}"
done <<EOF
$tools_table
EOF

# shellcheck disable=SC2086  # package lists are space-separated words on purpose
cmd_req="$(install_cmd $pkgs_req)"
# shellcheck disable=SC2086
cmd_opt="$(install_cmd $pkgs_opt)"

json_list() { # words... -> ["a","b"]
    local out="" w
    for w in "$@"; do out="$out${out:+,}\"$w\""; done
    printf '[%s]' "$out"
}

ok=true
[ -z "$missing_req" ] || ok=false

if [ "$json" = 1 ]; then
    # shellcheck disable=SC2086
    printf '{"os":"%s","arch":"%s","profile":"%s","ok":%s,"missing_required":%s,"missing_optional":%s,"install_required":"%s","install_optional":"%s","tools":[%s]}\n' \
        "$os" "$arch" "$profile" "$ok" "$(json_list $missing_req)" "$(json_list $missing_opt)" \
        "$cmd_req" "$cmd_opt" "$json_tools"
else
    if [ "$quiet" = 0 ]; then
        echo "host: $os ($arch), profile: $profile"
        printf '%s' "$human"
    fi
    if [ -n "$missing_req" ]; then
        echo >&2
        echo "check_host: missing required tools: $missing_req" >&2
        echo "Install them with:  $cmd_req" >&2
    else
        echo "check_host: all required tools present"
    fi
    if [ -n "$missing_opt" ]; then
        echo "check_host: optional tools not found: $missing_opt" >&2
        echo "Optional install:   $cmd_opt" >&2
    fi
    if [ "$os" = unknown ]; then
        echo "check_host: unsupported distribution (supported: Arch, Debian, Ubuntu); package names above are Debian's" >&2
    fi
    if [ -n "$missing_req" ] && printf '%s' "$missing_req" | grep -qw gdb; then
        echo "check_host: gdb is a hard requirement for the time64 gate (check_time32.sh); on Debian/Ubuntu it must be gdb-multiarch" >&2
    fi
fi
[ "$ok" = true ]
