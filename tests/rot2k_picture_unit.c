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
static int32_t wb_type = 1; /* "auto" is 1 on this fake: nothing in the module may assume 0 */
static int fx_val[8], fx_calls, nr_sets, nr_gets, wb_sets;
static int fail_fx, fail_nr_get, fail_nr_set, fail_wb;

static int f_effect_set(int type, int v)
{
    fx_calls++;
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
static int marker(int rec) { return nr3d[4 + rec * A2K_NR3D_REC + 4]; }
static int enable(int rec) { return nr3d[4 + rec * A2K_NR3D_REC]; }

static void fresh(void)
{
    a2k_pic_reset();
    a2k_pic_init(&OPS);
    fill_table();
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

    /* white balance: the type read before the first change is the automatic one */
    fresh();
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_CONF) == A2K_PIC_OK && wb_sets == 0, "wb conf on an untouched camera sends nothing");
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_MANUAL) == A2K_PIC_OK && wb_type == 0 && a2k_pic_get_wb() == A2K_PIC_WB_MANUAL, "wb manual sets the other type than the one read (1 -> 0)");
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_CONF) == A2K_PIC_OK && wb_type == 1, "wb conf puts the first-read type back");
    CHECK(a2k_pic_set_wb(7) == A2K_PIC_ARG, "an unknown white balance mode is refused");
    wb_type = 5;
    fresh();
    wb_type = 5;
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_MANUAL) == A2K_PIC_FAIL && wb_sets == 0, "a type that is not 0 or 1 is not trusted: nothing is sent");
    fresh();
    fail_wb = 1;
    CHECK(a2k_pic_set_wb(A2K_PIC_WB_MANUAL) == A2K_PIC_FAIL && a2k_pic_get_wb() == A2K_PIC_WB_CONF, "an ISP that refuses: failure, value kept");

    /* replay after a day / night switch */
    fresh();
    a2k_pic_set_saturation(-12);
    a2k_pic_set_denoise(3);
    a2k_pic_set_wb(A2K_PIC_WB_MANUAL);
    /* the switch reloads the conf set: everything is back to the file's values */
    fill_table();
    wb_type = 1;
    fx_val[2] = 0;
    fx_calls = 0;
    CHECK(a2k_pic_replay(1) == 0, "replay after the night switch reports no failure");
    CHECK(fx_val[2] == -12, "replay: saturation put back");
    CHECK(marker(1) == 100 + 4, "replay: the denoise shift is computed from the NEW original");
    CHECK(wb_type == 1, "replay at night: the manual white balance is a day setting (type stays automatic)");
    fill_table();
    wb_type = 1;
    CHECK(a2k_pic_replay(0) == 0 && wb_type == 0, "replay after the day switch: manual white balance again");
    fresh();
    CHECK(a2k_pic_replay(0) == 0 && fx_calls == 0 && nr_sets == 0 && wb_sets == 0, "replay with nothing set sends nothing");

    /* unbound */
    a2k_pic_init(NULL);
    CHECK(a2k_pic_set_saturation(5) == A2K_PIC_FAIL && a2k_pic_set_denoise(1) == A2K_PIC_FAIL && a2k_pic_set_wb(A2K_PIC_WB_MANUAL) == A2K_PIC_FAIL, "without an ISP every setter fails cleanly");

    printf("rot2k picture unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
