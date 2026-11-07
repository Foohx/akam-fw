#!/bin/sh
# gen_version.sh -- called by the Makefile before compiling version.c.
# Increments build_number.txt and writes version_info.h.

BUILD_NUM_FILE=build_number.txt

[ -f "$BUILD_NUM_FILE" ] || echo 0 > "$BUILD_NUM_FILE"
num=$(( $(cat "$BUILD_NUM_FILE") + 1 ))
echo "$num" > "$BUILD_NUM_FILE"

git=$(git describe --always --dirty=-dirty 2>/dev/null) || git=unknown
[ -n "$git" ] || git=unknown

ts=$(date '+%Y-%m-%d %H:%M:%S')

cat > version_info.h <<HDR
#pragma once
#define BUILD_NUMBER $num
#define BUILD_TIMESTAMP "$ts"
#define GIT_HASH "$git"
HDR

echo "[mk] build #$num  git:$git  $ts"
