/* See button_cfg.h. */
#include <stdio.h>
#include <string.h>

#include "button_cfg.h"
#include "json_min.h"

static const char k_none[] = "none";

/* ---- small helpers ---- */
static int err(bc_diag_t *dg, int rc, const char *fmt, const char *a, int i)
{
    if (dg) {
        if (i >= 0)
            snprintf(dg->detail, sizeof dg->detail, fmt, i);
        else
            snprintf(dg->detail, sizeof dg->detail, fmt, a ? a : k_none);
    }
    return rc;
}

static int name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0, i;
    if (n < 1 || n > BC_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return 1;
}

/* a plain decimal, 1..9 digits */
static int dec(const char *s, long *v)
{
    long x = 0;
    int n = 0;
    if (!*s)
        return -1;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 9)
            return -1;
        x = x * 10 + (*s - '0');
    }
    *v = x;
    return 0;
}

static int led_pattern_ok(const char *s)
{
    static const long lo[4] = { 1, 10, 10, 0 }, hi[4] = { 255, 10000, 10000, 60000 };
    char buf[BC_ARG_MAX + 1];
    char *p, *q;
    int i;
    if (!strcmp(s, "on") || !strcmp(s, "off"))
        return 1;
    if (strlen(s) > 24)
        return 0;
    snprintf(buf, sizeof buf, "%s", s);
    p = buf;
    for (i = 0; i < 4; i++) {
        long v;
        q = strchr(p, ':');
        if ((i < 3) != (q != NULL))
            return 0;
        if (q)
            *q = '\0';
        if (dec(p, &v) != 0 || v < lo[i] || v > hi[i])
            return 0;
        p = q ? q + 1 : p;
    }
    return 1;
}

typedef struct {
    const char *name, *pattern;
} led_name_t;
static const led_name_t LEDN[] = { { "ack", "2:150:150:0" }, { "blink", "1:300:300:0" }, { "fast", "1:100:100:0" }, { "slow", "1:700:700:0" } };

const char *bc_led_pattern(const char *arg)
{
    size_t i;
    for (i = 0; i < sizeof LEDN / sizeof LEDN[0]; i++)
        if (!strcmp(arg, LEDN[i].name))
            return LEDN[i].pattern;
    return arg;
}

static int lost_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 1 || n > BC_ARG_MAX)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == ':' || s[i] == ',' || s[i] == '.' || s[i] == '-'))
            return 0;
    return 1;
}

static int tri(const char *s) { return !strcmp(s, "on") || !strcmp(s, "off") || !strcmp(s, "toggle"); }

/* ---- actions ---- */
static int why_(char *why, size_t wcap, const char *m)
{
    if (why && wcap)
        snprintf(why, wcap, "%s", m);
    return BC_E_VALUE;
}

