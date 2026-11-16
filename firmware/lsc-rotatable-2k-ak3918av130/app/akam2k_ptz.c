/* PTZ controller of the Rotatable 2K own app (akam2k_ptz.h): one thread, one pending command, hal_ptz underneath. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "akam2k_hal.h"
#include "akam2k_log.h"
#include "akam2k_ptz.h"

enum { K_NONE, K_STEP, K_HOME, K_PARK, K_NUDGE, K_RECAL };
#define NUDGE_MAX 4000 /* the sum of queued nudges is bounded (the soft range clips it anyway) */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t tid;
    int running, stop, starting;
    struct { int kind, a, b; } pend;
    a2k_ptz_info_t info;
    int cal_mode, privacy_opt;
    hal_ptz_t *hp;
    hal_ptz_ops_t ops;
    int custom_ops;
    char conf_path[96], pos_path[96];
    unsigned flush_ms, scene_ms, scene_confirm_ms, scene_gap_ms;
    a2k_ptz_scene_t scene;
    int scene_on;          /* ptz_scene_check: boot check + saves (the drop always runs) */
    int scene_dirty;       /* a known position without a saved reference: save it when quiet */
    int scene_clean;       /* the stored reference is known to be invalid / absent: no need to drop again */
    int scene_matched;     /* this boot declared the saved position (its reference is still valid on disk) */
    int scene_tries;
    int scene_cand;        /* a candidate view was captured (waiting for the confirmation) */
    int scene_fresh;       /* a calibration ran since the last reference write */
    int stop_commit;       /* a2k_ptz_stop asked the thread to write the reference before it ends */
    int64_t cand_ms;
    int busy;              /* the thread runs a command */
    int64_t last_end_ms, last_save_ms;
    void (*notify)(const a2k_ptz_info_t *);
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .flush_ms = A2K_PTZ_FLUSH_MS,
        .conf_path = A2K_PTZ_CONF, .pos_path = A2K_PTZ_POS,
        .scene_ms = A2K_PTZ_SCENE_MS, .scene_confirm_ms = A2K_PTZ_SCENE_CONFIRM_MS, .scene_gap_ms = A2K_PTZ_SCENE_GAP_MS };
static volatile int g_privacy;

void a2k_ptz_defaults(a2k_ptz_cfg_t *c)
{
    c->cal = HAL_PTZ_CAL_AUTO;
    c->park_pan = c->park_tilt = -1;
    c->privacy = 0;
    c->scene_check = 1;
}

int a2k_ptz_parse_cal(const char *s)
{
    return !strcmp(s, "auto") ? HAL_PTZ_CAL_AUTO : !strcmp(s, "off") ? HAL_PTZ_CAL_OFF : -1;
}

int a2k_ptz_parse_park(const char *s, int *pan, int *tilt)
{
    long v[2] = { 0, 0 };
    int i, n;
    for (i = 0; i < 2; i++) {
        for (n = 0; *s >= '0' && *s <= '9'; s++, n++)
            if ((v[i] = v[i] * 10 + (*s - '0')) > 65535)
                return -1;
        if (!n || (i == 0 ? *s++ != ':' : *s != '\0'))
            return -1;
    }
    *pan = (int)v[0];
    *tilt = (int)v[1];
    return 0;
}

const char *a2k_ptz_state_name(int state)
{
    switch (state) {
    case HAL_PTZ_HOME: return "home";
    case HAL_PTZ_PARKED: return "parked";
    case HAL_PTZ_MOVED: return "moved";
    case HAL_PTZ_CALIBRATING: return "calibrating";
    default: return "unknown";
    }
}

static void log_line(const char *line) { a2k_log("%s", line); }

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void a2k_ptz_set_scene(const a2k_ptz_scene_t *s)
{
    memset(&G.scene, 0, sizeof G.scene);
    if (s)
        G.scene = *s;
}

void a2k_ptz_test_set_scene_timing(unsigned scene_ms, unsigned confirm_ms, unsigned gap_ms)
{
    G.scene_ms = scene_ms ? scene_ms : A2K_PTZ_SCENE_MS;
    G.scene_confirm_ms = confirm_ms ? confirm_ms : A2K_PTZ_SCENE_CONFIRM_MS;
    G.scene_gap_ms = gap_ms;
}

static int stopping(void) { return __atomic_load_n(&G.stop, __ATOMIC_SEQ_CST); }

