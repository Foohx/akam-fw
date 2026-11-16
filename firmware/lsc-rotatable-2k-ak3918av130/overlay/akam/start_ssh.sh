#!/bin/sh
# start_ssh.sh -- Dropbear, public-key-only root SSH on :22, for the LSC Rotatable 2K overlay card.
# Run in the background by akam_boot.sh. Never blocks the camera: every failure only logs and exits 0.
# Writes only to /tmp and the SD card.
#
# On the card (akam/ directory):
#   dropbearmulti                    static ARM multi-call Dropbear, built by tools/dropbear/build_dropbear.sh
#                                    (the build with the time32 shim: this kernel 4.4.302 does not run musl's
#                                    time64 clock_gettime syscall, see src/common/time32_shim.c)
#   authorized_keys                  the user's keys, copied there by build_card.sh (never from the repo)
#   dropbear_ed25519_host_key        generated on the first boot and kept on the card, so the host key
#                                    fingerprint stays stable across reboots (the card is the only place that
#                                    persists; do not share a card image publicly)
#
# Key policy as on the 1080p (tools/firmware_patch_templates/akam_ssh.sh): only plain "ssh-ed25519 <key>" lines,
# no options, no command=. Passwords are off: the Akam Dropbear build has no password auth compiled in (the -s option does not exist there).
# Dropbear refuses an authorized_keys file when any directory on the way up to root's HOME is group/other
# writable; "/" and /etc come from the read-only squashfs rootfs and are probably 0775, so as on the 1080p
# root's HOME is made a private tmpfs directory (private passwd copy bind-mounted over /etc/passwd; the real
# file is untouched) holding a copy of authorized_keys.
#
# UNVERIFIED (PLAN section 9): that the stock rootfs passwd/shells/permissions need these workarounds on this
# model (they were measured on the 1080p); the log lists the modes found.
#
# Paths prefixed by $AKAM_ROOT (see common.sh); $AKAM_SSH_PORT overrides the port (default 22).

