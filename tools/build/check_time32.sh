#!/usr/bin/env bash
# Check that a static ARM binary never issues a time64 system call that the
# camera's kernel acknowledges without executing (see src/common/time32_shim.c
# and docs/akam_v1_kernel_time64.md).
#
#   tools/build/check_time32.sh [--require-263] <elf>...
#
# The .text section is disassembled with gdb and every `svc` is matched with
# the constant loaded into r7 just before it (mov/orr, which is how musl and
# the shim build syscall numbers). It fails if one of the numbers that musl
# issues UNCONDITIONALLY (whatever the value) is found:
#   403 clock_gettime64   405 clock_adjtime64    408 timer_gettime64
#   409 timer_settime64   410 timerfd_gettime64  411 timerfd_settime64
#   417 recvmmsg_time64
# Other time64 numbers (clock_nanosleep, ppoll, pselect6, futex, utimensat,
# clock_settime, ...) are issued by musl only for values that do not fit in 32
# bits, so their presence in the libc code is harmless and is not reported.
# --require-263 additionally demands at least one clock_gettime32 (263), to
# prove the shim is the code that answers (use it for binaries that read a clock).
#
# gdb is a HARD requirement: an objdump/llvm-objdump fallback was evaluated and rejected
# (Arch and Debian binutils objdump are native-only, no ARM; llvm-objdump is not packaged
# the same way everywhere and its verdict could not be validated against gdb on the reference
# binaries). It must be an ARM-capable gdb: `gdb` on Arch, `gdb-multiarch` on Debian/Ubuntu
# (plain Debian gdb cannot disassemble ARM). tools/build/check_host.sh reports it.
#
# Exit: 0 ok, 1 forbidden syscall found / requirement missed, 2 usage or
# missing tool (gdb, readelf, awk).
set -euo pipefail

FORBIDDEN="403 405 408 409 410 411 417"
require263=0
if [ "${1:-}" = "--require-263" ]; then
    require263=1
    shift
fi
[ "$#" -ge 1 ] || { echo "usage: $0 [--require-263] <elf>..." >&2; exit 2; }
# shellcheck source=tools/lib/host.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/host.sh"
for t in readelf awk; do
    command -v "$t" >/dev/null 2>&1 || { echo "check_time32: '$t' not found" >&2; exit 2; }
done
GDB="$(host_arm_gdb)" || {
    echo "check_time32: no ARM-capable gdb found: $(host_pkg_install_hint gdb-multiarch) (Debian/Ubuntu) or gdb (Arch)" >&2
    exit 2
}

# svc_numbers <elf>: lines "<number> <count>" ("?" when r7 is not a visible constant).
svc_numbers() {
    local elf=$1 start size
    read -r start size < <(readelf -SW "$elf" | awk '{
        for (i = 1; i <= NF; i++) if ($i == ".text") { print "0x" $(i + 2), "0x" $(i + 4); exit } }')
    [ -n "${start:-}" ] || return 1
    "$GDB" -batch -ex "disassemble $start,$((start + size))" "$elf" 2>/dev/null | awk '
        function reset(   k) { for (k in cand) delete cand[k]; ncand = 0 }
        {
            line = $0
            sub(/^[^\t]*\t/, "", line)
            split(line, f, /[ \t,]+/)
            op = f[1]
            # A conditional form (movne r7, #N) only ADDS a candidate number: it
            # may or may not reach the svc, and every candidate is counted.
            if (op ~ /^mov(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?$/ && f[2] == "r7" && f[3] ~ /^#[0-9]+$/) {
                if (op == "mov") reset()
                cand[substr(f[3], 2) + 0] = 1; ncand++
                next
            }
            if (op ~ /^orr(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?$/ && f[2] == "r7" && f[3] == "r7" && f[4] ~ /^#[0-9]+$/ && ncand) {
                bits = substr(f[4], 2) + 0
                n = 0
                for (k in cand) vals[n++] = k + 0
                if (op == "orr") reset()
                for (i = 0; i < n; i++) { cand[or(vals[i], bits)] = 1; ncand++ }
                next
            }
            if (op == "svc") {
                if (ncand) { for (k in cand) cnt[k]++ } else cnt["?"]++
                reset()
                next
            }
            if (f[2] == "r7" && op !~ /^(cmp|cmn|tst|teq|str|strb|push)/) reset()
            if (op ~ /^(pop|ldm)/ && line ~ /r7/) reset()
        }
        END { for (n in cnt) print n, cnt[n] }'
}

rc=0
for elf in "$@"; do
    if [ ! -f "$elf" ]; then
        echo "check_time32: $elf: no such file" >&2
        rc=1
        continue
    fi
    out="$(svc_numbers "$elf")" || out=""
    if [ -z "$out" ]; then
        echo "check_time32: $elf: no svc found (not an ARM ELF, or the scan is broken)" >&2
        rc=1
        continue
    fi
    bad=""
    for n in $FORBIDDEN; do
        if printf '%s\n' "$out" | awk -v n="$n" '$1 == n { f = 1 } END { exit !f }'; then
            bad="$bad $n"
        fi
    done
    if [ -n "$bad" ]; then
        echo "check_time32: $elf: FORBIDDEN time64 syscall(s):$bad" >&2
        rc=1
        continue
    fi
    if [ "$require263" = 1 ] && ! printf '%s\n' "$out" | awk '$1 == 263 { f = 1 } END { exit !f }'; then
        echo "check_time32: $elf: no clock_gettime32 (263) found, the shim is not in" >&2
        rc=1
        continue
    fi
    echo "check_time32: $elf: ok ($(printf '%s\n' "$out" | awk '$1 != "?" { printf "%s ", $1 }')svc numbers seen)"
done
exit "$rc"
