/* Host unit test of the picture settings of the Rotatable 2K (platforms/anyka-ak3918av130/picture.c) against a fake ISP: saturation, denoise (the 3D NR
 * record shift) and white balance, the refusal of a table that is not as decoded, the replay after a day / night switch and the argument checks.
 * Prints "rot2k picture unit: N failed". */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picture.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...)
{
    (void)fmt;
}

/* ---- the fake ISP ---- */
static uint8_t nr3d[A2K_PIC_NR3D_SIZE];
static uint8_t nr2[A2K_PIC_NR2_SIZE];
static int nr2_sets, nr2_gets, fail_nr2_set, order[16], norder;
static int32_t wb_type = 1; /* "auto" is 1 on this fake: nothing in the module may assume 0 */
static int fx_val[8], fx_calls, nr_sets, nr_gets, wb_sets;
static int fail_fx, fail_nr_get, fail_nr_set, fail_wb;

static int f_effect_set(int type, int v)
{
    fx_calls++;
    if (type == A2K_FX_WDR && norder < 16)
        order[norder++] = 5;
    if (fail_fx)
        return 0x100;
    fx_val[type] = v;
    return 0;
}
static int f_get(int what, void *buf)
{
    if (what == A2K_IT_NR3D) {
        nr_gets++;
        if (fail_nr_get)
            return -1;
        memcpy(buf, nr3d, sizeof nr3d);
        return 0;
    }
    if (what == A2K_IT_NR2) {
        nr2_gets++;
        memcpy(buf, nr2, sizeof nr2);
        return 0;
    }
    if (what == A2K_IT_WBTYPE) {
        if (fail_wb)
            return -1;
        memcpy(buf, &wb_type, 4);
        return 0;
    }
    return -1;
}
static int f_set(int what, const void *buf)
{
    if (what == A2K_IT_NR3D) {
        nr_sets++;
        if (fail_nr_set)
            return -1;
        memcpy(nr3d, buf, sizeof nr3d);
        return 0;
    }
    if (what == A2K_IT_NR2) {
        nr2_sets++;
        if (norder < 16)
            order[norder++] = 2;
        if (fail_nr2_set)
            return -1;
        memcpy(nr2, buf, sizeof nr2);
        return 0;
    }
    if (what == A2K_IT_WBTYPE) {
        wb_sets++;
        if (fail_wb)
            return -1;
        memcpy(&wb_type, buf, 4);
        return 0;
    }
    return -1;
}
static const a2k_isptest_ops_t OPS = { f_effect_set, NULL, f_get, f_set, NULL };

/* record i of the fake table: tnr_enable (i % 2), a marker u16 at +4 that says which level it is */
static void fill_table(void)
{
    int i;
    memset(nr3d, 0, sizeof nr3d);
    nr3d[0] = 1; /* mode 1 */
    for (i = 0; i < A2K_REC_N; i++) {
        uint8_t *r = nr3d + 4 + i * A2K_NR3D_REC;
        r[0] = (uint8_t)(i % 2);
        r[4] = (uint8_t)(100 + i);
    }
}
/* nr2 record i: enable 1, pass2 0, k = i % 16 (0 for the manual record: 3), keep_th 10, intensity1 = 2 + i % 5, intensity2 = 1 */
static void fill_nr2(void)
{
    int i;
    memset(nr2, 0, sizeof nr2);
    nr2[0] = 1;
    for (i = 0; i < A2K_REC_N; i++) {
        uint8_t *r = nr2 + 4 + i * A2K_NR2_REC;
        r[0] = 1;
        r[4] = (uint8_t)(i == 0 ? 3 : i % 16);
        r[6] = 10;
        r[8] = (uint8_t)(2 + i % 5);
        r[10] = 1;
        r[14] = (uint8_t)(0x40 + i); /* the weight table: never touched */
    }
}
static int n2(int rec, int off) { return nr2[4 + rec * A2K_NR2_REC + off]; }
static int marker(int rec) { return nr3d[4 + rec * A2K_NR3D_REC + 4]; }
static int enable(int rec) { return nr3d[4 + rec * A2K_NR3D_REC]; }

static void fresh(void)
{
    a2k_pic_reset();
    a2k_pic_init(&OPS);
    fill_table();
    fill_nr2();
    nr2_sets = nr2_gets = fail_nr2_set = norder = 0;
    wb_type = 1;
    memset(fx_val, 0, sizeof fx_val);
    fx_calls = nr_sets = nr_gets = wb_sets = 0;
    fail_fx = fail_nr_get = fail_nr_set = fail_wb = 0;
}