/* hal_ptz ops: the scene check before the first motor command, and the drop before every calibration */
static int hook_cold_check(uint32_t *pan, uint32_t *tilt)
{
    if (!G.scene_on || !G.scene.boot_check)
        return 0;
    G.scene_matched = G.scene.boot_check(pan, tilt, stopping) == 1;
    return G.scene_matched;
}

/* 0 ok, -1: the stored reference could not be invalidated (the caller must not move: a stale reference would outlive the new position) */
static int hook_drop(void)
{
    if (G.scene.drop && !G.scene_clean) { /* PTZ thread only */
        if (G.scene.drop() != 0) {
            a2k_log("ptz: the scene reference cannot be invalidated: refusing to move (fail closed)");
            return -1;
        }
        G.scene_clean = 1;
    }
    return 0;
}

/* Publish the state and the privacy flag after a change; `moving` forces "moved" with no position (the camera leaves home /
 * park at once, so the privacy ends with the first motion). */
static void publish(int state, int moving)
{
    a2k_ptz_info_t i;
    void (*fn)(const a2k_ptz_info_t *);
    memset(&i, 0, sizeof i);
    i.state = state;
    if (!moving && G.hp && hal_ptz_pos(G.hp, &i.pan, &i.tilt))
        i.known = 1;
    pthread_mutex_lock(&G.mu);
    G.info = i;
    g_privacy = G.privacy_opt && state == HAL_PTZ_PARKED;
    fn = G.notify;
    pthread_mutex_unlock(&G.mu);
    if (fn)
        fn(&i);
}

static void run_cmd(int kind, int a, int b)
{
    static const char *const names[] = { "none", "step", "home", "park", "nudge", "recalibrate" };
    static const char *const dirs[] = { " up", " right", " down", " left" };
    int rc = HAL_PTZ_EREFUSED;
    a2k_ptz_info_t i;
    if (kind != K_RECAL && hook_drop() != 0) { /* the position is untrusted from now on: a stored reference must never be used for it */
        a2k_log("ptz: %s refused: the scene reference cannot be invalidated", names[kind]);
        return;
    }
    publish(kind == K_RECAL ? HAL_PTZ_CALIBRATING : HAL_PTZ_MOVED, 1);
    switch (kind) {
    case K_RECAL: rc = hal_ptz_recalibrate(G.hp); break;
    case K_STEP: rc = hal_ptz_step(G.hp, a); break;
    case K_HOME: rc = hal_ptz_home(G.hp); break;
    case K_PARK: rc = hal_ptz_park(G.hp); break;
    case K_NUDGE: rc = hal_ptz_nudge(G.hp, a, b); break;
    }
    if (kind == K_RECAL && rc == HAL_PTZ_OK) {
        pthread_mutex_lock(&G.mu);
        G.scene_fresh = 1; /* a full calibration ran: the next reference restarts the matched-boots counter */
        pthread_mutex_unlock(&G.mu);
    }
    publish(hal_ptz_state(G.hp), 0);
    a2k_ptz_info(&i);
    a2k_log("ptz: %s%s -> %s, %s, pan %u tilt %u", names[kind], kind == K_STEP && a >= 0 && a < 4 ? dirs[a] : "-",
            rc == HAL_PTZ_OK ? "ok" : rc == HAL_PTZ_ECANCEL ? "cancelled" : rc == HAL_PTZ_EREFUSED ? "refused (limit or unknown position)" : "FAILED",
            a2k_ptz_state_name(i.state), (unsigned)i.pan, (unsigned)i.tilt);
}

/* Under G.mu, on the PTZ thread, idle (nothing queued, not busy). Two steps, so that the thumbnail and the position belong to the same moment:
 * (1) capture: the candidate view is kept in RAM once the motor has been quiet for A2K_PTZ_SCENE_MS (the AE has settled);
 * (2) commit, A2K_PTZ_SCENE_CONFIRM_MS later (or at the shutdown): the driver counters are re-read (they must still equal the cached position)
 *     and the scene module writes the candidate only if the current view still matches it. Anything else: nothing is written. The write runs
 *     with the lock held: a command can only be queued after it (it would drop the reference again at its start). */
static void scene_fail_locked(const char *why)
{
    G.scene_cand = 0;
    if (++G.scene_tries >= 3) {
        G.scene_dirty = 0;
        a2k_log("ptz: scene reference not saved (%s), the next cold boot calibrates", why);
    }
}