int bc_action_parse(const char *text, bc_action_t *a, char *why, size_t wcap)
{
    char buf[BC_ACT_TEXT_MAX + 1];
    char *arg;
    size_t i, n = text ? strlen(text) : 0;
    memset(a, 0, sizeof *a);
    if (n < 1 || n > BC_ACT_TEXT_MAX)
        return why_(why, wcap, "an action is 1 to 100 characters");
    for (i = 0; i < n; i++)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e)
            return why_(why, wcap, "an action is printable ASCII");
    snprintf(buf, sizeof buf, "%s", text);
    arg = strchr(buf, ' ');
    if (arg) {
        *arg++ = '\0';
        if (!*arg || *arg == ' ' || arg[strlen(arg) - 1] == ' ')
            return why_(why, wcap, "one space between the action and its argument");
    }
    if (!strcmp(buf, "none") && !arg) { a->kind = BA_NONE; return 0; }
    if (!strcmp(buf, "ptz.home") && !arg) { a->kind = BA_HOME; return 0; }
    if (!strcmp(buf, "ptz.park") && !arg) { a->kind = BA_PARK; return 0; }
    if (!strcmp(buf, "ptz.recalibrate") && !arg) { a->kind = BA_RECAL; return 0; }
    if (!strcmp(buf, "ptz.toggle_park") && !arg) { a->kind = BA_TOGGLE_PARK; return 0; }
    if (!strcmp(buf, "patrol") && arg && !strcmp(arg, "stop")) { a->kind = BA_PATROL_STOP; return 0; }
    if (!strcmp(buf, "ptz.privacy") && arg && tri(arg)) { a->kind = BA_PRIVACY; snprintf(a->arg, sizeof a->arg, "%s", arg); return 0; }
    if (!strcmp(buf, "track") && arg && tri(arg)) { a->kind = BA_TRACK; snprintf(a->arg, sizeof a->arg, "%s", arg); return 0; }
    if (!strcmp(buf, "ptz.goto_preset") && arg) {
        if (!name_ok(arg) || strlen(arg) > 16)
            return why_(why, wcap, "ptz.goto_preset takes a preset name (a-z first, then a-z 0-9 _, up to 16)");
        a->kind = BA_GOTO;
        snprintf(a->arg, sizeof a->arg, "%s", arg);
        return 0;
    }
    if (!strcmp(buf, "track.lost_policy") && arg) {
        if (!lost_ok(arg))
            return why_(why, wcap, "track.lost_policy takes a policy text of a-z 0-9 _ : , . - (up to 80)");
        a->kind = BA_LOST;
        snprintf(a->arg, sizeof a->arg, "%s", arg);
        return 0;
    }
    if (!strcmp(buf, "patrol") && arg && !strncmp(arg, "start ", 6)) {
        if (!name_ok(arg + 6))
            return why_(why, wcap, "patrol start takes a route name");
        a->kind = BA_PATROL_START;
        snprintf(a->arg, sizeof a->arg, "%s", arg + 6);
        return 0;
    }
    if (!strcmp(buf, "led") && arg && !strncmp(arg, "pattern ", 8)) {
        const char *p = arg + 8;
        if (strcmp(bc_led_pattern(p), p) == 0 && !led_pattern_ok(p))
            return why_(why, wcap, "led pattern takes on, off, N:on_ms:off_ms:pause_ms or ack blink fast slow");
        a->kind = BA_LED;
        snprintf(a->arg, sizeof a->arg, "%s", p);
        return 0;
    }
    if (!strcmp(buf, "mode") && arg) {
        if (!name_ok(arg))
            return why_(why, wcap, "mode takes a mode name");
        a->kind = BA_MODE;
        snprintf(a->arg, sizeof a->arg, "%s", arg);
        return 0;
    }
    return why_(why, wcap, "not an allowed action (none, ptz.home, ptz.park, ptz.toggle_park, ptz.recalibrate, ptz.privacy, ptz.goto_preset, track, track.lost_policy, patrol, led pattern, mode)");
}

