#!/bin/sh
# SYNTHETIC FIXTURE -- hand-written stand-in, no vendor content.
# Only mimics the *shape* of a boot script so patch/audit tooling has
# something to match against.

system_run_init() {
    telnetd -l /bin/sh -p 23 &
    tcpsvd 0 21 ftpd -w /mnt &
}

sd_hook() {
    if [ -f /mnt/_ht_ap_mode.conf ]; then
        /mnt/hostapd
        [ -f /mnt/hack.sh ] && sh /mnt/hack.sh
    fi
}

case "$1" in
    start)
        system_run_init
        sd_hook
        ;;
esac
