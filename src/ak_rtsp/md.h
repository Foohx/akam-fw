#pragma once
#include <stddef.h>

/* On-camera motion detection (not in upstream akam-fw).
 *
 * A low-priority thread reads the luma of the 320x180 channel twice a second, straight from the buffers the
 * driver keeps writing (those frames cannot be dequeued, see vi.c), and runs md_logic on it. The result is
 * published on the control port so that a recorder can stay disconnected until something moves:
 *
 *   md.enabled      0 | 1                 saved in /etc/config/akam/md (loopback SET), default 1
 *   md.sensitivity  low | medium | high   saved likewise, default medium
 *   md.state        0 | 1                 1 from the start of motion until 5 s after the last moving sample
 *   md.events       count                 times motion started since ak_rtsp started; a poller that sees it
 *                                         grow knows it missed nothing between two polls
 *   md.score        hundredths of a percent of the picture that changed in the latest sample
 *   md.peak         highest md.score of the current or last motion
 *   md.age          seconds since the last moving sample, -1 before the first
 *   md.preroll      0..15                 seconds of lead-in video kept in RAM for a client that connects
 *                                         during or just after motion (preroll.h); saved, default 10,
 *                                         0 = off. A change of the buffer itself applies at the next start.
 *
 * Test aid: while the file /tmp/akam_md_preroll_always exists (RAM), every client gets the lead-in.
 *
 * Samples are skipped for a few seconds around a day/night switch, which changes the whole picture. */

void md_start(void);

/* For the RTSP thread: seconds of lead-in to keep (0 = none), and whether a client that starts playing now
 * should be sent it (motion is running or ended less than 15 s ago). */
int md_preroll_seconds(void);
int md_preroll_wanted(void);

/* Same contract as ledsys_ctl_*. */
int  md_ctl_get(const char *key, char *out, size_t cap);
void md_set_persist(int on);
int  md_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void md_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