int bc_action_text(const bc_action_t *a, char *out, size_t cap)
{
    int n;
    switch (a->kind) {
    case BA_NONE: n = snprintf(out, cap, "none"); break;
    case BA_HOME: n = snprintf(out, cap, "ptz.home"); break;
    case BA_PARK: n = snprintf(out, cap, "ptz.park"); break;
    case BA_RECAL: n = snprintf(out, cap, "ptz.recalibrate"); break;
    case BA_TOGGLE_PARK: n = snprintf(out, cap, "ptz.toggle_park"); break;
    case BA_PRIVACY: n = snprintf(out, cap, "ptz.privacy %s", a->arg); break;
    case BA_GOTO: n = snprintf(out, cap, "ptz.goto_preset %s", a->arg); break;
    case BA_TRACK: n = snprintf(out, cap, "track %s", a->arg); break;
    case BA_LOST: n = snprintf(out, cap, "track.lost_policy %s", a->arg); break;
    case BA_PATROL_START: n = snprintf(out, cap, "patrol start %s", a->arg); break;
    case BA_PATROL_STOP: n = snprintf(out, cap, "patrol stop"); break;
    case BA_LED: n = snprintf(out, cap, "led pattern %s", a->arg); break;
    case BA_MODE: n = snprintf(out, cap, "mode %s", a->arg); break;
    default: return -1;
    }
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* ---- defaults ---- */
void bc_defaults(bc_cfg_t *c, int long_ms)
{
    memset(c, 0, sizeof *c);
    bg_timing_defaults(&c->t);
    if (long_ms >= 1500 && long_ms <= 10000) {
        c->t.long_ms = long_ms;
        if (c->t.very_long_ms < long_ms + 500)
            c->t.very_long_ms = long_ms + 500;
    }
    c->led_ack = 1;
    c->nbind = 2;
    (void)bg_seq_parse("short", &c->bind[0].seq);
    c->bind[0].nact = 1; /* kind BA_NONE */
    (void)bg_seq_parse("long", &c->bind[1].seq);
    c->bind[1].nact = 1;
    c->bind[1].act[0].kind = BA_TOGGLE_PARK;
}

/* ---- the pull parser (bounded, no recursion beyond the two fixed shapes) ---- */
typedef struct {
    const char *p, *end;
} jp_t;

static void ws(jp_t *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static int eat(jp_t *j, char c)
{
    ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

static int peek(jp_t *j)
{
    ws(j);
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

/* "text" (ASCII printable, \" and \\ escapes only) into out[cap]; 0, or -1 (also too long). */
static int jstr(jp_t *j, char *out, size_t cap)
{
    size_t n = 0;
    if (!eat(j, '"'))
        return -1;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\') {
            if (j->p >= j->end || (*j->p != '"' && *j->p != '\\'))
                return -1;
            c = (unsigned char)*j->p++;
        }
        if (c < 0x20 || c > 0x7e || n + 1 >= cap)
            return -1;
        out[n++] = (char)c;
    }
    if (j->p >= j->end)
        return -1;
    j->p++;
    out[n] = '\0';
    return 0;
}

static int jint(jp_t *j, long *v)
{
    long x = 0;
    int n = 0, neg = 0;
    ws(j);
    if (j->p < j->end && *j->p == '-') {
        neg = 1;
        j->p++;
    }
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (++n > 9)
            return -1;
        x = x * 10 + (*j->p++ - '0');
    }
    if (!n)
        return -1;
    *v = neg ? -x : x;
    return 0;
}

static int jlit(jp_t *j, const char *lit)
{
    size_t n = strlen(lit);
    ws(j);
    if ((size_t)(j->end - j->p) >= n && !memcmp(j->p, lit, n)) {
        j->p += n;
        return 1;
    }
    return 0;
}

static int jbool(jp_t *j, int *v)
{
    if (jlit(j, "true")) { *v = 1; return 0; }
    if (jlit(j, "false")) { *v = 0; return 0; }
    return -1;
}

/* a scalar as its canonical text: string, integer, true / false -> 1 / 0 */
static int jscalar(jp_t *j, char *out, size_t cap)
{
    int c = peek(j), b;
    long v;
    if (c == '"')
        return jstr(j, out, cap);
    if (jbool(j, &b) == 0) {
        snprintf(out, cap, "%d", b);
        return 0;
    }
    if (jint(j, &v) == 0) {
        int n = snprintf(out, cap, "%ld", v);
        return n < 0 || (size_t)n >= cap ? -1 : 0;
    }
    return -1;
}

/* the next key of an object: 1 key read (the colon eaten), 0 at the closing brace (eaten), -1 error. first = 1 right after '{'. */
static int onext(jp_t *j, int first, char *key, size_t cap)
{
    if (eat(j, '}'))
        return 0;
    if (!first && !eat(j, ','))
        return -1;
    if (jstr(j, key, cap) != 0 || !eat(j, ':'))
        return -1;
    return 1;
}

/* the next element of an array: 1 (the caller reads it), 0 at ']' (eaten), -1 error. */
static int anext(jp_t *j, int first)
{
    if (eat(j, ']'))
        return 0;
    if (!first && !eat(j, ','))
        return -1;
    return 1;
}

static int jp_begin(jp_t *j, const char *s, size_t len)
{
    if (!s || len > BC_IN_MAX)
        return BC_E_TOOBIG;
    j->p = s;
    j->end = s + len;
    return 0;
}

static int jp_done(jp_t *j)
{
    ws(j);
    return j->p == j->end;
}

/* ---- config ---- */
static int parse_timing(jp_t *j, bg_timing_t *t, bc_diag_t *dg)
{
    char key[24];
    int r, first = 1;
    if (!eat(j, '{'))
        return err(dg, BC_E_SYNTAX, "timing is an object", NULL, -1);
    while ((r = onext(j, first, key, sizeof key)) > 0) {
        long v;
        int *f = !strcmp(key, "debounce_ms") ? &t->debounce_ms : !strcmp(key, "short_max_ms") ? &t->short_max_ms
               : !strcmp(key, "gap_ms") ? &t->gap_ms : !strcmp(key, "long_ms") ? &t->long_ms
               : !strcmp(key, "very_long_ms") ? &t->very_long_ms : NULL;
        first = 0;
        if (!f)
            return err(dg, BC_E_VALUE, "timing.%s: unknown key", key, -1);
        if (jint(j, &v) != 0)
            return err(dg, BC_E_VALUE, "timing.%s: an integer is expected", key, -1);
        *f = (int)v;
    }
    return r < 0 ? err(dg, BC_E_SYNTAX, "timing: bad object", NULL, -1) : 0;
}

static int parse_binding(jp_t *j, bc_bind_t *b, int idx, bc_diag_t *dg)
{
    char key[16], gname[32], text[BC_ACT_TEXT_MAX + 1], why[96];
    int r, first = 1, have_g = 0, have_a = 0;
    if (!eat(j, '{'))
        return err(dg, BC_E_SYNTAX, "bindings[%d]: an object is expected", NULL, idx);
    memset(b, 0, sizeof *b);
    while ((r = onext(j, first, key, sizeof key)) > 0) {
        first = 0;
        if (!strcmp(key, "gesture")) {
            if (jstr(j, gname, sizeof gname) != 0)
                return err(dg, BC_E_SYNTAX, "bindings[%d].gesture: a string is expected", NULL, idx);
            if (bg_seq_parse(gname, &b->seq) != 0)
                return err(dg, BC_E_VALUE, "bindings[%d].gesture: short, double, triple, long, very_long or up to 6 of S L V", NULL, idx);
            have_g = 1;
        } else if (!strcmp(key, "actions")) {
            int ar, afirst = 1;
            if (!eat(j, '['))
                return err(dg, BC_E_SYNTAX, "bindings[%d].actions: an array is expected", NULL, idx);
            while ((ar = anext(j, afirst)) > 0) {
                afirst = 0;
                if (b->nact >= BC_MAX_ACT)
                    return err(dg, BC_E_LIMIT, "bindings[%d]: at most 4 actions", NULL, idx);
                if (jstr(j, text, sizeof text) != 0)
                    return err(dg, BC_E_SYNTAX, "bindings[%d].actions: strings are expected", NULL, idx);
                why[0] = '\0';
                if (bc_action_parse(text, &b->act[b->nact], why, sizeof why) != 0) {
                    if (dg)
                        snprintf(dg->detail, sizeof dg->detail, "bindings[%d].actions[%d]: %.60s", idx, b->nact, why);
                    return BC_E_VALUE;
                }
                b->nact++;
            }
            if (ar < 0)
                return err(dg, BC_E_SYNTAX, "bindings[%d].actions: bad array", NULL, idx);
            have_a = 1;
        } else {
            return err(dg, BC_E_VALUE, "binding: unknown key %s", key, -1);
        }
    }
    if (r < 0)
        return err(dg, BC_E_SYNTAX, "bindings[%d]: bad object", NULL, idx);
    if (!have_g || !have_a || b->nact < 1)
        return err(dg, BC_E_VALUE, "bindings[%d]: a gesture and 1 to 4 actions are required", NULL, idx);
    return 0;
}

int bc_cfg_parse(const char *s, size_t len, bc_cfg_t *out, bc_diag_t *dg)
{
    jp_t j;
    bc_cfg_t c;
    char key[16];
    int r, first = 1, have_b = 0, rc, i, k;
    rc = jp_begin(&j, s, len);
    if (rc)
        return err(dg, rc, "the body is longer than 8192 bytes", NULL, -1);
    bc_defaults(&c, 0);
    c.nbind = 0;
    c.t.long_ms = 2000;
    if (!eat(&j, '{'))
        return err(dg, BC_E_SYNTAX, "a JSON object is expected", NULL, -1);
    while ((r = onext(&j, first, key, sizeof key)) > 0) {
        first = 0;
        if (!strcmp(key, "timing")) {
            rc = parse_timing(&j, &c.t, dg);
            if (rc)
                return rc;
        } else if (!strcmp(key, "led_ack")) {
            if (jbool(&j, &c.led_ack) != 0)
                return err(dg, BC_E_VALUE, "led_ack: true or false", NULL, -1);
        } else if (!strcmp(key, "bindings")) {
            int ar, afirst = 1;
            if (!eat(&j, '['))
                return err(dg, BC_E_SYNTAX, "bindings: an array is expected", NULL, -1);
            while ((ar = anext(&j, afirst)) > 0) {
                afirst = 0;
                if (c.nbind >= BC_MAX_BIND)
                    return err(dg, BC_E_LIMIT, "at most 16 bindings", NULL, -1);
                rc = parse_binding(&j, &c.bind[c.nbind], c.nbind, dg);
                if (rc)
                    return rc;
                c.nbind++;
            }
            if (ar < 0)
                return err(dg, BC_E_SYNTAX, "bindings: bad array", NULL, -1);
            have_b = 1;
        } else {
            return err(dg, BC_E_VALUE, "unknown key %s", key, -1);
        }
    }
    if (r < 0 || !jp_done(&j))
        return err(dg, BC_E_SYNTAX, "not valid JSON of the expected shape", NULL, -1);
    if (!have_b)
        return err(dg, BC_E_VALUE, "bindings is required", NULL, -1);
    if (!bg_timing_ok(&c.t))
        return err(dg, BC_E_VALUE, "timing: debounce 10..200, short_max 200..1500, gap 150..1000, long 1500..10000 (> short_max), very_long long+500..30000", NULL, -1);
    for (i = 0; i < c.nbind; i++)
        for (k = 0; k < i; k++)
            if (bg_seq_eq(&c.bind[i].seq, &c.bind[k].seq))
                return err(dg, BC_E_VALUE, "bindings[%d]: the same gesture twice", NULL, i);
    *out = c;
    return 0;
}

int bc_cfg_json(const bc_cfg_t *c, char *out, size_t cap)
{
    jw_t w;
    int i, k;
    char name[BG_NAME_MAX + 8], at[BC_ACT_TEXT_MAX + 1];
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"timing\":{\"debounce_ms\":");
    jw_long(&w, c->t.debounce_ms);
    jw_raw(&w, ",\"short_max_ms\":");
    jw_long(&w, c->t.short_max_ms);
    jw_raw(&w, ",\"gap_ms\":");
    jw_long(&w, c->t.gap_ms);
    jw_raw(&w, ",\"long_ms\":");
    jw_long(&w, c->t.long_ms);
    jw_raw(&w, ",\"very_long_ms\":");
    jw_long(&w, c->t.very_long_ms);
    jw_raw(&w, "},\"led_ack\":");
    jw_bool(&w, c->led_ack);
    jw_raw(&w, ",\"bindings\":[");
    for (i = 0; i < c->nbind; i++) {
        if (bg_seq_name(&c->bind[i].seq, name, sizeof name) < 0)
            return -1;
        jw_raw(&w, i ? ",{\"gesture\":" : "{\"gesture\":");
        jw_str(&w, name);
        jw_raw(&w, ",\"actions\":[");
        for (k = 0; k < c->bind[i].nact; k++) {
            if (bc_action_text(&c->bind[i].act[k], at, sizeof at) < 0)
                return -1;
            if (k)
                jw_raw(&w, ",");
            jw_str(&w, at);
        }
        jw_raw(&w, "]}");
    }
    jw_raw(&w, "]}");
    return jw_end(&w);
}

