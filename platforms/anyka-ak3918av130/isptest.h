/* RAM-only ISP test hook of the Rotatable 2K own app (--isp-test 1, default OFF): a poll thread reads /tmp/akam2k_isp_test, runs its lines
 * through the vendor ISP / VPSS getters and setters and logs the return codes and the before / after values (akam2k.log, "isp-test: ..."). Nothing
 * is written to flash and nothing is persistent: the ISP restarts from its conf file at the next boot, and "restore" puts back what the hook
 * first read. The ABI notes (symbols, struct layouts, confidence) are in docs/porting/lsc-rotatable-2k/isp-controls.md.
 *
 * File format: one command per line (up to 16 lines, digits and words only), the file is deleted once read.
 *   get all | fx | yuv | wb | ccm | nr1 | nr2 | uvnr | nr3d | awb | rgb      read and log (never writes)
 *   fx TYPE VALUE                  ak_vpss_effect_set(0, TYPE, VALUE), TYPE 0..5, VALUE -50..50 (0 = the conf value), then ak_vpss_effect_get
 *   hue V | bright V | sat V | contrast V | sharp V      = fx 0 | 1 | 2 | 3 | 4 (V -50..50)
 *   wb auto                        wb_type back to the type read when the hook first looked (the ISP's own AWB)
 *   wb manual R G B                manual gains 0..4095 (256 is taken as 1.0x, UNVERIFIED), then wb_type to the other value
 *   wb type N                      raw wb_type 0 or 1
 *   ccm mode N                     ccm_attr cc_mode 0 or 1 (1 = automatic, colour-temperature table: the day set of the conf file)
 *   ccm blend P                    every colour matrix (manual and automatic) = P percent of itself + (100 - P) percent identity (P 0..100)
 *   nr1 K | nr2d K [I1 I2] | uvnr K | nr3d E      nr1_k, nr2_k (+ ynr_intensity1/2), uvnr_k (0..15), tnr_enable (0 or 1) in the manual record and in every
 *                                  gain-linked record (the mode is untouched)
 *   rt NAME                        get, set the same bytes back, get again, compare (NAME: yuv wb ccm nr1 nr2 uvnr nr3d): proves the set path
 *   restore                        every attribute the hook changed is set back from its first read; effects set to 0
 */
#ifndef AKAM2K_ISPTEST_H
#define AKAM2K_ISPTEST_H
#include <stddef.h>
#include <stdint.h>

#define A2K_ISP_TEST_FILE "/tmp/akam2k_isp_test"
#define A2K_IT_MAXLINES 16
#define A2K_IT_BUF 16384 /* every get buffer: larger than the largest attribute (the 3D NR one is 7076 bytes) and than the 9414 bytes of the AWB statistics */

/* The attributes the hook reaches. */
enum { A2K_IT_YUV, A2K_IT_WBTYPE, A2K_IT_MWB, A2K_IT_CCM, A2K_IT_NR1, A2K_IT_NR2, A2K_IT_UVNR, A2K_IT_NR3D, A2K_IT_AWB, A2K_IT_N };

/* Layouts (offsets in bytes, little endian), see isp-controls.md for the confidence of each. */
#define A2K_YUV_SIZE 10      /* y_a u16 +0, y_b s16 +2, uv_a s16 +4 (saturation), uv_b s16 +6, dark_margin_en u16 +8 */
#define A2K_MWB_SIZE 12      /* r_gain u16, g_gain u16, b_gain u16 (each 0..4095), r_off s16, g_off s16, b_off s16 (-512..511) */
#define A2K_CCM_MODE 0       /* u32 0..1 */
#define A2K_CCM_MAN 18       /* manual ccm: s16 [3][3] at +18 */
#define A2K_CCM_NUM 36       /* u16 colour matrix count 3..10 */
#define A2K_CCM_AUTO 44      /* auto entries, stride 28: u16 cc_color_temp, enable, cnoise_yth, gain, slop at +0, s16 ccm[3][3] at +10 */
#define A2K_CCM_AUTO_STRIDE 28
#define A2K_CCM_AUTO_N 10
#define A2K_CCM_SIZE 324
#define A2K_NR1_REC 180      /* record 0 = manual at +4, records 1..16 = gain linked at +4 + 180 * j; nr1_k u16 at +2 of a record, nr1_enable +0 */
#define A2K_NR2_REC 58       /* nr2_enable +0, pass2 +2, nr2_k +4, keep_th +6, ynr_intensity1 +8, ynr_intensity2 +10 */
#define A2K_UVNR_REC 42      /* uvnr_enable +0, pass2 +2, uvnr_k +4 */
#define A2K_NR3D_REC 416     /* tnr_enable u16 +0, tnr_refFrame_format u16 +2 */
#define A2K_REC_N 17
#define A2K_AWB_RUN_OFF 9376 /* the 38 bytes of the AWB run info in the buffer of ak_vpss_get_awb_stat_info */
#define A2K_AWB_RUN_LEN 38

typedef struct {
    int (*effect_set)(int type, int value);
    int (*effect_get)(int type, int *value);
    int (*get)(int what, void *buf);          /* A2K_IT_*; buf = A2K_IT_BUF bytes, zeroed by the caller */
    int (*set)(int what, const void *buf);
    int (*rgb_avg)(int *r, int *g, int *b);
} a2k_isptest_ops_t;

enum { A2K_ITC_GET, A2K_ITC_FX, A2K_ITC_WB_AUTO, A2K_ITC_WB_MANUAL, A2K_ITC_WB_TYPE, A2K_ITC_CCM_MODE, A2K_ITC_CCM_BLEND, A2K_ITC_NR1, A2K_ITC_NR2D,
       A2K_ITC_UVNR, A2K_ITC_NR3D, A2K_ITC_RT, A2K_ITC_RESTORE };
#define A2K_IT_ALL 100 /* get target: everything */
#define A2K_IT_FX 101  /* get target: the six effect values */
#define A2K_IT_RGB 102 /* get target: the RGB average */

typedef struct {
    int kind;
    int a, b, c, d;
} a2k_isptest_cmd_t;

/* One line (a trailing newline is fine). 0 or -1 (unknown word, bad number, out of range, extra text). */
int a2k_isptest_parse(const char *line, a2k_isptest_cmd_t *out);

typedef struct {
    const a2k_isptest_ops_t *ops;
    uint8_t *orig[A2K_IT_N]; /* the first read of every attribute the hook changed (restore) */
    int auto_type;           /* wb_type read when the hook first looked, -1 = not read yet */
    int fx_touched;
} a2k_isptest_t;

void a2k_isptest_init(a2k_isptest_t *t, const a2k_isptest_ops_t *ops);
void a2k_isptest_free(a2k_isptest_t *t);
/* Runs one command, logs "isp-test: ..." lines, returns the first non-zero vendor return code (0 = ok, -1 = refused by the hook itself). */
int a2k_isptest_exec(a2k_isptest_t *t, const a2k_isptest_cmd_t *c);
/* Reads the command file (absent = 0), deletes it, parses and runs each line. Returns the number of lines run. */
int a2k_isptest_poll(a2k_isptest_t *t, const char *file);

/* The poll thread (every 500 ms until a2k_stop). 0 or -1. */
int a2k_isptest_start(const a2k_isptest_ops_t *ops);
void a2k_isptest_stop(void);

#endif
