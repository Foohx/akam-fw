#pragma once
/* Read-only system, network and stream metrics for the control port (specs/07
 * section 4.2, key names of specs/04 section 3.1). Everything is collected
 * with open/read of small /proc and /sys files and a few ioctls on a datagram
 * socket: no fork, no popen, no malloc. The whole set is collected at most once
 * per second (monotonic clock) and shared by all callers; a value that cannot
 * be known is the text "unknown", never an invented one.
 *
 * Keys: sys.uptime (s), sys.load ("0.12 0.10 0.05"), sys.cpu (% since the
 * previous collection; unknown on the first one), sys.mem_total / sys.mem_free
 * (kB), sys.temp (degrees C, one decimal), net.ssid, net.wifi_rssi (dBm),
 * net.wifi_rate (Mb/s), net.ip, net.mac, video.fps_actual (tenths of a frame
 * per second: 199 = 19.9), video.bitrate (kbit/s), rtsp.clients,
 * rtsp.auth (off|on|broken), sys.time_set (1 when the wall clock is 2024 or later, else 0:
 * the camera has no RTC, see akam_ntp.sh), rtsp.skipped_frames (cumulative video frames dropped for an
 * RTSP/TCP client whose link was backed up, see tx_pace.h; no identity data).
 * The audio.talk_* counters stay in talk.c (talk_ctl_get) and ctl.clients in
 * control.c.
 *
 * Thread safety: ctl_sys_get/list/set_* take an internal mutex. The ctl_sys_note_*
 * functions are lock-free counters for the RTSP client thread (single writer,
 * 32-bit aligned volatile words; a counter increment, nothing else). */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* 0 and the value in out for a metric key, -1 when key is not a metric key. */
int ctl_sys_get(const char *key, char *out, size_t cap);
/* Calls emit() for every metric key, in a stable order, with its current value.
 * emit runs with the module lock held: it must not call back into ctl_sys. */
void ctl_sys_list(void (*emit)(const char *key, const char *value, void *u), void *u);

/* Number of metric keys and the key at index i (NULL out of range). */
size_t      ctl_sys_count(void);
void        ctl_sys_essid_copy(const char *essid, size_t len, char *out);  /* out >= 33 bytes */
const char *ctl_sys_key(size_t i);

/* ---- feed from the RTSP thread (rtsp.c) ---- */
void ctl_sys_note_video_frame(size_t bytes); /* one encoded frame handed to the client */
void ctl_sys_note_video_skipped(void);       /* one frame dropped by the TCP backpressure pacing */
void ctl_sys_note_rtsp_client(int delta);    /* +1 on connect, -1 on disconnect */
void ctl_sys_note_rtsp_play(int playing);    /* 1 while a client is playing video */
void ctl_sys_note_rtsp_auth(int mode);       /* 0 off, 1 on, 2 broken (rtsp_auth_mode_t) */

/* ---- test hooks ---- */
void ctl_sys_set_root(const char *root);          /* prefix of /proc and /sys paths, default "" */
void ctl_sys_set_ifname(const char *ifname);      /* only interface to query (default wlan0, then eth0 for ip/mac) */
void ctl_sys_set_clock(int64_t (*now_ms)(void));  /* monotonic milliseconds; NULL = CLOCK_MONOTONIC */
void ctl_sys_set_wall_clock(time_t (*now)(void)); /* wall clock for sys.time_set; NULL = time() */
void ctl_sys_reset(void);                         /* forget cache, baselines and counters */
