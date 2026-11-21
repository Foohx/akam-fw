#!/bin/sh
# restore_param.sh -- undo the opt-in param.conf edit of enable_local.sh: put the original /config/param.conf
# (copied to the card at akam/backup/param.conf.orig before the first edit) back.
#
# Usage (as root, over SSH or telnet):  sh /mnt/sdcard/akam/restore_param.sh   then reboot.
# Safe: the copy is built next to the live file and moved into place (atomic), then synced. It is the only
# other script, besides enable_local.sh, that writes to the config partition (jffs2); it never touches firmware.
# Also remove the card flag akam/enable_param_conf, or the next boot edits the file again.
#
# Paths prefixed by $AKAM_ROOT (see common.sh); $AKAM_PARAM_CONF overrides the param.conf path.

HERE=${0%/*}
[ -r "$HERE/common.sh" ] || HERE=${AKAM_SD:-${AKAM_ROOT:-}/mnt/sdcard}/akam
# shellcheck disable=SC1091  # the helper lives on the card at run time
. "$HERE/common.sh"

PARAM=${AKAM_PARAM_CONF:-$ROOT/config/param.conf}
BACKUP=$AK/backup/param.conf.orig

if [ ! -s "$BACKUP" ]; then
    log restore "no backup at $BACKUP: nothing restored"
    echo "restore_param: no backup at $BACKUP" >&2
    exit 1
fi
if cp "$BACKUP" "$PARAM.akam.new" && same "$BACKUP" "$PARAM.akam.new" && sync && mv -f "$PARAM.akam.new" "$PARAM" && sync; then
    log restore "param.conf restored from $BACKUP (reboot to apply; remove flag akam/enable_param_conf)"
    echo "restore_param: restored $PARAM; remove akam/enable_param_conf from the card and reboot"
    exit 0
fi
rm -f "$PARAM.akam.new"
log restore "WARNING: restore failed, $PARAM left as it was"
echo "restore_param: restore failed" >&2
exit 1
