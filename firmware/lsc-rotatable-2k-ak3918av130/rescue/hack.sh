#!/bin/sh
# hack.sh -- rescue card for the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130).
# Run by YYOS /appfs/script/init.sh when the card's yy_debug.ini has mode=debug and
# start_sh=/mnt/sdcard/hack.sh.
# Goal: known root password + telnet, keep the camera running, optional WAN block.
# Writes NOTHING to flash. Backups of the flash are a deliberate, documented step (see README.md),
# never done automatically at boot.
#
# Paths are prefixed by $AKAM_ROOT (empty on the camera) so the host-side test can run the script in
# a fake root; $AKAM_SD, $AKAM_IP likewise.
ROOT=${AKAM_ROOT:-}
SD=${AKAM_SD:-$ROOT/mnt/sdcard}
IP=${AKAM_IP:-$ROOT/sbin/ip}
LOG=$SD/hack_log.txt
echo "$(date) - hack.sh start" >"$LOG"

# 1) Known root password (root/root). The $1$ hash below is the hash of "root".
#    (/etc/shadow is on a read-only rootfs -> bind-mount a tmpfs copy over it; RAM only.)
mkdir -p "$ROOT/tmp/etc"
# shellcheck disable=SC2016  # literal crypt hash, nothing to expand
echo 'root:$1$hack$26ZSPRIJi4E4euNXpW7mr1:10933:0:99999:7:::' >"$ROOT/tmp/etc/shadow"
mount --bind "$ROOT/tmp/etc/shadow" "$ROOT/etc/shadow"
echo "$(date) - shadow patched (root/root)" >>"$LOG"

# 2) Telnet (uses the static busybox on the card).
if "$SD/busybox" telnetd -l /bin/sh -p 23 2>/dev/null; then
    echo "$(date) - telnetd on :23" >>"$LOG"
fi

# 3) Hand off to the normal camera boot (required: in debug mode init runs us, not start.sh).
"$ROOT/appfs/script/start.sh" &

# 4) OPTIONAL local-only mode: block ALL WAN egress, keep LAN. Toggle with the flag file.
#    No iptables on the box, but ip + blackhole /1 routes survive DHCP renew and
#    override the default route while keeping the LAN /24. Reversible: rm the flag + reboot.
if [ -f "$SD/yy_local_only" ]; then
    (
        n=0
        while [ "$n" -lt "${AKAM_WAIT_TRIES:-60}" ]; do
            "$IP" addr show wlan0 2>/dev/null | grep -q "inet " && break
            sleep "${AKAM_WAIT_SLEEP:-1}"
            n=$((n + 1))
        done
        "$IP" route add blackhole 0.0.0.0/1 2>/dev/null
        "$IP" route add blackhole 128.0.0.0/1 2>/dev/null
        echo "$(date) - local-only: WAN blackholed" >>"$LOG"
    ) &
fi

echo "$(date) - hack.sh done (UART/telnet login: root/root)" >>"$LOG"