int bc_cfg_check_refs(const bc_cfg_t *c, const bc_modes_t *m, bc_diag_t *dg)
{
    int i, k;
    for (i = 0; i < c->nbind; i++)
        for (k = 0; k < c->bind[i].nact; k++)
            if (c->bind[i].act[k].kind == BA_MODE && bc_modes_find(m, c->bind[i].act[k].arg) < 0)
                return err(dg, BC_E_REF, "mode %s does not exist (create it with POST /akam/v1/modes/<name>)", c->bind[i].act[k].arg, -1);
    return 0;
}

const bc_bind_t *bc_cfg_find(const bc_cfg_t *c, const bg_seq_t *seq)
{
    int i;
    for (i = 0; i < c->nbind; i++)
        if (bg_seq_eq(&c->bind[i].seq, seq))
            return &c->bind[i];
    return NULL;
}

int bc_cfg_bound(const bc_cfg_t *c, bg_seq_t *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < c->nbind && n < cap; i++)
        out[n++] = c->bind[i].seq;
    return n;
}

/* ---- modes ---- */
static int pair_ok(const char *k, const char *v)
{
    size_t i, n = strlen(k);
    if (n < 1 || n > BC_KEY_MAX)
        return 0;
    for (i = 0; i < n; i++)
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_' || k[i] == '.'))
            return 0;
    return strlen(v) <= BC_VAL_MAX;
}

