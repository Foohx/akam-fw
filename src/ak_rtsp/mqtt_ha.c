/* See mqtt_ha.h. */
#include "mqtt_ha.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt.h"

enum {
    F_PERSIST = 1,   /* the setter writes flash for this entity (discrete choices; sliders stay in RAM) */
    F_OPT     = 2,   /* only published when the camera really has the value (temperature, Wi-Fi signal) */
    F_SCAN    = 4,   /* scanned for changes every tick: the controls and night.state, which cost no /proc read; every
                      * other value comes from ctl_sys (a /proc refresh) and is published every interval only */
    F_TENTHS  = 8,   /* the camera reports tenths: 199 is published as 19.9 */
    F_BOOL    = 16,  /* "1"/"0" is published as ON/OFF */
    F_DIAG    = 32,  /* entity_category diagnostic */
    F_MEASURE = 64   /* state_class measurement */
};

typedef struct {
    const char *id;       /* topic segment and unique id suffix */
    const char *name;
    const char *comp;     /* HA component */
    const char *key;      /* camera key (specs/control-port.md names) */
    const char *options;  /* select: comma separated */
    int         min, max; /* number */
    const char *unit;     /* JSON text, already escaped */
    const char *dclass;
    unsigned    flags;
} ent_t;

#define CTL (F_SCAN)
static const ent_t g_ent[] = {
    /* controls: the allow-list (decision U9) */
    { "status_led",   "Status LED",      "select", "led.status",      "on,off,auto",  0, 0,    NULL, NULL, CTL | F_PERSIST },
    { "ir_led",       "IR LEDs",         "select", "night.ir_led",    "auto,off",     0, 0,    NULL, NULL, CTL | F_PERSIST },
    { "night_mode",   "Day/night mode",  "select", "night.mode",      "auto,day,night", 0, 0,  NULL, NULL, CTL | F_PERSIST },
    { "volume",       "Speaker volume",  "number", "audio.out_volume", NULL,          0, 100,  "%", NULL, CTL },
    { "brightness",   "Brightness",      "number", "isp.brightness",  NULL,         -50, 50,  NULL, NULL, CTL },
    { "contrast",     "Contrast",        "number", "isp.contrast",    NULL,         -50, 50,  NULL, NULL, CTL },
    { "saturation",   "Saturation",      "number", "isp.saturation",  NULL,         -50, 50,  NULL, NULL, CTL },
    { "sharpness",    "Sharpness",       "number", "isp.sharpness",   NULL,         -50, 50,  NULL, NULL, CTL },
    { "talk",         "Talk enabled",    "switch", "audio.talk_enable", NULL,         0, 0,   NULL, NULL, CTL | F_PERSIST | F_BOOL },
    { "beep",         "Beep",            "button", "audio.out_test",  NULL,           0, 0,   NULL, NULL, 0 },
    /* sensors */
    { "night_state",  "Day/night state", "sensor", "night.state",     NULL,           0, 0,   NULL, NULL, CTL },
    { "clock_set",    "Clock set",       "binary_sensor", "sys.time_set", NULL,       0, 0,   NULL, NULL, F_BOOL | F_DIAG },
    { "viewers",      "Video viewers",   "sensor", "rtsp.clients",    NULL,           0, 0,   NULL, NULL, F_MEASURE },
    { "wifi_signal",  "Wi-Fi signal",    "sensor", "net.wifi_rssi",   NULL,           0, 0,   "dBm", "signal_strength", F_OPT | F_MEASURE | F_DIAG },
    { "uptime",       "Uptime",          "sensor", "sys.uptime",      NULL,           0, 0,   "s", "duration", F_DIAG },
    { "cpu",          "CPU load",        "sensor", "sys.cpu",         NULL,           0, 0,   "%", NULL, F_MEASURE | F_DIAG },
    { "mem_free",     "Free memory",     "sensor", "sys.mem_free",    NULL,           0, 0,   "kB", "data_size", F_MEASURE | F_DIAG },
    { "temperature",  "Temperature",     "sensor", "sys.temp",        NULL,           0, 0,   "\\u00b0C", "temperature", F_OPT | F_MEASURE | F_DIAG },
    { "fps",          "Frame rate",      "sensor", "video.fps_actual", NULL,          0, 0,   "fps", NULL, F_TENTHS | F_MEASURE },
    { "bitrate",      "Video bitrate",   "sensor", "video.bitrate",   NULL,           0, 0,   "kbit/s", "data_rate", F_MEASURE },
};
#define N_ENT (sizeof(g_ent) / sizeof(g_ent[0]))

