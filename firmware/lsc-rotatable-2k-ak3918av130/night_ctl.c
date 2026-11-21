/* The day / night engine (see night_ctl.h): pure code, everything hardware goes through the ops. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "night_ctl.h"

void a2k_night_defaults(a2k_night_cfg_t *c)
{
    c->mode = A2K_NIGHT_AUTO;
    c->ir_led_auto = 1;
    c->wdr = 1;
}

int a2k_night_parse_mode(const char *s)
{
    if (!strcmp(s, "auto"))
        return A2K_NIGHT_AUTO;
    if (!strcmp(s, "day"))
        return A2K_NIGHT_DAY;
    return !strcmp(s, "night") ? A2K_NIGHT_NIGHT : -1;
}

int a2k_night_parse_ir(const char *s)
{
    if (!strcmp(s, "auto"))
        return 1;
    return !strcmp(s, "off") ? 0 : -1;
}

int a2k_night_cfg_ok(const a2k_night_cfg_t *c)
{
    return c->mode >= A2K_NIGHT_AUTO && c->mode <= A2K_NIGHT_NIGHT && (c->ir_led_auto == 0 || c->ir_led_auto == 1) &&
           (c->wdr == 0 || c->wdr == 1);
}

static void say(a2k_night_ctl_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(a2k_night_ctl_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info.why, sizeof c->info.why, fmt, ap);
    va_end(ap);
    a2k_log("day/night: %s", c->info.why);
}

/* Vendor ISP calls are serialized (see vmu in night_ctl.h). */
static int vendor_isp_mode(a2k_night_ctl_t *c, int night)
{
    int rc;
    pthread_mutex_lock(&c->vmu);
    rc = c->ops->isp_mode(night);
    if (rc == 0 && c->cfg.wdr == 1 && c->ops->wdr) {
        /* the stock: keyframe requests (done by isp_mode), 100 ms, then WDR again (the mode switch resets it). Best effort. */
        c->ops->sleep_ms(A2K_NIGHT_SETTLE_MS);
        if (c->ops->wdr(1) != 0 && !c->wdr_warned) {
            c->wdr_warned = 1;
            a2k_log("day/night: re-applying WDR after the ISP mode switch failed (not logged again)");
        }
    }
    pthread_mutex_unlock(&c->vmu);
    return rc;
}

static int vendor_dn_init(a2k_night_ctl_t *c)
{
    int rc;
    pthread_mutex_lock(&c->vmu);
    rc = c->ops->dn_init();
    pthread_mutex_unlock(&c->vmu);
    return rc;
}

/* Every ISP / IR LED change: readings that began before it are stale, a pending verdict too. */
static void bump_gen(a2k_night_ctl_t *c)
{
    pthread_mutex_lock(&c->mu);
    c->gen++;
    c->seen_seq = c->seq; /* a published verdict not yet consumed described the old scene */
    c->kick = 1;
    pthread_mutex_unlock(&c->mu);
}

static void set_night(a2k_night_ctl_t *c, int night)
{
    pthread_mutex_lock(&c->mu);
    c->night = night;
    c->night_mirror = night;
    c->gen++;
    c->seen_seq = c->seq;
    pthread_mutex_unlock(&c->mu);
}

static void publish(a2k_night_ctl_t *c)
{
    c->info.valid = 1;
    c->info.night = c->night;
    c->info.mode = c->cfg.mode;
    c->info.ir_on = c->night && c->cfg.ir_led_auto && c->st.mode != NL_PROBING;
    if (c->ops->notify)
        c->ops->notify(&c->info);
}