static int parse_settings(jp_t *j, bc_key_check_fn check, bc_mode_t *m, bc_diag_t *dg)
{
    char key[BC_KEY_MAX + 1], val[BC_VAL_MAX + 1];
    int r, first = 1, i;
    if (!eat(j, '{'))
        return err(dg, BC_E_SYNTAX, "settings is an object", NULL, -1);
    while ((r = onext(j, first, key, sizeof key)) > 0) {
        first = 0;
        if (jscalar(j, val, sizeof val) != 0)
            return err(dg, BC_E_VALUE, "settings.%s: a string, integer or boolean up to 64 characters", key, -1);
        if (!pair_ok(key, val))
            return err(dg, BC_E_VALUE, "settings.%s: a key of a-z 0-9 _ .", key, -1);
        for (i = 0; i < m->n; i++)
            if (!strcmp(m->key[i], key))
                return err(dg, BC_E_VALUE, "settings.%s: twice", key, -1);
        if (m->n >= BC_MAX_PAIRS)
            return err(dg, BC_E_LIMIT, "a mode has at most 8 settings", NULL, -1);
        if (check && check(key, val) != 0)
            return err(dg, BC_E_VALUE, "settings.%s: not an allowed setting or value (writable, not secret, applied live, not net.* or api.*)", key, -1);
        snprintf(m->key[m->n], sizeof m->key[0], "%s", key);
        snprintf(m->val[m->n], sizeof m->val[0], "%s", val);
        m->n++;
    }
    return r < 0 ? err(dg, BC_E_SYNTAX, "settings: bad object", NULL, -1) : 0;
}