_Static_assert(N_ENT <= MQTT_HA_MAX_ENT, "raise MQTT_HA_MAX_ENT");

size_t mqtt_ha_count(void) { return N_ENT; }
const char *mqtt_ha_entity_id(size_t i) { return i < N_ENT ? g_ent[i].id : NULL; }
int mqtt_ha_is_periodic(size_t i) { return i < N_ENT && !(g_ent[i].flags & F_SCAN) && strcmp(g_ent[i].comp, "button") != 0; }
int mqtt_ha_entity_present(const mqtt_ha_t *h, size_t i) { return i < N_ENT && h->present[i]; }

size_t mqtt_ha_present(const mqtt_ha_t *h)
{
    size_t n = 0;
    for (size_t i = 0; i < N_ENT; i++) n += h->present[i];
    return n;
}

/* ------------------------------------------------------------------------ helpers */

static int is_cmd(const ent_t *e)
{
    return !strcmp(e->comp, "select") || !strcmp(e->comp, "number") || !strcmp(e->comp, "switch") ||
           !strcmp(e->comp, "button");
}

static int has_state(const ent_t *e) { return strcmp(e->comp, "button") != 0; }

int mqtt_ha_topic_ok(const char *s, size_t maxlen)
{
    size_t n = strlen(s);
    if (n == 0 || n > maxlen || s[0] == '/' || s[n - 1] == '/') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                 c == '/';
        if (!ok) return 0;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') return 0;
    }
    return 1;
}

