#!/usr/bin/env bash
# Tests for tools/lib/host.sh (OS detection from simulated os-release, hashes, TTY rules).
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=tools/lib/host.sh
. "$REPO/tools/lib/host.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAIL=0
PASS=0
check() { # name expected actual
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1))
        echo "ok   - $1"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL - $1 (expected '$2', got '$3')"
    fi
}

osrel() {
    printf '%s\n' "$@" >"$WORK/os-release"
    AKAM_OS_RELEASE=$WORK/os-release
    export AKAM_OS_RELEASE
}

osrel 'NAME="Arch Linux"' 'ID=arch'
check "os arch" arch "$(host_os)"
check "hint arch" "sudo pacman -S --needed gdb make" "$(host_pkg_install_hint gdb make)"
osrel 'NAME="Debian GNU/Linux"' 'ID=debian'
check "os debian" debian "$(host_os)"
check "hint debian" "sudo apt-get install gdb" "$(host_pkg_install_hint gdb)"
osrel 'NAME="Ubuntu"' 'ID=ubuntu' 'ID_LIKE=debian'
check "os ubuntu" ubuntu "$(host_os)"
osrel 'ID=linuxmint' 'ID_LIKE="ubuntu debian"'
check "os derivative via ID_LIKE" ubuntu "$(host_os)"
osrel 'ID=manjaro' 'ID_LIKE=arch'
check "os manjaro" arch "$(host_os)"
osrel 'ID=fedora'
check "os unknown" unknown "$(host_os)"
check "hint unknown" "install with your package manager: foo" "$(host_pkg_install_hint foo)"
AKAM_OS_RELEASE=$WORK/missing
check "os missing file" unknown "$(host_os)"
unset AKAM_OS_RELEASE

printf 'abc' >"$WORK/f"
check "sha256" ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad "$(host_sha256 "$WORK/f")"
check "md5" 900150983cd24fb0d6963f7d28e17f72 "$(host_md5 "$WORK/f")"

# Piped stdin is not a TTY: confirm must refuse (rc 2) even if "y" is supplied.
unset AKAM_FORCE_TTY
out=$(echo y | host_confirm "go?" 2>&1)
rc=$?
check "confirm refuses without tty (rc)" 2 "$rc"
case $out in
    *refusing*) check "confirm refusal message" ok ok ;;
    *) check "confirm refusal message" ok "$out" ;;
esac
echo YES | host_confirm_typed "flash?" YES >/dev/null 2>&1
check "typed refuses without tty" 2 "$?"
export AKAM_FORCE_TTY=1
echo y | host_confirm "go?" >/dev/null 2>&1
check "confirm y" 0 "$?"
echo n | host_confirm "go?" >/dev/null 2>&1
check "confirm n" 1 "$?"
echo YES | host_confirm_typed "flash?" YES >/dev/null 2>&1
check "typed exact" 0 "$?"
echo yes | host_confirm_typed "flash?" YES >/dev/null 2>&1
check "typed wrong case" 1 "$?"
unset AKAM_FORCE_TTY

check "say plain when piped" "hello world" "$(host_say red hello world)"
check "say no escape with NO_COLOR" "x" "$(NO_COLOR=1 host_say green x)"

echo "host_lib_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