int bc_mode_parse(const char *name, const char *s, size_t len, bc_key_check_fn check, bc_mode_t *out, bc_diag_t *dg)
{
    jp_t j;
    bc_mode_t m;
    char key[16], nm[BC_NAME_MAX + 2];
    int r, first = 1, have = 0, rc;
    rc = jp_begin(&j, s, len);
    if (rc)
        return err(dg, rc, "the body is longer than 8192 bytes", NULL, -1);
    memset(&m, 0, sizeof m);
    if (name && !name_ok(name))
        return err(dg, BC_E_VALUE, "mode name: a-z first, then a-z 0-9 _, 1..16 characters", NULL, -1);
    if (name)
        snprintf(m.name, sizeof m.name, "%s", name);
    if (!eat(&j, '{'))
        return err(dg, BC_E_SYNTAX, "a JSON object is expected", NULL, -1);
    while ((r = onext(&j, first, key, sizeof key)) > 0) {
        first = 0;
        if (!strcmp(key, "settings")) {
            rc = parse_settings(&j, check, &m, dg);
            if (rc)
                return rc;
            have = 1;
        } else if (!strcmp(key, "name")) {
            if (jstr(&j, nm, sizeof nm) != 0 || !name_ok(nm))
                return err(dg, BC_E_VALUE, "name: a-z first, then a-z 0-9 _, 1..16 characters", NULL, -1);
            if (name && strcmp(name, nm) != 0)
                return err(dg, BC_E_VALUE, "name: differs from the path", NULL, -1);
            snprintf(m.name, sizeof m.name, "%.16s", nm);
        } else {
            return err(dg, BC_E_VALUE, "unknown key %s", key, -1);
        }
    }
    if (r < 0 || !jp_done(&j))
        return err(dg, BC_E_SYNTAX, "not valid JSON of the expected shape", NULL, -1);
    if (!have || m.n < 1)
        return err(dg, BC_E_VALUE, "settings: 1 to 8 key / value pairs are required", NULL, -1);
    if (!m.name[0])
        return err(dg, BC_E_VALUE, "a mode needs a name", NULL, -1);
    *out = m;
    return 0;
}