static int mac_ok(const char *m)
{
    if (strlen(m) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        char c = m[i];
        if (i % 3 == 2) { if (c != ':') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static int plain_text_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= MQTT_HA_VAL_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e) return 0;
    return 1;
}

int mqtt_ha_init(mqtt_ha_t *h, const mqtt_ops_t *ops, const char *base, const char *prefix, int discovery)
{
    char v[64];
    memset(h, 0, sizeof(*h));
    h->reject_idx = -1;
    h->lim = &h->own_lim;
    h->ops = ops;
    h->discovery = discovery;
    ops->hostname(h->host);
    h->host[sizeof(h->host) - 1] = '\0';
    snprintf(h->version, sizeof(h->version), "%s", ops->version ? ops->version() : "");
    for (char *p = h->version; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '"' || *p == '\\') *p = '_';
    h->mac[0] = '\0';
    if (ops->get("net.mac", v, sizeof(v)) == 0 && mac_ok(v)) memcpy(h->mac, v, 18);                    /* mac_ok(): exactly 17 characters */
    if (h->mac[0]) {
        size_t o = 0;
        memcpy(h->node, "akam_", 5);
        o = 5;
        for (int i = 0; i < 17; i++)
            if (h->mac[i] != ':') h->node[o++] = h->mac[i];
        h->node[o] = '\0';
    } else {
        return -2;   /* no MAC yet (Wi-Fi not up): the node id must be the stable hardware one, never the renamable host name */
    }
    if (base && base[0]) {
        if (!mqtt_ha_topic_ok(base, 64)) return -1;
        snprintf(h->base, sizeof(h->base), "%s", base);
    } else {
        if (snprintf(h->base, sizeof(h->base), "akam/%s", h->host) >= (int)sizeof(h->base) || !mqtt_ha_topic_ok(h->base, 64)) return -1;
    }
    if (prefix && prefix[0]) {
        if (!mqtt_ha_topic_ok(prefix, 32)) return -1;
        snprintf(h->prefix, sizeof(h->prefix), "%s", prefix);
    } else {
        snprintf(h->prefix, sizeof(h->prefix), "homeassistant");
    }
    for (size_t i = 0; i < N_ENT; i++) {
        h->present[i] = 1;
        if (g_ent[i].flags & F_OPT) {
            h->present[i] = ops->get(g_ent[i].key, v, sizeof(v)) == 0 && strcmp(v, "unknown") != 0;
        }
    }
    return 0;
}

void mqtt_ha_use_limits(mqtt_ha_t *h, mqtt_ha_limits_t *lim) { h->lim = lim ? lim : &h->own_lim; }

int mqtt_ha_status_topic(const mqtt_ha_t *h, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/status", h->base);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int mqtt_ha_cmd_filter(const mqtt_ha_t *h, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/+/set", h->base);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* ------------------------------------------------------------------------ discovery JSON */

typedef struct { char *b; size_t cap, n; int bad; } jb_t;

static void jb_raw(jb_t *j, const char *s)
{
    size_t l = strlen(s);
    if (j->bad || l >= j->cap - j->n) { j->bad = 1; return; }
    memcpy(j->b + j->n, s, l + 1);
    j->n += l;
}

/* A JSON string: quotes, backslash and control characters escaped; bytes over 0x7e are dropped (every value
 * comes from the camera and is ASCII anyway). */
static void jb_str(jb_t *j, const char *s)
{
    jb_raw(j, "\"");
    for (; *s && !j->bad; s++) {
        unsigned char c = (unsigned char)*s;
        char t[8];
        if (c == '"' || c == '\\') { t[0] = '\\'; t[1] = (char)c; t[2] = 0; jb_raw(j, t); }
        else if (c >= 0x20 && c <= 0x7e) { t[0] = (char)c; t[1] = 0; jb_raw(j, t); }
    }
    jb_raw(j, "\"");
}

/* A string that is already JSON-escaped text (unit with °). */
static void jb_lit(jb_t *j, const char *s)
{
    jb_raw(j, "\"");
    jb_raw(j, s);
    jb_raw(j, "\"");
}

static void jb_kv(jb_t *j, const char *k, const char *v)
{
    jb_raw(j, ",\""); jb_raw(j, k); jb_raw(j, "\":");
    jb_str(j, v);
}

static void jb_topic(jb_t *j, const char *k, const mqtt_ha_t *h, const ent_t *e, const char *tail)
{
    char t[MQTT_HA_TOPIC_MAX];
    snprintf(t, sizeof(t), "%s/%s/%s", h->base, e->id, tail);
    jb_kv(j, k, t);
}

int mqtt_ha_discovery(const mqtt_ha_t *h, size_t i, char *topic, size_t tcap, char *payload, size_t pcap)
{
    const ent_t *e;
    char t[MQTT_HA_TOPIC_MAX], uid[80];
    jb_t j = { payload, pcap, 0, 0 };
    if (i >= N_ENT) return -1;
    e = &g_ent[i];
    if (snprintf(topic, tcap, "%s/%s/%s/%s/config", h->prefix, e->comp, h->node, e->id) >= (int)tcap) return -1;
    if (!h->present[i]) return 0;    /* the topic is still filled: the caller clears a stale retained document with it */
    if (pcap < 2) return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof(uid), "%s_%s", h->node, e->id);

    jb_raw(&j, "{\"name\":"); jb_str(&j, e->name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof(t), "%s/status", h->base);
    jb_kv(&j, "availability_topic", t);
    if (has_state(e)) jb_topic(&j, "state_topic", h, e, "state");
    if (is_cmd(e)) jb_topic(&j, "command_topic", h, e, "set");
    if (!strcmp(e->comp, "select")) {
        jb_raw(&j, ",\"options\":[");
        const char *p = e->options;
        while (*p) {
            const char *c = strchr(p, ',');
            size_t l = c ? (size_t)(c - p) : strlen(p);
            char o[24];
            snprintf(o, sizeof(o), "%.*s", (int)l, p);
            jb_str(&j, o);
            p += l;
            if (*p == ',') { jb_raw(&j, ","); p++; }
        }
        jb_raw(&j, "]");
    } else if (!strcmp(e->comp, "number")) {
        char n[48];
        snprintf(n, sizeof(n), ",\"min\":%d,\"max\":%d,\"step\":1,\"mode\":\"slider\"", e->min, e->max);
        jb_raw(&j, n);
    }
    if (e->unit) { jb_raw(&j, ",\"unit_of_measurement\":"); jb_lit(&j, e->unit); }
    if (e->dclass) jb_kv(&j, "device_class", e->dclass);
    if (e->flags & F_MEASURE) jb_kv(&j, "state_class", "measurement");
    if (e->flags & F_DIAG) jb_kv(&j, "entity_category", "diagnostic");
    jb_raw(&j, ",\"device\":{\"identifiers\":["); jb_str(&j, h->node); jb_raw(&j, "]");
    if (h->mac[0]) { jb_raw(&j, ",\"connections\":[[\"mac\","); jb_str(&j, h->mac); jb_raw(&j, "]]"); }
    jb_raw(&j, ",\"name\":"); jb_str(&j, h->host);
    jb_kv(&j, "manufacturer", "Akam");
    jb_kv(&j, "model", "AK3918AV130 IP camera");
    if (h->version[0]) jb_kv(&j, "sw_version", h->version);
    jb_raw(&j, "}}");
    if (j.bad) return -1;
    return (int)j.n;
}

