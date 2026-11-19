/* Host test of src/ak_rtsp/md_logic.c (run by tests/md_logic_test.sh). */
#include <stdio.h>
#include <string.h>

#include "md_logic.h"

/* Not assert(): the checks must run whatever the optimisation level defines. */
#define CHECK(c) do { if (!(c)) { printf("md_logic_test: FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static uint8_t frame[MD_W * MD_H];

/* A picture with vertical bands of different brightness, optionally with a dark box on it. */
static void scene(uint8_t *blocks, int offset, int box_x, int box_w)
{
    for (int y = 0; y < MD_H; y++)
        for (int x = 0; x < MD_W; x++) {
            int v = 40 + (x / 20) * 12 + offset;
            if (box_w && x >= box_x && x < box_x + box_w && y >= 40 && y < 140) v = 20;
            frame[y * MD_W + x] = (uint8_t)v;
        }
    md_reduce(frame, blocks);
}

int main(void)
{
    uint8_t a[MD_BLOCKS], b[MD_BLOCKS];
    md_params_t p;

    CHECK(md_preset("medium", &p) == 0 && p.pixel_threshold == 25 && p.area_x100 == 150);
    CHECK(md_preset("extreme", &p) == -1);
    md_preset("medium", &p);

    /* identical frames, and frames that differ by sensor-like noise */
    scene(a, 0, 0, 0);
    scene(b, 0, 0, 0);
    CHECK(md_score(a, b, p.pixel_threshold) == 0);
    for (int i = 0; i < MD_BLOCKS; i++) b[i] = (uint8_t)(a[i] + (i % 3) - 1);
    CHECK(md_score(a, b, p.pixel_threshold) == 0);

    /* the whole picture 30 levels brighter: an exposure step, not movement */
    scene(b, 30, 0, 0);
    CHECK(md_score(a, b, p.pixel_threshold) == 0);

    /* a 40x100 box appears: 4000 of 57600 pixels, about 7 % of the blocks */
    scene(b, 0, 100, 40);
    unsigned s = md_score(a, b, p.pixel_threshold);
    CHECK(s > 500 && s < 900);

    /* a hand over three quarters of the lens is movement too, not a lighting change */
    scene(b, 0, 0, 240);
    CHECK(md_score(a, b, p.pixel_threshold) > 1500);

    /* state machine: one moving sample is a glitch, two start motion, 5 s of quiet end it */
    md_state_t st;
    memset(&st, 0, sizeof(st));
    CHECK(md_feed(&st, &p, 700, 0) == 0 && !st.active);
    CHECK(md_feed(&st, &p, 0, 500) == 0 && !st.active);
    CHECK(md_feed(&st, &p, 700, 1000) == 0);
    CHECK(md_feed(&st, &p, 900, 1500) == 1 && st.active && st.events == 1 && st.peak == 900);
    CHECK(md_feed(&st, &p, 1200, 2000) == 0 && st.peak == 1200);
    CHECK(md_feed(&st, &p, 0, 6500) == 0 && st.active);   /* 4.5 s after the last moving sample */
    CHECK(md_feed(&st, &p, 0, 7000) == -1 && !st.active); /* 5 s */
    CHECK(st.events == 1);
    CHECK(md_feed(&st, &p, 800, 7500) == 0);
    CHECK(md_feed(&st, &p, 800, 8000) == 1 && st.events == 2);

    /* pauses: extended by a later announcement, never shortened, over when the time is up */
    int64_t until = 0;
    CHECK(!md_paused(until, 1000));
    until = md_pause_extend(until, 1000, 8000);
    CHECK(until == 9000 && md_paused(until, 1000) && md_paused(until, 8999) && !md_paused(until, 9000));
    until = md_pause_extend(until, 2000, 3000); /* a shorter one inside a longer one changes nothing */
    CHECK(until == 9000);
    until = md_pause_extend(until, 5000, 8000); /* a later one extends */
    CHECK(until == 13000);
    until = md_pause_extend(until, 6000, -5);   /* nonsense is no pause */
    CHECK(until == 13000);

    /* presets order */
    md_params_t lo, hi;
    md_preset("low", &lo);
    md_preset("high", &hi);
    CHECK(lo.area_x100 > p.area_x100 && hi.area_x100 < p.area_x100);
    CHECK(lo.pixel_threshold > p.pixel_threshold && hi.pixel_threshold < p.pixel_threshold);

    puts("md_logic_test: ok");
    return 0;
}
