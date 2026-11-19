#include "md_logic.h"

#include <string.h>

void md_reduce(const uint8_t *luma, uint8_t *blocks)
{
    for (int by = 0; by < MD_BH; by++) {
        for (int bx = 0; bx < MD_BW; bx++) {
            unsigned sum = 0;
            for (int y = 0; y < MD_BLOCK; y++) {
                const uint8_t *row = luma + (by * MD_BLOCK + y) * MD_W + bx * MD_BLOCK;
                for (int x = 0; x < MD_BLOCK; x++) sum += row[x];
            }
            blocks[by * MD_BW + bx] = (uint8_t)(sum / (MD_BLOCK * MD_BLOCK));
        }
    }
}

unsigned md_score(const uint8_t *before, const uint8_t *after, int pixel_threshold)
{
    long sum = 0;
    for (int i = 0; i < MD_BLOCKS; i++) sum += (int)after[i] - (int)before[i];
    int mean = (int)(sum / MD_BLOCKS);
    unsigned changed = 0;
    for (int i = 0; i < MD_BLOCKS; i++) {
        int d = (int)after[i] - (int)before[i] - mean;
        if (d < 0) d = -d;
        if (d >= pixel_threshold) changed++;
    }
    return changed * 10000u / MD_BLOCKS;
}

int md_feed(md_state_t *s, const md_params_t *p, unsigned score, int64_t now_ms)
{
    s->score = score;
    int moving = score >= p->area_x100;
    if (moving) {
        s->streak++;
        s->last_motion_ms = now_ms;
    } else {
        s->streak = 0;
    }
    if (!s->active) {
        if (s->streak >= p->trigger_samples) {
            s->active = 1;
            s->events++;
            s->peak = score;
            return 1;
        }
        return 0;
    }
    if (score > s->peak) s->peak = score;
    if (now_ms - s->last_motion_ms >= p->hold_ms) {
        s->active = 0;
        s->streak = 0;
        return -1;
    }
    return 0;
}

int64_t md_pause_extend(int64_t until_ms, int64_t now_ms, int pause_ms)
{
    int64_t wanted = now_ms + (pause_ms > 0 ? pause_ms : 0);
    return wanted > until_ms ? wanted : until_ms;
}

int md_paused(int64_t until_ms, int64_t now_ms) { return now_ms < until_ms; }

int md_preset(const char *name, md_params_t *p)
{
    /* "medium" is what camd used on the server; the other two sit a factor of three either side. */
    if (!strcmp(name, "low"))         { p->pixel_threshold = 31; p->area_x100 = 450; }
    else if (!strcmp(name, "medium")) { p->pixel_threshold = 25; p->area_x100 = 150; }
    else if (!strcmp(name, "high"))   { p->pixel_threshold = 19; p->area_x100 = 50; }
    else return -1;
    p->trigger_samples = 2;
    p->hold_ms = 5000;
    return 0;
}