int main(void)
{
    uint8_t out[A2K_PIC_NR3D_SIZE];
    printf("rot2k picture unit\n");

    /* sizes: the 3D NR attribute is the 7076 bytes the library validates */
    CHECK(A2K_PIC_NR3D_SIZE == 7076, "the 3D NR attribute is 4 + 17 x 416 = 7076 bytes");

    /* saturation: effect type 2, -50..50, live */
    fresh();
    CHECK(a2k_pic_set_saturation(20) == A2K_PIC_OK && fx_val[2] == 20 && a2k_pic_get_saturation() == 20, "saturation 20 goes to ak_vpss_effect_set type 2");
    CHECK(a2k_pic_set_saturation(-50) == A2K_PIC_OK && fx_val[2] == -50, "saturation -50 (the lowest) is accepted");
    CHECK(a2k_pic_set_saturation(50) == A2K_PIC_OK && fx_val[2] == 50, "saturation 50 (the highest) is accepted");
    CHECK(a2k_pic_set_saturation(51) == A2K_PIC_ARG && a2k_pic_set_saturation(-51) == A2K_PIC_ARG, "saturation outside -50..50 is refused");
    CHECK(fx_val[2] == 50 && a2k_pic_get_saturation() == 50, "a refused saturation changes nothing");
    fail_fx = 1;
    CHECK(a2k_pic_set_saturation(10) == A2K_PIC_FAIL && a2k_pic_get_saturation() == 50, "a failing ISP leaves the recorded value");
    fail_fx = 0;

    /* denoise: the shift rule */
    fresh();
    CHECK(a2k_pic_set_denoise(0) == A2K_PIC_OK && nr_sets == 0 && nr_gets == 0, "denoise 0 on an untouched camera sends nothing (the camera's own tables)");
    CHECK(a2k_pic_set_denoise(2) == A2K_PIC_OK && nr_sets == 1, "denoise 2 writes the table once");
    CHECK(marker(1) == 100 + 3 && marker(5) == 100 + 7 && marker(14) == 100 + 16, "linked level i takes the record 2 steps higher");
    CHECK(marker(15) == 100 + 16 && marker(16) == 100 + 16, "the last levels keep the last record");
    CHECK(marker(0) == 100, "the manual record (0) is untouched");
    CHECK(enable(2) == 0 && enable(1) == 1 && enable(3) == 1, "a borrowed record never lowers tnr_enable (level 2 was 0 but its donor 4 is 0: stays 0; level 1 stays 1)");
    CHECK(nr3d[0] == 1, "the mode word is untouched");
    CHECK(a2k_pic_set_denoise(4) == A2K_PIC_OK && marker(1) == 100 + 5 && marker(12) == 100 + 16, "denoise 4 is computed from the original, not on top of 2");
    CHECK(a2k_pic_set_denoise(0) == A2K_PIC_OK && marker(1) == 101 && marker(9) == 109 && marker(16) == 116, "denoise 0 puts the original table back");
    CHECK(a2k_pic_get_denoise() == 0, "the recorded value is 0 again");
    CHECK(a2k_pic_set_denoise(5) == A2K_PIC_ARG && a2k_pic_set_denoise(-1) == A2K_PIC_ARG, "denoise outside 0..4 is refused");

    /* pure helper */
    fill_table();
    CHECK(a2k_pic_nr3d_shift(nr3d, out, 0) == 0 && memcmp(out, nr3d, sizeof out) == 0, "shift 0 is a plain copy");
    nr3d[0] = 2;
    CHECK(a2k_pic_nr3d_shift(nr3d, out, 2) == 1, "a mode word above 1 is not as decoded");
    fill_table();
    nr3d[4 + 3 * A2K_NR3D_REC] = 7;
    CHECK(a2k_pic_nr3d_shift(nr3d, out, 2) == 1, "a tnr_enable above 1 is not as decoded");
    fill_table();
    nr3d[4 + 3 * A2K_NR3D_REC + 2] = 5;
    CHECK(a2k_pic_nr3d_shift(nr3d, out, 2) == 1, "a tnr_refFrame_format above 1 is not as decoded");

    /* refused and failing ISP */
    fresh();
    nr3d[4 + 9 * A2K_NR3D_REC] = 9;
    CHECK(a2k_pic_set_denoise(2) == A2K_PIC_REFUSED && nr_sets == 0 && a2k_pic_get_denoise() == 0, "a live table that is not as decoded: refused, nothing sent, value kept");
    fresh();
    fail_nr_get = 1;
    CHECK(a2k_pic_set_denoise(2) == A2K_PIC_FAIL && nr_sets == 0, "an ISP that cannot be read: failure, nothing sent");
    fail_nr_get = 0;
    fail_nr_set = 1;
    CHECK(a2k_pic_set_denoise(2) == A2K_PIC_FAIL && a2k_pic_get_denoise() == 0, "an ISP that refuses the write: failure, value kept");

    /* white balance: manual is refused (no way to freeze the automatic gains), conf is the only state, the ISP is never touched */
    fresh();
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_CONF) == A2K_PIC_OK && wb_sets == 0, "wb conf on an untouched camera sends nothing");
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_MANUAL) == A2K_PIC_REFUSED && wb_type == 1 && wb_sets == 0 && a2k_pic_get_wb() == A2K_PIC_WB_CONF, "wb manual is refused: nothing sent, the type is untouched, the value stays conf");
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_CONF) == A2K_PIC_OK && wb_sets == 0, "wb conf after a refused manual sends nothing");
    CHECK(a2k_pic_set_wb(7) == A2K_PIC_ARG, "an unknown white balance mode is refused");

    /* replay after a day / night switch */
    fresh();
    a2k_pic_set_saturation(-12);
    a2k_pic_set_denoise(3);
    (void)a2k_pic_set_wb(A2K_PIC_WB_MANUAL); /* refused */
    /* the switch reloads the conf set: everything is back to the file's values */
    fill_table();
    wb_type = 1;
    fx_val[2] = 0;
    fx_calls = 0;
    CHECK(a2k_pic_replay(1) == 0, "replay after the night switch reports no failure");
    CHECK(fx_val[2] == -12, "replay: saturation put back");
    CHECK(marker(1) == 100 + 4, "replay: the denoise shift is computed from the NEW original");
    CHECK(wb_type == 1 && wb_sets == 0, "replay at night: the white balance type is never touched");
    fill_table();
    wb_type = 1;
    CHECK(a2k_pic_replay(0) == 0 && wb_type == 1 && wb_sets == 0, "replay after the day switch: the white balance type is never touched either");
    fresh();
    CHECK(a2k_pic_replay(0) == 0 && fx_calls == 0 && nr_sets == 0 && wb_sets == 0, "replay with nothing set sends nothing");

    /* nr2: the scaling rule, from the pristine copy, never cumulative */
    fresh();
    CHECK(A2K_PIC_NR2_SIZE == 990, "the nr2 attribute is 4 + 17 x 58 = 990 bytes");
    CHECK(a2k_pic_set_nr2(0) == A2K_PIC_OK && nr2_sets == 0 && nr2_gets == 0, "nr2 0 on an untouched camera sends nothing (the camera's own table)");
    CHECK(a2k_pic_set_nr2(1) == A2K_PIC_OK && nr2_sets == 1 && a2k_pic_get_nr2() == 1, "nr2 1 writes the table once");
    CHECK(n2(0, 4) == 4 && n2(0, 8) == 3 && n2(0, 10) == 2, "level 1: the manual record gets +1 on nr2_k and both intensities");
    CHECK(n2(1, 4) == 2 && n2(8, 4) == 8 + 2 && n2(15, 4) == 15 && n2(16, 4) == 3, "level 1: the step grows with the linked record (1 for record 1, 2 for 8, 3 for 16, clamped at 15)");
    CHECK(n2(5, 8) == 3 && n2(5, 10) == 2, "level 1: ynr_intensity1 and 2 follow the same step");
    CHECK(n2(3, 6) == 10 && n2(3, 0) == 1 && n2(3, 14) == 0x43 && nr2[0] == 1, "keep_th, the enable, the weight table and the mode are untouched");
    CHECK(a2k_pic_set_nr2(4) == A2K_PIC_OK && n2(1, 4) == 1 + 3 && n2(15, 4) == 15 && n2(16, 8) == 3 + 10 && n2(0, 4) == 7, "level 4 is computed from the original, not on top of 1");
    CHECK(a2k_pic_set_nr2(0) == A2K_PIC_OK && n2(1, 4) == 1 && n2(16, 4) == 0 && n2(0, 4) == 3 && n2(5, 8) == 2, "nr2 0 puts the original table back");
    CHECK(a2k_pic_get_nr2() == 0, "the recorded level is 0 again");
    CHECK(a2k_pic_set_nr2(5) == A2K_PIC_ARG && a2k_pic_set_nr2(-1) == A2K_PIC_ARG, "nr2 outside 0..4 is refused");
    {
        uint8_t o2[A2K_PIC_NR2_SIZE];
        fill_nr2();
        CHECK(a2k_pic_nr2_scale(nr2, o2, 0) == 0 && memcmp(o2, nr2, sizeof o2) == 0, "scale level 0 is a plain copy");
        nr2[2] = 2;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "a mode word above 1 is not as decoded");
        fill_nr2();
        nr2[4 + 4 * A2K_NR2_REC + 4] = 16;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "nr2_k above 15 is not as decoded");
        fill_nr2();
        nr2[4 + 4 * A2K_NR2_REC + 10] = 16;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "ynr_intensity2 above 15 is not as decoded");
        fill_nr2();
        nr2[4 + 4 * A2K_NR2_REC + 8] = 16;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "ynr_intensity1 above 15 is not as decoded");
        fill_nr2();
        nr2[4 + 4 * A2K_NR2_REC + 6] = 32;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "ynr_keep_th above 31 is not as decoded");
        fill_nr2();
        nr2[4 + 4 * A2K_NR2_REC] = 2;
        CHECK(a2k_pic_nr2_scale(nr2, o2, 1) == 1, "nr2_enable above 1 is not as decoded");
    }
    fresh();
    nr2[4 + 9 * A2K_NR2_REC + 4] = 99;
    CHECK(a2k_pic_set_nr2(2) == A2K_PIC_REFUSED && nr2_sets == 0 && a2k_pic_get_nr2() == 0, "a live nr2 table that is not as decoded: refused, nothing sent, value kept");
    fresh();
    fail_nr2_set = 1;
    CHECK(a2k_pic_set_nr2(2) == A2K_PIC_FAIL && a2k_pic_get_nr2() == 0, "an ISP that refuses the nr2 write: failure, value kept");

    /* wdr: 0..100, default 90 = the stock's effect 40 */
    fresh();
    CHECK(a2k_pic_get_wdr() == 90 && a2k_pic_wdr_effect() == 40, "default isp.wdr is 90 = effect value 40, the stock's");
    CHECK(a2k_pic_set_wdr(90) == A2K_PIC_OK && fx_calls == 0, "wdr 90 on an untouched camera sends nothing");
    CHECK(a2k_pic_set_wdr(100) == A2K_PIC_OK && fx_val[5] == 50 && a2k_pic_get_wdr() == 100 && a2k_pic_wdr_effect() == 50, "wdr 100 = effect 50 (the conf curves in full)");
    CHECK(a2k_pic_set_wdr(0) == A2K_PIC_OK && fx_val[5] == -50 && a2k_pic_wdr_effect() == -50, "wdr 0 = effect -50 (the identity curve, WDR off)");
    CHECK(a2k_pic_set_wdr(90) == A2K_PIC_OK && fx_val[5] == 40 && a2k_pic_wdr_effect() == 40, "back to 90 after a change sends the stock's 40");
    CHECK(a2k_pic_set_wdr(101) == A2K_PIC_ARG && a2k_pic_set_wdr(-1) == A2K_PIC_ARG && fx_val[5] == 40, "wdr outside 0..100 is refused, nothing sent");
    fail_fx = 1;
    CHECK(a2k_pic_set_wdr(60) == A2K_PIC_FAIL && a2k_pic_get_wdr() == 90, "an ISP that refuses: failure, value kept");
    fail_fx = 0;

    /* replay: order, and nothing sent at the defaults */
    fresh();
    a2k_pic_set_saturation(-12);
    a2k_pic_set_denoise(3);
    a2k_pic_set_nr2(2);
    a2k_pic_set_wdr(60);
    fill_nr2(); /* the switch reloads the conf set */
    fill_table();
    fx_val[5] = 0;
    norder = nr2_sets = 0;
    CHECK(a2k_pic_replay(0) == 0, "replay with nr2 and wdr set reports no failure");
    CHECK(nr2_sets == 1 && n2(1, 4) == 2 && fx_val[5] == 10, "replay: nr2 is computed from the NEW original, wdr is put back (60 - 50)");
    CHECK(norder == 2 && order[0] == 2 && order[1] == 5, "replay order: nr2 before wdr");
    fresh();
    CHECK(a2k_pic_replay(1) == 0 && nr2_sets == 0 && nr2_gets == 0 && fx_calls == 0, "replay at the defaults sends nothing (no nr2, no wdr)");
    a2k_pic_set_wdr(100);
    a2k_pic_set_wdr(90);
    fx_calls = 0;
    CHECK(a2k_pic_replay(0) == 0 && fx_val[5] == 40 && fx_calls == 1, "wdr touched then back to 90: replayed as the stock's 40");

    /* unbound */
    a2k_pic_init(NULL);
    CHECK(a2k_pic_set_saturation(5) == A2K_PIC_FAIL && a2k_pic_set_denoise(1) == A2K_PIC_FAIL && a2k_pic_set_nr2(1) == A2K_PIC_FAIL && a2k_pic_set_wdr(50) == A2K_PIC_FAIL, "without an ISP every setter fails cleanly");

    printf("rot2k picture unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
