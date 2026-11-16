/* Pure light policy, see light_logic.h. */
#include <string.h>

#include "light_logic.h"

int light_find(const light_t *l, const char *name)
{
    if (!l || !name)
        return -1;
    for (int i = 0; i < l->desc->count; i++)
        if (strcmp(l->desc->emitters[i].name, name) == 0)
            return i;
    return -1;
}

int light_init(light_t *l, const hal_light_desc_t *desc, const hal_light_ops_t *ops)
{
    if (!l || !desc || !ops || !ops->set || !desc->emitters || desc->count < 1 || desc->count > LIGHT_MAX)
        return -1;
    memset(l, 0, sizeof *l);
    l->desc = desc;
    l->ops = ops;
    for (int i = 0; i < desc->count; i++) {
        l->mode[i] = LIGHT_OFF;
        l->level[i] = -1;
    }
    if ((desc->excl_a || desc->excl_b) && (light_find(l, desc->excl_a) < 0 || light_find(l, desc->excl_b) < 0))
        return -1;
    return 0;
}

int light_parse_mode(const char *s, int *mode)
{
    static const struct {
        const char *w;
        int m;
    } t[] = { { "off", LIGHT_OFF }, { "on", LIGHT_ON }, { "blink-fast", LIGHT_BLINK_FAST }, { "blink-slow", LIGHT_BLINK_SLOW } };
    if (!s || !mode)
        return -1;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcmp(s, t[i].w) == 0) {
            *mode = t[i].m;
            return 0;
        }
    return -1;
}

static int drive(light_t *l, int i, int level)
{
    if (l->level[i] == level)
        return 0;
    if (l->ops->set(&l->desc->emitters[i], level) != 0)
        return -1;
    l->level[i] = level;
    return 0;
}

int light_set(light_t *l, const char *name, int mode, long now_ms)
{
    int i = light_find(l, name);
    if (i < 0 || mode < LIGHT_OFF || mode > LIGHT_BLINK_SLOW)
        return -1;
    if (mode != LIGHT_OFF && l->desc->excl_a) {
        int a = light_find(l, l->desc->excl_a), b = light_find(l, l->desc->excl_b);
        int other = i == a ? b : i == b ? a : -1;
        if (other >= 0 && l->mode[other] != LIGHT_OFF) { /* an emitter this engine never lit is not touched */
            if (drive(l, other, 0) != 0)
                return -1;
            l->mode[other] = LIGHT_OFF;
        }
    }
    if (drive(l, i, mode == LIGHT_OFF ? 0 : 1) != 0)
        return -1;
    l->mode[i] = mode;
    l->start_ms[i] = now_ms;
    return 0;
}

int light_tick(light_t *l, long now_ms)
{
    int errs = 0;
    for (int i = 0; i < l->desc->count; i++) {
        long half = l->mode[i] == LIGHT_BLINK_FAST ? LIGHT_FAST_HALF_MS : l->mode[i] == LIGHT_BLINK_SLOW ? LIGHT_SLOW_HALF_MS : 0;
        long el = now_ms - l->start_ms[i];
        if (!half)
            continue;
        if (el < 0)
            el = 0;
        if (drive(l, i, ((el / half) & 1) == 0) != 0)
            errs++;
    }
    return errs;
}
