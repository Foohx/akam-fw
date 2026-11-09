#!/bin/sh
# SYNTHETIC fixture (no vendor content): only mimics the shape of the stock
# DHCP start-up code that akam patches at build time.
get_ip_from_ap()
{
    DHCP=1
    echo "dhcp:$DHCP"
    if test $DHCP -eq 1 ; then
        udhcpc -i wlan0 -x hostname:IPCamera &
    else
        ifconfig wlan0 192.168.1.2 netmask 255.255.255.0
    fi
}
