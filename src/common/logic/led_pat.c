/* LED blink patterns, see led_pat.h. */
#include <stdio.h>
#include <string.h>

#include "led_pat.h"

/* 1..6 digits (no sign, no space): the value, or -1 */
static long num(const char **s)
{
    long v = 0;
    int n = 0;
    while (**s >= '0' && **s <= '9' && n < 7) {
        v = v * 10 + (**s - '0');
        (*s)++;
        n++;
    }
    return n == 0 || n > 6 ? -1 : v;
}

int a2k_pat_parse(const char *s, a2k_pat_t *p)
{
    a2k_pat_t t;
    long v[4];
    if (!s || !p)
        return -1;
    memset(&t, 0, sizeof t);
    if (!strcmp(s, "on") || !strcmp(s, "off")) {
        t.kind = s[1] == 'n' ? A2K_PAT_ON : A2K_PAT_OFF;
        *p = t;
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        v[i] = num(&s);
        if (v[i] < 0 || (i < 3 ? *s != ':' : *s != '\0'))
            return -1;
        if (i < 3)
            s++;
    }
    if (v[0] < 1 || v[0] > 10 || v[1] < 50 || v[1] > 60000 || v[2] < 50 || v[2] > 60000 || v[3] > 60000 ||
        (v[3] != 0 && v[3] < 50) || v[0] * (v[1] + v[2]) + v[3] > 120000)
        return -1;
    t.kind = A2K_PAT_BLINK;
    t.n = (int)v[0];
    t.on_ms = (int)v[1];
    t.off_ms = (int)v[2];
    t.pause_ms = (int)v[3];
    *p = t;
    return 0;
}

void a2k_pat_format(const a2k_pat_t *p, char *out, size_t cap)
{
    if (p->kind == A2K_PAT_ON)
        snprintf(out, cap, "on");
    else if (p->kind == A2K_PAT_OFF)
        snprintf(out, cap, "off");
    else
        snprintf(out, cap, "%d:%d:%d:%d", p->n, p->on_ms, p->off_ms, p->pause_ms);
}

int a2k_pat_level(const a2k_pat_t *p, long t)
{
    long blink, cycle;
    if (p->kind != A2K_PAT_BLINK)
        return p->kind == A2K_PAT_ON;
    if (t < 0)
        t = 0;
    blink = p->on_ms + p->off_ms;
    cycle = p->n * blink + p->pause_ms;
    t %= cycle;
    return t < p->n * blink && t % blink < p->on_ms;
}

long a2k_pat_next_ms(const a2k_pat_t *p, long t)
{
    long blink, cycle, ph;
    if (p->kind != A2K_PAT_BLINK)
        return -1;
    if (t < 0)
        t = 0;
    blink = p->on_ms + p->off_ms;
    cycle = p->n * blink + p->pause_ms;
    ph = t % cycle;
    if (ph < p->n * blink) {
        long in = ph % blink;
        return in < p->on_ms ? p->on_ms - in : blink - in;
    }
    return cycle - ph; /* the pause: dark until the next cycle starts */
}