/* ------------------------------------------------------------------------ state */

static int format_state(const ent_t *e, const char *raw, char *out, size_t cap)
{
    if (!plain_text_ok(raw) || !strcmp(raw, "unknown")) return -1;
    if (e->flags & F_TENTHS) {
        char *end;
        long v = strtol(raw, &end, 10);
        if (*end || v < 0 || v > 100000) return -1;
        snprintf(out, cap, "%ld.%ld", v / 10, v % 10);
    } else if (e->flags & F_BOOL) {
        if (!strcmp(raw, "1")) snprintf(out, cap, "ON");
        else if (!strcmp(raw, "0")) snprintf(out, cap, "OFF");
        else return -1;
    } else {
        snprintf(out, cap, "%s", raw);
    }
    return 0;
}

int mqtt_ha_state(mqtt_ha_t *h, size_t i, int force, char *topic, size_t tcap, char *val, size_t vcap, int *retain)
{
    const ent_t *e;
    char raw[64];
    if (i >= N_ENT || !h->present[i]) return 0;
    e = &g_ent[i];
    if (!has_state(e)) return 0;
    if (h->ops->get(e->key, raw, sizeof(raw)) != 0) return 0;
    if (format_state(e, raw, val, vcap) != 0) return 0;
    if (!force && h->have[i] && !strcmp(h->last[i], val)) return 0;
    if (snprintf(topic, tcap, "%s/%s/state", h->base, e->id) >= (int)tcap) return 0;
    snprintf(h->last[i], sizeof(h->last[i]), "%s", val);
    h->have[i] = 1;
    *retain = (e->flags & F_SCAN) && is_cmd(e);   /* controls are retained so HA restores them */
    return 1;
}

/* ------------------------------------------------------------------------ commands */

static int parse_int_strict(const char *s, int lo, int hi, int *out)
{
    size_t i = 0, n = strlen(s);
    long v = 0;
    int neg = 0;
    if (n && s[0] == '-') { neg = 1; i = 1; }
    if (i >= n || n - i > 4) return -1;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (neg) v = -v;
    if (v < lo || v > hi) return -1;
    *out = (int)v;
    return 0;
}

/* The value to hand to the setter for payload p, or -1 when p is not allowed for this entity. */
static int command_value(const ent_t *e, const char *p, char *out, size_t cap)
{
    if (!strcmp(e->comp, "select")) {
        const char *o = e->options;
        while (*o) {
            const char *c = strchr(o, ',');
            size_t l = c ? (size_t)(c - o) : strlen(o);
            if (strlen(p) == l && !strncmp(p, o, l)) { snprintf(out, cap, "%s", p); return 0; }
            o += l;
            if (*o == ',') o++;
        }
        return -1;
    }
    if (!strcmp(e->comp, "number")) {
        int v;
        if (parse_int_strict(p, e->min, e->max, &v) != 0) return -1;
        snprintf(out, cap, "%d", v);
        return 0;
    }
    if (!strcmp(e->comp, "switch")) {
        if (!strcmp(p, "ON")) { snprintf(out, cap, "1"); return 0; }
        if (!strcmp(p, "OFF")) { snprintf(out, cap, "0"); return 0; }
        return -1;
    }
    if (!strcmp(e->comp, "button")) {
        if (!strcmp(p, "PRESS")) { snprintf(out, cap, "1"); return 0; }
        return -1;
    }
    return -1;
}

/* Token bucket in milli-tokens (1000 = one command). per_s tokens are added per second, i.e. per_s milli-tokens per
 * millisecond. A zeroed bucket starts full; a clock that goes backwards refills nothing. 1 = a token was taken. */
