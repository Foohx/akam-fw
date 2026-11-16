/* Status LED pattern engine, see akam2k_leds.h. */
#include <stdio.h>
#include <string.h>

#include "akam2k_leds.h"

static const char *const g_key[A2K_RULE_N] = { "led_boot", "led_nowifi", "led_person", "led_stream", "led_night", "led_idle" };
static const char *const g_def[A2K_RULE_N] = { "1:100:100:0", "1:250:250:0", "on", "on", "on", "on" };

const char *a2k_rule_key(int r) { return r >= 0 && r < A2K_RULE_N ? g_key[r] : "led_?"; }
const char *a2k_rule_default(int r) { return r >= 0 && r < A2K_RULE_N ? g_def[r] : "on"; }

int a2k_leds_parse_mode(const char *s)
{
    if (!s)
        return -1;
    return !strcmp(s, "on") ? A2K_LEDS_ON : !strcmp(s, "off") ? A2K_LEDS_OFF : !strcmp(s, "auto") ? A2K_LEDS_AUTO : -1;
}

const char *a2k_leds_mode_name(int m) { return m == A2K_LEDS_ON ? "on" : m == A2K_LEDS_OFF ? "off" : "auto"; }

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

int a2k_leds_pick(const a2k_leds_in_t *in)
{
    return in->boot ? A2K_RULE_BOOT : in->nowifi ? A2K_RULE_NOWIFI : in->person ? A2K_RULE_PERSON
         : in->stream ? A2K_RULE_STREAM : in->night ? A2K_RULE_NIGHT : A2K_RULE_IDLE;
}

int a2k_leds_init(a2k_leds_t *l, const hal_light_desc_t *desc, const hal_light_ops_t *drv, const a2k_leds_ops_t *ops, long now_ms)
{
    memset(l, 0, sizeof *l);
    if (light_init(&l->light, desc, drv) != 0 || light_find(&l->light, "blue") < 0)
        return -1;
    pthread_mutex_init(&l->mu, NULL);
    if (ops)
        l->ops = *ops;
    l->mode = A2K_LEDS_AUTO;
    for (int r = 0; r < A2K_RULE_N; r++)
        (void)a2k_pat_parse(g_def[r], &l->pat[r]);
    l->start_ms = now_ms;
    l->cur = -2;
    l->level = -1;
    l->last_rule = -1;
    l->fl_on = 0;
    return 0;
}

void a2k_leds_destroy(a2k_leds_t *l) { pthread_mutex_destroy(&l->mu); }

int a2k_leds_set_mode(a2k_leds_t *l, int mode)
{
    if (mode < A2K_LEDS_ON || mode > A2K_LEDS_AUTO)
        return -1;
    pthread_mutex_lock(&l->mu);
    l->mode = mode;
    pthread_mutex_unlock(&l->mu);
    return 0;
}

int a2k_leds_get_mode(a2k_leds_t *l)
{
    int m;
    pthread_mutex_lock(&l->mu);
    m = l->mode;
    pthread_mutex_unlock(&l->mu);
    return m;
}

int a2k_leds_set_pattern(a2k_leds_t *l, int rule, const char *text)
{
    a2k_pat_t p;
    if (rule < 0 || rule >= A2K_RULE_N || a2k_pat_parse(text, &p) != 0)
        return -1;
    pthread_mutex_lock(&l->mu);
    l->pat[rule] = p;
    pthread_mutex_unlock(&l->mu);
    return 0;
}

void a2k_leds_get_pattern(a2k_leds_t *l, int rule, char *out, size_t cap)
{
    a2k_pat_t p;
    if (rule < 0 || rule >= A2K_RULE_N) {
        if (cap)
            out[0] = 0;
        return;
    }
    pthread_mutex_lock(&l->mu);
    p = l->pat[rule];
    pthread_mutex_unlock(&l->mu);
    a2k_pat_format(&p, out, cap);
}

int a2k_leds_blip(a2k_leds_t *l, const char *pattern, long dur_ms, long now_ms)
{
    a2k_pat_t p;
    if (dur_ms < 1 || dur_ms > 60000 || a2k_pat_parse(pattern, &p) != 0)
        return -1;
    pthread_mutex_lock(&l->mu);
    l->fl_pat = p;
    l->fl_until_ms = now_ms + dur_ms;
    l->fl_on = 1;
    pthread_mutex_unlock(&l->mu);
    return 0;
}

void a2k_leds_blip_clear(a2k_leds_t *l)
{
    pthread_mutex_lock(&l->mu);
    l->fl_on = 0;
    pthread_mutex_unlock(&l->mu);
}

int a2k_leds_last_rule(a2k_leds_t *l)
{
    int r;
    pthread_mutex_lock(&l->mu);
    r = l->last_rule;
    pthread_mutex_unlock(&l->mu);
    return r;
}

void a2k_leds_tick(a2k_leds_t *l, long now_ms)
{
    a2k_leds_in_t in;
    a2k_pat_t p;
    int key, level;
    memset(&in, 0, sizeof in);
    if (l->ops.inputs)
        l->ops.inputs(l->ops.user, &in);
    pthread_mutex_lock(&l->mu);
    if (!in.nowifi)
        l->wifi_seen = 1;
    in.boot = !l->wifi_seen && now_ms - l->start_ms < A2K_LEDS_BOOT_MS;
    if (l->mode == A2K_LEDS_AUTO) {
        key = a2k_leds_pick(&in);
        p = l->pat[key];
    } else {
        key = -1;
        memset(&p, 0, sizeof p);
        p.kind = l->mode == A2K_LEDS_ON ? A2K_PAT_ON : A2K_PAT_OFF;
    }
    if (l->fl_on && now_ms >= l->fl_until_ms)
        l->fl_on = 0;
    if (l->fl_on && l->mode != A2K_LEDS_OFF) {
        key = -3;
        p = l->fl_pat;
    }
    l->last_rule = key;
    if (key != l->cur || memcmp(&p, &l->cur_pat, sizeof p) != 0) {
        l->cur = key;
        l->cur_pat = p;
        l->pat_start_ms = now_ms;
    }
    if (!l->taken && p.kind == A2K_PAT_ON) { /* the stock state: nothing to write */
        pthread_mutex_unlock(&l->mu);
        return;
    }
    level = a2k_pat_level(&p, now_ms - l->pat_start_ms);
    if (level != l->level && light_set(&l->light, "blue", level ? LIGHT_ON : LIGHT_OFF, now_ms) == 0) {
        l->level = level;
        l->taken = 1;
    }
    pthread_mutex_unlock(&l->mu);
}

void a2k_leds_release(a2k_leds_t *l)
{
    pthread_mutex_lock(&l->mu);
    if (l->taken && l->level != 1 && light_set(&l->light, "blue", LIGHT_ON, 0) == 0)
        l->level = 1;
    pthread_mutex_unlock(&l->mu);
}
