#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "night_logic.h"

/* Fixed parts of the rule (their reasons are in night_logic.h). The tunable ones are in night_tuning_t. */
#define PROBE_ABORT_AFTER    10   /* polls (1 s): before that the AE has not reacted to the LEDs going off */
#define PROBE_PASS_SAMPLES   10   /* consecutive low samples after the settling time */
#define EMA_START_POLLS     100   /* night average starts 10 s after the switch (the AE has converged) */
#define EMA_DIV             300   /* about 30 s time constant */
#define LAMP_DROP_PCT        50   /* a lamp: exposure below this % of the average ... */
#define LAMP_SAMPLES         30   /* ... for 3 s */
#define LAMP_GAP_POLLS      600   /* minimum gap (1 min) between two lamp probes, doubled per failure */
#define FLAP_WINDOW_POLLS  6000   /* back at night within 10 min of a return to day = a flap */
#define FLAP_MAX              3
#define FAIL_BACKOFF_MAX      3   /* periodic probe interval grows up to x8 */

static uint32_t polls(int ms)
{
    if (ms <= 0) return 0;
    return ((uint32_t)ms + NL_POLL_MS - 1) / NL_POLL_MS;
}

const char *nl_mode_name(nl_mode_t m)
{
    switch (m) {
    case NL_NIGHT:   return "night";
    case NL_PROBING: return "night-probe";
    default:         return "day";
    }
}

static uint32_t min_night_polls(const nl_state_t *st, const night_tuning_t *t)
{
    return polls(t->min_night_ms) << st->flap;
}

static uint32_t probe_interval_polls(const nl_state_t *st, const night_tuning_t *t)
{
    int f = st->fails < FAIL_BACKOFF_MAX ? st->fails : FAIL_BACKOFF_MAX;
    return polls(t->probe_ms) << f;
}

static void schedule_first_probe(nl_state_t *st, const night_tuning_t *t)
{
    uint32_t first = min_night_polls(st, t);
    uint32_t p = polls(t->probe_ms);
    st->next_probe = st->tick + (first > p ? first : p);
    st->lamp_ok_at = st->tick;
}

void nl_reset(nl_state_t *st, const night_tuning_t *t, int is_night)
{
    memset(st, 0, sizeof(*st));
    st->mode = is_night ? NL_NIGHT : NL_DAY;
    if (is_night) schedule_first_probe(st, t);
}

static uint64_t gain_q8(const nl_sample_t *s)
{
    return (uint64_t)s->sgain * s->isp / 256u;
}

int nl_saturated(const night_tuning_t *t, const nl_sample_t *s, const nl_limits_t *lim)
{
    if (s->hw_exp < t->trigger_hw_exp) return 0;
    if (lim && lim->sgain_max && lim->isp_max) {
        uint64_t gmax = (uint64_t)lim->sgain_max * lim->isp_max / 256u;
        if (gain_q8(s) * 100u < (uint64_t)t->gain_pct * gmax) return 0;
    }
    return 1;
}

static void enter(nl_state_t *st, const night_tuning_t *t, nl_mode_t m)
{
    st->mode = m;
    st->mode_since = st->tick;
    st->lock_until = st->tick + polls(t->lock_ms);
    st->confirm = 0;
    st->lamp_cnt = 0;
    st->lamp_pending = 0;
    st->switches++;
}

static void note(nl_state_t *st, char *why, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void note(nl_state_t *st, char *why, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->last, sizeof(st->last), fmt, ap);
    va_end(ap);
    if (why && cap) {
        strncpy(why, st->last, cap - 1);
        why[cap - 1] = '\0';
    }
}

static nl_action_t probe_fail(nl_state_t *st, const night_tuning_t *t, const nl_sample_t *s, char *why, size_t cap)
{
    if (st->fails < 30) st->fails++;
    st->mode = NL_NIGHT;
    st->probe_pass = 0;
    st->next_probe = st->tick + probe_interval_polls(st, t);
    uint32_t gap = LAMP_GAP_POLLS << (st->fails < 4 ? st->fails : 4);
    st->lamp_ok_at = st->tick + gap;
    st->lamp_cnt = 0;
    note(st, why, cap, "probe failed: hw_exp=%u >= %u in the ambient light, IR back on, next in %u s (fail %u)",
         s ? s->hw_exp : 0, t->day_hw_exp, (st->next_probe - st->tick) * NL_POLL_MS / 1000u, (uint32_t)st->fails);
    return NL_PROBE_END;
}

