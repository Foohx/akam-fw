# shellcheck shell=bash
# Source this file (". tools/build/env.sh") to get the pinned Zig toolchain.
# Exports: ZIG, CAMYN_TARGET, CAMYN_MCPU, CAMYN_CC, CAMYN_BUSYBOX
# (if installed). Does not download anything;
# run tools/build/bootstrap.sh first.

_camyn_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_camyn_root="$(cd "$_camyn_env_dir/../.." && pwd)"
# shellcheck source=tools/build/deps.lock
. "$_camyn_env_dir/deps.lock"

ZIG="$_camyn_root/tools/.toolchain/zig-$ZIG_VERSION-x86_64-linux/zig"
if [ ! -x "$ZIG" ]; then
    echo "env.sh: pinned Zig not found at $ZIG; run tools/build/bootstrap.sh first" >&2
    unset _camyn_env_dir _camyn_root
    return 1
fi
export ZIG
export CAMYN_TARGET=arm-linux-musleabi
export CAMYN_MCPU=arm926ej_s
export CAMYN_CC="$ZIG cc -target $CAMYN_TARGET -mcpu=$CAMYN_MCPU"
# Make "zig" resolvable too: the ak_rtsp Makefile default CC is "zig cc ...".
PATH="$(dirname "$ZIG"):$PATH"
export PATH
# Pinned static x86_64 BusyBox for host-side ash tests (see bootstrap.sh). Only
# exported if present; never put on PATH or installed system-wide.
if [ -x "$_camyn_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox" ]; then
    export CAMYN_BUSYBOX="$_camyn_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox"
else
    unset CAMYN_BUSYBOX
fi
unset _camyn_env_dir _camyn_root
