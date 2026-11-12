#include <stdio.h>
#include <string.h>

#include "klog.h"

/* Core of the kernel log filter, see klog.h. No I/O, no clock: everything is injected. */

static const char *const NOISE[] = {
    "gpio_led_set value=", /* vendor LED driver: one line per brightness write */
    NULL
};

int klog_is_noise(const char *msg)
{
    for (int i = 0; NOISE[i]; i++)
        if (strstr(msg, NOISE[i])) return 1;
    return 0;
}

/* "<N>rest" -> prio (N & 7) and rest; no prefix -> info. */
static const char *split_prio(const char *line, int *prio)
{
    *prio = 6;
    if (line[0] == '<') {
        int n = 0, i = 1;
        while (line[i] >= '0' && line[i] <= '9' && i < 5) n = n * 10 + (line[i++] - '0');
        if (i > 1 && line[i] == '>') {
            *prio = n & 7;
            return line + i + 1;
        }
    }
    return line;
}

/* Normalised comparison key: leading "[...]" counter or timestamp removed, digit runs folded to '#'. */
static void make_key(const char *msg, char *key, size_t cap)
{
    const char *p = msg;
    if (*p == '[') {
        const char *q = p + 1;
        while (*q == ' ' || *q == '.' || (*q >= '0' && *q <= '9')) q++;
        if (*q == ']') {
            p = q + 1;
            if (*p == ' ') p++;
        }
    }
    size_t n = 0;
    int in_digits = 0;
    for (; *p && n + 1 < cap; p++) {
        if (*p >= '0' && *p <= '9') {
            if (!in_digits) key[n++] = '#';
            in_digits = 1;
        } else {
            key[n++] = *p;
            in_digits = 0;
        }
    }
    key[n] = '\0';
}

void klog_init(klog_t *k, klog_emit_fn emit, void *user, long window_s)
{
    memset(k, 0, sizeof(*k));
    k->emit = emit;
    k->user = user;
    k->window = window_s > 0 ? window_s : KLOG_WINDOW_S;
}

static void emit_summary(klog_t *k, klog_ent_t *e)
{
    char buf[KLOG_TEXT_MAX + 80];
    snprintf(buf, sizeof(buf), "message repeated %u times in the last %lds: %s", e->suppressed, k->window,
             e->text);
    k->emit(k->user, e->prio, buf);
    e->suppressed = 0;
}

static void set_text(klog_ent_t *e, const char *msg, int prio)
{
    snprintf(e->text, sizeof(e->text), "%s", msg);
    e->prio = prio;
}

/* A fresh entry; when the table is full the entry quiet for the longest is reported and recycled. */
static klog_ent_t *make_entry(klog_t *k, const char *key, long now)
{
    klog_ent_t *free_e = NULL, *oldest = NULL;
    for (int i = 0; i < KLOG_MAX_KEYS; i++) {
        klog_ent_t *e = &k->e[i];
        if (!e->used) { if (!free_e) free_e = e; continue; }
        if (!oldest || e->last_emit < oldest->last_emit) oldest = e;
    }
    klog_ent_t *e = free_e;
    if (!e) {
        e = oldest;
        if (e->suppressed) emit_summary(k, e);
    }
    memset(e, 0, sizeof(*e));
    e->used = 1;
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->last_emit = now;
    return e;
}

void klog_feed(klog_t *k, const char *line, long now)
{
    int prio;
    const char *msg = split_prio(line, &prio);
    if (klog_is_noise(msg)) { k->dropped++; return; }

    char key[KLOG_KEY_MAX];
    make_key(msg, key, sizeof(key));
    klog_ent_t *e = NULL;
    for (int i = 0; i < KLOG_MAX_KEYS; i++)
        if (k->e[i].used && !strcmp(k->e[i].key, key)) { e = &k->e[i]; break; }

    if (e && now - e->last_emit < k->window) {
        e->suppressed++;
        set_text(e, msg, prio);
        return;
    }
    if (!e) e = make_entry(k, key, now);
    else if (e->suppressed) emit_summary(k, e); /* window over, the tick has not run yet */
    set_text(e, msg, prio);
    e->last_emit = now;
    k->emit(k->user, prio, msg);
}

void klog_tick(klog_t *k, long now)
{
    for (int i = 0; i < KLOG_MAX_KEYS; i++) {
        klog_ent_t *e = &k->e[i];
        if (!e->used || now - e->last_emit < k->window) continue;
        if (e->suppressed) {
            emit_summary(k, e);
            e->last_emit = now; /* a still-running burst is summarised once per window */
        } else {
            e->used = 0;
        }
    }
}

long klog_next_due(const klog_t *k, long now)
{
    long best = -1;
    for (int i = 0; i < KLOG_MAX_KEYS; i++) {
        const klog_ent_t *e = &k->e[i];
        if (!e->used || !e->suppressed) continue; /* idle entries need no wakeup: freed at the next tick */
        long d = e->last_emit + k->window - now;
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void klog_flush(klog_t *k)
{
    for (int i = 0; i < KLOG_MAX_KEYS; i++)
        if (k->e[i].used && k->e[i].suppressed) emit_summary(k, &k->e[i]);
}
