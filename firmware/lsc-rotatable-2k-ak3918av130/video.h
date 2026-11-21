/* Video encoder settings of the Rotatable 2K own app, per stream (0 = main 2304x1296, 1 = sub 640x360): target and peak bit rate, frame rate, QP
 * range, GOP and rate-control mode. Pure data and rules (host unit tested, tests/rot2k_video_unit.c) plus the live copy the Home Assistant
 * `number` / `select` entities edit and the capture thread applies.
 *
 * Sources, lowest to highest priority: the built-in defaults (unchanged: 15 fps, VBR, QP 32..51 main / 32..50 sub, target from main_kbps /
 * sub_kbps, peak 1.25 x target, GOP from `gop`), the akam2k.conf keys video<N>_kbps, video<N>_max_kbps, video<N>_fps, video<N>_min_qp,
 * video<N>_max_qp, video<N>_gop, video<N>_rc, then the runtime file /config/akam/video written by the Home Assistant entities. The runtime file
 * lists ONLY the keys that differ from the conf-derived base (a value set back to the base disappears from it), is stored atomically
 * (akam2k_store) and is ignored as a whole when invalid. An invalid conf value is refused by the supervisor script or by the app and the default stays.
 *
 * APPLYING A CHANGE: the capture thread first tries the live setters (vlive.h: set_fps, set_qp, get/set_attr, set_rc_type; conf
 * video_live=0 skips this); when any is refused, or the change is one only a re-open makes (avbr), every change re-opens THAT channel's encoder
 * (the vendor open_ex is the only complete configuration; close + open_ex + IDR) in its own capture
 * thread: the other stream and the JPEG snapshot encoder are not touched, the RTSP clients stay connected and see a gap of about 1 to 2 s ending on
 * a keyframe. Several changes made in a burst become one re-open. A re-open that fails puts the previous settings back (and re-opens with them). */
#ifndef AKAM2K_VIDEO_H
#define AKAM2K_VIDEO_H
#include <stddef.h>

#define A2K_VID_STREAMS 2

enum { A2K_VP_KBPS, A2K_VP_MAXKBPS, A2K_VP_FPS, A2K_VP_MINQP, A2K_VP_MAXQP, A2K_VP_GOP, A2K_VP_RC, A2K_VP_N };
/* The values of A2K_VP_RC are the vendor rc_mode numbers (abi.h). */
enum { A2K_RC_CBR = 0, A2K_RC_VBR = 1, A2K_RC_AVBR = 4 };
#define A2K_VID_FPS_MAX 15 /* the VI channels run at 15 fps */

typedef struct {
    int v[A2K_VP_N]; /* A2K_VP_MAXKBPS 0 = automatic: 1.25 x the target */
} a2k_vid_t;

/* Range of parameter p of stream chn: 0 and *lo, *hi filled, -1 for a bad index. A2K_VP_MAXKBPS also accepts 0 (automatic). */
int a2k_vid_range(int chn, int p, int *lo, int *hi);
/* 1 when val is acceptable for parameter p of stream chn (range, and for rc one of the three modes). */
int a2k_vid_value_ok(int chn, int p, int val);
/* 1 when the whole set is valid: every value in range, min QP <= max QP, effective peak >= target. */
int a2k_vid_valid(int chn, const a2k_vid_t *t);
/* The peak bit rate in force (explicit, or 1.25 x target when automatic), capped at 65535 (the vendor field is 16 bit). */
int a2k_vid_eff_max_kbps(const a2k_vid_t *t);
/* The built-in defaults of stream chn for the given target and GOP. */
void a2k_vid_defaults(int chn, int kbps, int gop, a2k_vid_t *out);
/* Strict text of a value: digits only (at most 5), or for rc vbr | cbr | avbr; no sign, no space. 0 and *out, or -1. */
int a2k_vid_parse(int p, const char *text, int *out);
const char *a2k_vid_rc_name(int rc); /* vbr, cbr or avbr; a question mark for anything else */
/* Text of a value (the number, or the rc word) into out: 0, or -1 when it does not fit. */
int a2k_vid_text(int p, int v, char *out, size_t cap);
/* "video0_kbps" ... into out. The key of parameter p of stream chn. */
void a2k_vid_key(int chn, int p, char *out, size_t cap);
/* Parses the runtime file text (lines key=value, LF, only video<N>_<param> keys, at most one line per key): applies them on top of base[] into out[]
 * and checks both sets. 0, or -1 (nothing usable: out untouched). */
int a2k_vid_parse_file(const char *text, size_t len, const a2k_vid_t base[A2K_VID_STREAMS], a2k_vid_t out[A2K_VID_STREAMS]);
/* The runtime file text of cur[] against base[]: the differing keys only. Returns the length, -1 when it does not fit. */
int a2k_vid_format_file(const a2k_vid_t base[A2K_VID_STREAMS], const a2k_vid_t cur[A2K_VID_STREAMS], char *out, size_t cap);

/* ---- the live copy (video.c; the rules above are in video_cfg.c, which the MQTT module also links) ---- */
/* base = defaults + conf of both streams (must be valid); store_path = the runtime file (NULL = RAM only). Returns 1 when the runtime file was used,
 * else 0. Call before the capture threads. */
int a2k_vid_init(const a2k_vid_t base[A2K_VID_STREAMS], const char *store_path);
void a2k_vid_get(int chn, a2k_vid_t *out);
/* Text of parameter p of stream chn as Home Assistant shows it (a number, or the rc word). 0 or -1. */
int a2k_vid_get_text(int chn, int p, char *out, size_t cap);
/* Sets one parameter from its text: validated together with the others (an inconsistent set is refused whole). 0 applied (the capture thread
 * re-opens that encoder; also 0 when the value is already in force), -1 refused (nothing changed). Stores the runtime file. */
int a2k_vid_set(int chn, int p, const char *text);
/* Capture thread of stream chn: 1 and *want filled when the settings in force differ from what the encoder runs with. */
int a2k_vid_pending(int chn, a2k_vid_t *want);
/* The capture thread's verdict on the re-open: ok = the encoder now runs with the wanted values; not ok = it did not, the previous values are put
 * back (and stored). ran = the values the encoder was opened with (the *want of a2k_vid_pending): a change made meanwhile stays pending. */
void a2k_vid_applied(int chn, int ok, const a2k_vid_t *ran);
/* An encoder could not be opened with the runtime values at boot: forget the runtime file and return to the base. 1 when there was anything to forget. */
int a2k_vid_reset_runtime(void);
/* Called (any thread, cheap) after a value was put back, so the MQTT module can publish the real values. */
void a2k_vid_set_notify(void (*fn)(void));

#endif