static void scene_capture_locked(void)
{
    int rc;
    if (!G.scene.capture || !G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !G.info.known) {
        G.scene_dirty = 0;
        return;
    }
    rc = G.scene.capture();
    if (rc == 0) {
        G.scene_cand = 1;
        G.cand_ms = mono_ms();
    } else if (rc > 0) {
        G.scene_dirty = 0; /* nothing worth saving: no log noise, no write */
    } else {
        scene_fail_locked("no fresh frame or unknown day/night mode");
    }
}

static void scene_commit_locked(const char *why)
{
    a2k_ptz_info_t i = G.info;
    int rc;
    if (!G.scene.commit || !G.scene_on || G.busy || G.pend.kind != K_NONE || !i.known || !G.scene_cand) {
        G.scene_dirty = G.scene_cand = 0;
        return;
    }
    if (hal_ptz_verify_pos(G.hp) != 0) {
        G.scene_dirty = G.scene_cand = 0;
        a2k_log("ptz: scene reference not saved: the driver counters no longer match the position %u/%u", (unsigned)i.pan, (unsigned)i.tilt);
        return;
    }
    rc = G.scene.commit(i.pan, i.tilt, G.scene_fresh);
    if (rc == 0) {
        G.scene_dirty = G.scene_cand = 0;
        G.scene_clean = 0;
        G.scene_fresh = 0;
        G.last_save_ms = mono_ms();
        a2k_log("ptz: scene reference saved (%s), position %u/%u", why, (unsigned)i.pan, (unsigned)i.tilt);
    } else if (rc > 0) {
        G.scene_dirty = G.scene_cand = 0;
    } else {
        scene_fail_locked("the view changed or the write failed");
    }
}

static void *ptz_thread(void *arg)
{
    a2k_ptz_info_t i;
    (void)arg;
    publish(G.cal_mode == HAL_PTZ_CAL_OFF ? HAL_PTZ_UNKNOWN : HAL_PTZ_CALIBRATING, 1);
    if (hal_ptz_start(G.hp, G.cal_mode) != 0)
        a2k_log("ptz: not available");
    publish(hal_ptz_state(G.hp), 0);
    a2k_ptz_info(&i);
    a2k_log("ptz: ready, state %s%s", a2k_ptz_state_name(i.state), i.known ? "." : " (position unknown)");
    pthread_mutex_lock(&G.mu);
    G.starting = 0; /* commands are accepted from now on */
    G.last_end_ms = mono_ms();
    G.scene_dirty = G.scene_on && G.scene.capture && i.known && !G.scene_matched; /* calibrated or adopted: a reference is due */
    G.scene_fresh = hal_ptz_cal_count(G.hp) > 0; /* a full calibration ran in this start: the matched-boots counter restarts (an adoption keeps it) */
    if (G.scene_matched)
        G.scene_clean = 0; /* the stored reference is valid on disk */
    for (;;) {
        int kind, a, b;
        while (!G.stop && G.pend.kind == K_NONE) {
            int hdirty = hal_ptz_dirty(G.hp), sdirty = G.scene_dirty;
            if (hdirty || sdirty) {
                struct timespec ts;
                unsigned wait_ms = G.flush_ms;
                if (!hdirty) {
                    if (G.scene_cand) {
                        wait_ms = G.scene_confirm_ms;
                    } else {
                        int64_t gap = G.last_save_ms ? G.last_save_ms + G.scene_gap_ms - mono_ms() : 0;
                        wait_ms = G.scene_ms;
                        if (gap > (int64_t)wait_ms)
                            wait_ms = (unsigned)gap;
                    }
                }
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += wait_ms / 1000;
                ts.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                if (pthread_cond_timedwait(&G.cv, &G.mu, &ts) == ETIMEDOUT && !G.stop && G.pend.kind == K_NONE) {
                    if (hdirty) {
                        pthread_mutex_unlock(&G.mu);
                        hal_ptz_flush(G.hp); /* quiet for a while: ONE write of the final position */
                        pthread_mutex_lock(&G.mu);
                    } else if (G.scene_cand) {
                        scene_commit_locked("idle");
                    } else {
                        scene_capture_locked();
                    }
                }
            } else {
                pthread_cond_wait(&G.cv, &G.mu);
            }
        }
        if (G.stop)
            break;
        kind = G.pend.kind;
        a = G.pend.a;
        b = G.pend.b;
        G.pend.kind = K_NONE;
        hal_ptz_mark(G.hp); /* under the lock: a cancel from submit() after this point aborts the command at its start */
        G.busy = 1;
        if (kind == K_RECAL)
            G.starting = 1; /* no command is accepted (and none cancels it) while the calibration runs */
        pthread_mutex_unlock(&G.mu);
        run_cmd(kind, a, b);
        pthread_mutex_lock(&G.mu);
        G.busy = 0;
        G.starting = 0;
        G.last_end_ms = mono_ms();
        G.scene_dirty = G.scene_on && G.scene.capture != NULL;
        G.scene_cand = 0;
        G.scene_tries = 0;
    }
    if (G.stop_commit) /* a2k_ptz_stop: a settled candidate waits for its write; this thread owns the driver */
        scene_commit_locked("shutdown");
    pthread_mutex_unlock(&G.mu);
    hal_ptz_close(G.hp);
    return NULL;
}