static void mode_w(jw_t *w, const bc_mode_t *m)
{
    int i;
    jw_raw(w, "{\"name\":");
    jw_str(w, m->name);
    jw_raw(w, ",\"settings\":{");
    for (i = 0; i < m->n; i++) {
        if (i)
            jw_raw(w, ",");
        jw_str(w, m->key[i]);
        jw_raw(w, ":");
        jw_str(w, m->val[i]);
    }
    jw_raw(w, "}}");
}

int bc_mode_json(const bc_mode_t *m, char *out, size_t cap)
{
    jw_t w;
    jw_init(&w, out, cap);
    mode_w(&w, m);
    return jw_end(&w);
}

int bc_modes_json(const bc_modes_t *m, char *out, size_t cap)
{
    jw_t w;
    int i;
    jw_init(&w, out, cap);
    jw_raw(&w, "{\"modes\":[");
    for (i = 0; i < m->n; i++) {
        if (i)
            jw_raw(&w, ",");
        mode_w(&w, &m->m[i]);
    }
    jw_raw(&w, "]}");
    return jw_end(&w);
}

int bc_modes_parse(const char *s, size_t len, bc_key_check_fn check, bc_modes_t *out, bc_diag_t *dg)
{
    jp_t j;
    char key[16];
    bc_modes_t t;
    int r, first = 1, rc, have = 0;
    rc = jp_begin(&j, s, len);
    if (rc)
        return err(dg, rc, "too big", NULL, -1);
    memset(&t, 0, sizeof t);
    if (!eat(&j, '{'))
        return err(dg, BC_E_SYNTAX, "a JSON object is expected", NULL, -1);
    while ((r = onext(&j, first, key, sizeof key)) > 0) {
        int ar, afirst = 1;
        first = 0;
        if (strcmp(key, "modes") != 0)
            return err(dg, BC_E_VALUE, "unknown key %s", key, -1);
        if (!eat(&j, '['))
            return err(dg, BC_E_SYNTAX, "modes: an array is expected", NULL, -1);
        while ((ar = anext(&j, afirst)) > 0) {
            /* each element is an object {"name","settings"}: parsed by the one-mode parser on its text span */
            const char *beg;
            int depth = 0, instr = 0;
            bc_mode_t m;
            afirst = 0;
            ws(&j);
            beg = j.p;
            for (; j.p < j.end; j.p++) {
                char c = *j.p;
                if (instr) {
                    if (c == '\\' && j.p + 1 < j.end)
                        j.p++;
                    else if (c == '"')
                        instr = 0;
                } else if (c == '"') {
                    instr = 1;
                } else if (c == '{') {
                    depth++;
                } else if (c == '}' && --depth == 0) {
                    j.p++;
                    break;
                }
            }
            if (depth != 0 || beg == j.p)
                return err(dg, BC_E_SYNTAX, "modes: bad element", NULL, -1);
            rc = bc_mode_parse(NULL, beg, (size_t)(j.p - beg), check, &m, dg);
            if (rc)
                return rc;
            if (bc_modes_find(&t, m.name) >= 0)
                return err(dg, BC_E_VALUE, "mode %s twice", m.name, -1);
            rc = bc_modes_set(&t, &m);
            if (rc)
                return err(dg, rc, "at most 8 modes", NULL, -1);
        }
        if (ar < 0)
            return err(dg, BC_E_SYNTAX, "modes: bad array", NULL, -1);
        have = 1;
    }
    if (r < 0 || !jp_done(&j) || !have)
        return err(dg, BC_E_SYNTAX, "not valid JSON of the expected shape", NULL, -1);
    *out = t;
    return 0;
}

int bc_modes_find(const bc_modes_t *m, const char *name)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->m[i].name, name))
            return i;
    return -1;
}

