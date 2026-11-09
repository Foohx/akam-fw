# shellcheck shell=bash
# Shared set-up for the camera tests (camsh_test.sh, transfer_test.sh, wizard_camera_test.sh):
# starts tools/wizard/fake_camera.sh on a free localhost port. Source it after defining ROOT.
#
#   fc_require_busybox      prints "SKIP: ..." and exits 0 when the pinned BusyBox is missing
#   fc_start [flash-card IMAGE FLASH_TOOL]
#                           init + start; sets FC_ROOT, FC_PORT, AKAM_PORT_TELNET, AKAM_PORT_XFER_IN/OUT
#   fc_stop                 stop the server
#   fc_cleanup              stop it and remove every tree made by fc_start
#   fc_ctl NAME N           write a fault-injection counter (drop, close, corrupt_rx, corrupt_tx)
#   fc_cmds                 print the lines the camera received

FC_ROOT=""
FC_PORT=""
FC_DIRS=""

fc_require_busybox() {
    local bb=${AKAM_BUSYBOX:-$ROOT/tools/.toolchain/busybox-1.35.0-x86_64/busybox}
    if [ ! -x "$bb" ]; then
        echo "SKIP: the pinned BusyBox is required (run tools/build/bootstrap.sh)"
        exit 0
    fi
    export AKAM_BUSYBOX=$bb
}

# fc_free_port: a localhost TCP port nobody listens on. Drawn below the kernel's ephemeral range
# (32768-60999): in that range a port can be taken by the source side of any local connection (the
# wizard's /dev/tcp retries) between the probe and the moment a fake nc binds it.
fc_free_port() {
    local p i=0
    while [ "$i" -lt 50 ]; do
        p=$((12000 + RANDOM % 20000))
        if ! (exec 9<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
            echo "$p"
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

fc_start() {
    FC_ROOT=$(mktemp -d)/camera
    FC_DIRS="$FC_DIRS $(dirname "$FC_ROOT")"
    "$ROOT/tools/wizard/fake_camera.sh" init "$FC_ROOT" "$@" >/dev/null || return 1
    FC_PORT=$(fc_free_port)
    AKAM_PORT_XFER_IN=$(fc_free_port)
    AKAM_PORT_XFER_OUT=$(fc_free_port)
    # the three ports must be pairwise different (each draw is independent)
    while [ "$AKAM_PORT_XFER_IN" = "$FC_PORT" ]; do AKAM_PORT_XFER_IN=$(fc_free_port); done
    while [ "$AKAM_PORT_XFER_OUT" = "$FC_PORT" ] || [ "$AKAM_PORT_XFER_OUT" = "$AKAM_PORT_XFER_IN" ]; do AKAM_PORT_XFER_OUT=$(fc_free_port); done
    export AKAM_PORT_XFER_IN AKAM_PORT_XFER_OUT
    AKAM_PORT_TELNET=$FC_PORT
    export AKAM_PORT_TELNET
    "$ROOT/tools/wizard/fake_camera.sh" start "$FC_ROOT" "$FC_PORT"
}

fc_stop() {
    if [ -n "$FC_ROOT" ]; then "$ROOT/tools/wizard/fake_camera.sh" stop "$FC_ROOT"; fi
}

# fc_cleanup: stop the server and remove every fake camera tree made by fc_start.
fc_cleanup() {
    local d
    fc_stop
    for d in $FC_DIRS; do
        chmod -R u+rwX "$d" 2>/dev/null || true
        rm -rf "${d:?}"
    done
}

fc_ctl() { echo "$2" >"$FC_ROOT/ctl/$1"; }
fc_cmds() { cat "$FC_ROOT/ctl/commands.log"; }
