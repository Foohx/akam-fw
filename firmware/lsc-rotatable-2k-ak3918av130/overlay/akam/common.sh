#!/bin/sh
# common.sh -- shared helpers of the Akam overlay card for the LSC Rotatable 2K
# (lsc-rotatable-2k-ak3918av130). SOURCED by akam_boot.sh and the scripts next to it; never run alone.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) so the host-side tests can run every script in a
# fake root. Variables a caller may set before sourcing:
#   AKAM_ROOT   prefix of every absolute path (default: empty)
#   AKAM_SD     where the card is mounted (default: $AKAM_ROOT/mnt/sdcard)
#   AKAM_LOG    log file (default: <card>/akam/akam_boot.log)
# Provides: ROOT, SD, AK (the card's akam/ directory), TMP, LOG, log(), flag(), same().
# Everything here is best effort: a failing log write must never stop the camera from booting.

ROOT=${AKAM_ROOT:-}
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
AK=$SD/akam
# shellcheck disable=SC2034  # used by the scripts that source this file
TMP=${AKAM_TMP_DIR:-$ROOT/tmp}
LOG=${AKAM_LOG:-$AK/akam_boot.log}
LOG_MAX_BYTES=262144
LOG_KEEP_LINES=400

# log <tag> <message...>: one line on the card, trimmed in place when the file gets big (FAT, no rotation).
log() {
    _tag=$1
    shift
    echo "$(date '+%Y-%m-%d %H:%M:%S') $_tag: $*" >>"$LOG" 2>/dev/null
    if [ "$(wc -c <"$LOG" 2>/dev/null)" -gt "$LOG_MAX_BYTES" ] 2>/dev/null; then
        tail -n "$LOG_KEEP_LINES" "$LOG" >"$LOG.tmp" 2>/dev/null && cat "$LOG.tmp" >"$LOG" 2>/dev/null
        rm -f "$LOG.tmp"
    fi
    return 0
}

# flag <name>: true when the empty file akam/<name> exists on the card (the card's on/off switches).
flag() {
    [ -e "$AK/$1" ]
}

# same <file> <file>: true when both files have the same content. cmp when the camera's busybox has it, else
# md5sum, else size only (a weaker check, still fails on a short copy).
same() {
    if command -v cmp >/dev/null 2>&1; then
        cmp -s "$1" "$2"
    elif command -v md5sum >/dev/null 2>&1; then
        [ "$(md5sum <"$1")" = "$(md5sum <"$2")" ]
    else
        [ "$(wc -c <"$1")" = "$(wc -c <"$2")" ]
    fi
}
