# shellcheck shell=bash
# transfer: move files between the PC and the camera with bash /dev/tcp and the camera's BusyBox
# `nc -l`. The PC always connects OUT (its firewall blocks inbound connections); the camera
# listens. No netcat, python or telnet client is needed on the PC. Every transfer is verified
# (size + hash computed on both sides) and retried, 3 tries by default (spec 2.5).
#
#   xfer_put LOCAL REMOTE        PC -> camera   camera: nc -l -p 8765 > REMOTE     PC: cat > /dev/tcp
#   xfer_get REMOTE LOCAL [SIZE [HASH]]
#                                camera -> PC   camera: nc -l -p 8766 < REMOTE     PC: cat < /dev/tcp
#
# The camera has no `wc` (the stock rootfs lacks it): sizes come from `stat -c%s`. The hash is
# sha256sum when the camera has it, else md5sum (XFER_HT says which one).
# Needs camsh.sh (a session opened with camsh_open, or manual mode), ui.sh, host.sh.
# Test hooks: AKAM_PORT_XFER_IN (8765), AKAM_PORT_XFER_OUT (8766), AKAM_XFER_TRIES (3),
# AKAM_XFER_CONNECT_TRIES (20, one per second).

XFER_HT=""
XFER_REMOTE_SIZE=""
XFER_REMOTE_HASH=""

xfer_tries() { printf '%s\n' "${AKAM_XFER_TRIES:-3}"; }

# _xfer_q STR: single-quote STR for the camera shell (paths only; refuses a quote inside).
_xfer_q() {
    case $1 in
        *"'"* | *$'\n'*) return 1 ;;
    esac
    printf "'%s'" "$1"
}

# xfer_hash_tool: set XFER_HT to sha256sum or md5sum, whichever the camera has.
xfer_hash_tool() {
    [ -n "$XFER_HT" ] && return 0
    if camsh_dry; then XFER_HT=sha256sum; return 0; fi
    camsh_exec 'command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo md5sum' || return 1
    case $(camsh_line 1) in
        sha256sum) XFER_HT=sha256sum ;;
        md5sum) XFER_HT=md5sum ;;
        *) ui_err "cannot tell which hash tool the camera has (got: $(camsh_line 1))."; return 1 ;;
    esac
}

# xfer_local_hash FILE: hash of FILE with the same algorithm as the camera.
xfer_local_hash() {
    if [ "$XFER_HT" = md5sum ]; then host_md5 "$1"; else host_sha256 "$1"; fi
}

# xfer_remote_info REMOTE: set XFER_REMOTE_SIZE and XFER_REMOTE_HASH (read on the camera).
xfer_remote_info() {
    local q
    XFER_REMOTE_SIZE="" XFER_REMOTE_HASH=""
    q=$(_xfer_q "$1") || return 1
    xfer_hash_tool || return 1
    camsh_exec "stat -c%s $q; $XFER_HT $q" 600 || return 1
    [ "$CAMSH_RC" = 0 ] || return 1
    XFER_REMOTE_SIZE=$(camsh_line 1 | tr -d '[:space:]')
    XFER_REMOTE_HASH=$(camsh_line 2 | awk '{print $1}')
    case $XFER_REMOTE_SIZE in '' | *[!0-9]*) return 1 ;; esac
    [ -n "$XFER_REMOTE_HASH" ]
}

# _xfer_timeout SECS CMD...: run CMD, killed after SECS when `timeout` exists.
_xfer_timeout() {
    local s=$1
    shift
    if host_have timeout; then timeout "$s" "$@"; else "$@"; fi
}

# _xfer_send FILE HOST PORT: connect (retrying while the listener is not up yet), send FILE.
_xfer_send() {
    local f=$1 h=$2 p=$3 i=0 max=${AKAM_XFER_CONNECT_TRIES:-20} rc size secs
    size=$(wc -c <"$f" | tr -d ' ')
    secs=$((30 + size / 100000))
    while [ "$i" -lt "$max" ]; do
        rc=0
        (
            exec 8>"/dev/tcp/$h/$p" || exit 3
            _xfer_timeout "$secs" cat -- "$f" >&8 || exit 4
        ) 2>/dev/null || rc=$?
        case $rc in
            0) return 0 ;;
            4) return 1 ;; # connected, but the data did not go through
        esac
        i=$((i + 1))
        sleep 1
    done
    return 1
}

# _xfer_recv HOST PORT OUT: connect, read until the camera closes, write OUT.
_xfer_recv() {
    local h=$1 p=$2 out=$3 i=0 max=${AKAM_XFER_CONNECT_TRIES:-20} rc
    while [ "$i" -lt "$max" ]; do
        rc=0
        (
            exec 8<"/dev/tcp/$h/$p" || exit 3
            _xfer_timeout 600 cat <&8 >"$out" || exit 4
        ) 2>/dev/null || rc=$?
        case $rc in
            0) return 0 ;;
            4) return 1 ;;
        esac
        i=$((i + 1))
        sleep 1
    done
    return 1
}

# _xfer_session_ok: make sure a camera session exists (reopen after a failed try).
_xfer_session_ok() {
    [ "$CAMSH_MANUAL" = 1 ] && return 0
    [ "$CAMSH_OPEN" = 1 ] && return 0
    camsh_open "$CAMSH_HOST" "$CAMSH_PORT"
}

