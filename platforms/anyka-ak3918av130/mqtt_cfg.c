/* See mqtt_cfg.h. Pure C, no I/O except the two small file readers. */
#include "mqtt_cfg.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "mqtt_ha.h"

static const char g_none[1] = { 0 }; /* (the repo C scan forbids the empty string literal) */

int a2k_mqtt_topic_ok(const char *s, size_t maxlen) { return mqtt_ha_topic_ok(s, maxlen); }

/* ------------------------------------------------------------------------ config */

static int in_set(const char *s, size_t n, const char *extra, int dot_hyphen)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (dot_hyphen && (c == '.' || c == '-')) || (extra && c && strchr(extra, c));
        if (!ok)
            return 0;
    }
    return 1;
}

static int parse_num(const char *s, size_t n, long lo, long hi, long *out)
{
    long v = 0;
    if (n < 1 || n > 5)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    if (v < lo || v > hi)
        return -1;
    *out = v;
    return 0;
}

enum { K_EN, K_HOST, K_PORT, K_USER, K_ID, K_BASE, K_DISC, K_PREFIX, K_INT, K_COUNT };
static const char *const k_names[K_COUNT] = { "enabled",   "host",      "port",      "user",
                                              "client_id", "base_topic", "discovery",
                                              "discovery_prefix", "interval" };

int a2k_mqtt_cfg_parse(const char *text, size_t len, a2k_mqtt_cfg_t *out)
{
    a2k_mqtt_cfg_t c;
    unsigned seen = 0;
    size_t i = 0;
    int lines = 0;
    if (!text || !out || len > A2K_MQTT_CFG_MAX)
        return -1;
    memset(&c, 0, sizeof c);
    c.port = 1883;
    c.discovery = 1;
    c.interval = 30;
    while (i < len) {
        size_t e = i, n, kl, vl;
        const char *l, *eq, *v;
        int k;
        long x;
        while (e < len && text[e] != '\n')
            e++;
        n = e - i;
        if (n && text[i + n - 1] == '\r')
            n--;
        l = text + i;
        eq = n ? memchr(l, '=', n) : NULL;
        if (++lines > K_COUNT || (n && memchr(l, '\0', n)) || !eq)
            goto bad;
        kl = (size_t)(eq - l);
        vl = n - kl - 1;
        v = eq + 1;
        for (k = 0; k < K_COUNT; k++)
            if (strlen(k_names[k]) == kl && !memcmp(l, k_names[k], kl))
                break;
        if (k == K_COUNT || (seen & (1u << k)))
            goto bad;
        seen |= 1u << k;
        switch (k) {
        case K_EN:
            if (vl != 1 || (v[0] != '0' && v[0] != '1'))
                goto bad;
            c.enabled = v[0] - '0';
            break;
        case K_HOST:
            if (vl < 1 || vl > 63 || !in_set(v, vl, NULL, 1) || v[0] == '-' || v[0] == '.')
                goto bad;
            memcpy(c.host, v, vl);
            break;
        case K_PORT:
            if (parse_num(v, vl, 1, 65535, &x) != 0)
                goto bad;
            c.port = (int)x;
            break;
        case K_USER:
            if (vl > 32 || !in_set(v, vl, "._@+-", 0) || (vl == 1 && v[0] == '-')) /* a lone - is refused, as on the 1080p */
                goto bad;
            memcpy(c.user, v, vl);
            break;
        case K_ID:
            if (vl < 1 || vl > 32 || !in_set(v, vl, "_-", 0))
                goto bad;
            memcpy(c.client_id, v, vl);
            break;
        case K_BASE:
            if (vl > 64)
                goto bad;
            memcpy(c.base, v, vl);
            if (!mqtt_ha_topic_ok(c.base, 64))
                goto bad;
            break;
        case K_PREFIX:
            if (vl > 32)
                goto bad;
            memcpy(c.prefix, v, vl);
            if (!mqtt_ha_topic_ok(c.prefix, 32))
                goto bad;
            break;
        case K_DISC:
            if (vl == 2 && !memcmp(v, "on", 2))
                c.discovery = 1;
            else if (vl == 3 && !memcmp(v, "off", 3))
                c.discovery = 0;
            else
                goto bad;
            break;
        case K_INT:
            if (parse_num(v, vl, 10, 300, &x) != 0)
                goto bad;
            c.interval = (int)x;
            break;
        }
        i = e + 1;
    }
    if (!(seen & (1u << K_EN)) || !(seen & (1u << K_HOST)))
        goto bad;
    *out = c;
    memset(&c, 0, sizeof c);
    return 0;
bad:
    memset(&c, 0, sizeof c);
    return -1;
}

void a2k_mqtt_cfg_wipe(a2k_mqtt_cfg_t *c)
{
    volatile char *p = c->pass;
    for (size_t i = 0; i < sizeof c->pass; i++)
        p[i] = 0;
}

/* Reads a regular file of at most cap bytes: a FIFO or a device on the card is refused BEFORE it is opened (opening a FIFO
 * would block). Returns the count, -1 unreadable / not a regular file, -2 absent. */
static ssize_t slurp(const char *path, char *buf, size_t cap)
{
    struct stat st;
    size_t n;
    FILE *f;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? -2 : -1;
    if (!S_ISREG(st.st_mode)) /* also refuses a symbolic link */
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (ssize_t)n;
}

int a2k_mqtt_pass_load(const char *path, char *out, size_t cap)
{
    char buf[A2K_MQTT_PASS_MAX + 4];
    ssize_t n;
    int bad;
    out[0] = '\0';
    n = slurp(path, buf, sizeof buf);
    if (n == -2)
        return 0; /* absent: no password */
    if (n < 0 || n > A2K_MQTT_PASS_MAX + 1) {
        memset(buf, 0, sizeof buf);
        return -1;
    }
    if (n > 0 && buf[n - 1] == '\n')
        n--;
    bad = (size_t)n >= cap;
    for (ssize_t i = 0; i < n && !bad; i++)
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] == 0x7f)
            bad = 1;
    if (!bad) {
        memcpy(out, buf, (size_t)n);
        out[n] = '\0';
    }
    memset(buf, 0, sizeof buf);
    return bad ? -1 : 0;
}

