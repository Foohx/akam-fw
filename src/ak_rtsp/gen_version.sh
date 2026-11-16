#!/bin/sh
# gen_version.sh -- called by the Makefile before compiling version.c.
# Increments build_number.txt and writes version_info.h.

BUILD_NUM_FILE=build_number.txt

if [ "${AKAM_REPRO:-0}" = 1 ]; then
    # Reproducible build (make repro): every stamp comes from the environment, nothing from the clock, the
    # counter file or the git state, so two builds of the same sources give the same bytes.
    num=0
    git=${AKAM_REPRO_GIT:-repro}
    ts=$(date -u -d "@${SOURCE_DATE_EPOCH:-0}" '+%Y-%m-%d %H:%M:%S')
else
    [ -f "$BUILD_NUM_FILE" ] || echo 0 > "$BUILD_NUM_FILE"
    num=$(( $(cat "$BUILD_NUM_FILE") + 1 ))
    echo "$num" > "$BUILD_NUM_FILE"

    git=$(git describe --always --dirty=-dirty 2>/dev/null) || git=unknown
    [ -n "$git" ] || git=unknown

    ts=$(date '+%Y-%m-%d %H:%M:%S')
fi

cat > version_info.h <<HDR
#pragma once
#define BUILD_NUMBER $num
#define BUILD_TIMESTAMP "$ts"
#define GIT_HASH "$git"
HDR

echo "[mk] build #$num  git:$git  $ts"
