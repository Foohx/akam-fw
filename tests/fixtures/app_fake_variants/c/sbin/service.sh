#!/bin/sh
# SYNTHETIC FIXTURE (variant c) -- hand-written, no vendor content.
# Launch through a variable, vsftpd, an unrelated mention in a comment, and a
# SD hook gated on a different condition than _ht_ap_mode.conf.

# telnetd is started below (this comment must stay untouched)
TELNET_BIN=/usr/sbin/telnetd
FTP_BIN=vsftpd

system_run_init() {
    echo "init"
    $TELNET_BIN -p 24 -l /bin/sh &
    $FTP_BIN /etc/vsftpd.conf &
}

system_run_init
if [ -d /mnt/custom ]; then
    sh /mnt/hack.sh
fi
