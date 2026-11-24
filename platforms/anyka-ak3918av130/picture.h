/* Picture settings of the Rotatable 2K that the 1080p also has: saturation, denoise and white balance, same names, ranges and meaning as the
 * 1080p control port (isp.saturation, isp.denoise, img.wb), applied live through the vendor ISP API (libplat_vpss) and replayed after every
 * day / night mode switch (ak_vi_switch_mode reloads the whole conf set, which drops them). RAM only here; the settings registry persists them.
 * Status: host-tested only. Every layout below is read from the libraries (docs/porting/lsc-rotatable-2k/isp-controls.md) and has never run on a camera.
 *
 *   isp.saturation  -50..50, 0 = the conf file's own saturation. ak_vpss_effect_set(0, 2, v): the vendor adds 3 * v to the UV gain of the YUV effect.
 *                   Same range and meaning as the 1080p (its own delta on the conf value).
 *   isp.denoise     0..4, 0 = the camera's own tables (the 2K default until judged on a camera; the 1080p defaults to 2). N = the 3D noise-reduction
 *                   (temporal) record of each light level comes from the level N steps higher (the last one stays), the 1080p's rule. The 3D NR attribute is
 *                   read, edited and written whole; if what the ISP returns is not as decoded nothing is sent and the call is refused.
 *   img.wb          conf = the ISP's own automatic white balance (default, what the stock runs), manual = fixed gains: the wb type switched to manual with the
 *                   conf's own manual gains (they are not changed), so the picture stops following the light. Day set only (the night set is mono).
 *
 * The vendor access is behind a2k_isptest_ops_t (isptest.h), so the host test drives this module with a fake ISP. */
#ifndef AKAM2K_PICTURE_H
#define AKAM2K_PICTURE_H

#include "isp.h"
#include "isptest.h"

#define A2K_PIC_SAT_MIN (-50)
#define A2K_PIC_SAT_MAX 50
#define A2K_PIC_DENOISE_MAX 4
#define A2K_PIC_NR3D_SIZE (4 + A2K_REC_N * A2K_NR3D_REC) /* 7076 */

enum { A2K_PIC_WB_CONF = 0, A2K_PIC_WB_MANUAL = 1 };
/* Return codes: 0 done, 1 refused (the live table is not as decoded: nothing was sent), -1 the ISP refused or is not ready, -2 bad argument. */
enum { A2K_PIC_OK = 0, A2K_PIC_REFUSED = 1, A2K_PIC_FAIL = -1, A2K_PIC_ARG = -2 };

/* Binds the vendor access. Call once before the others (a NULL ops leaves everything refusing with A2K_PIC_FAIL). */
void a2k_pic_init(const a2k_isptest_ops_t *ops);
/* Forgets every captured original, applied value and the cached auto type (tests). */
void a2k_pic_reset(void);

int a2k_pic_set_saturation(int v);
int a2k_pic_get_saturation(void); /* the value applied (0 before any set) */
int a2k_pic_set_denoise(int bias);
int a2k_pic_get_denoise(void);
int a2k_pic_set_wb(int mode); /* A2K_PIC_WB_* */
int a2k_pic_get_wb(void);

/* After a successful ak_vi_switch_mode(0, night): the conf set was reloaded, so the captured originals are stale; re-applies what the user set.
 * Returns the number of calls that failed. Under the caller's ISP lock. */
int a2k_pic_replay(int night);

/* Pure helper, host tested: given the pristine 3D NR attribute, the shifted one for this bias. 0 done, 1 when the attribute is not as decoded
 * (mode not 0 or 1, a record with tnr_enable or tnr_refFrame_format above 1). `out` may not alias `in`. bias 0 copies. */
int a2k_pic_nr3d_shift(const uint8_t *in, uint8_t *out, int bias);

#endif