/* The two sequences of the stock. Returns 0 when the filter moved (the ISP mode is best effort: logged, not fatal). */
static int apply(a2k_night_ctl_t *c, int night)
{
    int rc = 0;
    bump_gen(c);
    if (night) {
        if (c->cfg.ir_led_auto && c->ops->led(1) != 0)
            a2k_log("day/night: IR LED did not switch on");
        c->ops->sleep_ms(A2K_NIGHT_SETTLE_MS);
        if (vendor_isp_mode(c, 1) != 0)
            a2k_log("day/night: ISP night mode failed");
        rc = c->ops->ircut(0);
        if (rc != 0) { /* the filter did not move: no IR LED lit and no night ISP set under a filter that is still in */
            (void)c->ops->led(0);
            if (vendor_isp_mode(c, 0) != 0)
                a2k_log("day/night: ISP day mode failed");
        }
    } else {
        rc = c->ops->ircut(1);
        if (c->ops->led(0) != 0)
            a2k_log("day/night: IR LED did not switch off");
        c->ops->sleep_ms(A2K_NIGHT_SETTLE_MS);
        if (vendor_isp_mode(c, 0) != 0)
            a2k_log("day/night: ISP day mode failed");
    }
    if (rc != 0)
        a2k_log("day/night: the IR-cut pulse failed");
    bump_gen(c);
    return rc;
}

void a2k_night_ctl_init(a2k_night_ctl_t *c, const a2k_night_cfg_t *cfg, const a2k_night_ops_t *ops)
{
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->vmu, NULL);
    c->cfg = *cfg;
    c->ops = ops;
    c->ir_req = -1;
    /* the shared policy's own defaults (night.h), the lock being the stock's 60 s */
    c->tun.trigger_hw_exp = NIGHT_DEF_TRIGGER_HW_EXP;
    c->tun.gain_pct = NIGHT_DEF_GAIN_PCT;
    c->tun.day_hw_exp = NIGHT_DEF_DAY_HW_EXP;
    c->tun.confirm_samples = NIGHT_DEF_CONFIRM_SAMPLES;
    c->tun.day_confirm_samples = A2K_NIGHT_DAY_CONFIRM_NOIR; /* only used without IR LED */
    c->tun.lock_ms = NIGHT_DEF_LOCK_MS;
    c->tun.min_night_ms = NIGHT_DEF_MIN_NIGHT_MS;
    c->tun.probe_ms = A2K_NIGHT_PROBE_MS;
    c->tun.probe_settle_ms = A2K_NIGHT_PROBE_SETTLE_MS;
    c->tun.override = NIGHT_MODE_AUTO;
    c->lim.sgain_max = 256;
    c->lim.isp_max = 256;
}

int a2k_night_ctl_start(a2k_night_ctl_t *c, int64_t now_ms)
{
    int want_night = c->cfg.mode == A2K_NIGHT_NIGHT;
    if (c->ops->init() != 0) {
        a2k_log("day/night: the pins cannot be opened, no day/night (the filter and the IR LED are left alone)");
        return -1;
    }
    /* the filter position after a crash is unknown: move it to a known one first (the stock does so at init) */
    if (apply(c, want_night) != 0) {
        (void)c->ops->release();
        a2k_log("day/night: the first filter move failed");
        return -1;
    }
    set_night(c, want_night);
    c->last_switch_ms = now_ms;
    c->held_valid = 0;
    c->read_due = 0;
    if (c->cfg.mode == A2K_NIGHT_AUTO) {
        if (vendor_dn_init(c) != 0) {
            c->cfg.mode = A2K_NIGHT_DAY; /* no measurement: stay in the known day state */
            c->info.auto_off = 1;
            say(c, "the vendor day/night parameters were refused, automatic mode off, staying in day");
        } else {
            nl_reset(&c->st, &c->tun, 0);
            /* no switch in the first minute: the start counts as a switch for the 60 s rule */
            c->st.lock_until = (NIGHT_DEF_LOCK_MS + NL_POLL_MS - 1) / NL_POLL_MS;
            say(c, "automatic, start in day (filter in), vendor decision read every %d s", A2K_NIGHT_READ_EVERY / 10);
        }
    } else {
        say(c, "forced %s", want_night ? "night" : "day");
    }
    c->info.last_switch_epoch = c->ops->epoch ? c->ops->epoch() : 0;
    publish(c);
    return 0;
}