nl_action_t nl_step(nl_state_t *st, const night_tuning_t *t, const nl_sample_t *s, const nl_limits_t *lim,
                    int ir_active, char *why, size_t why_cap)
{
    if (why && why_cap) why[0] = '\0';
    st->tick++;

    if (st->mode == NL_PROBING) {
        if (!s) return probe_fail(st, t, s, why, why_cap);
        uint32_t pt = st->tick - st->probe_start;
        if (pt >= PROBE_ABORT_AFTER && s->hw_exp >= t->day_hw_exp)
            return probe_fail(st, t, s, why, why_cap);
        if (pt > polls(t->probe_settle_ms) && ++st->probe_pass >= PROBE_PASS_SAMPLES) {
            note(st, why, why_cap,
                 "IR off, ambient light: hw_exp=%u < %u for %u samples after %u s -> lit room, back to day",
                 s->hw_exp, t->day_hw_exp, (uint32_t)PROBE_PASS_SAMPLES, pt * NL_POLL_MS / 1000u);
            enter(st, t, NL_DAY);
            st->left_night_valid = 1;
            st->left_night_at = st->tick;
            st->fails = 0;
            return NL_GO_DAY;
        }
        return NL_NONE;
    }

    if (!s) {
        st->confirm = 0;
        st->lamp_cnt = 0;
        return NL_NONE;
    }
    /* The night averages run through the anti-flap lock too, so a lamp switched on right after the switch is seen. */
    if (st->mode == NL_NIGHT) {
        uint32_t since = st->tick - st->mode_since;
        if (since >= EMA_START_POLLS) {
            if (!st->ema_ok) {
                st->ema_exp = s->hw_exp;
                st->ema_ok = 1;
            } else {
                int64_t d = (int64_t)s->hw_exp - (int64_t)st->ema_exp;
                st->ema_exp = (uint32_t)((int64_t)st->ema_exp + d / EMA_DIV);
            }
        }
        if (ir_active) {
            if (st->ema_ok && (uint64_t)s->hw_exp * 100u < (uint64_t)st->ema_exp * LAMP_DROP_PCT) st->lamp_cnt++;
            else st->lamp_cnt = 0;
            /* Latched: a lamp switched on during the minimum dwell must still be probed when the dwell is over,
             * by when the slow average has followed the drop. */
            if (st->lamp_cnt >= LAMP_SAMPLES) st->lamp_pending = 1;
        }
    }
    if (st->tick < st->lock_until) {
        st->confirm = 0;
        return NL_NONE;
    }

    if (st->mode == NL_DAY) {
        if (!nl_saturated(t, s, lim)) {
            st->confirm = 0;
            return NL_NONE;
        }
        int need = t->confirm_samples > 0 ? t->confirm_samples : 1;
        if (++st->confirm < need) return NL_NONE;
        uint32_t flap_new = st->flap;
        if (st->left_night_valid && st->tick - st->left_night_at < FLAP_WINDOW_POLLS)
            flap_new = st->flap < FLAP_MAX ? (uint32_t)st->flap + 1 : FLAP_MAX;
        else
            flap_new = 0;
        enter(st, t, NL_NIGHT);
        st->flap = (int)flap_new;
        st->ema_ok = 0;
        st->fails = 0;
        schedule_first_probe(st, t);
        note(st, why, why_cap, "AE saturated: hw_exp=%u (>= %u) gain=%u/256 (>= %u %% of the AE maximum)",
             s->hw_exp, t->trigger_hw_exp, (uint32_t)gain_q8(s), t->gain_pct);
        return NL_GO_NIGHT;
    }

    /* NL_NIGHT */
    uint32_t elapsed = st->tick - st->mode_since;
    int eligible = elapsed >= min_night_polls(st, t);

    if (!ir_active) {
        /* No IR light of our own: the exposure is the ambient light, read it directly. */
        if (s->hw_exp < t->day_hw_exp) st->confirm++;
        else st->confirm = 0;
        int need = t->day_confirm_samples > 0 ? t->day_confirm_samples : 1;
        if (eligible && st->confirm >= need) {
            note(st, why, why_cap, "IR LEDs off by policy, hw_exp=%u < %u for %u samples -> back to day",
                 s->hw_exp, t->day_hw_exp, (uint32_t)st->confirm);
            enter(st, t, NL_DAY);
            st->left_night_valid = 1;
            st->left_night_at = st->tick;
            return NL_GO_DAY;
        }
        return NL_NONE;
    }

    if (!eligible) return NL_NONE;
    int periodic = st->tick >= st->next_probe;
    int lamp = st->lamp_pending && st->tick >= st->lamp_ok_at;
    if (!periodic && !lamp) return NL_NONE;

    st->mode = NL_PROBING;
    st->probe_start = st->tick;
    st->probe_pass = 0;
    st->lamp_pending = 0;
    st->probes++;
    if (lamp && !periodic)
        note(st, why, why_cap, "exposure fell to %u (< %u %% of the average %u): lamp? probe with the IR off",
             s->hw_exp, (uint32_t)LAMP_DROP_PCT, st->ema_exp);
    else
        note(st, why, why_cap, "periodic probe with the IR off (exposure %u, average %u, probe %u)", s->hw_exp,
             st->ema_exp, st->probes);
    return NL_PROBE_START;
}

void nl_status(const nl_state_t *st, const night_tuning_t *t, char *out, size_t cap)
{
    uint32_t next = 0, lock = 0;
    if (st->mode == NL_NIGHT && st->next_probe > st->tick)
        next = (st->next_probe - st->tick) * NL_POLL_MS / 1000u;
    if (st->lock_until > st->tick) lock = (st->lock_until - st->tick) * NL_POLL_MS / 1000u;
    (void)t;
    snprintf(out, cap, "mode=%s confirm=%d lock_s=%u probes=%u fails=%d flap=%d next_probe_s=%u switches=%u last=%s",
             nl_mode_name(st->mode), st->confirm, lock, st->probes, st->fails, st->flap, next, st->switches,
             st->last[0] ? st->last : "-");
}
