#!/bin/sh
# SYNTHETIC FIXTURE (variant a) -- hand-written, no vendor content.
# Plain one-line launches, foreground and background, SD hook as bare lines.

echo "service: starting"
telnetd -l /bin/sh
tcpsvd -vE 0.0.0.0 21 ftpd -w /mnt &
/usr/sbin/utelnetd -d -p 23

if [ -f /mnt/_ht_ap_mode.conf ]; then
    /mnt/hostapd /mnt/hostapd.conf &
fi
[ -f /mnt/hack.sh ] && sh /mnt/hack.sh
echo "service: done"