static int bucket_take(int64_t *milli, int64_t *last_ms, int64_t now, int burst, double per_s)
{
    int64_t cap = (int64_t)burst * 1000;
    if (*last_ms == 0 && *milli == 0) *milli = cap;       /* zeroed state: starts full */
    else if (now > *last_ms) {
        int64_t add = (int64_t)((double)(now - *last_ms) * per_s);
        *milli = *milli + add > cap ? cap : *milli + add;
    }
    *last_ms = now ? now : 1;
    if (*milli < 1000) return 0;
    *milli -= 1000;
    return 1;
}

int mqtt_ha_command(mqtt_ha_t *h, const char *topic, size_t tlen, const uint8_t *payload, size_t plen, int retained)
{
    size_t bl = strlen(h->base);
    char id[24], pl[MQTT_HA_VAL_MAX], val[24], cur[64], msg[96];
    const char *rest;
    size_t rl, il;
    h->reject_idx = -1;
    /* not ours: BASE/<id>/set only */
    if (tlen <= bl + 5 || memcmp(topic, h->base, bl) != 0 || topic[bl] != '/') return -2;
    if (memcmp(topic + tlen - 4, "/set", 4) != 0) return -2;
    rest = topic + bl + 1;
    rl = tlen - bl - 1 - 4;
    il = rl;
    if (il == 0 || il >= sizeof(id) || memchr(rest, '/', rl)) { h->invalid++; return -1; }
    memcpy(id, rest, il);
    id[il] = '\0';
    size_t idx = N_ENT;
    for (size_t i = 0; i < N_ENT; i++)
        if (is_cmd(&g_ent[i]) && !strcmp(g_ent[i].id, id)) { idx = i; break; }
    if (idx == N_ENT || !h->present[idx]) { h->invalid++; return -1; }
    if (h->ops->now_ms &&
        !bucket_take(&h->lim->cmd_milli, &h->lim->cmd_ms, h->ops->now_ms(), MQTT_HA_CMD_BURST, MQTT_HA_CMD_PER_S)) {
        h->invalid++;                            /* flood: dropped, not answered */
        return -1;
    }
    h->reject_idx = (int)idx;                    /* until the command is applied */
    if (retained) { h->invalid++; return -1; }   /* a retained command would replay at every reconnect */
    if (plen == 0 || plen >= sizeof(pl)) { h->invalid++; return -1; }
    for (size_t i = 0; i < plen; i++)
        if (payload[i] < 0x20 || payload[i] > 0x7e) { h->invalid++; return -1; }
    memcpy(pl, payload, plen);
    pl[plen] = '\0';
    if (command_value(&g_ent[idx], pl, val, sizeof(val)) != 0) { h->invalid++; return -1; }
    msg[0] = '\0';
    int saved = (g_ent[idx].flags & F_PERSIST) ? 1 : 0;
    int button = !strcmp(g_ent[idx].comp, "button");
    int64_t now = h->ops->now_ms ? h->ops->now_ms() : 0;
    int limited = h->ops->now_ms != NULL;
    /* the same value as now: nothing to apply, nothing to write, no token */
    if (!button && h->ops->get(g_ent[idx].key, cur, sizeof(cur)) == 0 && !strcmp(cur, val)) {
        h->reject_idx = -1;
        h->applied++;
        return (int)idx;
    }
    if (limited && saved && !bucket_take(&h->lim->persist_milli, &h->lim->persist_ms, now, MQTT_HA_PERSIST_BURST,
                                         1000.0 / MQTT_HA_PERSIST_REFILL_MS)) {
        h->invalid++;                             /* flash wear: bounded burst, then one saved change per refill */
        return -1;
    }
    if (limited && button && h->lim->have_beep && now - h->lim->beep_ms < MQTT_HA_BEEP_GAP_MS) {
        h->invalid++;
        h->reject_idx = -1;                       /* a button has no state to show */
        return -1;
    }
    if (h->ops->set(g_ent[idx].key, val, saved, msg, sizeof(msg)) != 0) {
        h->invalid++;
        return -1;
    }
    if (button) { h->lim->beep_ms = now; h->lim->have_beep = 1; }
    h->reject_idx = -1;
    h->applied++;
    return (int)idx;
}