static void resync(a2k_night_ctl_t *c)
{
    nl_reset(&c->st, &c->tun, c->night);
    c->st.lock_until = c->st.tick + (NIGHT_DEF_LOCK_MS + NL_POLL_MS - 1) / NL_POLL_MS;
}

/* The IR LED of a probe: off for the probe, back on after it. Never touched when the setting says no IR LED. */
static void probe_led(a2k_night_ctl_t *c, int on)
{
    bump_gen(c);
    if (c->cfg.ir_led_auto && c->ops->led(on) != 0)
        a2k_log("day/night: IR LED did not switch %s for the probe", on ? "back on" : "off");
    bump_gen(c);
}

int a2k_night_ctl_busy(const a2k_night_ctl_t *c, int64_t now_ms)
{
    return c->probing || now_ms < c->busy_until_ms;
}

void a2k_night_ctl_set_async(a2k_night_ctl_t *c) { c->async = 1; }

int a2k_night_ctl_take_kick(a2k_night_ctl_t *c)
{
    int k;
    pthread_mutex_lock(&c->mu);
    k = c->kick;
    c->kick = 0;
    pthread_mutex_unlock(&c->mu);
    return k;
}

void a2k_night_ctl_read_begin(a2k_night_ctl_t *c, a2k_night_rd_t *rd)
{
    pthread_mutex_lock(&c->mu);
    rd->gen = c->gen;
    rd->cur_night = c->night_mirror;
    pthread_mutex_unlock(&c->mu);
}

void a2k_night_ctl_read_end(a2k_night_ctl_t *c, const a2k_night_rd_t *rd, int rc, int want_night, int64_t ts_ms)
{
    pthread_mutex_lock(&c->mu);
    if (rd->gen == c->gen) {
        c->v_ok = rc == 0;
        c->v_want = want_night != 0;
        c->v_ts = ts_ms;
        c->seq++;
    }
    pthread_mutex_unlock(&c->mu);
}

void a2k_night_ctl_read_once(a2k_night_ctl_t *c)
{
    a2k_night_rd_t rd;
    int want = 0, rc;
    pthread_mutex_lock(&c->vmu); /* vendor lock first (waits out an ISP switch), then the state snapshot */
    a2k_night_ctl_read_begin(c, &rd);
    rc = c->ops->dn_level(rd.cur_night, &want); /* blocks 1..1.5 s: only vmu held, never mu */
    pthread_mutex_unlock(&c->vmu);
    a2k_night_ctl_read_end(c, &rd, rc, want, c->ops->now_ms ? c->ops->now_ms() : 0);
}

/* The sample of the policy for this tick. Outside a probe: the held vendor verdict. During a probe (IR LED off, so the
 * verdict is the ambient light and cannot be fooled by our own IR): "dark" at the first night verdict after the grace time,
 * or when the settling time is over and the last A2K_NIGHT_PROBE_NEED readings were not all day; "bright" otherwise (the
 * policy aborts a probe on the first dark sample after 1 s, so the AE settling must read bright, never absent). */
