#!/bin/sh
# Synthetic stand-in for the stock sbin/drop_caches.sh (same loop shape, written for the test fixture).
WPA_FILE="/tmp/wpa_supplicant"
while true
do
	sleep 5
	sync && echo 3 > /proc/sys/vm/drop_caches
	if [ -f $WPA_FILE ]; then
		rm /tmp/wpa_supplicant
	fi
done
