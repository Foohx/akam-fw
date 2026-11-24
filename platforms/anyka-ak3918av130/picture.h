/* Picture settings of the Rotatable 2K that the 1080p also has: saturation, denoise and white balance (plus the 2K's own wdr and nr2), same names, ranges and meaning as the
 * 1080p control port (isp.saturation, isp.denoise, img.wb), applied live through the vendor ISP API (libplat_vpss) and replayed after every
 * day / night mode switch (ak_vi_switch_mode reloads the whole conf set, which drops them). RAM only here; the settings registry persists them.
 * Status: host-tested only. Every layout below is read from the libraries (docs/porting/lsc-rotatable-2k/isp-controls.md) and has never run on a camera.
 *
 *   isp.saturation  -50..50, 0 = the conf file's own saturation. ak_vpss_effect_set(0, 2, v): the vendor adds 3 * v to the UV gain of the YUV effect.
 *                   Same range and meaning as the 1080p (its own delta on the conf value).
 *   isp.denoise     0..4, 0 = the camera's own tables (the 2K default until judged on a camera; the 1080p defaults to 2). N = the 3D noise-reduction
 *                   (temporal) record of each light level comes from the level N steps higher (the last one stays), the 1080p's rule. The 3D NR attribute is
 *                   read, edited and written whole; if what the ISP returns is not as decoded nothing is sent and the call is refused.
 *   isp.nr2         0..4, 0 = the camera's own table (nothing is sent). N = stronger spatial (luma 2D) noise reduction: nr2_k, ynr_intensity1 and
 *                   ynr_intensity2 of every record are raised by a step that grows with the record (the gain-linked records 1..16 run low gain to
 *                   high gain, the grain of a backlit shadow is a high-gain one), clamped to 15. Get-modify-set of the 990-byte nr2 attribute from
 *                   the pristine copy, so N is never cumulative; the enables are untouched; refused with nothing sent if the attribute is not as decoded.
 *                   That a HIGHER field value is a STRONGER filter is the vendor's naming, not proved on a camera.
 *   isp.wdr         0..100, default 90 = the stock. The backlight / WDR strength, effect type 5 of ak_vpss_effect_set with the value N - 50 (so 90 is
 *                   the stock's 40). The vendor blends the conf file's 6 x 65-entry WDR curves with its identity ramp (default_wdr_para, 0, 16 .. 1023)
 *                   by (value + 50) / 100 from a cached copy of the conf curves: 0 = identity (WDR off), 100 = the conf curves in full, linear in
 *                   between and not cumulative. Not a get-modify-set: the vendor does it (libplat_vpss ccd8, evidence in isp-controls.md).
 *   img.wb          conf = the ISP's own automatic white balance, always. manual is REFUSED (A2K_PIC_REFUSED, nothing sent): freezing the current AWB
 *                   gains needs the AWB run info, whose fields are unnamed, and a gain unit that is unverified; switching wb_type to manual with the conf's
 *                   own manual gains gave a strong blue cast on a camera (2026-11-24). See isp-controls.md.
 *
 * The vendor access is behind a2k_isptest_ops_t (isptest.h), so the host test drives this module with a fake ISP. */
#ifndef AKAM2K_PICTURE_H
#define AKAM2K_PICTURE_H

#include "isp.h"
#include "isptest.h"

#define A2K_PIC_SAT_MIN (-50)
#define A2K_PIC_SAT_MAX 50
#define A2K_PIC_DENOISE_MAX 4
#define A2K_PIC_NR2_MAX 4
#define A2K_PIC_NR2_SIZE (4 + A2K_REC_N * A2K_NR2_REC) /* 990 */
#define A2K_PIC_WDR_MAX 100
#define A2K_PIC_WDR_DEFAULT 90 /* effect value 40, the stock's */
#define A2K_PIC_NR3D_SIZE (4 + A2K_REC_N * A2K_NR3D_REC) /* 7076 */

enum { A2K_PIC_WB_CONF = 0, A2K_PIC_WB_MANUAL = 1 };
/* Return codes: 0 done, 1 refused (the live table is not as decoded: nothing was sent), -1 the ISP refused or is not ready, -2 bad argument. */
enum { A2K_PIC_OK = 0, A2K_PIC_REFUSED = 1, A2K_PIC_FAIL = -1, A2K_PIC_ARG = -2 };

/* Binds the vendor access. Call once before the others (a NULL ops leaves everything refusing with A2K_PIC_FAIL). */
void a2k_pic_init(const a2k_isptest_ops_t *ops);
/* Forgets every captured original and applied value (tests). */
void a2k_pic_reset(void);

int a2k_pic_set_saturation(int v);
int a2k_pic_get_saturation(void); /* the value applied (0 before any set) */
int a2k_pic_set_denoise(int bias);
int a2k_pic_get_denoise(void);
int a2k_pic_set_nr2(int level);
int a2k_pic_get_nr2(void);
int a2k_pic_set_wdr(int strength); /* 0..100, 90 = stock */
int a2k_pic_get_wdr(void);
/* The ak_vpss_effect_set(0, 5, v) value the app applies for "WDR on": strength - 50, 40 unless the user changed isp.wdr (a2k_isp_wdr, the stock's replay after a mode switch). */
int a2k_pic_wdr_effect(void);
int a2k_pic_set_wb(int mode); /* A2K_PIC_WB_* */
int a2k_pic_get_wb(void);

/* After a successful ak_vi_switch_mode(0, night): the conf set was reloaded, so the captured originals are stale; re-applies what the user set.
 * Returns the number of calls that failed. Under the caller's ISP lock. */
int a2k_pic_replay(int night);

/* Pure helper, host tested: given the pristine nr2 attribute, the stronger one for this level. 0 done, 1 when the attribute is not as decoded
 * (mode above 1, or in a record: an enable above 1, nr2_k, ynr_intensity1 / 2 above 15, ynr_keep_th above 31). `out` may not alias `in`. level 0 copies. */
int a2k_pic_nr2_scale(const uint8_t *in, uint8_t *out, int level);

/* Pure helper, host tested: given the pristine 3D NR attribute, the shifted one for this bias. 0 done, 1 when the attribute is not as decoded
 * (mode not 0 or 1, a record with tnr_enable or tnr_refFrame_format above 1). `out` may not alias `in`. bias 0 copies. */
int a2k_pic_nr3d_shift(const uint8_t *in, uint8_t *out, int bias);

#endif
