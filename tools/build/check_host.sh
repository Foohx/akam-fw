#!/usr/bin/env bash
# Check that the host tools needed to build Camyn are installed.
# Prints the pacman command for missing ones (Arch). Never runs sudo.
set -euo pipefail

# tool:arch-package
tools=(
    make:make git:git curl:curl tar:tar xz:xz bzip2:bzip2
    mksquashfs:squashfs-tools unsquashfs:squashfs-tools
    shellcheck:shellcheck file:file readelf:binutils sha256sum:coreutils
)

missing=()
pkgs=()
for entry in "${tools[@]}"; do
    tool="${entry%%:*}"
    pkg="${entry##*:}"
    if command -v "$tool" >/dev/null 2>&1; then
        echo "ok       $tool"
    else
        echo "MISSING  $tool (package: $pkg)"
        missing+=("$tool")
        case " ${pkgs[*]:-} " in
            *" $pkg "*) ;;
            *) pkgs+=("$pkg") ;;
        esac
    fi
done

if [ "${#missing[@]}" -gt 0 ]; then
    echo >&2
    echo "check_host: missing tools: ${missing[*]}" >&2
    echo "Install them with:  sudo pacman -S --needed ${pkgs[*]}" >&2
    exit 1
fi
echo "check_host: all required tools present"