void a2k_ptz_test_set(const hal_ptz_ops_t *ops, const char *conf_path, const char *pos_path, unsigned flush_ms)
{
    if (ops) {
        G.ops = *ops;
        G.custom_ops = 1;
    } else {
        G.custom_ops = 0;
    }
    snprintf(G.conf_path, sizeof G.conf_path, "%s", conf_path ? conf_path : A2K_PTZ_CONF);
    snprintf(G.pos_path, sizeof G.pos_path, "%s", pos_path ? pos_path : A2K_PTZ_POS);
    G.flush_ms = flush_ms ? flush_ms : A2K_PTZ_FLUSH_MS;
}

int a2k_ptz_start(const a2k_ptz_cfg_t *cfg)
{
    hal_ptz_desc_t d = a2k_ptz_desc;
    char conf[1024], err[96] = { 0 };
    int n;
    pthread_attr_t at;
    if (G.running)
        return 0;
    if (!G.custom_ops)
        G.ops = *hal_ptz_linux_ops(d.dev);
    G.ops.log = log_line;
    G.ops.cold_check = hook_cold_check;
    G.ops.scene_drop = hook_drop;
    errno = 0;
    n = G.ops.load_file(G.conf_path, conf, sizeof conf);
    if (n < 0 && errno != ENOENT) {
        a2k_log("ptz: cannot read %s (%s): PTZ off", G.conf_path, strerror(errno));
        return -1;
    }
    if (cfg->park_pan >= 0 && cfg->park_tilt >= 0) {
        d.pan_park = (uint32_t)cfg->park_pan;
        d.tilt_park = (uint32_t)cfg->park_tilt;
    }
    G.hp = hal_ptz_new(&d, &G.ops, n >= 0 ? conf : NULL, G.pos_path, err, sizeof err);
    if (!G.hp) {
        a2k_log("ptz: bad %s: %s: PTZ off (the motor is never touched)", G.conf_path, err);
        return -1;
    }
    G.stop = 0;
    G.pend.kind = K_NONE;
    G.starting = 1;
    G.cal_mode = cfg->cal;
    G.scene_on = cfg->scene_check;
    G.scene_dirty = G.scene_matched = G.scene_clean = G.scene_tries = G.busy = G.scene_cand = G.scene_fresh = G.stop_commit = 0;
    G.last_save_ms = 0;
    G.privacy_opt = cfg->privacy;
    memset(&G.info, 0, sizeof G.info);
    g_privacy = 0;
    d = *hal_ptz_desc(G.hp);
    a2k_log("ptz: soft range pan %u..%u tilt %u..%u, home %u/%u, park %u/%u, speed %u, ptz_cal %s, ptz_privacy %s, ptz_scene_check %s",
            (unsigned)d.pan_min, (unsigned)d.pan_max, (unsigned)d.tilt_min, (unsigned)d.tilt_max, (unsigned)d.pan_home,
            (unsigned)d.tilt_home, (unsigned)d.pan_park, (unsigned)d.tilt_park, (unsigned)d.speed,
            cfg->cal == HAL_PTZ_CAL_OFF ? "off" : "auto", cfg->privacy ? "on" : "off", cfg->scene_check && G.scene.boot_check ? "on" : "off");
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&G.tid, &at, ptz_thread, NULL) != 0) {
        pthread_attr_destroy(&at);
        a2k_log("ptz: thread start failed: PTZ off");
        hal_ptz_free(G.hp);
        G.hp = NULL;
        return -1;
    }
    pthread_attr_destroy(&at);
    G.running = 1;
    return 0;
}