HERE=${0%/*}
[ -r "$HERE/common.sh" ] || HERE=${AKAM_SD:-${AKAM_ROOT:-}/mnt/sdcard}/akam
# shellcheck disable=SC1091  # the helper lives on the card at run time
. "$HERE/common.sh"

BIN=$AK/dropbearmulti
CARD_KEYS=$AK/authorized_keys
CARD_HOSTKEY=$AK/dropbear_ed25519_host_key
WORK=$TMP/akam_ssh
HOMEDIR=$TMP/akam_home
PORT=${AKAM_SSH_PORT:-22}
PIDFILE=$TMP/dropbear.pid
umask 077

slog() { log ssh "$*"; }

if [ ! -f "$BIN" ]; then
    slog "no $BIN on the card: SSH not started"
    exit 0
fi
mkdir -p "$WORK" "$HOMEDIR" 2>/dev/null || {
    slog "cannot create $WORK or $HOMEDIR: SSH not started"
    exit 0
}
chmod 700 "$HOMEDIR" 2>/dev/null

# --- authorized_keys: validate the card's file into root's private HOME --------------------------------------
if [ ! -f "$CARD_KEYS" ]; then
    slog "no akam/authorized_keys on the card: SSH not started (key-only, nobody could log in)"
    exit 0
fi
NEWKEYS=$HOMEDIR/.authorized_keys.new
: >"$NEWKEYS" || exit 0
n=0
bad=0
cr=$(printf '\r')
while IFS= read -r line || [ -n "$line" ]; do
    while [ "${line%"$cr"}" != "$line" ]; do line=${line%"$cr"}; done
    case $line in
        '' | '#'*) continue ;;
        *command=*)
            bad=$((bad + 1))
            continue
            ;;
    esac
    if printf '%s\n' "$line" | grep -Eq '^ssh-ed25519 AAAAC3NzaC1lZDI1NTE5[A-Za-z0-9+/]{43,}={0,2}( [^[:cntrl:]]*)?$'; then
        printf '%s\n' "$line" >>"$NEWKEYS"
        n=$((n + 1))
    else
        bad=$((bad + 1))
    fi
done <"$CARD_KEYS"
slog "card keys: $n accepted, $bad rejected"
if [ "$n" -eq 0 ]; then
    rm -f "$NEWKEYS"
    slog "no valid ssh-ed25519 key on the card: SSH not started"
    exit 0
fi
chmod 600 "$NEWKEYS"
mv -f "$NEWKEYS" "$HOMEDIR/authorized_keys"

# --- host key: persisted on the card ---------------------------------------------------------------------------
HOSTKEY=$CARD_HOSTKEY
if [ ! -s "$HOSTKEY" ]; then
    slog "generating the ed25519 host key (first boot, may wait for entropy)"
    rm -f "$WORK/hostkey.new"
    if "$BIN" dropbearkey -t ed25519 -f "$WORK/hostkey.new" >/dev/null 2>&1 && [ -s "$WORK/hostkey.new" ]; then
        if cp "$WORK/hostkey.new" "$CARD_HOSTKEY.new" 2>/dev/null && mv -f "$CARD_HOSTKEY.new" "$CARD_HOSTKEY" 2>/dev/null; then
            slog "host key stored on the card (stable fingerprint)"
        else
            rm -f "$CARD_HOSTKEY.new"
            HOSTKEY=$WORK/hostkey.new
            slog "WARNING: could not store the host key on the card: ephemeral key, the fingerprint changes at every boot"
        fi
    else
        slog "host key generation failed: SSH not started"
        exit 0
    fi
fi

# --- /etc/shells and root's HOME (workarounds measured on the 1080p, see the header) ---------------------------
for d in "$ROOT/" "$ROOT/etc"; do
    slog "mode of $d: $(stat -c '%a %u' "$d" 2>/dev/null || echo unknown)"
done
SHELLS=$ROOT/etc/shells
if [ -f "$SHELLS" ] && ! grep -qx /bin/sh "$SHELLS" 2>/dev/null; then
    if echo /bin/sh >"$WORK/shells" && chmod 644 "$WORK/shells" && mount --bind "$WORK/shells" "$SHELLS" 2>/dev/null; then
        slog "/etc/shells lacked /bin/sh, bind-mounted a private list"
    else
        slog "WARNING: /etc/shells lacks /bin/sh and the bind mount failed; root login may be refused"
    fi
fi
PW=$ROOT/etc/passwd
if ! grep -q "^root:[^:]*:[^:]*:[^:]*:[^:]*:$HOMEDIR:" "$PW" 2>/dev/null; then
    if grep -q '^root:' "$PW" 2>/dev/null; then
        sed "s|^\\(root:[^:]*:[^:]*:[^:]*:[^:]*:\\)[^:]*:|\\1$HOMEDIR:|" "$PW" >"$WORK/passwd"
    else
        { cat "$PW" 2>/dev/null; echo "root:x:0:0:root:$HOMEDIR:/bin/sh"; } >"$WORK/passwd"
    fi
    chmod 644 "$WORK/passwd"
    if [ -s "$WORK/passwd" ] && mount --bind "$WORK/passwd" "$PW" 2>/dev/null; then
        slog "root HOME is now $HOMEDIR (private passwd bind-mounted)"
    else
        slog "WARNING: could not set root HOME to $HOMEDIR; login may be refused if / or /etc is group-writable"
    fi
fi

# --- start ---------------------------------------------------------------------------------------------------------
if [ -s "$PIDFILE" ] && read -r pid <"$PIDFILE" && kill -0 "$pid" 2>/dev/null; then
    slog "dropbear already running (pid $pid)"
    exit 0
fi
"$BIN" dropbear -r "$HOSTKEY" -D "$HOMEDIR" -p "$PORT" -P "$PIDFILE" -T 3 -E >/dev/null 2>>"$LOG" </dev/null
slog "dropbear launcher returned status $? on port $PORT (daemon detached)"
exit 0
