/* See button_gesture.h. */
#include <string.h>

#include "button_gesture.h"

void bg_timing_defaults(bg_timing_t *t)
{
    t->debounce_ms = 50;
    t->short_max_ms = 1000;
    t->gap_ms = 400;
    t->long_ms = 2000;
    t->very_long_ms = 5000;
}

int bg_timing_ok(const bg_timing_t *t)
{
    return t->debounce_ms >= 10 && t->debounce_ms <= 200 && t->short_max_ms >= 200 && t->short_max_ms <= 1500 && t->gap_ms >= 150 &&
           t->gap_ms <= 1000 && t->long_ms >= 1500 && t->long_ms <= 10000 && t->long_ms > t->short_max_ms &&
           t->very_long_ms >= t->long_ms + 500 && t->very_long_ms <= 30000;
}

static const char TOK[] = { '?', 'S', 'L', 'V' };

static int tok_of(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    return c == 'S' ? BG_S : c == 'L' ? BG_L : c == 'V' ? BG_V : 0;
}

static void one(bg_seq_t *s, int el)
{
    s->n = 1;
    s->el[0] = (uint8_t)el;
}

int bg_seq_parse(const char *str, bg_seq_t *out)
{
    size_t i, n = str ? strlen(str) : 0;
    memset(out, 0, sizeof *out);
    if (n == 0 || n > 24)
        return -1;
    if (!strcmp(str, "short")) {
        one(out, BG_S);
        return 0;
    }
    if (!strcmp(str, "double") || !strcmp(str, "triple")) {
        out->n = str[0] == 'd' ? 2 : 3;
        for (i = 0; i < out->n; i++)
            out->el[i] = BG_S;
        return 0;
    }
    if (!strcmp(str, "long")) {
        one(out, BG_L);
        return 0;
    }
    if (!strcmp(str, "very_long")) {
        one(out, BG_V);
        return 0;
    }
    /* tokens: a letter, then (one separator, a letter)* */
    for (i = 0; i < n; i += 2) {
        int t = tok_of(str[i]);
        if (!t || out->n >= BG_MAX_SEQ)
            return -1;
        out->el[out->n++] = (uint8_t)t;
        if (i + 1 < n && ((str[i + 1] != ' ' && str[i + 1] != '_') || i + 2 >= n))
            return -1; /* a bad or a trailing separator */
    }
    return 0;
}

int bg_seq_name(const bg_seq_t *s, char *out, size_t cap)
{
    size_t i, n = 0;
    const char *alias = NULL;
    if (s->n == 1 && s->el[0] == BG_S) alias = "short";
    else if (s->n == 1 && s->el[0] == BG_L) alias = "long";
    else if (s->n == 1 && s->el[0] == BG_V) alias = "very_long";
    else if (s->n == 2 && s->el[0] == BG_S && s->el[1] == BG_S) alias = "double";
    else if (s->n == 3 && s->el[0] == BG_S && s->el[1] == BG_S && s->el[2] == BG_S) alias = "triple";
    if (alias) {
        n = strlen(alias);
        if (n + 1 > cap)
            return -1;
        memcpy(out, alias, n + 1);
        return (int)n;
    }
    if (s->n == 0 || (size_t)s->n * 2 > cap)
        return -1;
    for (i = 0; i < s->n; i++) {
        if (i)
            out[n++] = ' ';
        out[n++] = TOK[s->el[i] <= BG_V ? s->el[i] : 0];
    }
    out[n] = '\0';
    return (int)n;
}

int bg_seq_eq(const bg_seq_t *a, const bg_seq_t *b)
{
    return a->n == b->n && memcmp(a->el, b->el, a->n) == 0;
}

int bg_seq_has_v(const bg_seq_t *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->el[i] == BG_V)
            return 1;
    return 0;
}

void bg_init(bg_t *g, const bg_timing_t *t)
{
    memset(g, 0, sizeof *g);
    if (t && bg_timing_ok(t))
        g->t = *t;
    else
        bg_timing_defaults(&g->t);
}

void bg_set_bound(bg_t *g, const bg_seq_t *list, int n)
{
    int i;
    g->nbound = n < 0 ? 0 : n > BG_MAX_BOUND ? BG_MAX_BOUND : n;
    g->use_v = 0;
    for (i = 0; i < g->nbound; i++) {
        g->bound[i] = list[i];
        g->use_v |= bg_seq_has_v(&list[i]);
    }
}

int bg_is_bound(const bg_t *g, const bg_seq_t *s)
{
    int i;
    for (i = 0; i < g->nbound; i++)
        if (bg_seq_eq(&g->bound[i], s))
            return 1;
    return 0;
}

/* 1 when a bound sequence is longer than cur and starts with it: more presses may still come. */
static int extendable(const bg_t *g)
{
    int i;
    for (i = 0; i < g->nbound; i++)
        if (g->bound[i].n > g->cur.n && memcmp(g->bound[i].el, g->cur.el, g->cur.n) == 0)
            return 1;
    return 0;
}

static int finish(bg_t *g, bg_seq_t *out)
{
    *out = g->cur;
    memset(&g->cur, 0, sizeof g->cur);
    return BG_GESTURE;
}

void bg_press(bg_t *g, int64_t now_ms)
{
    g->down = 1;
    g->t_down = now_ms;
}

int bg_release(bg_t *g, int64_t now_ms, bg_seq_t *out)
{
    int64_t held = now_ms - g->t_down;
    int el;
    if (!g->down)
        return BG_NONE;
    g->down = 0;
    g->t_up = now_ms;
    if (held >= g->t.very_long_ms && g->use_v)
        el = BG_V;
    else if (held >= g->t.long_ms)
        el = BG_L;
    else if (held < g->t.short_max_ms)
        el = BG_S;
    else {
        memset(&g->cur, 0, sizeof g->cur); /* dead zone */
        return BG_DEAD;
    }
    g->cur.el[g->cur.n++] = (uint8_t)el;
    if (g->cur.n >= BG_MAX_SEQ || !extendable(g))
        return finish(g, out);
    return BG_NONE;
}

int bg_tick(bg_t *g, int64_t now_ms, bg_seq_t *out)
{
    if (g->down || g->cur.n == 0 || now_ms - g->t_up < g->t.gap_ms)
        return BG_NONE;
    return finish(g, out);
}
