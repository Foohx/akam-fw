#!/bin/sh
# source_hash.sh -- sha256 of the sources of akam2k_rtsp: app/*.c, app/*.h, app/Makefile and the SHARED 1080p-tree
# files it compiles unmodified (src/common, the SHARED_AK list of the Makefile, keep both lists in step; night.h of src/ak_rtsp).
# Used by `make poc3-dist` (BUILDINFO source_sha256) and by build_card.sh (refuses a stale binary). Host only.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
# `source_hash.sh sntp`: hash of the akam2k_sntp sources (../sntp) only; no argument: the akam2k_rtsp sources.
if [ "${1:-}" = sntp ]; then
    cd "$here/../sntp"
    for f in $(printf '%s\n' ./*.c ./*.h | LC_ALL=C sort); do cat "$f"; done | sha256sum | cut -d' ' -f1
    exit 0
fi
# `source_hash.sh audio`: hash of the akam2k_mic sources (../audio) only.
if [ "${1:-}" = audio ]; then
    cd "$here/../audio"
    for f in $(printf '%s\n' ./*.c ./*.h | LC_ALL=C sort); do cat "$f"; done | sha256sum | cut -d' ' -f1
    exit 0
fi
# `source_hash.sh led`: hash of the akam2k_led sources (the LED_SRC files of the Makefile and their headers, in this order).
if [ "${1:-}" = led ]; then
    cd "$here"
    for f in akam2k_led.c light_logic.c light_logic.h hal_light.h hal_ptz.h hal_light_sysfs.c akam2k_hal.c akam2k_hal.h akam2k_pin.c akam2k_pin.h; do cat "$f"; done | sha256sum | cut -d' ' -f1
    exit 0
fi
ak=$(cd "$here/../../../src/ak_rtsp" && pwd)
common=$(cd "$here/../../../src/common" && pwd)
{
    cd "$here"
    for f in $(printf '%s\n' ./*.c ./*.h Makefile | LC_ALL=C sort); do cat "$f"; done
    cat ../ptz/akam_ptz_safe.h # the safe-motion layer that hal_ptz.c includes
    cd "$common"
    for n in h264_params rtsp_sdp rtsp_auth rtsp_in rtsp_idle rtp_clock tx_pace net_tx md5 mqtt mqtt_ha night_logic cfg_file; do
        for e in c h; do
            for d in net logic; do [ ! -f "$d/$n.$e" ] || cat "$d/$n.$e"; done
        done
    done
    cd "$ak"
    cat night.h # the header night_logic.h includes (its source night.c is NOT compiled by the 2K app)
} | sha256sum | cut -d' ' -f1
