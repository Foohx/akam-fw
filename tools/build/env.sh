# shellcheck shell=bash
# Source this file (". tools/build/env.sh") to get the pinned Zig toolchain.
# Exports whatever is installed: ZIG, AKAM_TARGET, AKAM_MCPU, AKAM_CC (needs Zig) and
# AKAM_BUSYBOX (needs the pinned BusyBox, absent = silently unset). Missing Zig is reported
# on stderr and makes the file's status 1, AFTER everything that exists has been exported.
# Does not download anything; run tools/build/bootstrap.sh first.

_akam_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_akam_root="$(cd "$_akam_env_dir/../.." && pwd)"
# shellcheck source=tools/build/deps.lock
. "$_akam_env_dir/deps.lock"
# shellcheck source=tools/lib/host.sh
. "$_akam_root/tools/lib/host.sh"

_akam_rc=0
ZIG="$_akam_root/tools/.toolchain/zig-$ZIG_VERSION-$(host_arch)-linux/zig"
if [ -x "$ZIG" ]; then
    export ZIG
    export AKAM_TARGET=arm-linux-musleabi
    export AKAM_MCPU=arm926ej_s
    export AKAM_CC="$ZIG cc -target $AKAM_TARGET -mcpu=$AKAM_MCPU"
    # Make "zig" resolvable too: the ak_rtsp Makefile default CC is "zig cc ...".
    PATH="$(dirname "$ZIG"):$PATH"
    export PATH
else
    echo "env.sh: pinned Zig not found at $ZIG; run tools/build/bootstrap.sh first" >&2
    unset ZIG AKAM_TARGET AKAM_MCPU AKAM_CC
    _akam_rc=1
fi
# Pinned static x86_64 BusyBox for host-side ash tests (see bootstrap.sh). Only
# exported if present; never put on PATH or installed system-wide.
if [ -x "$_akam_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox" ]; then
    export AKAM_BUSYBOX="$_akam_root/tools/.toolchain/busybox-$BUSYBOX_VERSION-x86_64/busybox"
else
    unset AKAM_BUSYBOX
fi
unset _akam_env_dir _akam_root
# The status of a sourced file is that of its last command.
if [ "$_akam_rc" = 0 ]; then
    unset _akam_rc
    true
else
    unset _akam_rc
    false
fi
