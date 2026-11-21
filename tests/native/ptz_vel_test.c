/* Host test of src/features/ptz/ptz_vel.c: the velocity -> motor divisor mapping table, the dead zone, the diagonal cap, the strict number and
 * duration parsers, the lease clamp. Prints "ptz_vel_test: all passed". */
#include <stdio.h>
#include <string.h>

#include "ptz_vel.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void table(void)
{
    /* |v| in thousandths -> divisor of a single axis; pinned so a change of the formula is deliberate */
    static const struct { int milli, div; } t[] = {
        { 50, 20 }, { 60, 18 }, { 100, 14 }, { 150, 10 }, { 200, 8 }, { 250, 7 }, { 300, 6 }, { 400, 5 }, { 500, 4 }, { 600, 3 }, { 700, 3 },
        { 800, 2 }, { 900, 2 }, { 1000, 2 },
    };
    int prev = 99;
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int sg, d, sg2, d2;
        ptzv_axis(t[i].milli, &sg, &d);
        ptzv_axis(-t[i].milli, &sg2, &d2);
        if (sg != 1 || d != t[i].div || sg2 != -1 || d2 != t[i].div) {
            printf("FAIL: |v|=%d expected divisor %d, got sign %d divisor %d / %d %d\n", t[i].milli, t[i].div, sg, d, sg2, d2);
            fails++;
        }
    }
    /* monotonic: a larger |v| never gives a larger divisor; always inside the validated set */
    for (int m = 50; m <= 1000; m++) {
        int sg, d;
        ptzv_axis(m, &sg, &d);
        CHECK(sg == 1 && d >= PTZV_DIV_MIN && d <= PTZV_DIV_MAX, "divisor inside 2..20 above the dead zone");
        CHECK(d <= prev, "the divisor never grows with |v|");
        prev = d;
    }
}

static void dead_zone_and_map(void)
{
    ptzv_cmd_t c;
    int sg, d;
    ptzv_axis(49, &sg, &d);
    CHECK(sg == 0 && d == 0, "0.049 is inside the dead zone");
    ptzv_axis(-49, &sg, &d);
    CHECK(sg == 0 && d == 0, "-0.049 too");
    ptzv_axis(0, &sg, &d);
    CHECK(sg == 0, "0 is no motion");
    CHECK(ptzv_map(0, 0, &c) == 0 && !c.sx && !c.sy, "both axes at 0: a stop");
    CHECK(ptzv_map(40, -30, &c) == 0 && !c.sx && !c.sy, "both axes in the dead zone: a stop");
    CHECK(ptzv_map(1000, 0, &c) == 1 && c.sx == 1 && c.sy == 0 && c.dx == 2 && c.dy == 0, "full right: divisor 2");
    CHECK(ptzv_map(-1000, 0, &c) == 1 && c.sx == -1 && c.dx == 2, "full left");
    /* the REQUESTED divisor; the motor's safe layer makes the tilt effective divisor 3 (tilt_min_div, pinned in tests/rot2k_ptz_cont_unit.c integration_cases) */
    CHECK(ptzv_map(0, 1000, &c) == 1 && c.sy == 1 && c.sx == 0 && c.dy == 2 && c.dx == 0, "full up: tilt + is up, requested divisor 2 (effective 3 after the safe layer)");
    CHECK(ptzv_map(0, -1000, &c) == 1 && c.sy == -1 && c.dy == 2, "full down");
    CHECK(ptzv_map(1000, 1000, &c) == 1 && c.sx == 1 && c.sy == 1 && c.dx == 3 && c.dy == 3, "full diagonal: capped at divisor 3 per axis");
    CHECK(ptzv_map(-1000, 1000, &c) == 1 && c.sx == -1 && c.sy == 1 && c.dx == 3 && c.dy == 3, "diagonal up-left");
    CHECK(ptzv_map(1000, 200, &c) == 1 && c.dx == 3 && c.dy == 8, "diagonal: only the fast axis is capped");
    CHECK(ptzv_map(500, 500, &c) == 1 && c.dx == 4 && c.dy == 4, "diagonal at 0.5 keeps its divisor 4 (above the cap)");
    CHECK(ptzv_map(1000, 40, &c) == 1 && c.sx == 1 && c.sy == 0 && c.dx == 2 && c.dy == 0, "a dead-zone tilt makes it a single-axis move (divisor 2 allowed)");
    CHECK(ptzv_map(5000, -9000, &c) == 1 && c.sx == 1 && c.sy == -1 && c.dx == 3 && c.dy == 3, "out-of-range input is clamped, never wraps");
}