static const nl_sample_t *tick_sample(a2k_night_ctl_t *c, nl_sample_t *probe_s)
{
    uint32_t pt;
    if (c->st.mode == NL_NIGHT && c->cfg.ir_led_auto) {
        /* Night with our own IR light on: the vendor verdict is not about the room (hardware 2026-11-15: "day" in a dark room;
         * 2026-11-18: it flickers, and each "day" run, read as exposure 0, fired the policy's lamp detector: three useless
         * IR-off probes in 15 min). It is not fed to the policy at all: a constant "dark" sample, so the lamp detector (a drop
         * below half of the average) can never fire and the only trigger left is the periodic probe with its back-off. */
        *probe_s = c->held;
        probe_s->hw_exp = 16384u;
        probe_s->sgain = probe_s->isp = 256;
        probe_s->lum = 0;
        return probe_s;
    }
    if (c->st.mode != NL_PROBING)
        return c->held_valid ? &c->held : NULL;
    pt = c->st.tick + 1 - c->st.probe_start;
    *probe_s = c->held;
    probe_s->hw_exp = (c->probe_night_seen || (pt * NL_POLL_MS >= (uint32_t)c->tun.probe_settle_ms &&
                                               c->probe_streak < A2K_NIGHT_PROBE_NEED)) ? 16384u : 0u;
    return probe_s;
}

/* One counted reading of the vendor decision. past_grace: the reading is old enough into the probe (the AE has settled). */
static void take_reading(a2k_night_ctl_t *c, int ok, int want, int past_grace)
{
    if (ok) {
        /* the vendor decision as a sample of the 1080p policy: "dark" = saturated AE (ceiling exposure, maximum gain) */
        c->held.hw_exp = want ? 16384u : 0u;
        c->held.sgain = c->held.isp = 256;
        c->held.lum = 0;
        c->held_valid = 1;
        if (c->st.mode == NL_PROBING && past_grace) {
            if (want) {
                c->probe_night_seen = 1;
                c->probe_streak = 0;
            } else {
                c->probe_streak++;
            }
        }
    } else {
        c->held_valid = 0; /* not stable, not ready: no sample, the confirmations restart (a probe just does not count it) */
    }
}

void a2k_night_ctl_ir_request(a2k_night_ctl_t *c, int ir_auto)
{
    pthread_mutex_lock(&c->mu);
    c->ir_req = ir_auto ? 1 : 0;
    pthread_mutex_unlock(&c->mu);
}

void a2k_night_ctl_ir_apply(a2k_night_ctl_t *c)
{
    int v;
    pthread_mutex_lock(&c->mu);
    v = c->ir_req;
    c->ir_req = -1;
    pthread_mutex_unlock(&c->mu);
    if (v < 0 || v == c->cfg.ir_led_auto)
        return;
    c->cfg.ir_led_auto = v;
    /* night and not in an IR-off probe: the LED follows the new setting at once (a probe restores it by itself) */
    if (c->night && c->st.mode != NL_PROBING) {
        bump_gen(c);
        if (c->ops->led(v) != 0)
            a2k_log("day/night: IR LED did not switch %s", v ? "on" : "off");
        bump_gen(c);
    }
    if (c->cfg.mode == A2K_NIGHT_AUTO && !c->info.auto_off)
        resync(c);
    say(c, "ir_led %s (runtime)", v ? "auto" : "off");
    publish(c);
}

