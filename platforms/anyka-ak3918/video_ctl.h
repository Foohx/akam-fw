/* 8091 keys of the saved video settings (video_cfg.h): video.bitrate_cfg, video.fps_cfg, video.gop_cfg (the saved
 * value, not the measured video.bitrate / video.fps_actual of ctl_sys) and video.cfg_pending (1 when the saved
 * file differs from what the running encoder uses). Same contract as imgq_ctl_*: _get returns 0 and fills `out`
 * ("key=value") for a key it owns, else -1; _set returns -1 for a key it does not own, else 0 (done) or 1 (refused)
 * with the answer in `msg`; _list emits one line per key.
 *
 * A SET validates, then rewrites /etc/config/akam/video (all defaults = file removed). It takes effect at the next
 * start of ak_rtsp and the answer says so ("restart to apply"). Only a local (loopback) SET is saved: from the
 * network it is refused, because nothing would apply it. */
#pragma once
#include <stddef.h>

void video_ctl_set_persist(int on); /* control.c: 1 for a loopback SET (default 1), 0 for a network peer */
int  video_ctl_get(const char *key, char *out, size_t cap);
int  video_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void video_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
