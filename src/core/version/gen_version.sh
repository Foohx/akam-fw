#!/bin/sh
# gen_version.sh -- called by the Makefile before compiling version.c.
# Increments the build counter and writes version_info.h. Where: AKAM_VERSION_HEADER (default ./version_info.h) and
# AKAM_BUILD_NUMBER_FILE (default ./build_number.txt); the platform Makefiles point both below build/<model-id>.

BUILD_NUM_FILE=${AKAM_BUILD_NUMBER_FILE:-build_number.txt}
VERSION_HEADER=${AKAM_VERSION_HEADER:-version_info.h}

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

cat > "$VERSION_HEADER" <<HDR
#pragma once
#define BUILD_NUMBER $num
#define BUILD_TIMESTAMP "$ts"
#define GIT_HASH "$git"
HDR

echo "[mk] build #$num  git:$git  $ts"
