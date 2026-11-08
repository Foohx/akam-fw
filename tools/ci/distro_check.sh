#!/usr/bin/env bash
# Reproduce the Debian 12 / Ubuntu 24.04 container test: install exactly what check_host.sh asks
# for, then bootstrap -> make -> build_dropbear -> check_time32 -> local_check.
# Usage: tools/ci/distro_check.sh [--engine docker|podman] [image ...]
#        default images: debian:12 ubuntu:24.04
# The committed state of HEAD is cloned into a temp dir and mounted read-only; the containers
# run with --rm and never use sudo. Needs network (apt, GitHub). Exit 0 only if every image
# ends with "LOCAL CHECK: OK".
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

engine=""
images=()
while [ $# -gt 0 ]; do
    case "$1" in
        --engine) engine=${2:-}; shift 2 ;;
        -h | --help) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) images+=("$1"); shift ;;
    esac
done
[ ${#images[@]} -gt 0 ] || images=(debian:12 ubuntu:24.04)
if [ -z "$engine" ]; then
    for e in docker podman; do
        if command -v "$e" >/dev/null 2>&1; then engine=$e; break; fi
    done
fi
{ [ -n "$engine" ] && command -v "$engine" >/dev/null 2>&1; } || { echo "distro_check: need docker or podman" >&2; exit 2; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
git clone -q "$REPO" "$TMP/src" || { echo "distro_check: clone failed" >&2; exit 2; }

cat >"$TMP/run.sh" <<'INNER'
#!/bin/bash
# Runs inside a fresh container.
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq python3 git >/dev/null 2>&1   # only to parse JSON / git metadata
cp -a /src /work && cd /work || exit 1
git config --global --add safe.directory /work
echo "== check_host (before)"
tools/build/check_host.sh --profile all --json >/tmp/h.json
python3 -c 'import json;d=json.load(open("/tmp/h.json"));print(d["os"],d["arch"],"missing:",d["missing_required"])'
CMD=$(python3 -c 'import json;print(json.load(open("/tmp/h.json"))["install_required"])' | sed 's/^sudo //')
OPT=$(python3 -c 'import json;print(json.load(open("/tmp/h.json"))["install_optional"])' | sed 's/^sudo //')
echo "== install: $CMD"
[ -n "$CMD" ] && { eval "$CMD -qq" >/tmp/apt.log 2>&1 || { echo "INSTALL FAILED"; tail -5 /tmp/apt.log; }; }
[ -n "$OPT" ] && { eval "$OPT -qq" >/tmp/apt2.log 2>&1 || { echo "OPTIONAL INSTALL FAILED"; tail -5 /tmp/apt2.log; }; }
echo "== check_host (after)"; tools/build/check_host.sh --profile all | tail -3
echo "== bootstrap"; tools/build/bootstrap.sh >/tmp/bs.log 2>&1 && echo OK || { echo FAIL; tail -5 /tmp/bs.log; }
. tools/build/env.sh || true
echo "== make"
(cd src/ak_rtsp && make >/tmp/make.log 2>&1 && echo "OK warnings=$(grep -c 'warning:' /tmp/make.log)" || { echo FAIL; tail -5 /tmp/make.log; })
echo "== dropbear"; tools/dropbear/build_dropbear.sh >/tmp/db.log 2>&1 && echo OK || { echo FAIL; tail -5 /tmp/db.log; }
echo "== time32 gate"
tools/build/check_time32.sh --require-263 src/ak_rtsp/ak_rtsp dist/dropbear/dropbearmulti 2>&1 | sed 's/(.*//'
echo "== local_check"; tools/ci/local_check.sh >/tmp/lc.log 2>&1; grep -E 'FAIL|SKIP' /tmp/lc.log | head -10; tail -1 /tmp/lc.log
INNER

rc=0
for img in "${images[@]}"; do
    echo "######## $img ($engine)"
    out=$("$engine" run --rm -v "$TMP/src:/src:ro" -v "$TMP/run.sh:/run.sh:ro" "$img" bash /run.sh 2>&1)
    printf '%s\n' "$out"
    if printf '%s\n' "$out" | tail -1 | grep -q '^LOCAL CHECK: OK'; then
        echo "distro_check: $img OK"
    else
        echo "distro_check: $img FAILED" >&2
        rc=1
    fi
done
exit $rc
