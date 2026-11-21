/* Live apply of the video settings (no encoder re-open), tried by the capture thread BEFORE the close + open_ex of video.h. The vendor
 * setters are those RE'd in docs/porting/lsc-rotatable-2k/venc-live-params.md (the driver's acceptance mid-GOP is unverified on hardware, hence:
 * any non-zero return = the caller re-opens with the same `want`, and a setter that failed once is switched off for the rest of the run).
 * The vendor calls go through an ops table so the logic is host unit tested (tests/rot2k_vlive_unit.c) with a fake encoder.
 *   fps -> set_fps; min/max QP -> set_qp; kbps / max kbps / gop -> get_attr, edit, set_attr (width, height, profile, enc_type are never changed);
 *   rc vbr <-> cbr -> set_rc_type (avbr in either value = re-open).
 * Also the debug hook of the hardware test (--venc-test 1): one command per file, see a2k_vlive_parse_cmd. */
#ifndef AKAM2K_VLIVE_H
#define AKAM2K_VLIVE_H
#include "abi.h"
#include "video.h"

typedef struct {
    int (*set_fps)(int h, int fps);
    int (*set_qp)(int h, int min_qp, int max_qp);
    int (*get_attr)(int h, struct venc_param *out);
    int (*set_attr)(int h, const struct venc_param *p);
    int (*set_rc_type)(int h, int type);
    int (*get_cur_qp)(int h, void *out); /* out: at least A2K_VL_RAW bytes */
    int (*get_rc_stat)(int h, void *out);
} a2k_vlive_ops_t;
#define A2K_VL_RAW 64

enum { A2K_VL_FPS, A2K_VL_QP, A2K_VL_ATTR, A2K_VL_RC, A2K_VL_N }; /* the setters that can be switched off */

typedef struct {
    const a2k_vlive_ops_t *ops;
    int enabled;          /* video_live: 0 = never live */
    int off[A2K_VL_N];    /* a setter that failed once */
} a2k_vlive_t;

void a2k_vlive_init(a2k_vlive_t *v, const a2k_vlive_ops_t *ops, int enabled);
/* Tries to bring the running encoder (handle h, running with *vp) to `want`. 1 = every difference was applied live and *vp now holds the new
 * values; 0 = nothing was tried (live off, a needed setter off, or a change only a re-open can make) or a setter failed (logged, switched off):
 * the caller re-opens with `want`; *vp may then be partly updated and is rewritten by the re-open. */
int a2k_vlive_apply(a2k_vlive_t *v, int chn, int h, struct venc_param *vp, const a2k_vid_t *want);

/* ---- debug hook ---- */
#define A2K_VENC_TEST_FILE "/tmp/akam2k_venc_test"
enum { A2K_VC_FPS, A2K_VC_QP, A2K_VC_KBPS, A2K_VC_GOP, A2K_VC_RC, A2K_VC_ATTR };
typedef struct { int chn, kind, a, b; } a2k_vlive_cmd_t; /* a, b: the numbers; rc: a = 0 cbr / 1 vbr */
/* "main|sub fps N" | "qp MIN MAX" | "kbps TARGET MAX" | "gop N" | "rc cbr|vbr" | "attr", one line (a trailing newline is fine), digits only. 0 or -1. */
int a2k_vlive_parse_cmd(const char *text, a2k_vlive_cmd_t *out);
/* The poll of the capture thread of stream chn: reads the command file (path from the caller; absent = 0). A command for chn is deleted and
 * returned (1); one for the other stream stays; an unparsable one is deleted by the main-stream thread (logged). */
int a2k_vlive_poll(const char *file, int chn, a2k_vlive_cmd_t *out);
/* Runs ONE command through the live setters only (no fallback, no disabled flags), logs the vendor return code(s) and returns the first non-zero
 * one (0 = ok). On success *vp follows (so the caller can follow the new fps / gop). */
int a2k_vlive_run_cmd(const a2k_vlive_t *v, int h, struct venc_param *vp, const a2k_vlive_cmd_t *c);

#endif
