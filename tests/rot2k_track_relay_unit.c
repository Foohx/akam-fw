/* Host unit test of the detector-miss relay (track_relay.c) on synthetic 320x192 luma frames: follows a textured person moving at several speeds, the detector box wins,
 * gives up on a flat patch / a vanished patch (low ZNCC) / after the cap, no relay before a box, and the per-step cost. Prints "rot2k track relay unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/features/track/track_relay.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s (line %d)\n", msg, __LINE__); fails++; } } while (0)
#define W 320
#define H 192

static uint8_t bg[W * H], body[200 * 200];
static unsigned rs = 12345;
static int rnd(void) { rs = rs * 1103515245u + 12345u; return (int)((rs >> 16) & 0x7fff); }

/* smooth-ish texture: a coarse random grid bilinearly blended */
static void texture(uint8_t *dst, int w, int h, int cell, int lo, int hi) {
    int gw = w / cell + 2, gh = h / cell + 2, g[64 * 64], x, y;
    for (int i = 0; i < gw * gh; i++) g[i] = lo + rnd() % (hi - lo);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int gx = x / cell, gy = y / cell, fx = x % cell, fy = y % cell;
            int a = g[gy * gw + gx], b = g[gy * gw + gx + 1], c = g[(gy + 1) * gw + gx], d = g[(gy + 1) * gw + gx + 1];
            int top = a * (cell - fx) + b * fx, bot = c * (cell - fx) + d * fx;
            dst[y * w + x] = (uint8_t)((top * (cell - fy) + bot * fy) / (cell * cell));
        }
}

/* the frame: background with the body texture pasted at (px, py), w x h of it (clipped); flat = a uniform patch instead */
static void frame(uint8_t *f, int px, int py, int bw, int bh, int flat, int noise) {
    memcpy(f, bg, sizeof bg);
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int fx = px + x, fy = py + y;
            if (fx < 0 || fy < 0 || fx >= W || fy >= H) continue;
            f[fy * W + fx] = noise ? (uint8_t)(rnd() & 255) : flat ? 128 : body[y * 200 + x];
        }
}

static int absi(int v) { return v < 0 ? -v : v; }

/* walk: the person moves (dx, dy) per frame at 5 Hz; the detector sees it for `seen` frames and then misses; returns the frames relayed and the final error in px */
static int walk(int dx, int dy, int seen, int total, int flat, int *err, int *last_ms, int noise_after) {
    tr_t t; uint8_t f[W * H]; int px = 60, py = 40, bw = 100, bh = 110, relayed = 0, out[4]; int64_t T = 1000;
    tr_init(&t);
    *last_ms = 0;
    for (int i = 0; i < total; i++, T += 200, px += dx, py += dy) {
        int det[4] = { px, py, px + bw, py + bh }, r;
        frame(f, px, py, bw, bh, flat && i >= seen - 1, noise_after && i >= seen);
        r = tr_step(&t, f, W, H, W, i < seen ? det : NULL, T, out);
        if (i >= seen && r) { relayed++; *last_ms = (int)(T - (1000 + (seen - 1) * 200)); *err = absi(out[0] - px) > absi(out[1] - py) ? absi(out[0] - px) : absi(out[1] - py); }
    }
    return relayed;
}

int main(void) {
    int err = 0, ms = 0, n;
    texture(bg, W, H, 8, 60, 200);
    texture(body, 200, 200, 6, 30, 230);
    printf("== track relay (ZNCC on the 320x192 luma, detector box wins, gated)\n");
    n = walk(6, 0, 5, 40, 0, &err, &ms, 0);
    CHECK(n >= 10 && err <= 8, "a textured person walking 6 px per frame sideways is relayed after the detector missed it (error <= 8 px)");
    printf("   relayed %d frames, last error %d px, last relay %d ms after the last box\n", n, err, ms);
    CHECK(ms <= TR_MAX_MS + 200 && ms >= TR_MAX_MS - 400, "the relay runs up to the cap (3.5 s) and not longer");
    n = walk(0, -2, 5, 40, 0, &err, &ms, 0);
    CHECK(n >= 10 && err <= 8, "the same for a person rising 2 px per frame (vertical)");
    n = walk(5, 2, 5, 40, 0, &err, &ms, 0); printf("   diagonal: relayed %d, err %d\n", n, err);
    CHECK(n >= 10 && err <= 10, "diagonal 5 + 2 px per frame (inside the +-20 px search window)");
    n = walk(0, 0, 5, 40, 1, &err, &ms, 0);
    CHECK(n == 0, "a flat patch (no texture) is never relayed");
    n = walk(0, 0, 5, 40, 0, &err, &ms, 1);
    CHECK(n == 0, "the patch vanishes into noise: low ZNCC, no relay");
    n = walk(30, 0, 5, 40, 0, &err, &ms, 0);
    CHECK(n <= 1, "a jump of 30 px per frame (beyond the search window): given up, not guessed");
    { /* the detector box always wins, and no relay before a box */
        tr_t t; uint8_t f[W * H]; int out[4], det[4] = { 80, 20, 180, 170 }, r;
        tr_init(&t); frame(f, 80, 20, 100, 150, 0, 0);
        r = tr_step(&t, f, W, H, W, NULL, 1000, out);
        CHECK(r == 0, "no relay before any detector box");
        r = tr_step(&t, f, W, H, W, det, 1200, out);
        CHECK(r == 0, "a frame with a detector box returns 0 (the box is the truth)");
        r = tr_step(&t, f, W, H, W, NULL, 1400, out);
        CHECK(r == 1 && absi(out[0] - 80) <= 4, "the first miss after a box relays that region");
        r = tr_step(&t, f, W, H, W, NULL, 1400 + TR_MAX_MS, out);
        CHECK(r == 0, "after the cap nothing is relayed");
        r = tr_step(&t, f, W, H, W, det, 6000, out); r = tr_step(&t, f, W, H, W, NULL, 6200, out);
        CHECK(r == 1, "a new detector box restarts the relay (the cap is from the last box)");
    }
    { /* top-clipped close box (user log: 88,2,301,188): relayed, region keeps its clipped top */
        tr_t t; uint8_t f[W * H]; int out[4], det[4] = { 88, 0, 301, 188 };
        tr_init(&t); frame(f, 88, -10, 200, 200, 0, 0);
        tr_step(&t, f, W, H, W, det, 1000, out);
        CHECK(tr_step(&t, f, W, H, W, NULL, 1200, out) == 1 && out[1] == 0 && out[3] >= 180, "a frame-filling top-clipped box is relayed with its clipped top");
    }
    { /* cost of one relayed step, host */
        tr_t t; uint8_t f[W * H]; int out[4], det[4] = { 80, 20, 180, 170 }; struct timespec a, b; double us; int N = 2000;
        frame(f, 80, 20, 100, 150, 0, 0);
        tr_init(&t); tr_step(&t, f, W, H, W, det, 1000, out);
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < N; i++) { tr_step(&t, f, W, H, W, NULL, 1200, out); t.dead = 0; t.det_ms = 1000; }
        clock_gettime(CLOCK_MONOTONIC, &b);
        us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / N;
        printf("   relay step cost on the host: %.1f us per frame (downsample + match)\n", us);
        CHECK(us < 1000, "a relay step costs well under 1 ms on the host");
    }
    printf("rot2k track relay unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
