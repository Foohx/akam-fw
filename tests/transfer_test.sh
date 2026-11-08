#!/usr/bin/env bash
# Tests for tools/wizard/lib/transfer.sh against the fake camera: PC->camera and camera->PC
# transfers over bash /dev/tcp and BusyBox nc, size+hash verification, a corrupted transfer is
# detected and retried, a persistent corruption fails (nothing kept), dry-run, manual mode.
# Nothing leaves localhost.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tests/fixtures/fake_camera_env.sh
. "$ROOT/tests/fixtures/fake_camera_env.sh"
fc_require_busybox
# shellcheck source=tools/lib/host.sh
. "$ROOT/tools/lib/host.sh"
# shellcheck source=tools/wizard/lib/log.sh
. "$ROOT/tools/wizard/lib/log.sh"
# shellcheck source=tools/wizard/lib/ui.sh
. "$ROOT/tools/wizard/lib/ui.sh"
# shellcheck source=tools/wizard/lib/camsh.sh
. "$ROOT/tools/wizard/lib/camsh.sh"
# shellcheck source=tools/wizard/lib/transfer.sh
. "$ROOT/tools/wizard/lib/transfer.sh"

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "PASS  $1"; }
ko() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }
check() { # name, command...
    local n=$1
    shift
    if "$@"; then ok "$n"; else ko "$n"; fi
}
T=$(mktemp -d)
trap 'fc_stop; kill $(jobs -p) 2>/dev/null; rm -rf "$T"' EXIT
export NO_COLOR=1
export AKAM_XFER_CONNECT_TRIES=8
fc_start "$@" || { echo "cannot start the fake camera"; exit 1; }
head -c 300000 /dev/urandom >"$T/data.bin"

t_put_ok() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    xfer_put "$T/data.bin" /tmp/put_ok.bin >"$T/o" 2>&1 || exit 1
    cmp -s "$T/data.bin" "$FC_ROOT/tmp/put_ok.bin" && grep -q 'Verified on the camera' "$T/o"
)
t_put_corrupt_retried() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl corrupt_rx 1
    xfer_put "$T/data.bin" /tmp/put_retry.bin >"$T/o" 2>&1 || exit 1
    # first try detected as different, second try verified, final file identical
    grep -q 'received a different file' "$T/o" && grep -q 'try 2 of 3' "$T/o" &&
        grep -q 'Verified on the camera' "$T/o" && cmp -s "$T/data.bin" "$FC_ROOT/tmp/put_retry.bin"
)
t_put_always_corrupt_fails() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl corrupt_rx 10
    ! xfer_put "$T/data.bin" /tmp/put_bad.bin >"$T/o" 2>&1 &&
        grep -q 'failed after 3 tries' "$T/o" && ! grep -q 'Verified on the camera' "$T/o"
)
t_put_missing_source() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    ! xfer_put "$T/nope.bin" /tmp/x.bin >/dev/null 2>&1
)
t_get_ok() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    xfer_get /dev/mtdblock4 "$T/k.bin" >"$T/o" 2>&1 || exit 1
    cmp -s "$T/k.bin" "$FC_ROOT/dev/mtdblock4" && [ ! -e "$T/k.bin.part" ] && grep -q 'Received and verified' "$T/o"
)
t_get_known_size_hash() (
    local h
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    h=$(host_sha256 "$FC_ROOT/dev/mtdblock3")
    xfer_get /dev/mtdblock3 "$T/d.bin" 65536 "$h" >"$T/o" 2>&1 || exit 1
    cmp -s "$T/d.bin" "$FC_ROOT/dev/mtdblock3"
)
t_get_corrupt_retried() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl corrupt_tx 1
    xfer_get /dev/mtdblock5 "$T/r.bin" >"$T/o" 2>&1 || exit 1
    grep -q 'differs' "$T/o" && grep -q 'try 2 of 3' "$T/o" && cmp -s "$T/r.bin" "$FC_ROOT/dev/mtdblock5"
)
t_get_always_corrupt_fails() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    fc_ctl corrupt_tx 10
    ! xfer_get /dev/mtdblock2 "$T/bad.bin" >"$T/o" 2>&1 &&
        [ ! -e "$T/bad.bin" ] && [ ! -e "$T/bad.bin.part" ] && grep -q 'failed after 3 tries' "$T/o"
)
t_get_bad_remote_path() (
    camsh_open 127.0.0.1 "$FC_PORT" || exit 1
    ! xfer_get "/tmp/it's" "$T/q.bin" >/dev/null 2>&1
)
t_dry_run() (
    local before after
    before=$(fc_cmds | wc -l)
    UI_DRY=1
    xfer_put "$T/data.bin" /tmp/dry.bin >"$T/o" 2>&1
    xfer_get /dev/mtdblock1 "$T/dry.bin" >>"$T/o" 2>&1
    after=$(fc_cmds | wc -l)
    [ "$before" = "$after" ] && [ ! -e "$T/dry.bin" ] && grep -c WOULD "$T/o" | grep -q 2
)
t_put_manual_mode() (
    # Manual mode: the "user" runs the nc command in their own window (a listener started here),
    # pastes the camera outputs, and the file is still verified. Nothing is sent to the fake camera.
    local h before after
    before=$(fc_cmds | wc -l)
    "$AKAM_BUSYBOX" nc -l -p "$AKAM_PORT_XFER_IN" >"$T/manual_rx" </dev/null &
    sleep 0.5
    h=$(host_sha256 "$T/data.bin")
    camsh_manual_on
    CAMSH_HOST=127.0.0.1
    xfer_put "$T/data.bin" /tmp/manual.bin <<EOF >"$T/o" 2>&1
sha256sum
END
y

END
y
300000
$h  /tmp/manual.bin
END
y
EOF
    after=$(fc_cmds | wc -l)
    cmp -s "$T/data.bin" "$T/manual_rx" && [ "$before" = "$after" ] && grep -q 'Verified on the camera' "$T/o"
)

for t in t_put_ok t_put_corrupt_retried t_put_always_corrupt_fails t_put_missing_source t_get_ok \
    t_get_known_size_hash t_get_corrupt_retried t_get_always_corrupt_fails t_get_bad_remote_path \
    t_dry_run t_put_manual_mode; do
    check "$t" "$t"
done

echo "transfer_test: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