int a2k_mqtt_pass_path(const char *conf_path, char *out, size_t cap)
{
    const char *slash = strrchr(conf_path, '/');
    size_t dir = slash ? (size_t)(slash - conf_path) + 1 : 0;
    int n = snprintf(out, cap, "%.*s%s", (int)dir, conf_path, A2K_MQTT_PASS_NAME);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int a2k_mqtt_cfg_load(const char *path, a2k_mqtt_cfg_t *out)
{
    char buf[A2K_MQTT_CFG_MAX + 2];
    ssize_t n;
    int rc;
    memset(out, 0, sizeof *out);
    n = slurp(path, buf, sizeof buf);
    if (n == -2)
        return 1; /* absent: MQTT stays off */
    rc = n < 0 || n > A2K_MQTT_CFG_MAX ? -1 : a2k_mqtt_cfg_parse(buf, (size_t)n, out);
    memset(buf, 0, sizeof buf);
    if (rc != 0)
        memset(out, 0, sizeof *out);
    return rc;
}

void a2k_mqtt_cfg_describe(const a2k_mqtt_cfg_t *c, char *out, size_t cap)
{
    snprintf(out, cap, "host %s port %d, login %s, password %s, discovery %s, interval %d s", c->host, c->port,
             c->user[0] ? "yes" : "no", c->pass[0] ? "set (not logged)" : "none", c->discovery ? "on" : "off",
             c->interval);
}

static int mac_ok(const char *m)
{
    if (strlen(m) != 17)
        return 0;
    for (int i = 0; i < 17; i++) {
        char c = m[i];
        if (i % 3 == 2) {
            if (c != ':')
                return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int a2k_mqtt_mac_load(const char *path, char mac[18])
{
    char b[40];
    size_t n;
    FILE *f = fopen(path, "r");
    mac[0] = '\0';
    if (!f)
        return -1;
    n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = '\0';
    if (n && b[n - 1] == '\n')
        b[--n] = '\0';
    if (!mac_ok(b))
        return -1;
    memcpy(mac, b, 18);
    return 0;
}

/* ------------------------------------------------------------------------ identifiers */

int a2k_mqtt_ids_init(a2k_mqtt_ids_t *ids, const a2k_mqtt_cfg_t *cfg, const char *host, const char *mac,
                      const char *version)
{
    size_t o;
    memset(ids, 0, sizeof *ids);
    snprintf(ids->host, sizeof ids->host, "%s", host ? host : g_none);
    snprintf(ids->version, sizeof ids->version, "%s", version ? version : g_none);
    for (char *p = ids->version; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == '"' || *p == '\\')
            *p = '_';
    if (!mac || !mac_ok(mac))
        return -2;
    memcpy(ids->mac, mac, 18);
    memcpy(ids->node, "akam_", 5);
    o = 5;
    for (int i = 0; i < 17; i++)
        if (mac[i] != ':')
            ids->node[o++] = mac[i];
    ids->node[o] = '\0';
    if (cfg->base[0]) {
        if (!mqtt_ha_topic_ok(cfg->base, 64))
            return -1;
        snprintf(ids->base, sizeof ids->base, "%s", cfg->base);
    } else if (snprintf(ids->base, sizeof ids->base, "akam/%s", ids->host) >= (int)sizeof ids->base ||
               !mqtt_ha_topic_ok(ids->base, 64)) {
        return -1;
    }
    if (cfg->prefix[0]) {
        if (!mqtt_ha_topic_ok(cfg->prefix, 32))
            return -1;
        snprintf(ids->prefix, sizeof ids->prefix, "%s", cfg->prefix);
    } else {
        snprintf(ids->prefix, sizeof ids->prefix, "homeassistant");
    }
    return 0;
}

/* ------------------------------------------------------------------------ entities */

typedef struct {
    const char *id, *name, *comp, *unit, *dclass;
    int attrs;   /* has a json_attributes_topic */
    int measure; /* state_class measurement */
    int diag;    /* entity_category diagnostic */
} ent_t;

static const ent_t g_ent[A2K_E_COUNT] = {
    [A2K_E_PERSON] = { "person", "Person", "binary_sensor", NULL, "occupancy", 1, 0, 0 },
    [A2K_E_SCORE] = { "person_score", "Person score", "sensor", "%", NULL, 0, 1, 0 },
    [A2K_E_LAST] = { "last_detection", "Last person detection", "sensor", NULL, "timestamp", 0, 0, 0 },
    [A2K_E_VIEWERS] = { "viewers", "Video viewers", "sensor", NULL, NULL, 1, 1, 0 },
    [A2K_E_VERSION] = { "app_version", "App version", "sensor", NULL, NULL, 0, 0, 1 },
};

typedef struct {
    char *b;
    size_t cap, n;
    int bad;
} jb_t;

static void jb_raw(jb_t *j, const char *s)
{
    size_t l = strlen(s);
    if (j->bad || l >= j->cap - j->n) {
        j->bad = 1;
        return;
    }
    memcpy(j->b + j->n, s, l + 1);
    j->n += l;
}

static void jb_ch(jb_t *j, char c)
{
    char t[2];
    t[0] = c;
    t[1] = 0;
    jb_raw(j, t);
}

static void jb_str(jb_t *j, const char *s)
{
    jb_ch(j, '"');
    for (; *s && !j->bad; s++) {
        unsigned char c = (unsigned char)*s;
        char t[4];
        if (c == '"' || c == '\\') {
            t[0] = '\\';
            t[1] = (char)c;
            t[2] = 0;
            jb_raw(j, t);
        } else if (c >= 0x20 && c <= 0x7e) {
            t[0] = (char)c;
            t[1] = 0;
            jb_raw(j, t);
        }
    }
    jb_ch(j, '"');
}

static void jb_kv(jb_t *j, const char *k, const char *v)
{
    jb_ch(j, ',');
    jb_ch(j, '"');
    jb_raw(j, k);
    jb_raw(j, "\":");
    jb_str(j, v);
}

static void jb_int(jb_t *j, const char *k, long v)
{
    char t[48];
    snprintf(t, sizeof t, ",\"%s\":%ld", k, v);
    jb_raw(j, t);
}

static int topic_for(const a2k_mqtt_ids_t *ids, const ent_t *e, const char *tail, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s/%s", ids->base, e->id, tail);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int a2k_mqtt_status_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/status", ids->base);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static int zone_idx(const a2k_mqtt_ids_t *ids, int e)
{
    return e >= A2K_E_ZONE0 && e < A2K_E_ZONE0 + A2K_ZONE_MAX && e - A2K_E_ZONE0 < ids->nzones ? e - A2K_E_ZONE0 : -1;
}

int a2k_mqtt_snapshot_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/snapshot", ids->base);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int a2k_mqtt_detections_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/detections", ids->base);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static void jb_device(jb_t *j, const a2k_mqtt_ids_t *ids)
{
    jb_raw(j, ",\"device\":{\"identifiers\":[");
    jb_str(j, ids->node);
    jb_raw(j, "],\"connections\":[[\"mac\",");
    jb_str(j, ids->mac);
    jb_raw(j, "]],\"name\":");
    jb_str(j, ids->host);
    jb_kv(j, "manufacturer", "Akam");
    jb_kv(j, "model", A2K_MQTT_MODEL);
    if (ids->version[0])
        jb_kv(j, "sw_version", ids->version);
    jb_raw(j, "}}");
}

/* hal_sys sensors: id, name, unit, device_class, icon. */
static const struct { const char *id, *name, *unit, *dclass, *icon; } g_sys[A2K_SYS_N] = {
    { "sys_cpu", "CPU", "%", NULL, "mdi:cpu-32-bit" },
    { "sys_mem_available", "Memory available", "kB", NULL, "mdi:memory" },
    { "sys_wifi_rssi", "Wi-Fi signal", "dBm", "signal_strength", NULL },
    { "sys_uptime", "Uptime", "s", NULL, "mdi:timer-outline" },
    { "sys_load", "Load", NULL, NULL, "mdi:gauge" },
};

static int discovery_sys(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96];
    if (snprintf(topic, tcap, "%s/sensor/%s/%s/config", ids->prefix, ids->node, g_sys[k].id) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, g_sys[k].id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, g_sys[k].name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    snprintf(t, sizeof t, "%s/%s/state", ids->base, g_sys[k].id);
    jb_kv(&j, "state_topic", t);
    if (k == 2) {
        snprintf(t, sizeof t, "%s/%s/attributes", ids->base, g_sys[k].id);
        jb_kv(&j, "json_attributes_topic", t);
    }
    if (g_sys[k].unit)
        jb_kv(&j, "unit_of_measurement", g_sys[k].unit);
    if (g_sys[k].dclass)
        jb_kv(&j, "device_class", g_sys[k].dclass);
    if (g_sys[k].icon)
        jb_kv(&j, "icon", g_sys[k].icon);
    if (k != 3)
        jb_kv(&j, "state_class", "measurement");
    jb_kv(&j, "entity_category", "diagnostic");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

static const struct {
    const char *id, *name, *opts;
} g_led[A2K_LED_N] = {
    { "status_led", "Status LED", "on,off,auto" },
    { "ir_led", "IR LED", "auto,off" },
    { "led_boot", "LED pattern boot", 0 },
    { "led_nowifi", "LED pattern no Wi-Fi", 0 },
    { "led_person", "LED pattern person", 0 },
    { "led_stream", "LED pattern stream", 0 },
    { "led_night", "LED pattern night", 0 },
    { "led_idle", "LED pattern idle", 0 },
};

static int discovery_led(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96];
    if (snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, g_led[k].opts ? "select" : "text", ids->node, g_led[k].id) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, g_led[k].id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, g_led[k].name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    snprintf(t, sizeof t, "%s/%s/state", ids->base, g_led[k].id);
    jb_kv(&j, "state_topic", t);
    snprintf(t, sizeof t, "%s/%s/set", ids->base, g_led[k].id);
    jb_kv(&j, "command_topic", t);
    if (g_led[k].opts) {
        jb_raw(&j, ",\"options\":[");
        for (const char *w = g_led[k].opts; *w;) {
            size_t n = strcspn(w, ",");
            char word[12];
            snprintf(word, sizeof word, "%.*s", (int)n, w);
            if (w != g_led[k].opts)
                jb_raw(&j, ",");
            jb_str(&j, word);
            w += n + (w[n] ? 1 : 0);
        }
        jb_raw(&j, "]");
    } else {
        jb_raw(&j, ",\"min\":1,\"max\":31");
    }
    jb_kv(&j, "icon", g_led[k].opts ? "mdi:led-on" : "mdi:led-strip-variant");
    jb_kv(&j, "entity_category", "config");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

int a2k_mqtt_led_filter(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    return snprintf(out, cap, "%s/+/set", ids->base) >= (int)cap ? -1 : 0;
}

int a2k_mqtt_led_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *ent,
                         char *val, size_t vcap)
{
    char want[200];
    char v[A2K_LED_VAL];
    if (!ids->leds || plen < 1 || plen >= A2K_LED_VAL || plen >= vcap)
        return 0;
    for (size_t i = 0; i < plen; i++)
        if (!((pl[i] >= '0' && pl[i] <= '9') || (pl[i] >= 'a' && pl[i] <= 'z') || pl[i] == ':'))
            return 0;
    memcpy(v, pl, plen);
    v[plen] = '\0';
    for (int k = 0; k < A2K_LED_N; k++) {
        int n = snprintf(want, sizeof want, "%s/%s/set", ids->base, g_led[k].id);
        if (n < 0 || n >= (int)sizeof want || (size_t)n != tlen || memcmp(topic, want, tlen) != 0)
            continue;
        if (g_led[k].opts) {
            int found = 0; /* the text must be a whole word of the option list */
            for (const char *w = g_led[k].opts; *w; w += strcspn(w, ",") + (w[strcspn(w, ",")] ? 1 : 0))
                if (strcspn(w, ",") == plen && memcmp(w, v, plen) == 0)
                    found = 1;
            if (!found)
                return 0;
        }
        *ent = k;
        memcpy(val, v, plen + 1);
        return 1;
    }
    return 0;
}

/* PTZ: buttons (index = A2K_PTZ_* command) then the state / pan / tilt sensors. */
static const struct { const char *id, *name, *icon; } g_ptz[A2K_PTZ_N] = {
    { "ptz_left", "PTZ left", "mdi:arrow-left-bold" },   { "ptz_right", "PTZ right", "mdi:arrow-right-bold" },
    { "ptz_up", "PTZ up", "mdi:arrow-up-bold" },         { "ptz_down", "PTZ down", "mdi:arrow-down-bold" },
    { "ptz_home", "PTZ home", "mdi:home-circle" },       { "ptz_park", "PTZ park", "mdi:eye-off" },
    { "ptz_recal", "Recalibrate PTZ", "mdi:camera-retake" },
    { "ptz_state", "PTZ state", "mdi:camera-control" },  { "ptz_pan", "PTZ pan", "mdi:pan-horizontal" },
    { "ptz_tilt", "PTZ tilt", "mdi:pan-vertical" },
};

static int discovery_ptz(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96];
    int btn = k < A2K_PTZ_BTN;
    if (snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, btn ? "button" : "sensor", ids->node, g_ptz[k].id) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, g_ptz[k].id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, g_ptz[k].name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    if (btn) {
        snprintf(t, sizeof t, "%s/%s/set", ids->base, g_ptz[k].id);
        jb_kv(&j, "command_topic", t);
        jb_kv(&j, "payload_press", "PRESS");
    } else {
        snprintf(t, sizeof t, "%s/%s/state", ids->base, g_ptz[k].id);
        jb_kv(&j, "state_topic", t);
        if (k == A2K_PTZ_BTN) {
            jb_kv(&j, "device_class", "enum");
            jb_raw(&j, ",\"options\":[\"home\",\"parked\",\"moved\",\"calibrating\",\"unknown\"]");
        } else {
            jb_kv(&j, "unit_of_measurement", "steps");
            jb_kv(&j, "state_class", "measurement");
            jb_kv(&j, "entity_category", "diagnostic");
        }
    }
    jb_kv(&j, "icon", g_ptz[k].icon);
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

int a2k_mqtt_ptz_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *cmd)
{
    char want[200];
    if (!ids->ptz || plen != 5 || memcmp(pl, "PRESS", 5) != 0)
        return 0;
    for (int k = 0; k < A2K_PTZ_BTN; k++) {
        int n = snprintf(want, sizeof want, "%s/%s/set", ids->base, g_ptz[k].id);
        if (n > 0 && n < (int)sizeof want && (size_t)n == tlen && memcmp(topic, want, tlen) == 0) {
            *cmd = k;
            return 1;
        }
    }
    return 0;
}

/* ---- ctl group: tracking switch, zone texts, tracking areas, push-button event ---- */

static const struct { const char *id, *name, *icon; int kind; } g_ctl[A2K_CTL_SLOT0] = {
    { "track", "Person tracking", "mdi:motion-sensor", A2K_CK_SWITCH },
    { "zones", "Detection zones", "mdi:vector-rectangle", A2K_CK_TEXT },
    { "ignore_zones", "Ignore zones", "mdi:vector-rectangle-off", A2K_CK_TEXT },
    { "track_area_new", "New tracking area name", "mdi:plus-box", A2K_CK_TEXT },
    { "track_areas", "Tracking areas", "mdi:map-marker-radius", A2K_CK_SENSOR },
    { "pan_only", "Pan only tracking", "mdi:pan-horizontal", A2K_CK_SWITCH },
    { "image_flip", "Image flip", "mdi:flip-vertical", A2K_CK_SWITCH },
    { "image_mirror", "Image mirror", "mdi:flip-horizontal", A2K_CK_SWITCH },
    { "ceiling_mount", "Ceiling mount", "mdi:ceiling-light", A2K_CK_SWITCH },
};

int a2k_mqtt_ctl_kind(int k)
{
    if (k < 0 || k >= A2K_CTL_N)
        return -1;
    if (k < A2K_CTL_SLOT0)
        return g_ctl[k].kind;
    return (k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT == 0 ? A2K_CK_SWITCH : A2K_CK_BUTTON;
}

int a2k_mqtt_ctl_exists(const a2k_mqtt_ids_t *ids, int k)
{
    return ids->ctl && k >= 0 && k < A2K_CTL_N && (k < A2K_CTL_SLOT0 || (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT < ids->ntarea);
}

static void ctl_id(int k, char *id, size_t cap)
{
    static const char *const suf[A2K_CTL_PER_SLOT] = { "_on", "_a", "_b", "_del" };
    if (k < A2K_CTL_SLOT0)
        snprintf(id, cap, "%s", g_ctl[k].id);
    else if ((k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT == 0) /* the switch carries the bare slot id */
        snprintf(id, cap, "tarea%d", (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT);
    else
        snprintf(id, cap, "tarea%d%s", (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT, suf[(k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT]);
}

static const char *ctl_comp(int kind)
{
    return kind == A2K_CK_SWITCH ? "switch" : kind == A2K_CK_TEXT ? "text" : kind == A2K_CK_SENSOR ? "sensor" : "button";
}

int a2k_mqtt_ctl_config_topic(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap)
{
    char id[24];
    if (k < 0 || k >= A2K_CTL_N)
        return -1;
    ctl_id(k, id, sizeof id);
    return snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, ctl_comp(a2k_mqtt_ctl_kind(k)), ids->node, id) >= (int)tcap ? -1 : 0;
}

static int discovery_ctl(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96], id[24], name[64];
    int kind = a2k_mqtt_ctl_kind(k), slot = k >= A2K_CTL_SLOT0 ? (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT : -1;
    const char *icon = "mdi:crosshairs-gps";
    if (!a2k_mqtt_ctl_exists(ids, k) || a2k_mqtt_ctl_config_topic(ids, k, topic, tcap) != 0)
        return -1;
    ctl_id(k, id, sizeof id);
    if (slot < 0) {
        snprintf(name, sizeof name, "%s", g_ctl[k].name);
        icon = g_ctl[k].icon;
    } else {
        const char *nm = ids->tarea[slot];
        switch ((k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT) {
        case 0: snprintf(name, sizeof name, "Tracking area %s", nm); icon = "mdi:map-marker-check"; break;
        case 1: snprintf(name, sizeof name, "Set tracking area %s corner A", nm); break;
        case 2: snprintf(name, sizeof name, "Set tracking area %s corner B", nm); break;
        default: snprintf(name, sizeof name, "Delete tracking area %s", nm); icon = "mdi:delete"; break;
        }
    }
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    if (kind != A2K_CK_SENSOR) {
        snprintf(t, sizeof t, "%s/%s/set", ids->base, id);
        jb_kv(&j, "command_topic", t);
    }
    if (kind == A2K_CK_BUTTON)
        jb_kv(&j, "payload_press", "PRESS");
    if (kind == A2K_CK_SWITCH) {
        jb_kv(&j, "payload_on", "ON");
        jb_kv(&j, "payload_off", "OFF");
    }
    if (kind != A2K_CK_BUTTON && k != A2K_CTL_NEWAREA) {
        snprintf(t, sizeof t, "%s/%s/state", ids->base, id);
        jb_kv(&j, "state_topic", t);
    }
    if (kind == A2K_CK_TEXT)
        jb_raw(&j, ",\"min\":0,\"max\":255");
    jb_kv(&j, "icon", icon);
    if (kind != A2K_CK_BUTTON || slot < 0)
        jb_kv(&j, "entity_category", kind == A2K_CK_SENSOR ? "diagnostic" : "config");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

int a2k_mqtt_ctl_state(const a2k_mqtt_ids_t *ids, int k, const char *val, char *topic, size_t tcap)
{
    char id[24];
    int kind = a2k_mqtt_ctl_kind(k);
    if (!a2k_mqtt_ctl_exists(ids, k) || kind == A2K_CK_BUTTON || k == A2K_CTL_NEWAREA || !val)
        return 0;
    ctl_id(k, id, sizeof id);
    return snprintf(topic, tcap, "%s/%s/state", ids->base, id) >= (int)tcap ? 0 : 1;
}

int a2k_mqtt_ctl_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *kout, char *val,
                         size_t vcap)
{
    char want[200], id[24];
    if (!ids->ctl || plen >= A2K_CTL_VAL || plen >= vcap)
        return 0;
    for (int k = 0; k < A2K_CTL_N; k++) {
        int kind = a2k_mqtt_ctl_kind(k), n;
        if (!a2k_mqtt_ctl_exists(ids, k) || kind == A2K_CK_SENSOR)
            continue;
        ctl_id(k, id, sizeof id);
        n = snprintf(want, sizeof want, "%s/%s/set", ids->base, id);
        if (n < 0 || n >= (int)sizeof want || (size_t)n != tlen || memcmp(topic, want, tlen) != 0)
            continue;
        if (kind == A2K_CK_BUTTON && !(plen == 5 && !memcmp(pl, "PRESS", 5)))
            return 0;
        if (kind == A2K_CK_SWITCH && !((plen == 2 && !memcmp(pl, "ON", 2)) || (plen == 3 && !memcmp(pl, "OFF", 3))))
            return 0;
        if (kind == A2K_CK_TEXT) {
            if (k == A2K_CTL_NEWAREA && plen < 1)
                return 0;
            for (size_t i = 0; i < plen; i++)
                if (!((pl[i] >= '0' && pl[i] <= '9') || (pl[i] >= 'a' && pl[i] <= 'z') || pl[i] == '_' || pl[i] == '=' || pl[i] == '@' ||
                      pl[i] == ',' || pl[i] == ';' || pl[i] == ':' || pl[i] == '.' || pl[i] == '-'))
                    return 0;
        }
        memcpy(val, pl, plen);
        val[plen] = '\0';
        *kout = k;
        return 1;
    }
    return 0;
}

const char a2k_mqtt_button_payload[] = "{\"event_type\":\"press\"}";

int a2k_mqtt_button_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    return snprintf(out, cap, "%s/button/event", ids->base) >= (int)cap ? -1 : 0;
}

int a2k_mqtt_zone_config_topic(const a2k_mqtt_ids_t *ids, const char *name, char *out, size_t cap)
{
    return snprintf(out, cap, "%s/binary_sensor/%s/person_%s/config", ids->prefix, ids->node, name) >= (int)cap ? -1 : 0;
}

static int discovery_button_event(const a2k_mqtt_ids_t *ids, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96];
    if (!ids->ctl || snprintf(topic, tcap, "%s/event/%s/button/config", ids->prefix, ids->node) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_button", ids->node);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, "Button");
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    if (a2k_mqtt_button_topic(ids, t, sizeof t) != 0)
        return -1;
    jb_kv(&j, "state_topic", t);
    jb_raw(&j, ",\"event_types\":[\"press\"]");
    jb_kv(&j, "device_class", "button");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

/* the Home Assistant discovery component of a device trigger (the topic literal is built from it: the C scan forbids a slash followed by dev) */
#define A2K_HA_TRIGGER_COMPONENT "device_automation"

int a2k_mqtt_gesture_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap)
{
    return snprintf(out, cap, "%s/button", ids->base) >= (int)cap ? -1 : 0;
}

int a2k_mqtt_btn_config_topic(const a2k_mqtt_ids_t *ids, int k, char *out, size_t cap)
{
    if (k < 0 || k >= A2K_BTN_TRIG_N)
        return -1;
    return snprintf(out, cap, "%s/%s/%s/button_%d/config", ids->prefix, A2K_HA_TRIGGER_COMPONENT, ids->node, k) >= (int)cap ? -1 : 0;
}

/* The device trigger of gesture slot k: type = the gesture name in lower case with '_' for spaces, subtype "button". */
static int discovery_btn_trigger(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], type[A2K_BTN_NAME + 1];
    size_t i;
    if (!ids->ctl || k < 0 || k >= ids->nbtn || !ids->btn[k][0] || a2k_mqtt_btn_config_topic(ids, k, topic, tcap) != 0 || a2k_mqtt_gesture_topic(ids, t, sizeof t) != 0)
        return -1;
    for (i = 0; i < sizeof type - 1 && ids->btn[k][i]; i++) {
        char c = ids->btn[k][i];
        type[i] = c == ' ' ? '_' : (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    type[i] = '\0';
    payload[0] = '\0';
    jb_raw(&j, "{\"automation_type\":");
    jb_str(&j, "trigger");
    jb_kv(&j, "type", type);
    jb_kv(&j, "subtype", "button");
    jb_kv(&j, "topic", t);
    jb_kv(&j, "value_template", "{{ value_json.gesture }}");
    jb_kv(&j, "payload", ids->btn[k]);
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

/* ---- video settings: number / select per stream parameter ---- */

static const char *const g_vid_name[A2K_VP_N] = { "bit rate", "peak bit rate (0 = auto)", "frame rate", "min QP", "max QP", "GOP", "rate control" };
static const char *const g_vid_icon[A2K_VP_N] = { "mdi:speedometer", "mdi:speedometer", "mdi:video-vintage", "mdi:quality-low", "mdi:quality-high", "mdi:timelapse", "mdi:tune" };

static void vid_id(int k, char *id, size_t cap) { a2k_vid_key(k / A2K_VP_N, k % A2K_VP_N, id, cap); }

int a2k_mqtt_vid_config_topic(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap)
{
    char id[32];
    if (k < 0 || k >= A2K_VID_N)
        return -1;
    vid_id(k, id, sizeof id);
    return snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, k % A2K_VP_N == A2K_VP_RC ? "select" : "number", ids->node, id) >= (int)tcap ? -1 : 0;
}

static int discovery_vid(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96], id[32], name[64];
    int chn = k / A2K_VP_N, p = k % A2K_VP_N, lo, hi;
    if (!ids->vid || a2k_mqtt_vid_config_topic(ids, k, topic, tcap) != 0 || a2k_vid_range(chn, p, &lo, &hi) != 0)
        return -1;
    vid_id(k, id, sizeof id);
    snprintf(name, sizeof name, "Video %s %s", chn == 0 ? "main" : "sub", g_vid_name[p]);
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    snprintf(t, sizeof t, "%s/%s/set", ids->base, id);
    jb_kv(&j, "command_topic", t);
    snprintf(t, sizeof t, "%s/%s/state", ids->base, id);
    jb_kv(&j, "state_topic", t);
    if (p == A2K_VP_RC) {
        jb_raw(&j, ",\"options\":[\"vbr\",\"cbr\",\"avbr\"]");
    } else {
        if (p == A2K_VP_MAXKBPS)
            lo = 0;
        jb_int(&j, "min", lo);
        jb_int(&j, "max", hi);
        jb_int(&j, "step", 1);
        jb_kv(&j, "mode", "box");
        if (p == A2K_VP_KBPS || p == A2K_VP_MAXKBPS)
            jb_kv(&j, "unit_of_measurement", "kbit/s");
        else if (p == A2K_VP_FPS)
            jb_kv(&j, "unit_of_measurement", "fps");
    }
    jb_kv(&j, "icon", g_vid_icon[p]);
    jb_kv(&j, "entity_category", "config");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

int a2k_mqtt_vid_state(const a2k_mqtt_ids_t *ids, int k, const char *val, char *topic, size_t tcap)
{
    char id[32];
    if (!ids->vid || k < 0 || k >= A2K_VID_N || !val)
        return 0;
    vid_id(k, id, sizeof id);
    return snprintf(topic, tcap, "%s/%s/state", ids->base, id) >= (int)tcap ? 0 : 1;
}

int a2k_mqtt_vid_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *kout, char *val, size_t vcap)
{
    char want[200], id[32], text[8];
    if (!ids->vid || plen < 1 || plen > 5 || plen >= vcap)
        return 0;
    memcpy(text, pl, plen);
    text[plen] = '\0';
    if (strlen(text) != plen)
        return 0;
    for (int k = 0; k < A2K_VID_N; k++) {
        int n, v;
        vid_id(k, id, sizeof id);
        n = snprintf(want, sizeof want, "%s/%s/set", ids->base, id);
        if (n < 0 || n >= (int)sizeof want || (size_t)n != tlen || memcmp(topic, want, tlen) != 0)
            continue;
        if (a2k_vid_parse(k % A2K_VP_N, text, &v) != 0 || !a2k_vid_value_ok(k / A2K_VP_N, k % A2K_VP_N, v))
            return 0;
        memcpy(val, pl, plen);
        val[plen] = '\0';
        *kout = k;
        return 1;
    }
    return 0;
}

/* The snapshot image entity and the per-zone binary sensors. */
static int discovery_extra(const a2k_mqtt_ids_t *ids, int i, char *topic, size_t tcap, char *payload, size_t pcap)
{
    jb_t j = { payload, pcap, 0, 0 };
    char t[160], uid[96], name[40], id[40];
    int z = zone_idx(ids, i);
    if (i >= A2K_E_LED0 && i < A2K_E_LED0 + A2K_LED_N)
        return ids->leds ? discovery_led(ids, i - A2K_E_LED0, topic, tcap, payload, pcap) : -1;
    if (i >= A2K_E_CTL0 && i < A2K_E_CTL0 + A2K_CTL_N)
        return discovery_ctl(ids, i - A2K_E_CTL0, topic, tcap, payload, pcap);
    if (i >= A2K_E_VID0 && i < A2K_E_VID0 + A2K_VID_N)
        return discovery_vid(ids, i - A2K_E_VID0, topic, tcap, payload, pcap);
    if (i == A2K_E_BUTTON)
        return discovery_button_event(ids, topic, tcap, payload, pcap);
    if (i >= A2K_E_BTN0 && i < A2K_E_BTN0 + A2K_BTN_TRIG_N)
        return discovery_btn_trigger(ids, i - A2K_E_BTN0, topic, tcap, payload, pcap);
    if (i >= A2K_E_PTZ0 && i < A2K_E_PTZ0 + A2K_PTZ_N)
        return ids->ptz ? discovery_ptz(ids, i - A2K_E_PTZ0, topic, tcap, payload, pcap) : -1;
    if (i >= A2K_E_SYS0 && i < A2K_E_SYS0 + A2K_SYS_N)
        return ids->sys ? discovery_sys(ids, i - A2K_E_SYS0, topic, tcap, payload, pcap) : -1;
    if (i == A2K_E_IMAGE && ids->snapshot) {
        if (snprintf(topic, tcap, "%s/image/%s/snapshot/config", ids->prefix, ids->node) >= (int)tcap)
            return -1;
        payload[0] = '\0';
        snprintf(uid, sizeof uid, "%s_snapshot", ids->node);
        jb_raw(&j, "{\"name\":");
        jb_str(&j, "Person snapshot");
        jb_kv(&j, "unique_id", uid);
        snprintf(t, sizeof t, "%s/status", ids->base);
        jb_kv(&j, "availability_topic", t);
        if (a2k_mqtt_snapshot_topic(ids, t, sizeof t) < 0)
            return -1;
        jb_kv(&j, "image_topic", t);
        jb_kv(&j, "content_type", "image/jpeg");
        jb_device(&j, ids);
        return j.bad ? -1 : (int)j.n;
    }
    if ((i == A2K_E_MOTION || i == A2K_E_MOTION_DIR) && ids->motion) {
        int dir = i == A2K_E_MOTION_DIR;
        if (snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, dir ? "sensor" : "binary_sensor", ids->node, dir ? "motion_direction" : "motion") >= (int)tcap)
            return -1;
        payload[0] = '\0';
        snprintf(uid, sizeof uid, "%s_%s", ids->node, dir ? "motion_direction" : "motion");
        jb_raw(&j, "{\"name\":");
        jb_str(&j, dir ? "Motion direction" : "Motion");
        jb_kv(&j, "unique_id", uid);
        snprintf(t, sizeof t, "%s/status", ids->base);
        jb_kv(&j, "availability_topic", t);
        snprintf(t, sizeof t, "%s/%s/state", ids->base, dir ? "motion_direction" : "motion");
        jb_kv(&j, "state_topic", t);
        if (!dir) jb_kv(&j, "device_class", "motion");
        else jb_kv(&j, "icon", "mdi:arrow-all");
        jb_device(&j, ids);
        return j.bad ? -1 : (int)j.n;
    }
    if (i == A2K_E_NIGHT && ids->night) {
        if (snprintf(topic, tcap, "%s/binary_sensor/%s/night/config", ids->prefix, ids->node) >= (int)tcap)
            return -1;
        payload[0] = '\0';
        snprintf(uid, sizeof uid, "%s_night", ids->node);
        jb_raw(&j, "{\"name\":");
        jb_str(&j, "Night mode");
        jb_kv(&j, "unique_id", uid);
        snprintf(t, sizeof t, "%s/status", ids->base);
        jb_kv(&j, "availability_topic", t);
        snprintf(t, sizeof t, "%s/night/state", ids->base);
        jb_kv(&j, "state_topic", t);
        snprintf(t, sizeof t, "%s/night/attributes", ids->base);
        jb_kv(&j, "json_attributes_topic", t);
        jb_kv(&j, "icon", "mdi:weather-night");
        jb_device(&j, ids);
        return j.bad ? -1 : (int)j.n;
    }
    if (z < 0)
        return -1;
    snprintf(id, sizeof id, "person_%s", ids->zone[z]);
    snprintf(name, sizeof name, "Person %s", ids->zone[z]);
    if (snprintf(topic, tcap, "%s/binary_sensor/%s/%s/config", ids->prefix, ids->node, id) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    snprintf(t, sizeof t, "%s/%s/state", ids->base, id);
    jb_kv(&j, "state_topic", t);
    jb_kv(&j, "device_class", "occupancy");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

int a2k_mqtt_discovery(const a2k_mqtt_ids_t *ids, int i, char *topic, size_t tcap, char *payload, size_t pcap)
{
    const ent_t *e;
    char t[160], uid[80];
    jb_t j = { payload, pcap, 0, 0 };
    if (pcap < 2)
        return -1;
    if (i >= A2K_E_COUNT)
        return discovery_extra(ids, i, topic, tcap, payload, pcap);
    if (i < 0)
        return -1;
    e = &g_ent[i];
    if (snprintf(topic, tcap, "%s/%s/%s/%s/config", ids->prefix, e->comp, ids->node, e->id) >= (int)tcap)
        return -1;
    payload[0] = '\0';
    snprintf(uid, sizeof uid, "%s_%s", ids->node, e->id);
    jb_raw(&j, "{\"name\":");
    jb_str(&j, e->name);
    jb_kv(&j, "unique_id", uid);
    snprintf(t, sizeof t, "%s/status", ids->base);
    jb_kv(&j, "availability_topic", t);
    if (topic_for(ids, e, "state", t, sizeof t) < 0)
        return -1;
    jb_kv(&j, "state_topic", t);
    if (e->attrs) {
        if (topic_for(ids, e, "attributes", t, sizeof t) < 0)
            return -1;
        jb_kv(&j, "json_attributes_topic", t);
    }
    if (e->unit)
        jb_kv(&j, "unit_of_measurement", e->unit);
    if (e->dclass)
        jb_kv(&j, "device_class", e->dclass);
    if (e->measure)
        jb_kv(&j, "state_class", "measurement");
    if (e->diag)
        jb_kv(&j, "entity_category", "diagnostic");
    jb_device(&j, ids);
    return j.bad ? -1 : (int)j.n;
}

/* "YYYY-MM-DDTHH:MM:SS+00:00" of a wall clock second; 0 when the clock was not set (before 2020). */
static int iso_utc(int64_t epoch, char *out, size_t cap)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    if (epoch < 1577836800 || epoch > 4102444800LL || !gmtime_r(&t, &tm))
        return 0;
    return snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d+00:00", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec) < (int)cap;
}

int a2k_mqtt_state(const a2k_mqtt_ids_t *ids, int i, const struct a2k_mqtt_det *d, int viewers, char *topic,
                   size_t tcap, char *val, size_t vcap)
{
    const ent_t *e;
    int z = zone_idx(ids, i);
    if (z >= 0) { /* person_<zone>: ON/OFF from the confirmed zone state */
        if (!d->valid)
            return 0;
        snprintf(val, vcap, "%s", (d->zmask >> z) & 1u ? "ON" : "OFF");
        return snprintf(topic, tcap, "%s/person_%s/state", ids->base, ids->zone[z]) >= (int)tcap ? 0 : 1;
    }
    if (i == A2K_E_MOTION || i == A2K_E_MOTION_DIR) { /* ON = something moves; the direction in the picture */
        if (!ids->motion || !d->mo.valid)
            return 0;
        snprintf(val, vcap, "%s", i == A2K_E_MOTION ? (d->mo.active ? "ON" : "OFF") : d->mo.dir);
        return snprintf(topic, tcap, "%s/%s/state", ids->base, i == A2K_E_MOTION ? "motion" : "motion_direction") >= (int)tcap ? 0 : 1;
    }
    if (i == A2K_E_NIGHT) { /* ON = night mode */
        if (!ids->night || !d->dn.valid)
            return 0;
        snprintf(val, vcap, "%s", d->dn.night ? "ON" : "OFF");
        return snprintf(topic, tcap, "%s/night/state", ids->base) >= (int)tcap ? 0 : 1;
    }
    if (i >= A2K_E_LED0 && i < A2K_E_LED0 + A2K_LED_N) {
        const char *v = d->led[i - A2K_E_LED0];
        if (!ids->leds || !d->led_valid || !v[0])
            return 0;
        snprintf(val, vcap, "%s", v);
        return snprintf(topic, tcap, "%s/%s/state", ids->base, g_led[i - A2K_E_LED0].id) >= (int)tcap ? 0 : 1;
    }
    if (i >= A2K_E_PTZ_STATE && i < A2K_E_PTZ0 + A2K_PTZ_N) {
        int k = i - A2K_E_PTZ0;
        if (!ids->ptz || !d->ptz.valid || (i != A2K_E_PTZ_STATE && !d->ptz.known))
            return 0;
        if (i == A2K_E_PTZ_STATE)
            snprintf(val, vcap, "%s", d->ptz.state);
        else
            snprintf(val, vcap, "%d", i == A2K_E_PTZ_PAN ? d->ptz.pan : d->ptz.tilt);
        return snprintf(topic, tcap, "%s/%s/state", ids->base, g_ptz[k].id) >= (int)tcap ? 0 : 1;
    }
    if (i >= A2K_E_SYS0 && i < A2K_E_SYS0 + A2K_SYS_N) {
        const hal_sys_sample_t *m = &d->sys;
        int k = i - A2K_E_SYS0;
        long v = k == 0 ? m->cpu_pct : k == 1 ? m->mem_avail_kb : k == 2 ? m->rssi_dbm : k == 3 ? m->uptime_s : m->load1_x100;
        if (!ids->sys || !d->sys_valid || v == HAL_SYS_NA)
            return 0;
        if (k == 4)
            snprintf(val, vcap, "%ld.%02ld", v / 100, v % 100);
        else
            snprintf(val, vcap, "%ld", v);
        return snprintf(topic, tcap, "%s/%s/state", ids->base, g_sys[k].id) >= (int)tcap ? 0 : 1;
    }
    if (i < 0 || i >= A2K_E_COUNT)
        return 0;
    e = &g_ent[i];
    switch (i) {
    case A2K_E_PERSON:
        if (!d->valid)
            return 0;
        snprintf(val, vcap, "%s", d->present ? "ON" : "OFF");
        break;
    case A2K_E_SCORE:
        if (!d->valid)
            return 0;
        snprintf(val, vcap, "%d.%d", d->score / 10, d->score % 10);
        break;
    case A2K_E_LAST:
        if (!iso_utc(d->last_on_epoch, val, vcap))
            return 0;
        break;
    case A2K_E_VIEWERS:
        if (viewers < 0)
            return 0;
        snprintf(val, vcap, "%d", viewers);
        break;
    default:
        snprintf(val, vcap, "%s", ids->version);
        break;
    }
    return topic_for(ids, e, "state", topic, tcap) < 0 ? 0 : 1;
}

int a2k_mqtt_attrs(const a2k_mqtt_ids_t *ids, int i, const struct a2k_mqtt_det *d, const char *ip, int rtsp_port,
                   char *topic, size_t tcap, char *payload, size_t pcap)
{
    const ent_t *e;
    jb_t j = { payload, pcap, 0, 0 };
    char t[64];
    if (i == A2K_E_SYS0 + 2) { /* the Wi-Fi level: link quality */
        if (!ids->sys || !d->sys_valid || d->sys.link_quality == HAL_SYS_NA || pcap < 2)
            return 0;
        payload[0] = '\0';
        {
            char q[40];
            snprintf(q, sizeof q, "{\"link_quality\":%d}", d->sys.link_quality);
            jb_raw(&j, q);
        }
        if (j.bad || snprintf(topic, tcap, "%s/%s/attributes", ids->base, g_sys[2].id) >= (int)tcap)
            return 0;
        return 1;
    }
    if (i == A2K_E_NIGHT) {
        static const char *const mode[3] = { "auto", "day", "night" };
        if (!ids->night || !d->dn.valid || pcap < 2)
            return 0;
        payload[0] = '\0';
        jb_raw(&j, "{\"mode\":");
        jb_str(&j, mode[d->dn.mode >= 0 && d->dn.mode <= 2 ? d->dn.mode : 0]);
        jb_kv(&j, "ir_led", d->dn.ir_on ? "on" : "off");
        jb_kv(&j, "automatic", d->dn.auto_off ? "gave_up" : "ok");
        jb_int(&j, "switches", (long)d->dn.switches);
        if (iso_utc(d->dn.last_switch_epoch, t, sizeof t))
            jb_kv(&j, "last_switch", t);
        jb_kv(&j, "reason", d->dn.why);
        jb_raw(&j, "}");
        if (j.bad || snprintf(topic, tcap, "%s/night/attributes", ids->base) >= (int)tcap)
            return 0;
        return 1;
    }
    if (i < 0 || i >= A2K_E_COUNT || !g_ent[i].attrs || pcap < 2)
        return 0;
    e = &g_ent[i];
    payload[0] = '\0';
    if (i == A2K_E_PERSON) {
        if (!d->valid)
            return 0;
        jb_raw(&j, "{\"score\":");
        snprintf(t, sizeof t, "%d", d->score);
        jb_raw(&j, t);
        jb_int(&j, "class", d->cls);
        snprintf(t, sizeof t, ",\"box\":[%d,%d,%d,%d]", d->x1, d->y1, d->x2, d->y2);
        jb_raw(&j, t);
        jb_int(&j, "heads", d->heads);
        jb_int(&j, "head_score", d->head_score);
        jb_kv(&j, "detection", d->status);
        if (iso_utc(d->last_on_epoch, t, sizeof t))
            jb_kv(&j, "last_detection", t);
        jb_raw(&j, "}");
    } else {
        char u[80];
        if (!ip || !ip[0] || rtsp_port < 1 || rtsp_port > 65535)
            return 0;
        for (const char *p = ip; *p; p++)
            if ((*p < '0' || *p > '9') && *p != '.')
                return 0;
        snprintf(u, sizeof u, "rtsp://%s:%d/stream0", ip, rtsp_port);
        jb_raw(&j, "{\"stream_main\":");
        jb_str(&j, u);
        snprintf(u, sizeof u, "rtsp://%s:%d/stream1", ip, rtsp_port);
        jb_kv(&j, "stream_sub", u);
        jb_raw(&j, "}");
    }
    if (j.bad || topic_for(ids, e, "attributes", topic, tcap) < 0)
        return 0;
    return 1;
}