static void parse(void)
{
    int m = 7;
    static const struct { const char *s; int v; } ok[] = {
        { "0", 0 }, { "1", 1000 }, { "-1", -1000 }, { "0.5", 500 }, { "-0.35", -350 }, { "1.0", 1000 }, { "1.0000", 1000 }, { "0.0005", 1 }, { "0.0004", 0 },
        { "-0", 0 }, { "0.05", 50 }, { "00.5", 500 },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        CHECK(ptzv_parse_milli(ok[i].s, &m) == 0 && m == ok[i].v, ok[i].s);
    CHECK(ptzv_parse_milli("0.12345", &m) == -1, "5 fraction digits refused");
    CHECK(ptzv_parse_milli("1.0006", &m) == -2 && ptzv_parse_milli("1.0004", &m) == 0 && m == 1000, "1.0006 is out of range, 1.0004 rounds to 1");
    CHECK(ptzv_parse_milli("-1.5", &m) == -2 && ptzv_parse_milli("2", &m) == -2 && ptzv_parse_milli("99999999999", &m) < 0, "out of range / huge");
    CHECK(ptzv_parse_milli("", &m) == -1 && ptzv_parse_milli("-", &m) == -1 && ptzv_parse_milli(".5", &m) == -1 && ptzv_parse_milli("1.", &m) == -1, "empty, sign only, .5, 1. refused");
    CHECK(ptzv_parse_milli("1e0", &m) == -1 && ptzv_parse_milli("+1", &m) == -1 && ptzv_parse_milli("0x1", &m) == -1 && ptzv_parse_milli("1 ", &m) == -1 && ptzv_parse_milli("NaN", &m) == -1, "exponent, plus sign, hex, trailing blank, NaN refused");
    CHECK(ptzv_parse_milli(NULL, &m) == -1, "NULL");
}

static void duration(void)
{
    long ms = -1;
    static const struct { const char *s; long v; } ok[] = {
        { "PT1S", 1000 }, { "PT0.5S", 500 }, { "PT1.5S", 1500 }, { "PT2S", 2000 }, { "PT0S", 0 }, { "PT0.45S", 450 }, { "PT1M", 60000 }, { "PT1M30S", 90000 },
        { "PT0.1234S", 123 }, { "PT10S", 10000 }, { "PT1H", 3600000 },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
        CHECK(ptzv_parse_duration_ms(ok[i].s, &ms) == 0 && ms == ok[i].v, ok[i].s);
    CHECK(ptzv_parse_duration_ms("", &ms) < 0 && ptzv_parse_duration_ms("P", &ms) < 0 && ptzv_parse_duration_ms("PT", &ms) < 0, "empty forms refused");
    CHECK(ptzv_parse_duration_ms("1S", &ms) < 0 && ptzv_parse_duration_ms("PT-1S", &ms) < 0 && ptzv_parse_duration_ms("-PT1S", &ms) < 0, "no P / negative refused");
    CHECK(ptzv_parse_duration_ms("P1D", &ms) < 0 && ptzv_parse_duration_ms("PT1X", &ms) < 0 && ptzv_parse_duration_ms("PT1", &ms) < 0 && ptzv_parse_duration_ms("PT1.S", &ms) < 0, "days / bad designator / no designator refused");
    CHECK(ptzv_parse_duration_ms("PT0.5M", &ms) < 0, "a fractional minute is refused");
    CHECK(ptzv_parse_duration_ms("PT99999999999S", &ms) < 0, "huge refused");
    CHECK(ptzv_lease_clamp(0) == 450 && ptzv_lease_clamp(-5) == 450, "absent = 450 ms");
    CHECK(ptzv_lease_clamp(1) == 150 && ptzv_lease_clamp(149) == 150 && ptzv_lease_clamp(150) == 150, "lower clamp 150");
    CHECK(ptzv_lease_clamp(450) == 450 && ptzv_lease_clamp(2000) == 2000 && ptzv_lease_clamp(2001) == 2000 && ptzv_lease_clamp(60000) == 2000, "upper clamp 2000");
}

static void args(void)
{
    ptzv_cmd_t c;
    int lease = 0;
    const char *why = "";
    CHECK(ptzv_parse_args("1", NULL, NULL, &c, &lease, &why) == 0 && c.sx == 1 && c.sy == 0 && c.dx == 2 && lease == 450, "pan only, default lease");
    CHECK(ptzv_parse_args(NULL, "-0.5", "300", &c, &lease, &why) == 0 && c.sy == -1 && c.dy == 4 && lease == 300, "tilt only, lease 300");
    CHECK(ptzv_parse_args("0", "0", "2000", &c, &lease, &why) == 0 && !c.sx && !c.sy && lease == 2000, "both zero: a stop");
    CHECK(ptzv_parse_args("0.5", "0.5", "150", &c, &lease, &why) == 0 && c.dx == 4 && c.dy == 4 && lease == 150, "diagonal, lease 150");
    CHECK(ptzv_parse_args(NULL, NULL, NULL, &c, &lease, &why) == -1 && why[0], "nothing to do is refused with a reason");
    CHECK(ptzv_parse_args("x", NULL, NULL, &c, &lease, &why) == -1 && ptzv_parse_args("2", NULL, NULL, &c, &lease, &why) == -1 && ptzv_parse_args(NULL, "1e0", NULL, &c, &lease, &why) == -1, "bad or out-of-range velocity");
    CHECK(ptzv_parse_args("1", NULL, "149", &c, &lease, &why) == -1 && ptzv_parse_args("1", NULL, "2001", &c, &lease, &why) == -1 && ptzv_parse_args("1", NULL, "", &c, &lease, &why) == -1 &&
          ptzv_parse_args("1", NULL, "4.5", &c, &lease, &why) == -1 && ptzv_parse_args("1", NULL, "-300", &c, &lease, &why) == -1 && ptzv_parse_args("1", NULL, "0300x", &c, &lease, &why) == -1 &&
          ptzv_parse_args("1", NULL, "9999999999", &c, &lease, &why) == -1, "lease out of range or not whole digits");
}

int main(void)
{
    table();
    dead_zone_and_map();
    parse();
    duration();
    args();
    if (fails) {
        printf("ptz_vel_test: %d FAILED\n", fails);
        return 1;
    }
    printf("ptz_vel_test: all passed\n");
    return 0;
}
