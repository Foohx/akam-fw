/* Hysteresis state machine and state-file writer of the person detection (see akam2k_detstate.h). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_detstate.h"

void a2k_det_init(a2k_det_t *d, const struct a2k_det_cfg *cfg)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (d->cfg.on_count < 1)
        d->cfg.on_count = 1;
    if (d->cfg.off_ms < 0)
        d->cfg.off_ms = 0;
}

void a2k_det_obs_reset(struct a2k_det_obs *o) { memset(o, 0, sizeof *o); }

void a2k_det_obs_add(struct a2k_det_obs *o, const struct a2k_det_box *b)
{
    if (b->cls == A2K_DET_CLASS_PERSON) {
        if (!o->person || b->score > o->best.score) {
            o->best = *b;
            o->person = 1;
        }
    } else if (b->cls == A2K_DET_CLASS_HEAD) {
        o->heads++;
        if (b->score > o->head_score)
            o->head_score = b->score;
    }
}

void a2k_det_obs_add_z(struct a2k_det_obs *o, const struct a2k_det_box *b, unsigned hit)
{
    int i;
    if (b->cls != A2K_DET_CLASS_PERSON) {
        a2k_det_obs_add(o, b);
        return;
    }
    for (i = 0; i < A2K_ZONE_MAX; i++)
        if ((hit & (1u << i)) && (!(o->zmask & (1u << i)) || b->score > o->zbest[i].score)) {
            o->zbest[i] = *b;
            o->zmask |= 1u << i;
        }
    if (hit && (!o->person || b->score > o->best.score)) {
        o->best = *b;
        o->person = 1;
    }
}

void a2k_det_obs_finish(struct a2k_det_obs *o, int nz, int all)
{
    a2k_det_obs_finish_mask(o, nz >= A2K_ZONE_MAX ? (1u << A2K_ZONE_MAX) - 1 : (1u << nz) - 1, all);
}

void a2k_det_obs_finish_mask(struct a2k_det_obs *o, unsigned full, int all)
{
    if (all && (o->zmask & full) != full) {
        o->person = 0;
        memset(&o->best, 0, sizeof o->best);
    }
}

void a2k_zdet_init(a2k_zdet_t *zd, int n, const struct a2k_det_cfg *cfg)
{
    int i;
    memset(zd, 0, sizeof *zd);
    zd->n = n < 0 ? 0 : n > A2K_ZONE_MAX ? A2K_ZONE_MAX : n;
    for (i = 0; i < zd->n; i++)
        a2k_det_init(&zd->z[i], cfg);
}

unsigned a2k_zdet_update(a2k_zdet_t *zd, int64_t now_ms, const struct a2k_det_obs *obs, struct a2k_det_event ev[A2K_ZONE_MAX])
{
    unsigned changed = 0;
    int i;
    for (i = 0; i < zd->n; i++) {
        struct a2k_det_obs o;
        a2k_det_obs_reset(&o);
        o.heads = obs->heads;
        o.head_score = obs->head_score;
        if (obs->zmask & (1u << i)) {
            o.person = 1;
            o.best = obs->zbest[i];
        }
        if (a2k_det_update(&zd->z[i], now_ms, &o, &ev[i]))
            changed |= 1u << i;
    }
    return changed;
}

unsigned a2k_zdet_mask(const a2k_zdet_t *zd)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < zd->n; i++)
        if (zd->z[i].present)
            m |= 1u << i;
    return m;
}

int a2k_det_update(a2k_det_t *d, int64_t now_ms, const struct a2k_det_obs *obs, struct a2k_det_event *ev)
{
    d->inferences++;
    d->heads = obs->heads;
    d->head_score = obs->head_score;
    if (obs->person) {
        d->last_pos_ms = now_ms;
        d->last_box = obs->best;
        if (d->streak == 0 || obs->best.score > d->streak_best.score)
            d->streak_best = obs->best;
        if (d->streak < 1000000)
            d->streak++;
        if (!d->present && d->streak >= d->cfg.on_count) {
            d->present = 1;
            d->on_ms = now_ms;
            d->last_event = A2K_DET_EV_ON;
            d->last_event_ms = now_ms;
            memset(ev, 0, sizeof *ev);
            ev->type = A2K_DET_EV_ON;
            ev->t_ms = now_ms;
            ev->box = d->streak_best;
            return 1;
        }
        return 0;
    }
    d->streak = 0;
    if (d->present && now_ms - d->last_pos_ms >= d->cfg.off_ms) {
        d->present = 0;
        d->last_event = A2K_DET_EV_OFF;
        d->last_event_ms = now_ms;
        memset(ev, 0, sizeof *ev);
        ev->type = A2K_DET_EV_OFF;
        ev->t_ms = now_ms;
        ev->box = d->last_box;
        ev->present_ms = now_ms - d->on_ms;
        ev->absent_ms = now_ms - d->last_pos_ms;
        return 1;
    }
    return 0;
}

int a2k_det_state_format(char *buf, unsigned cap, const struct a2k_det_snapshot *s)
{
    const a2k_det_t *d = s->det;
    const struct a2k_det_box *b = &d->last_box;
    const char *ev = d->last_event == A2K_DET_EV_ON ? "on" : d->last_event == A2K_DET_EV_OFF ? "off" : "none";
    int on = d->present;
    int n = snprintf(buf, cap,
                     "version=1\nstatus=%s\nenabled=%d\nhz=%d\nthreshold=%d\nseq=%lld\nupdated=%lld\nuptime_ms=%lld\n"
                     "person=%d\nscore=%d\nclass=%d\nbox=%d,%d,%d,%d\nheads=%d\nhead_score=%d\n"
                     "last_event=%s\nlast_event_ms=%lld\nlast_event_time=%lld\ninferences=%lu\n",
                     s->status, s->enabled, s->hz, s->threshold, (long long)s->seq, (long long)s->now_epoch,
                     (long long)s->uptime_ms, on, on ? b->score : 0, on ? b->cls : 0, on ? b->x1 : 0, on ? b->y1 : 0,
                     on ? b->x2 : 0, on ? b->y2 : 0, d->heads, d->head_score, ev, (long long)d->last_event_ms,
                     (long long)d->last_event_epoch, d->inferences);
    int i;
    if (n < 0 || (unsigned)n >= cap)
        return -1;
    if (s->zn > 0) {
        int m = snprintf(buf + n, cap - (unsigned)n, "zones=%d\nzone_mode=%s\n", s->zn, s->zall ? "all" : "any");
        if (m < 0 || (unsigned)m >= cap - (unsigned)n)
            return -1;
        n += m;
        for (i = 0; i < s->zn && i < A2K_ZONE_MAX; i++) {
            m = snprintf(buf + n, cap - (unsigned)n, "zone_%s=%d\n", s->zname[i] ? s->zname[i] : "?", (s->zmask >> i) & 1u);
            if (m < 0 || (unsigned)m >= cap - (unsigned)n)
                return -1;
            n += m;
        }
    }
    return n;
}

int a2k_det_state_write(const char *path, const struct a2k_det_snapshot *s)
{
    char tmp[128], buf[768];
    int n;
    FILE *f;
    size_t pl = strlen(path);

    if (pl + 5 > sizeof tmp)
        return -1;
    memcpy(tmp, path, pl);
    memcpy(tmp + pl, ".tmp", 5);
    n = a2k_det_state_format(buf, sizeof buf, s);
    if (n < 0)
        return -1;
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n || fflush(f) != 0) {
        fclose(f);
        unlink(tmp);
        return -1;
    }
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int a2k_model_path_ok(const char *path)
{
    static const char dir[] = "/appfs/resource/model/";
    return path && strncmp(path, dir, sizeof dir - 1) == 0 && path[sizeof dir - 1] != '\0' && !strstr(path, "..");
}

void a2k_agewin_init(struct a2k_agewin *w, int window)
{
    int i;
    memset(w, 0, sizeof *w);
    w->window = window ? 1 : 0;
    for (i = 0; i < A2K_AGEWIN_S; i++)
        w->sec[i] = -1;
}

long long a2k_agewin_age(struct a2k_agewin *w, long long now_ms, long long raw)
{
    long long s = now_ms / 1000, m;
    int i, b = (int)(s % A2K_AGEWIN_S);
    if (!w->have || raw < w->all_min) {
        w->all_min = raw;
        w->have = 1;
    }
    if (!w->window)
        return raw - w->all_min;
    if (w->sec[b] != s) {
        w->sec[b] = s;
        w->min_raw[b] = raw;
    } else if (raw < w->min_raw[b]) {
        w->min_raw[b] = raw;
    }
    m = raw;
    for (i = 0; i < A2K_AGEWIN_S; i++)
        if (w->sec[i] >= 0 && w->sec[i] > s - A2K_AGEWIN_S && w->sec[i] <= s && w->min_raw[i] < m)
            m = w->min_raw[i];
    return raw - m;
}
