# shellcheck shell=bash
# Source this file (". tools/build/env.sh") to get the pinned Zig toolchain.
# Exports: ZIG, AKAM_TARGET, AKAM_MCPU, AKAM_CC, AKAM_BUSYBOX
# (if installed). Does not download anything;
# run tools/build/bootstrap.sh first.

_akam_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_akam_root="$(cd "$_akam_env_dir/../.." && pwd)"
# shellcheck source=tools/build/deps.lock
. "$_akam_env_dir/deps.lock"

ZIG="$_akam_root/tools/.toolchain/zig-$ZIG_VERSION-x86_64-linux/zig"
if [ ! -x "$ZIG" ]; then
    echo "env.sh: pinned Zig not found at $ZIG; run tools/build/bootstrap.sh first" >&2
    unset _akam_env_dir _akam_root
    return 1
fi
export ZIG
export AKAM_TARGET=arm-linux-musleabi
export AKAM_MCPU=arm926ej_s
export AKAM_CC="$ZIG cc -target $AKAM_TARGET -mcpu=$AKAM_MCPU"
# Make "zig" resolvable too: the ak_rtsp Makefile default CC is "zig cc ...".
PATH="$(dirname "$ZIG"):$PATH"
export PATH
# Pinned static x86_64 BusyBox for host-side ash tests (see bootstrap.sh). Only
# exported if present; never put on PATH or installed system-wide.
if [ -x "$_akam_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox" ]; then
    export AKAM_BUSYBOX="$_akam_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox"
else
    unset AKAM_BUSYBOX
fi
unset _akam_env_dir _akam_root
