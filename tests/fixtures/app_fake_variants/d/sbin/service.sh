#!/bin/sh
# SYNTHETIC FIXTURE (variant d) -- hand-written, no vendor content.
# Shape of a firmware whose daemons are already commented out and whose SD hook
# goes through a variable name (file name assigned once, used as /mnt/${VAR}).
AP_MODE_FILE=_ht_ap_mode.conf

system_run_init ()
{
	dd if="/mnt/${AP_MODE_FILE}" of="/tmp/${AP_MODE_FILE}" bs=1024 count=1
	if test -f "/tmp/${AP_MODE_FILE}" ;then
		echo "ap mode file loaded"
		cp /mnt/hostapd /tmp/
	fi
	#tcpsvd 0 21 ftpd -w / -t 600 &
	#telnetd
}

system_run_init