void a2k_night_ctl_poll(a2k_night_ctl_t *c, int64_t now_ms)
{
    nl_action_t a;
    nl_sample_t probe_s;
    char why[160];
    a2k_night_ctl_ir_apply(c);
    if (c->cfg.mode != A2K_NIGHT_AUTO || c->info.auto_off)
        return;
    c->last_tick_ms = now_ms;
    if (c->async) {
        int have = 0, ok = 0, want = 0;
        int64_t ts = 0;
        pthread_mutex_lock(&c->mu);
        if (c->seq != c->seen_seq) { /* each reading is consumed once */
            c->seen_seq = c->seq;
            have = 1;
            ok = c->v_ok;
            want = c->v_want;
            ts = c->v_ts;
        }
        pthread_mutex_unlock(&c->mu);
        if (have)
            take_reading(c, ok, want, ts ? ts - c->probe_start_ms >= A2K_NIGHT_PROBE_GRACE_MS : 0);
    } else if (c->read_due-- <= 0) {
        int want = 0, rc;
        c->read_due = A2K_NIGHT_READ_EVERY - 1;
        pthread_mutex_lock(&c->vmu);
        rc = c->ops->dn_level(c->night, &want);
        pthread_mutex_unlock(&c->vmu);
        take_reading(c, rc == 0, want,
                     (c->st.tick + 1 - c->st.probe_start) * NL_POLL_MS >= A2K_NIGHT_PROBE_GRACE_MS);
    }
    /* ir_active: our own IR LED lights the scene in night mode, so the verdict cannot be trusted for "day" there: the policy
     * then probes with the LED off. Without an IR LED (ir_led = off) the verdict is read directly. */
    a = nl_step(&c->st, &c->tun, tick_sample(c, &probe_s), &c->lim, c->cfg.ir_led_auto, why, sizeof why);
    if (a == NL_PROBE_START) {
        c->probe_streak = c->probe_night_seen = 0;
        c->probe_start_ms = now_ms;
        c->probing = 1;
        c->busy_until_ms = now_ms + A2K_NIGHT_BUSY_MS;
        probe_led(c, 0);
        /* the only reason is the schedule: under our IR light nothing else tells a lit room from a dark one */
        say(c, "IR-off probe %u (scheduled; the vendor verdict is not usable with the IR on; the interval doubles per failed probe, up to 40 min)",
            c->st.probes);
        publish(c);
        return;
    }
    if (a == NL_PROBE_END) {
        c->probing = 0;
        c->busy_until_ms = now_ms + A2K_NIGHT_BUSY_MS; /* the AE flash after the IR comes back */
        probe_led(c, 1);
        c->held_valid = 0; /* the readings of the probe say nothing about the lit scene */
        c->read_due = 0;
        say(c, "%s", why);
        publish(c);
        return;
    }
    if (a != NL_GO_NIGHT && a != NL_GO_DAY)
        return;
    c->probing = 0;
    c->busy_until_ms = now_ms + A2K_NIGHT_BUSY_MS;
    {
        int to_night = a == NL_GO_NIGHT;
        if (now_ms - c->last_switch_ms < A2K_NIGHT_MIN_SWITCH_MS) { /* the policy lock makes this unreachable: a guard */
            a2k_log("day/night: switch refused, under %d s since the last one", A2K_NIGHT_MIN_SWITCH_MS / 1000);
            if (c->night)
                probe_led(c, 1); /* a probe that was ending */
            resync(c);
            return;
        }
        if (apply(c, to_night) != 0) {
            (void)c->ops->release();
            if (++c->fails >= A2K_NIGHT_MAX_FAILS) {
                c->info.auto_off = 1;
                if (c->night && apply(c, 0) == 0)
                    set_night(c, 0);
                if (c->night)
                    probe_led(c, 1); /* still in night (the filter did not move back): the probe's LED-off must not stay */
                say(c, "%d failed switches in a row, automatic mode off, back to day", c->fails);
            } else {
                a2k_log("day/night: switch to %s failed (%d), staying in %s", to_night ? "night" : "day", c->fails,
                        c->night ? "night" : "day");
                if (c->night)
                    probe_led(c, 1); /* the failed day switch (or its probe) left the IR LED off */
                resync(c);
            }
            publish(c);
            return;
        }
        c->fails = 0;
        set_night(c, to_night);
        c->last_switch_ms = now_ms;
        c->info.switches++;
        c->info.last_switch_epoch = c->ops->epoch ? c->ops->epoch() : 0;
        if (to_night)
            say(c, "night: the vendor day/night decision held %d s", NIGHT_DEF_CONFIRM_SAMPLES / 10);
        else
            say(c, "day: %s", why);
        publish(c);
    }
}

void a2k_night_ctl_stop(a2k_night_ctl_t *c)
{
    if (c->night && apply(c, 0) == 0)
        set_night(c, 0);
    (void)c->ops->led(0);
    (void)c->ops->release();
    if (c->info.valid) {
        c->info.night = c->night;
        c->info.ir_on = 0;
    }
}