# _xfer_settle SECS: wait for the camera's nc to exit. It does as soon as the PC closes the
# connection (the receiving nc runs with "< /dev/null": with its stdin left on the telnet session,
# BusyBox 1.35 nc never exits). If a version still waits, Ctrl-C ends it: the data is already written.
_xfer_settle() {
    local rc=0
    camsh_finish "$1" || rc=$?
    if [ "$rc" -eq 1 ] && [ "$CAMSH_MANUAL" != 1 ]; then
        camsh_interrupt
        camsh_finish "$1" || true
    fi
    return 0
}

# _xfer_put_once SRC DST Q PORT SIZE HASH: one try. 0 only when the camera holds the same file.
_xfer_put_once() {
    local src=$1 dst=$2 q=$3 port=$4 size=$5 hash=$6
    _xfer_session_ok || { ui_warn "no camera session."; return 1; }
    xfer_hash_tool || return 1
    camsh_start "nc -l -p $port > $q < /dev/null" || return 1
    if [ "$CAMSH_MANUAL" = 1 ]; then
        camsh_manual_wait "Run the command above in your telnet window; it waits for the file." || return 1
    fi
    if ! _xfer_send "$src" "$CAMSH_HOST" "$port"; then
        ui_warn "could not send the file to the camera on port $port."
        camsh_close
        return 1
    fi
    _xfer_settle 15
    xfer_remote_info "$dst" || return 1
    if [ "$XFER_REMOTE_SIZE" != "$size" ] || [ "$XFER_REMOTE_HASH" != "$hash" ]; then
        ui_warn "the camera received a different file (size $XFER_REMOTE_SIZE, $XFER_HT $XFER_REMOTE_HASH; expected $size, $hash)."
        return 1
    fi
}

# xfer_put LOCAL REMOTE [TRIES]: send LOCAL to REMOTE on the camera and verify size and hash.
xfer_put() {
    local src=$1 dst=$2 tries=${3:-$(xfer_tries)} try=1 q size hash port
    port=${AKAM_PORT_XFER_IN:-8765}
    [ -f "$src" ] || { ui_err "transfer: no such file: $src"; return 1; }
    q=$(_xfer_q "$dst") || { ui_err "transfer: bad remote path"; return 1; }
    if camsh_dry; then
        ui_say "  WOULD send $src to the camera as $dst (camera: nc -l -p $port > $dst; PC: cat > /dev/tcp/<camera>/$port), then compare size and hash."
        return 0
    fi
    size=$(wc -c <"$src" | tr -d ' ')
    while [ "$try" -le "$tries" ]; do
        xfer_hash_tool || true
        hash=$(xfer_local_hash "$src" 2>/dev/null || true)
        ui_say "  Sending $(basename "$src") ($size bytes), try $try of $tries ..."
        if [ -n "$hash" ] && _xfer_put_once "$src" "$dst" "$q" "$port" "$size" "$hash"; then
            ui_good "  Verified on the camera: $dst ($size bytes, $XFER_HT $hash)."
            return 0
        fi
        try=$((try + 1))
    done
    ui_err "transfer to the camera failed after $tries tries: $dst"
    return 1
}

# _xfer_get_once SRC PART Q PORT SIZE HASH: one try; 0 when PART is a verified copy.
_xfer_get_once() {
    local part=$2 q=$3 port=$4 size=$5 hash=$6 got
    _xfer_session_ok || { ui_warn "no camera session."; return 1; }
    xfer_hash_tool || return 1
    rm -f "$part"
    camsh_start "nc -l -p $port < $q" || return 1
    if [ "$CAMSH_MANUAL" = 1 ]; then
        camsh_manual_wait "Run the command above in your telnet window; it waits for the PC." || return 1
    fi
    if ! _xfer_recv "$CAMSH_HOST" "$port" "$part"; then
        ui_warn "could not read the file from the camera on port $port."
        camsh_close
        return 1
    fi
    _xfer_settle 15
    got=$(wc -c <"$part" | tr -d ' ')
    if [ "$got" != "$size" ] || [ "$(xfer_local_hash "$part")" != "$hash" ]; then
        ui_warn "the received file differs (size $got, expected $size, or hash mismatch)."
        return 1
    fi
}

# xfer_get REMOTE LOCAL [SIZE [HASH]]: fetch REMOTE into LOCAL. SIZE and HASH (same algorithm as
# XFER_HT) are read on the camera when not given. LOCAL appears only when both match.
xfer_get() {
    local src=$1 dst=$2 size=${3:-} hash=${4:-} tries try=1 q port part
    tries=$(xfer_tries)
    port=${AKAM_PORT_XFER_OUT:-8766}
    q=$(_xfer_q "$src") || { ui_err "transfer: bad remote path"; return 1; }
    if camsh_dry; then
        ui_say "  WOULD fetch $src from the camera into $dst (camera: nc -l -p $port < $src; PC: cat < /dev/tcp/<camera>/$port), then compare size and hash."
        return 0
    fi
    if [ -z "$size" ] || [ -z "$hash" ]; then
        xfer_remote_info "$src" || { ui_err "cannot read the size and hash of $src on the camera."; return 1; }
        size=$XFER_REMOTE_SIZE
        hash=$XFER_REMOTE_HASH
    fi
    part="$dst.part"
    while [ "$try" -le "$tries" ]; do
        ui_say "  Fetching $(basename "$dst") ($size bytes), try $try of $tries ..."
        if _xfer_get_once "$src" "$part" "$q" "$port" "$size" "$hash"; then
            mv "$part" "$dst"
            ui_good "  Received and verified: $(basename "$dst") ($size bytes, $XFER_HT $hash)."
            return 0
        fi
        rm -f "$part"
        try=$((try + 1))
    done
    ui_err "transfer from the camera failed after $tries tries: $src"
    return 1
}