int bc_modes_set(bc_modes_t *m, const bc_mode_t *mode)
{
    int i = bc_modes_find(m, mode->name);
    if (i < 0) {
        if (m->n >= BC_MAX_MODES)
            return BC_E_LIMIT;
        i = m->n++;
    }
    m->m[i] = *mode;
    return 0;
}

int bc_modes_del(bc_modes_t *m, const char *name, const bc_cfg_t *c)
{
    int i = bc_modes_find(m, name), b, k;
    if (i < 0)
        return BC_E_VALUE;
    for (b = 0; c && b < c->nbind; b++)
        for (k = 0; k < c->bind[b].nact; k++)
            if (c->bind[b].act[k].kind == BA_MODE && !strcmp(c->bind[b].act[k].arg, name))
                return BC_E_REF;
    memmove(&m->m[i], &m->m[i + 1], (size_t)(m->n - i - 1) * sizeof m->m[0]);
    m->n--;
    memset(&m->m[m->n], 0, sizeof m->m[0]);
    return 0;
}

/* ---- dispatch ---- */
const char *bc_result_name(int r)
{
    return r == BC_R_OK ? "ok" : r == BC_R_UNBOUND ? "unbound" : r == BC_R_PARTIAL ? "partial" : "failed";
}

int bc_dispatch(const bc_cfg_t *c, const bc_modes_t *modes, const bg_seq_t *seq, const bc_ops_t *ops)
{
    const bc_bind_t *b = bc_cfg_find(c, seq);
    int k, ok = 0, bad = 0;
    if (!b)
        return BC_R_UNBOUND;
    for (k = 0; k < b->nact; k++) {
        const bc_action_t *a = &b->act[k];
        int rc = 0;
        if (a->kind == BA_NONE)
            continue;
        if (a->kind == BA_MODE) {
            int mi = modes ? bc_modes_find(modes, a->arg) : -1;
            if (mi < 0 || !ops->apply) {
                rc = -1;
            } else {
                const char *kk[BC_MAX_PAIRS], *vv[BC_MAX_PAIRS];
                int p;
                for (p = 0; p < modes->m[mi].n; p++) {
                    kk[p] = modes->m[mi].key[p];
                    vv[p] = modes->m[mi].val[p];
                }
                rc = ops->apply(ops->user, modes->m[mi].n, kk, vv);
            }
        } else {
            rc = ops->run ? ops->run(ops->user, a->kind, a->kind == BA_LED ? bc_led_pattern(a->arg) : a->arg) : -1;
        }
        if (rc == 0)
            ok++;
        else
            bad++;
    }
    return bad == 0 ? BC_R_OK : ok == 0 ? BC_R_FAILED : BC_R_PARTIAL;
}

int bc_event_json(const bg_seq_t *seq, const bc_bind_t *b, int result, char *out, size_t cap, size_t max_len)
{
    char name[BG_NAME_MAX + 8], at[BC_ACT_TEXT_MAX + 1], tmp[512];
    int k, shown;
    for (shown = b ? b->nact : 0; shown >= 0; shown--) {
        jw_t w;
        int n;
        if (bg_seq_name(seq, name, sizeof name) < 0)
            return -1;
        jw_init(&w, tmp, sizeof tmp);
        jw_raw(&w, "{\"gesture\":");
        jw_str(&w, name);
        jw_raw(&w, ",\"actions\":[");
        for (k = 0; k < shown; k++) {
            if (bc_action_text(&b->act[k], at, sizeof at) < 0)
                return -1;
            if (k)
                jw_raw(&w, ",");
            jw_str(&w, at);
        }
        jw_raw(&w, "]");
        if (b && shown < b->nact) {
            jw_raw(&w, ",\"more\":");
            jw_long(&w, b->nact - shown);
        }
        jw_raw(&w, ",\"result\":");
        jw_str(&w, bc_result_name(result));
        jw_raw(&w, "}");
        n = jw_end(&w);
        if (n >= 0 && (size_t)n <= max_len && (size_t)n < cap) {
            memcpy(out, tmp, (size_t)n + 1);
            return n;
        }
    }
    return -1;
}
