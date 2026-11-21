#!/usr/bin/env bash
# Pull the tracker recording of the 2K (track_debug=2: /tmp/akam2k_track.raw, plus the decision log /tmp/akam2k_track.log) over ssh into
# <repo>/.local/logs/track-sessions/<timestamp>/ (not versioned). READ ONLY on the camera: two `cat`, nothing is written there.
# Usage: tools/dev/track_fetch.sh [--dry-run] [--host IP] [--key FILE] [--known-hosts FILE] [--out DIR]
#   --dry-run   print the ssh commands and the destination, touch neither the camera nor the disk
# Defaults: root@$CAM (the camera address: env CAM, else the first line of <repo>/.local/secrets/rot2k_ip; never written in a tracked file), key ~/.ssh/akam, known hosts <repo>/.local/secrets/known_hosts_rot2k. Score the result with tools/dev/track_score replay <dir>/akam2k_track.raw.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LOCAL=${AKAM_LOCAL_DIR:-$REPO/.local}
if [ ! -d "$LOCAL/secrets" ] && MAIN=$(git -C "$REPO" rev-parse --git-common-dir 2>/dev/null) && [ -d "$MAIN/../.local/secrets" ]; then
    LOCAL=$(cd "$MAIN/../.local" && pwd)   # a git worktree: the git-ignored .local of the main checkout
fi
HOST=${CAM:-} KEY=$HOME/.ssh/akam KH=$LOCAL/secrets/known_hosts_rot2k OUT="" DRY=0
while [ $# -gt 0 ]; do
    case $1 in
        --dry-run) DRY=1 ;;
        --host) HOST=${2:?}; shift ;;
        --key) KEY=${2:?}; shift ;;
        --known-hosts) KH=${2:?}; shift ;;
        --out) OUT=${2:?}; shift ;;
        -h|--help) sed -n 2,7p "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$HOST" ] || { [ -r "$LOCAL/secrets/rot2k_ip" ] && HOST=$(head -1 "$LOCAL/secrets/rot2k_ip"); }
if [ -z "$HOST" ]; then
    if [ "$DRY" = 1 ]; then HOST="<camera-ip>"; else echo "no camera address: set CAM or put it in $LOCAL/secrets/rot2k_ip" >&2; exit 2; fi
fi
TS=$(date +%Y%m%d_%H%M%S)
[ -n "$OUT" ] || OUT=$LOCAL/logs/track-sessions/$TS
SSH=(ssh -i "$KEY" -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8 -o UserKnownHostsFile="$KH" -o StrictHostKeyChecking=yes "root@$HOST")
if [ "$DRY" = 1 ]; then
    echo "dry-run: destination $OUT"
    for f in akam2k_track.raw akam2k_track.log; do
        echo "dry-run: ${SSH[*]} cat /tmp/$f > $OUT/$f"
    done
    exit 0
fi
[ -r "$KEY" ] || { echo "no ssh key $KEY" >&2; exit 2; }
[ -r "$KH" ] || { echo "no known_hosts file $KH" >&2; exit 2; }
mkdir -p "$OUT"
for f in akam2k_track.raw akam2k_track.log; do
    "${SSH[@]}" "cat /tmp/$f" > "$OUT/$f" || echo "warning: /tmp/$f not readable (track_debug=2 off, or nothing recorded yet)" >&2
done
echo "$OUT"
wc -c "$OUT"/akam2k_track.* | sed 's/^/  /'
