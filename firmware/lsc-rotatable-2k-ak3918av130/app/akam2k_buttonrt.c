/* See akam2k_buttonrt.h. */
#include "akam2k_buttonrt.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "akam2k_log.h"
#include "akam2k_store.h"

static const char k_empty[1] = { 0 };

static struct {
    a2k_btn_ops_t ops;
    pthread_mutex_t mu, edit;
    bc_cfg_t cfg;
    bc_modes_t modes;
    unsigned gen;               /* bumped at every change of cfg (the button thread re-reads it) */
    unsigned writes;
    int tokens;
    int64_t token_ms;
    char last[BG_NAME_MAX + 4];
    /* button thread only */
    btn_t b;
    unsigned seen_gen;
    int have_b;
    bc_cfg_t snap_cfg;          /* the dispatch copies: a gesture runs from a snapshot, never under the lock */
    bc_modes_t snap_modes;
} G = { .mu = PTHREAD_MUTEX_INITIALIZER, .edit = PTHREAD_MUTEX_INITIALIZER };

static char g_path_button[160] = A2K_STORE_BUTTON, g_path_modes[160] = A2K_STORE_MODES;

void a2k_btn_set_paths(const char *button, const char *modes)
{
    snprintf(g_path_button, sizeof g_path_button, "%s", button ? button : A2K_STORE_BUTTON);
    snprintf(g_path_modes, sizeof g_path_modes, "%s", modes ? modes : A2K_STORE_MODES);
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- the ops of the pure layers ---- */
static int op_run(void *u, int kind, const char *arg)
{
    (void)u;
    return G.ops.run ? G.ops.run(G.ops.user, kind, arg) : -1;
}

static int op_apply(void *u, int n, const char *const *k, const char *const *v)
{
    (void)u;
    return G.ops.apply ? G.ops.apply(G.ops.user, n, k, v) : -1;
}

static int act_locked(void *u)
{
    (void)u;
    return G.ops.locked ? G.ops.locked(G.ops.user) : 0;
}

static void act_led(void *u, int cue)
{
    (void)u;
    if (G.ops.led)
        G.ops.led(G.ops.user, cue);
}

static void act_gesture(void *u, const bg_seq_t *seq, int bound)
{
    static const bc_ops_t dops = { NULL, op_run, op_apply };
    char name[BG_NAME_MAX + 4], json[A2K_BTN_EVT_MAX + 1], acts[BC_MAX_ACT * (BC_ACT_TEXT_MAX + 2) + 8];
    const bc_bind_t *bind;
    int r, n, i;
    (void)u;
    (void)bound;
    pthread_mutex_lock(&G.mu);
    memcpy(&G.snap_cfg, &G.cfg, sizeof G.cfg);
    memcpy(&G.snap_modes, &G.modes, sizeof G.modes);
    pthread_mutex_unlock(&G.mu);
    if (bg_seq_name(seq, name, sizeof name) < 0)
        return;
    r = bc_dispatch(&G.snap_cfg, &G.snap_modes, seq, &dops);
    bind = bc_cfg_find(&G.snap_cfg, seq);
    n = bc_event_json(seq, bind, r, json, sizeof json, A2K_BTN_EVT_MAX);
    pthread_mutex_lock(&G.mu);
    snprintf(G.last, sizeof G.last, "%s", name);
    pthread_mutex_unlock(&G.mu);
    acts[0] = '\0';
    for (i = 0; bind && i < bind->nact; i++) {
        char t[BC_ACT_TEXT_MAX + 1];
        if (bc_action_text(&bind->act[i], t, sizeof t) > 0 && strlen(acts) + strlen(t) + 3 < sizeof acts)
            snprintf(acts + strlen(acts), sizeof acts - strlen(acts), "%s%s", i ? "; " : k_empty, t);
    }
    a2k_log("button: gesture %s, actions [%s], %s", name, acts, bc_result_name(r));
    if (n > 0) {
        if (G.ops.event)
            G.ops.event(G.ops.user, json);
        if (G.ops.mqtt)
            G.ops.mqtt(G.ops.user, name, json);
    }
}

/* ---- persistence ---- */
static void load_files(int long_ms)
{
    static char buf[A2K_STORE_MAX + 1];
    bc_diag_t dg;
    int n;
    bc_defaults(&G.cfg, long_ms);
    memset(&G.modes, 0, sizeof G.modes);
    n = a2k_store_read(g_path_modes, buf, sizeof buf);
    if (n > 0) {
        bc_modes_t t;
        if (bc_modes_parse(buf, (size_t)n, NULL, &t, &dg) == 0)
            G.modes = t;
        else
            a2k_log("button: %s is invalid (%s): ignored", g_path_modes, dg.detail);
    }
    n = a2k_store_read(g_path_button, buf, sizeof buf);
    if (n > 0) {
        bc_cfg_t c;
        if (bc_cfg_parse(buf, (size_t)n, &c, &dg) == 0 && bc_cfg_check_refs(&c, &G.modes, &dg) == 0)
            G.cfg = c;
        else
            a2k_log("button: %s is invalid (%s): the defaults apply", g_path_button, dg.detail);
    }
}

int a2k_btn_init(const a2k_btn_ops_t *ops, int long_ms)
{
    memset(&G.ops, 0, sizeof G.ops);
    if (ops)
        G.ops = *ops;
    pthread_mutex_lock(&G.mu);
    load_files(long_ms);
    G.gen++;
    G.writes = 0;
    G.tokens = A2K_BTN_WRITE_BURST;
    G.token_ms = 0;
    snprintf(G.last, sizeof G.last, "none");
    pthread_mutex_unlock(&G.mu);
    G.have_b = 0;
    return 0;
}

void a2k_btn_test_reset(void)
{
    pthread_mutex_lock(&G.mu);
    memset(&G.cfg, 0, sizeof G.cfg);
    memset(&G.modes, 0, sizeof G.modes);
    bc_defaults(&G.cfg, 0);
    G.gen++;
    G.writes = 0;
    snprintf(G.last, sizeof G.last, "none");
    pthread_mutex_unlock(&G.mu);
    G.have_b = 0;
}

/* ---- the button thread ---- */
static void sync_engine(void)
{
    bg_seq_t list[BG_MAX_BOUND];
    bg_timing_t t;
    int n, ack;
    pthread_mutex_lock(&G.mu);
    t = G.cfg.t;
    ack = G.cfg.led_ack;
    n = bc_cfg_bound(&G.cfg, list, BG_MAX_BOUND);
    G.seen_gen = G.gen;
    pthread_mutex_unlock(&G.mu);
    if (!G.have_b) {
        btn_init(&G.b, &t);
        G.have_b = 1;
    } else {
        bg_init(&G.b.bg, &t); /* a change of the bindings drops a half-made sequence; the debouncer and the press in progress go on */
        G.b.bg.down = G.b.down && !G.b.wait_release;
        G.b.bg.t_down = G.b.t_down;
    }
    G.b.led_ack = ack;
    btn_set_bound(&G.b, list, n);
}

int a2k_btn_step(const hal_input_t *in, int64_t now_ms, int raw)
{
    static const btn_actions_t act = { NULL, act_locked, act_gesture, act_led };
    unsigned gen;
    pthread_mutex_lock(&G.mu);
    gen = G.gen;
    pthread_mutex_unlock(&G.mu);
    if (!G.have_b || gen != G.seen_gen)
        sync_engine();
    return btn_step(&G.b, in, now_ms, raw, &act);
}

/* ---- state ---- */
void a2k_btn_last_gesture(char *out, size_t cap)
{
    pthread_mutex_lock(&G.mu);
    snprintf(out, cap, "%s", G.last[0] ? G.last : "none");
    pthread_mutex_unlock(&G.mu);
}

unsigned a2k_btn_writes(void)
{
    unsigned w;
    pthread_mutex_lock(&G.mu);
    w = G.writes;
    pthread_mutex_unlock(&G.mu);
    return w;
}

int a2k_btn_gestures(char names[][BG_NAME_MAX])
{
    int i, n;
    pthread_mutex_lock(&G.mu);
    n = G.cfg.nbind;
    for (i = 0; i < n; i++)
        if (bg_seq_name(&G.cfg.bind[i].seq, names[i], BG_NAME_MAX) < 0)
            names[i][0] = '\0';
    pthread_mutex_unlock(&G.mu);
    return n;
}

int a2k_btn_led_ack(void)
{
    int v;
    pthread_mutex_lock(&G.mu);
    v = G.cfg.led_ack;
    pthread_mutex_unlock(&G.mu);
    return v;
}

int a2k_btn_mode_get(const char *name, bc_mode_t *out)
{
    int i, rc = -1;
    pthread_mutex_lock(&G.mu);
    i = bc_modes_find(&G.modes, name);
    if (i >= 0) {
        *out = G.modes.m[i];
        rc = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return rc;
}

/* ---- HTTP ---- */
int a2k_btn_path(const char *sub)
{
    if (!strcmp(sub, "button") || !strcmp(sub, "modes"))
        return 1;
    return !strncmp(sub, "modes/", 6) && sub[6] != '\0';
}

static int status_of(int rc)
{
    return rc == BC_E_SYNTAX ? 400 : rc == BC_E_TOOBIG ? 413 : rc == BC_E_REF ? 409 : 422;
}

static const char *code_of(int rc)
{
    return rc == BC_E_SYNTAX ? "syntax" : rc == BC_E_TOOBIG ? "too_big" : rc == BC_E_REF ? "in_use" : rc == BC_E_LIMIT ? "limit" : "value";
}

static int fail(int rc, const char *detail, char *out, size_t cap)
{
    snprintf(out, cap, "{\"error\":\"%s\",\"detail\":", code_of(rc));
    {
        size_t l = strlen(out);
        /* the detail text is ours (printable ASCII); quotes and backslashes are escaped by hand */
        size_t i;
        if (l + 4 >= cap)
            return status_of(rc);
        out[l++] = '"';
        for (i = 0; detail && detail[i] && l + 4 < cap; i++) {
            if (detail[i] == '"' || detail[i] == '\\')
                out[l++] = '\\';
            out[l++] = detail[i] >= 0x20 && detail[i] < 0x7f ? detail[i] : '?';
        }
        out[l++] = '"';
        out[l++] = '}';
        out[l] = '\0';
    }
    return status_of(rc);
}

static int err_plain(int st, const char *code, const char *detail, char *out, size_t cap)
{
    snprintf(out, cap, "{\"error\":\"%s\",\"detail\":\"%s\"}", code, detail);
    return st;
}

/* the write limiter (G.mu held): 1 when a write may happen now */
static int token_take(unsigned *retry_s)
{
    int64_t now = mono_ms(), gap = (int64_t)A2K_BTN_WRITE_GAP_S * 1000;
    if (G.token_ms == 0)
        G.token_ms = now;
    while (G.tokens < A2K_BTN_WRITE_BURST && now - G.token_ms >= gap) {
        G.tokens++;
        G.token_ms += gap;
    }
    if (G.tokens >= A2K_BTN_WRITE_BURST)
        G.token_ms = now;
    if (G.tokens > 0) {
        G.tokens--;
        return 1;
    }
    *retry_s = (unsigned)((G.token_ms + gap - now + 999) / 1000);
    return 0;
}

/* Saves text to path when it differs from the stored one (the store skips an identical file; the counter follows what was really written).
 * 0 ok (*wrote 1 / 0), 1 rate limited, -1 failed. Called with the edit mutex, not the data one. */
static int save(const char *path, const char *text, size_t len, int changed, unsigned *retry_s, int *wrote)
{
    unsigned before;
    int ok;
    *wrote = 0;
    if (!changed)
        return 0;
    pthread_mutex_lock(&G.mu);
    ok = token_take(retry_s);
    pthread_mutex_unlock(&G.mu);
    if (!ok)
        return 1;
    before = a2k_store_writes();
    if (a2k_store_write(path, text, len) != 0) {
        a2k_log("button: cannot write %s (the change is refused)", path);
        return -1;
    }
    *wrote = a2k_store_writes() != before;
    return 0;
}

static int rc_reply(int rc, const char *detail, char *out, size_t cap)
{
    return fail(rc, detail, out, cap);
}

static int post_button(const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    bc_cfg_t *w = malloc(sizeof *w);
    char *text = malloc(BC_TEXT_MAX + 64), *cur = malloc(BC_TEXT_MAX + 64);
    bc_diag_t dg;
    int rc, n1, n2, wrote = 0, st;
    memset(&dg, 0, sizeof dg);
    if (!w || !text || !cur) {
        st = err_plain(500, "oom", "out of memory", out, cap);
        goto done;
    }
    rc = bc_cfg_parse(body ? (const char *)body : k_empty, len, w, &dg);
    if (rc) {
        st = rc_reply(rc, dg.detail, out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    rc = bc_cfg_check_refs(w, &G.modes, &dg);
    pthread_mutex_unlock(&G.mu);
    if (rc) {
        st = err_plain(422, "no_such_mode", dg.detail, out, cap);
        goto done;
    }
    n1 = bc_cfg_json(w, text, BC_TEXT_MAX + 64);
    if (n1 < 0 || n1 > BC_TEXT_MAX) {
        st = err_plain(413, "too_big", "the configuration does not fit the store", out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    n2 = bc_cfg_json(&G.cfg, cur, BC_TEXT_MAX + 64);
    pthread_mutex_unlock(&G.mu);
    rc = save(g_path_button, text, (size_t)n1, n1 != n2 || memcmp(text, cur, (size_t)n1) != 0, retry_s, &wrote);
    if (rc == 1) {
        st = err_plain(429, "rate", "config writes are rate limited", out, cap);
        goto done;
    }
    if (rc < 0) {
        st = err_plain(500, "store_failed", "the configuration could not be saved", out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    G.cfg = *w;
    G.gen++;
    if (wrote)
        G.writes++;
    pthread_mutex_unlock(&G.mu);
    if (G.ops.changed)
        G.ops.changed(G.ops.user);
    snprintf(out, cap, "{\"ok\":true,\"changed\":%s}", n1 != n2 || memcmp(text, cur, (size_t)n1) != 0 ? "true" : "false");
    st = 200;
done:
    free(w);
    free(text);
    free(cur);
    return st;
}

static int write_modes(const bc_modes_t *m, unsigned *retry_s, char *out, size_t cap, int *failed)
{
    char *text = malloc(BC_TEXT_MAX + 64), *cur = malloc(BC_TEXT_MAX + 64);
    int n1, n2, rc, wrote = 0, st = 0, changed;
    *failed = 1;
    if (!text || !cur) {
        st = err_plain(500, "oom", "out of memory", out, cap);
        goto done;
    }
    n1 = bc_modes_json(m, text, BC_TEXT_MAX + 64);
    if (n1 < 0 || n1 > BC_TEXT_MAX) {
        st = err_plain(413, "too_big", "the modes do not fit the store", out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    n2 = bc_modes_json(&G.modes, cur, BC_TEXT_MAX + 64);
    pthread_mutex_unlock(&G.mu);
    changed = n1 != n2 || memcmp(text, cur, (size_t)n1) != 0;
    rc = save(g_path_modes, text, (size_t)n1, changed, retry_s, &wrote);
    if (rc == 1) {
        st = err_plain(429, "rate", "config writes are rate limited", out, cap);
        goto done;
    }
    if (rc < 0) {
        st = err_plain(500, "store_failed", "the modes could not be saved", out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    G.modes = *m;
    if (wrote)
        G.writes++;
    pthread_mutex_unlock(&G.mu);
    *failed = 0;
done:
    free(text);
    free(cur);
    return st;
}

static int mode_mutate(const char *method, const char *name, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    bc_modes_t *w = malloc(sizeof *w);
    bc_mode_t *m = malloc(sizeof *m);
    bc_diag_t dg;
    int rc, st, failed, created = 0, del = !strcmp(method, "DELETE");
    memset(&dg, 0, sizeof dg);
    if (!w || !m) {
        st = err_plain(500, "oom", "out of memory", out, cap);
        goto done;
    }
    pthread_mutex_lock(&G.mu);
    *w = G.modes;
    pthread_mutex_unlock(&G.mu);
    if (del) {
        bc_cfg_t *c = malloc(sizeof *c);
        if (!c) {
            st = err_plain(500, "oom", "out of memory", out, cap);
            goto done;
        }
        pthread_mutex_lock(&G.mu);
        *c = G.cfg;
        pthread_mutex_unlock(&G.mu);
        rc = bc_modes_del(w, name, c);
        free(c);
        if (rc == BC_E_VALUE) {
            st = err_plain(404, "not_found", "no such mode", out, cap);
            goto done;
        }
        if (rc == BC_E_REF) {
            st = err_plain(409, "in_use", "a binding uses this mode: change the binding first", out, cap);
            goto done;
        }
    } else {
        rc = bc_mode_parse(name, body ? (const char *)body : k_empty, len, G.ops.key_check, m, &dg);
        if (rc) {
            st = rc_reply(rc, dg.detail, out, cap);
            goto done;
        }
        created = bc_modes_find(w, m->name) < 0;
        rc = bc_modes_set(w, m);
        if (rc) {
            st = err_plain(422, "limit", "at most 8 modes", out, cap);
            goto done;
        }
    }
    st = write_modes(w, retry_s, out, cap, &failed);
    if (failed)
        goto done;
    if (del)
        snprintf(out, cap, "{\"ok\":true,\"deleted\":\"%s\"}", name);
    else
        snprintf(out, cap, "{\"ok\":true,\"created\":%s,\"name\":\"%s\"}", created ? "true" : "false", m->name);
    st = 200;
done:
    free(w);
    free(m);
    return st;
}

static int mode_post_unnamed(const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    /* POST /modes with the name in the body: find it, then take the named path */
    bc_mode_t *m = malloc(sizeof *m);
    bc_diag_t dg;
    int rc, st;
    if (!m)
        return err_plain(500, "oom", "out of memory", out, cap);
    rc = bc_mode_parse(NULL, body ? (const char *)body : k_empty, len, G.ops.key_check, m, &dg);
    if (rc) {
        st = rc_reply(rc, dg.detail, out, cap);
    } else {
        char name[BC_NAME_MAX + 1];
        snprintf(name, sizeof name, "%.16s", m->name);
        st = mode_mutate("POST", name, body, len, out, cap, retry_s);
    }
    free(m);
    return st;
}

int a2k_btn_http(const char *method, const char *sub, const void *body, size_t len, char *out, size_t cap, unsigned *retry_s)
{
    int get = !strcmp(method, "GET"), post = !strcmp(method, "POST"), del = !strcmp(method, "DELETE");
    int is_button = !strcmp(sub, "button");
    const char *name = strncmp(sub, "modes/", 6) == 0 ? sub + 6 : NULL;
    int st;
    if (!a2k_btn_path(sub))
        return err_plain(404, "unknown_path", "no such path", out, cap);
    if (!(get || post || del) || (is_button && del) || (!is_button && !name && del)) {
        (void)err_plain(405, "method", "method not allowed on this path", out, cap);
        return 405;
    }
    if (get) {
        int n;
        if (is_button) {
            char *t = malloc(BC_TEXT_MAX + 64);
            if (!t)
                return err_plain(500, "oom", "out of memory", out, cap);
            pthread_mutex_lock(&G.mu);
            n = bc_cfg_json(&G.cfg, t, BC_TEXT_MAX + 64);
            pthread_mutex_unlock(&G.mu);
            if (n < 0 || (size_t)n >= cap) {
                free(t);
                return err_plain(500, "too_big", "reply does not fit", out, cap);
            }
            memcpy(out, t, (size_t)n + 1);
            free(t);
            return 200;
        } else {
            bc_modes_t *m = malloc(sizeof *m);
            if (!m)
                return err_plain(500, "oom", "out of memory", out, cap);
            pthread_mutex_lock(&G.mu);
            *m = G.modes;
            pthread_mutex_unlock(&G.mu);
            if (name) {
                int i = bc_modes_find(m, name);
                n = i < 0 ? -2 : bc_mode_json(&m->m[i], out, cap);
            } else {
                n = bc_modes_json(m, out, cap);
            }
            free(m);
            if (n == -2)
                return err_plain(404, "not_found", "no such mode", out, cap);
            return n < 0 ? err_plain(500, "too_big", "reply does not fit", out, cap) : 200;
        }
    }
    pthread_mutex_lock(&G.edit);
    if (is_button)
        st = post_button(body, len, out, cap, retry_s);
    else if (name)
        st = mode_mutate(method, name, body, len, out, cap, retry_s);
    else
        st = mode_post_unnamed(body, len, out, cap, retry_s);
    pthread_mutex_unlock(&G.edit);
    return st;
}
