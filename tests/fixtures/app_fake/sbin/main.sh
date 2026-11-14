#!/bin/sh
# SYNTHETIC FIXTURE -- hand-written stand-in, no vendor content.
syslogd -n -O /var/log/messages -s 200 -b 3 &
klogd -n & # -c prio
/tmp/service.sh start
