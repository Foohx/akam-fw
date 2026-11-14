#!/bin/sh
# SYNTHETIC FIXTURE (variant b) -- hand-written, no vendor content.
# Launches as the only command of a function and of an if body (the disabled
# line must keep the syntax valid), one-line if, SD hook inside a function.

start_net_services() {
    telnetd -l /bin/sh -p 23 &
}

start_ftp() {
    if [ -f /etc/config/ftp_enable ]; then
        tcpsvd 0 21 ftpd -w /mnt &
    fi
}

start_ftp_oneliner() {
    if [ -x /usr/sbin/bftpd ]; then bftpd -d; fi
}

run_sd_hook() {
    if [ -f /mnt/_ht_ap_mode.conf ]
    then
        sh /mnt/hack.sh
    fi
}

case "$1" in
    start)
        start_net_services
        start_ftp
        start_ftp_oneliner
        run_sd_hook
        ;;
    *)
        echo "usage: $0 start"
        ;;
esac