void a2k_ptz_stop(void)
{
    hal_ptz_t *hp;
    if (!G.running)
        return;
    pthread_mutex_lock(&G.mu);
    /* A clean stop at a known, settled position: the reference is saved before the thread ends (a frame taken right after a motion is not). */
    G.stop_commit = G.scene_dirty && G.scene_cand && !G.starting && !G.busy && G.pend.kind == K_NONE &&
                    mono_ms() - G.cand_ms >= A2K_PTZ_SCENE_QUIET_STOP_MS;
    G.stop = 1;
    if (G.hp && !G.stop_commit)
        hal_ptz_cancel(G.hp); /* the running motion (or calibration) ends within about 50 ms (idle: nothing to cancel, the driver is read by the thread) */
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mu);
    pthread_join(G.tid, NULL);
    pthread_mutex_lock(&G.mu); /* a2k_ptz_range / submit (the tracker, the detection thread) read these under the lock */
    G.running = 0;
    g_privacy = 0;
    hp = G.hp;
    G.hp = NULL;
    pthread_mutex_unlock(&G.mu);
    hal_ptz_free(hp);
}

/* Queue under the lock. Nudges add up and never cancel; any other command replaces the pending one and cancels the running motion. */
static int submit(int kind, int a, int b)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (G.running && !G.stop && !G.starting) {
        if (kind == K_NUDGE && G.pend.kind != K_NONE && G.pend.kind != K_NUDGE) {
            pthread_mutex_unlock(&G.mu); /* a queued user command (step, home, park) is never replaced by a nudge (tracking) */
            return -1;
        }
        if (kind == K_NUDGE && G.pend.kind == K_NUDGE) {
            G.pend.a += a;
            G.pend.b += b;
            G.pend.a = G.pend.a > NUDGE_MAX ? NUDGE_MAX : G.pend.a < -NUDGE_MAX ? -NUDGE_MAX : G.pend.a;
            G.pend.b = G.pend.b > NUDGE_MAX ? NUDGE_MAX : G.pend.b < -NUDGE_MAX ? -NUDGE_MAX : G.pend.b;
        } else {
            G.pend.kind = kind;
            G.pend.a = a;
            G.pend.b = b;
            if (kind != K_NUDGE)
                hal_ptz_cancel(G.hp);
        }
        pthread_cond_signal(&G.cv);
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_ptz_command(int cmd)
{
    switch (cmd) {
    case A2K_PTZ_LEFT: return submit(K_STEP, HAL_PTZ_LEFT, 0);
    case A2K_PTZ_RIGHT: return submit(K_STEP, HAL_PTZ_RIGHT, 0);
    case A2K_PTZ_UP: return submit(K_STEP, HAL_PTZ_UP, 0);
    case A2K_PTZ_DOWN: return submit(K_STEP, HAL_PTZ_DOWN, 0);
    case A2K_PTZ_HOME: return submit(K_HOME, 0, 0);
    case A2K_PTZ_PARK: return submit(K_PARK, 0, 0);
    case A2K_PTZ_RECAL: return G.cal_mode == HAL_PTZ_CAL_OFF ? -1 : submit(K_RECAL, 0, 0); /* ptz_cal=off: the motor is never touched */
    default: return -1;
    }
}

int a2k_ptz_toggle_park(void)
{
    a2k_ptz_info_t i;
    a2k_ptz_info(&i);
    return a2k_ptz_command(i.state == HAL_PTZ_PARKED ? A2K_PTZ_HOME : A2K_PTZ_PARK);
}

int a2k_ptz_nudge(int dx, int dy)
{
    if (!dx && !dy)
        return 0;
    return submit(K_NUDGE, dx, dy);
}

void a2k_ptz_info(a2k_ptz_info_t *out)
{
    pthread_mutex_lock(&G.mu);
    *out = G.info;
    pthread_mutex_unlock(&G.mu);
}

int a2k_ptz_range(uint32_t *pan_min, uint32_t *pan_max, uint32_t *tilt_min, uint32_t *tilt_max)
{
    int rc = -1;
    pthread_mutex_lock(&G.mu);
    if (G.running && G.hp) {
        const hal_ptz_desc_t *d = hal_ptz_desc(G.hp);
        *pan_min = d->pan_min;
        *pan_max = d->pan_max;
        *tilt_min = d->tilt_min;
        *tilt_max = d->tilt_max;
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

int a2k_ptz_privacy_active(void) { return g_privacy; }

void a2k_ptz_set_notify(void (*fn)(const a2k_ptz_info_t *))
{
    pthread_mutex_lock(&G.mu);
    G.notify = fn;
    pthread_mutex_unlock(&G.mu);
}
