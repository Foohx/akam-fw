/* Host test of src/ak_rtsp/mqtt_ha.c with a fake camera: entity table, Home Assistant discovery documents (strict
 * JSON check, required keys, unique ids, size), state formatting and change detection, and the command allow-list
 * (every entity against valid, invalid, oversized and hostile payloads; nothing outside the allow-list reaches the
 * setter, whatever the topic). Run under ASan/UBSan. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mqtt_ha.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); g_failed++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want))) { \
    fprintf(stderr, "FAIL line %d:\n  got  [%s]\n  want [%s]\n", __LINE__, (got), (want)); g_failed++; } } while (0)

/* ------------------------------------------------------------------ fake camera */
typedef struct { const char *key; char val[48]; } kv_t;
static kv_t g_kv[] = {
    { "led.status", "auto" }, { "night.ir_led", "auto" }, { "night.mode", "auto" }, { "audio.out_volume", "60" },
    { "isp.brightness", "0" }, { "isp.contrast", "5" }, { "isp.saturation", "-7" }, { "isp.sharpness", "50" },
    { "audio.talk_enable", "1" }, { "audio.out_test", "0" }, { "night.state", "day" }, { "sys.time_set", "0" },
    { "rtsp.clients", "0" }, { "net.wifi_rssi", "-61" }, { "sys.uptime", "1234" }, { "sys.cpu", "7" },
    { "sys.mem_free", "20480" }, { "sys.temp", "unknown" }, { "video.fps_actual", "149" }, { "video.bitrate", "1500" },
    { "net.mac", "aa:bb:cc:dd:ee:0f" },
};
#define NKV (sizeof(g_kv) / sizeof(g_kv[0]))

static int f_get(const char *key, char *out, size_t cap)
{
    for (size_t i = 0; i < NKV; i++)
        if (!strcmp(g_kv[i].key, key)) { snprintf(out, cap, "%s", g_kv[i].val); return 0; }
    return -1;
}
static void kv_set(const char *key, const char *v)
{
    for (size_t i = 0; i < NKV; i++)
        if (!strcmp(g_kv[i].key, key)) { snprintf(g_kv[i].val, sizeof(g_kv[i].val), "%s", v); return; }
}

static int g_set_calls, g_set_persist;
static char g_set_key[64], g_set_val[64];
static int g_set_fail;
static int f_set(const char *key, const char *value, int persist, char *msg, size_t cap)
{
    g_set_calls++;
    g_set_persist = persist;
    snprintf(g_set_key, sizeof(g_set_key), "%s", key);
    snprintf(g_set_val, sizeof(g_set_val), "%s", value);
    if (g_set_fail) { snprintf(msg, cap, "ERR refused"); return -1; }
    kv_set(key, value);
    snprintf(msg, cap, "OK %s=%s", key, value);
    return 0;
}
static void f_host(char *out) { strcpy(out, "akam-0f0f"); }
static const char *f_ver(void) { return "b12-abc1234"; }
static const mqtt_ops_t g_ops = { f_get, f_set, f_host, f_ver };

/* ------------------------------------------------------------------ strict JSON validator (objects of strings, numbers, arrays, objects) */
typedef struct { const char *p, *end; int err; int depth; } js_t;
static void js_ws(js_t *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static void js_value(js_t *j);
static void js_string(js_t *j)
{
    if (j->p >= j->end || *j->p != '"') { j->err = 1; return; }
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 0x20) { j->err = 1; return; }
        if (c == '\\') {
            j->p++;
            if (j->p >= j->end) { j->err = 1; return; }
            if (*j->p == 'u') {
                for (int i = 1; i <= 4; i++)
                    if (j->p + i >= j->end || !strchr("0123456789abcdefABCDEF", j->p[i])) { j->err = 1; return; }
                j->p += 4;
            } else if (!strchr("\"\\/bfnrt", *j->p)) { j->err = 1; return; }
        }
        j->p++;
    }
    if (j->p >= j->end) { j->err = 1; return; }
    j->p++;
}
static void js_value(js_t *j)
{
    js_ws(j);
    if (j->err || j->p >= j->end || ++j->depth > 8) { j->err = 1; return; }
    if (*j->p == '"') js_string(j);
    else if (*j->p == '{') {
        j->p++;
        js_ws(j);
        if (j->p < j->end && *j->p == '}') j->p++;
        else for (;;) {
            js_ws(j); js_string(j); js_ws(j);
            if (j->err || j->p >= j->end || *j->p != ':') { j->err = 1; break; }
            j->p++;
            js_value(j);
            js_ws(j);
            if (j->err || j->p >= j->end) { j->err = 1; break; }
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == '}') { j->p++; break; }
            j->err = 1; break;
        }
    } else if (*j->p == '[') {
        j->p++;
        js_ws(j);
        if (j->p < j->end && *j->p == ']') j->p++;
        else for (;;) {
            js_value(j);
            js_ws(j);
            if (j->err || j->p >= j->end) { j->err = 1; break; }
            if (*j->p == ',') { j->p++; continue; }
            if (*j->p == ']') { j->p++; break; }
            j->err = 1; break;
        }
    } else if ((*j->p >= '0' && *j->p <= '9') || *j->p == '-') {
        j->p++;
        while (j->p < j->end && *j->p >= '0' && *j->p <= '9') j->p++;
    } else j->err = 1;
    j->depth--;
}
static int json_ok(const char *s)
{
    js_t j = { s, s + strlen(s), 0, 0 };
    js_value(&j);
    js_ws(&j);
    return !j.err && j.p == j.end;
}
static int has(const char *doc, const char *frag) { return strstr(doc, frag) != NULL; }

/* ------------------------------------------------------------------ tests */
static void test_init(void)
{
    mqtt_ha_t h;
    CHECK(mqtt_ha_init(&h, &g_ops, "", "", 1) == 0);
    CHECK_STR(h.base, "akam/akam-0f0f");
    CHECK_STR(h.prefix, "homeassistant");
    CHECK_STR(h.node, "akam_aabbccddee0f");
    CHECK_STR(h.mac, "aa:bb:cc:dd:ee:0f");
    CHECK_STR(h.version, "b12-abc1234");
    CHECK(!mqtt_ha_entity_present(&h, 17));            /* temperature: unknown on this camera */
    CHECK(mqtt_ha_present(&h) == mqtt_ha_count() - 1);
    CHECK(mqtt_ha_count() <= MQTT_HA_MAX_ENT);

    kv_set("sys.temp", "41.5");
    kv_set("net.wifi_rssi", "unknown");
    mqtt_ha_init(&h, &g_ops, "home/cam1", "ha", 0);
    CHECK_STR(h.base, "home/cam1");
    CHECK_STR(h.prefix, "ha");
    CHECK(mqtt_ha_present(&h) == mqtt_ha_count() - 1);  /* now the Wi-Fi signal is the missing one */
    int rssi = -1, temp = -1;
    for (size_t i = 0; i < mqtt_ha_count(); i++) {
        if (!strcmp(mqtt_ha_entity_id(i), "wifi_signal")) rssi = mqtt_ha_entity_present(&h, i);
        if (!strcmp(mqtt_ha_entity_id(i), "temperature")) temp = mqtt_ha_entity_present(&h, i);
    }
    CHECK(rssi == 0 && temp == 1);
    kv_set("sys.temp", "unknown");
    kv_set("net.wifi_rssi", "-61");

    /* an unknown MAC falls back to the host name */
    kv_set("net.mac", "unknown");
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    CHECK_STR(h.node, "akam_akam-0f0f");
    CHECK(h.mac[0] == '\0');
    kv_set("net.mac", "AA:BB:CC:DD:EE:0F");              /* upper case is not what ctl_sys reports: refused, no half-trusted id */
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    CHECK(h.mac[0] == '\0');
    kv_set("net.mac", "aa:bb:cc:dd:ee:0f");

    /* invalid topics are refused */
    CHECK(mqtt_ha_init(&h, &g_ops, "a/+/b", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "a/#", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "/a", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "a/", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "a//b", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "a b", "", 1) == -1);
    CHECK(mqtt_ha_init(&h, &g_ops, "", "x y", 1) == -1);
    char longb[80];
    memset(longb, 'a', 65); longb[65] = 0;
    CHECK(mqtt_ha_init(&h, &g_ops, longb, "", 1) == -1);
    longb[64] = 0;
    CHECK(mqtt_ha_init(&h, &g_ops, longb, "", 1) == 0);
    CHECK(mqtt_ha_topic_ok("a-b_c/D1", 64) && !mqtt_ha_topic_ok("", 64) && !mqtt_ha_topic_ok("é", 64));
}

static void test_discovery(void)
{
    mqtt_ha_t h;
    char t[MQTT_HA_TOPIC_MAX], d[MQTT_HA_PAYLOAD_MAX];
    char uids[MQTT_HA_MAX_ENT][100];
    size_t nuid = 0, maxlen = 0;
    kv_set("sys.temp", "41.5");
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    for (size_t i = 0; i < mqtt_ha_count(); i++) {
        int n = mqtt_ha_discovery(&h, i, t, sizeof(t), d, sizeof(d));
        CHECK(n > 0 && (size_t)n == strlen(d));
        if (n <= 0) continue;
        if ((size_t)n > maxlen) maxlen = (size_t)n;
        CHECK(json_ok(d));
        CHECK(has(t, "homeassistant/") && has(t, "/akam_aabbccddee0f/") && has(t, "/config"));
        CHECK(has(d, "\"name\":\"") && has(d, "\"unique_id\":\"akam_aabbccddee0f_"));
        CHECK(has(d, "\"availability_topic\":\"akam/akam-0f0f/status\""));
        CHECK(has(d, "\"device\":{\"identifiers\":[\"akam_aabbccddee0f\"]"));
        CHECK(has(d, "\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:0f\"]]"));
        CHECK(has(d, "\"sw_version\":\"b12-abc1234\"") && has(d, "\"manufacturer\":\"Akam\""));
        int is_btn = has(t, "/button/"), is_sel = has(t, "/select/"), is_num = has(t, "/number/"), is_sw = has(t, "/switch/");
        CHECK(has(d, "\"state_topic\":") == !is_btn);
        CHECK(has(d, "\"command_topic\":\"akam/akam-0f0f/") == (is_btn || is_sel || is_num || is_sw));
        CHECK(has(d, "\"options\":[") == is_sel);
        CHECK(has(d, "\"mode\":\"slider\"") == is_num);
        /* the topic's entity segment is the one in the unique id and in every topic of the document */
        const char *id = mqtt_ha_entity_id(i);
        char frag[64];
        snprintf(frag, sizeof(frag), "/%s/config", id);
        CHECK(has(t, frag));
        snprintf(frag, sizeof(frag), "_%s\"", id);
        CHECK(has(d, frag));
        /* unique ids are unique */
        const char *u = strstr(d, "\"unique_id\":\"") + 13;
        snprintf(uids[nuid], sizeof(uids[nuid]), "%.*s", (int)strcspn(u, "\""), u);
        for (size_t k = 0; k < nuid; k++) CHECK(strcmp(uids[k], uids[nuid]));
        nuid++;
        /* no secret and no network identity other than the MAC the spec allows */
        CHECK(!has(d, "pass") && !has(d, "ssid") && !has(d, "192."));
    }
    CHECK(nuid == mqtt_ha_present(&h));
    CHECK(maxlen < 900);
    printf("mqtt_ha_test: %zu entities, longest discovery document %zu bytes\n", nuid, maxlen);

    /* golden documents, character for character: the keys HA reads, in the order the builder writes them */
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    mqtt_ha_discovery(&h, 0, t, sizeof(t), d, sizeof(d));
    CHECK_STR(t, "homeassistant/select/akam_aabbccddee0f/status_led/config");
    CHECK_STR(d, "{\"name\":\"Status LED\",\"unique_id\":\"akam_aabbccddee0f_status_led\","
                 "\"availability_topic\":\"akam/akam-0f0f/status\",\"state_topic\":\"akam/akam-0f0f/status_led/state\","
                 "\"command_topic\":\"akam/akam-0f0f/status_led/set\",\"options\":[\"on\",\"off\",\"auto\"],"
                 "\"device\":{\"identifiers\":[\"akam_aabbccddee0f\"],\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:0f\"]],"
                 "\"name\":\"akam-0f0f\",\"manufacturer\":\"Akam\",\"model\":\"AK3918AV130 IP camera\","
                 "\"sw_version\":\"b12-abc1234\"}}");
    size_t vol = 3;
    CHECK_STR(mqtt_ha_entity_id(vol), "volume");
    mqtt_ha_discovery(&h, vol, t, sizeof(t), d, sizeof(d));
    CHECK(has(d, "\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_measurement\":\"%\""));
    size_t bri = 4;
    mqtt_ha_discovery(&h, bri, t, sizeof(t), d, sizeof(d));
    CHECK(has(d, "\"min\":-50,\"max\":50,\"step\":1"));
    for (size_t i = 0; i < mqtt_ha_count(); i++) {
        mqtt_ha_discovery(&h, i, t, sizeof(t), d, sizeof(d));
        if (!strcmp(mqtt_ha_entity_id(i), "temperature")) CHECK(has(d, "\"unit_of_measurement\":\"\\u00b0C\"") && json_ok(d));
        if (!strcmp(mqtt_ha_entity_id(i), "wifi_signal")) CHECK(has(d, "\"device_class\":\"signal_strength\"") && has(d, "\"unit_of_measurement\":\"dBm\""));
        if (!strcmp(mqtt_ha_entity_id(i), "beep")) CHECK(!has(d, "state_topic") && has(t, "/button/"));
    }

    /* an entity the camera does not have publishes nothing */
    kv_set("sys.temp", "unknown");
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    CHECK(mqtt_ha_discovery(&h, 17, t, sizeof(t), d, sizeof(d)) == 0);
    /* buffers too small are refused, never overrun */
    mqtt_ha_discovery(&h, 0, t, sizeof(t), d, sizeof(d));
    size_t full = strlen(d);
    for (size_t cap = 2; cap <= full; cap += 37) {
        char *small = malloc(cap);
        CHECK(mqtt_ha_discovery(&h, 0, t, sizeof(t), small, cap) == -1);
        free(small);
    }
    char *exact = malloc(full + 1);
    CHECK(mqtt_ha_discovery(&h, 0, t, sizeof(t), exact, full + 1) == (int)full);
    free(exact);
    char st[10];
    CHECK(mqtt_ha_discovery(&h, 0, st, sizeof(st), d, sizeof(d)) == -1);     /* topic does not fit */
    CHECK(mqtt_ha_discovery(&h, 99, t, sizeof(t), d, sizeof(d)) == -1);

    /* a hostile version string cannot break the JSON */
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    snprintf(h.version, sizeof(h.version), "x\"y\\z\n");
    mqtt_ha_discovery(&h, 0, t, sizeof(t), d, sizeof(d));
    CHECK(json_ok(d));

    char f[MQTT_HA_TOPIC_MAX];
    CHECK(mqtt_ha_status_topic(&h, f, sizeof(f)) > 0 && !strcmp(f, "akam/akam-0f0f/status"));
    CHECK(mqtt_ha_cmd_filter(&h, f, sizeof(f)) > 0 && !strcmp(f, "akam/akam-0f0f/+/set"));
    CHECK(mqtt_ha_status_topic(&h, f, 5) == -1 && mqtt_ha_cmd_filter(&h, f, 5) == -1);
}

static size_t idx_of(const char *id)
{
    for (size_t i = 0; i < mqtt_ha_count(); i++)
        if (!strcmp(mqtt_ha_entity_id(i), id)) return i;
    return (size_t)-1;
}

static int state_of(mqtt_ha_t *h, const char *id, int force, char *val, size_t vcap, int *retain)
{
    char t[MQTT_HA_TOPIC_MAX];
    return mqtt_ha_state(h, idx_of(id), force, t, sizeof(t), val, vcap, retain);
}

static void test_state(void)
{
    mqtt_ha_t h;
    char v[MQTT_HA_VAL_MAX], t[MQTT_HA_TOPIC_MAX];
    int retain = -1;
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    CHECK(state_of(&h, "status_led", 0, v, sizeof(v), &retain) == 1 && !strcmp(v, "auto") && retain == 1);
    CHECK(state_of(&h, "status_led", 0, v, sizeof(v), &retain) == 0);            /* unchanged */
    CHECK(state_of(&h, "status_led", 1, v, sizeof(v), &retain) == 1);            /* forced */
    kv_set("led.status", "off");
    CHECK(state_of(&h, "status_led", 0, v, sizeof(v), &retain) == 1 && !strcmp(v, "off"));
    mqtt_ha_state(&h, idx_of("status_led"), 1, t, sizeof(t), v, sizeof(v), &retain);
    CHECK_STR(t, "akam/akam-0f0f/status_led/state");
    CHECK(state_of(&h, "volume", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "60") && retain == 1);
    CHECK(state_of(&h, "saturation", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "-7"));
    CHECK(state_of(&h, "talk", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "ON") && retain == 1);
    kv_set("audio.talk_enable", "0");
    CHECK(state_of(&h, "talk", 0, v, sizeof(v), &retain) == 1 && !strcmp(v, "OFF"));
    CHECK(state_of(&h, "clock_set", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "OFF") && retain == 0);
    CHECK(state_of(&h, "fps", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "14.9") && retain == 0);
    kv_set("video.fps_actual", "150");
    state_of(&h, "fps", 1, v, sizeof(v), &retain);
    CHECK_STR(v, "15.0");
    kv_set("video.fps_actual", "x1");
    CHECK(state_of(&h, "fps", 1, v, sizeof(v), &retain) == 0);                   /* junk is not published */
    kv_set("video.fps_actual", "unknown");
    CHECK(state_of(&h, "fps", 1, v, sizeof(v), &retain) == 0);
    CHECK(state_of(&h, "wifi_signal", 1, v, sizeof(v), &retain) == 1 && !strcmp(v, "-61"));
    CHECK(state_of(&h, "beep", 1, v, sizeof(v), &retain) == 0);                  /* a button has no state */
    CHECK(state_of(&h, "temperature", 1, v, sizeof(v), &retain) == 0);           /* absent entity */
    kv_set("night.state", "bad\x01value");
    CHECK(state_of(&h, "night_state", 1, v, sizeof(v), &retain) == 0);           /* control characters never leave */
    kv_set("night.state", "0123456789012345678901234");                         /* too long for the buffer */
    CHECK(state_of(&h, "night_state", 1, v, sizeof(v), &retain) == 0);
    kv_set("night.state", "night");
    CHECK(state_of(&h, "night_state", 0, v, sizeof(v), &retain) == 1 && !strcmp(v, "night") && retain == 0);
    char small[3];
    CHECK(mqtt_ha_state(&h, idx_of("uptime"), 1, t, sizeof(t), small, sizeof(small), &retain) >= 0);   /* no overrun (ASan) */
    CHECK(mqtt_ha_state(&h, 999, 1, t, sizeof(t), v, sizeof(v), &retain) == 0);
    CHECK(mqtt_ha_state(&h, idx_of("uptime"), 1, t, 4, v, sizeof(v), &retain) == 0);   /* topic too small */

    /* scan and periodic sets are disjoint and cover every entity that has a state */
    for (size_t i = 0; i < mqtt_ha_count(); i++) {
        const char *id = mqtt_ha_entity_id(i);
        int periodic = mqtt_ha_is_periodic(i);
        if (!strcmp(id, "uptime") || !strcmp(id, "cpu") || !strcmp(id, "fps") || !strcmp(id, "wifi_signal")) CHECK(periodic);
        if (!strcmp(id, "status_led") || !strcmp(id, "night_state") || !strcmp(id, "beep")) CHECK(!periodic);
    }
}

typedef struct { const char *id; const char *payload; int ok; const char *key; const char *val; int persist; } cmd_t;

static const cmd_t k_cmds[] = {
    { "status_led", "on", 1, "led.status", "on", 1 }, { "status_led", "off", 1, "led.status", "off", 1 },
    { "status_led", "auto", 1, "led.status", "auto", 1 }, { "status_led", "ON", 0, 0, 0, 0 },
    { "status_led", "blink", 0, 0, 0, 0 }, { "status_led", "auto ", 0, 0, 0, 0 }, { "status_led", " auto", 0, 0, 0, 0 },
    { "status_led", "au", 0, 0, 0, 0 }, { "status_led", "autoo", 0, 0, 0, 0 }, { "status_led", "1", 0, 0, 0, 0 },
    { "ir_led", "auto", 1, "night.ir_led", "auto", 1 }, { "ir_led", "off", 1, "night.ir_led", "off", 1 },
    { "ir_led", "on", 0, 0, 0, 0 },
    { "night_mode", "day", 1, "night.mode", "day", 1 }, { "night_mode", "night", 1, "night.mode", "night", 1 },
    { "night_mode", "auto", 1, "night.mode", "auto", 1 }, { "night_mode", "Day", 0, 0, 0, 0 },
    { "volume", "0", 1, "audio.out_volume", "0", 0 }, { "volume", "100", 1, "audio.out_volume", "100", 0 },
    { "volume", "55", 1, "audio.out_volume", "55", 0 }, { "volume", "101", 0, 0, 0, 0 }, { "volume", "-1", 0, 0, 0, 0 },
    { "volume", "5.0", 0, 0, 0, 0 }, { "volume", "1e1", 0, 0, 0, 0 }, { "volume", "+5", 0, 0, 0, 0 },
    { "volume", "0x10", 0, 0, 0, 0 }, { "volume", "", 0, 0, 0, 0 }, { "volume", "-", 0, 0, 0, 0 },
    { "volume", "99999", 0, 0, 0, 0 }, { "volume", "00005", 0, 0, 0, 0 }, { "volume", "50 ", 0, 0, 0, 0 },
    { "volume", "ON", 0, 0, 0, 0 },
    { "brightness", "-50", 1, "isp.brightness", "-50", 0 }, { "brightness", "50", 1, "isp.brightness", "50", 0 },
    { "brightness", "-51", 0, 0, 0, 0 }, { "brightness", "51", 0, 0, 0, 0 },
    { "contrast", "7", 1, "isp.contrast", "7", 0 }, { "contrast", "--7", 0, 0, 0, 0 },
    { "saturation", "-7", 1, "isp.saturation", "-7", 0 }, { "sharpness", "0", 1, "isp.sharpness", "0", 0 },
    { "talk", "ON", 1, "audio.talk_enable", "1", 1 }, { "talk", "OFF", 1, "audio.talk_enable", "0", 1 },
    { "talk", "on", 0, 0, 0, 0 }, { "talk", "1", 0, 0, 0, 0 }, { "talk", "TOGGLE", 0, 0, 0, 0 },
    { "beep", "PRESS", 1, "audio.out_test", "1", 0 }, { "beep", "press", 0, 0, 0, 0 }, { "beep", "1", 0, 0, 0, 0 },
    { "beep", "PRESSPRESS", 0, 0, 0, 0 },
};

static int cmd(mqtt_ha_t *h, const char *id, const char *payload, int retained)
{
    char topic[160];
    int n = snprintf(topic, sizeof(topic), "%s/%s/set", h->base, id);
    size_t pl = strlen(payload);
    uint8_t *p = malloc(pl ? pl : 1);                  /* exact size: no NUL, no read past the end */
    memcpy(p, payload, pl);
    int r = mqtt_ha_command(h, topic, (size_t)n, p, pl, retained);
    free(p);
    return r;
}

static void test_commands(void)
{
    mqtt_ha_t h;
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    for (size_t i = 0; i < sizeof(k_cmds) / sizeof(k_cmds[0]); i++) {
        const cmd_t *c = &k_cmds[i];
        g_set_calls = 0;
        int r = cmd(&h, c->id, c->payload, 0);
        if (c->ok) {
            CHECK(r == (int)idx_of(c->id));
            CHECK(g_set_calls == 1 && !strcmp(g_set_key, c->key) && !strcmp(g_set_val, c->val) && g_set_persist == c->persist);
        } else {
            if (r != -1 || g_set_calls != 0) fprintf(stderr, "  rejected case %s [%s] -> %d, setter calls %d\n", c->id, c->payload, r, g_set_calls);
            CHECK(r == -1 && g_set_calls == 0);
        }
    }
    CHECK(h.applied > 0 && h.invalid > 0);

    /* a setter that refuses: not applied, counted */
    unsigned inv = h.invalid;
    g_set_fail = 1;
    CHECK(cmd(&h, "volume", "10", 0) == -1 && h.invalid == inv + 1);
    g_set_fail = 0;

    /* retained messages are never obeyed */
    g_set_calls = 0;
    CHECK(cmd(&h, "beep", "PRESS", 1) == -1 && g_set_calls == 0);

    /* entities that are not commands, or do not exist: the allow-list is the table */
    static const char *const hostile[] = { "uptime", "cpu", "wifi_signal", "fps", "night_state", "clock_set", "reboot",
        "restart", "wifi", "password", "ssid", "hostname", "rtsp_auth", "led.status", "audio.out_volume", "..", ".",
        "", "status", "STATUS_LED", "status_led/", "volume/x", "+", "#", "temperature" };
    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
        g_set_calls = 0;
        CHECK(cmd(&h, hostile[i], "PRESS", 0) < 0 && g_set_calls == 0);
        CHECK(cmd(&h, hostile[i], "ON", 0) < 0 && g_set_calls == 0);
        CHECK(cmd(&h, hostile[i], "reboot", 0) < 0 && g_set_calls == 0);
    }

    /* topics that are not command topics at all answer -2 and are not counted */
    inv = h.invalid;
    static const char *const other[] = { "akam/akam-0f0f/status", "akam/akam-0f0f/volume/state", "akam/other/volume/set",
        "akam/akam-0f0f", "akam/akam-0f0f/", "akam/akam-0f0f/set", "akam/akam-0f0fx/volume/set", "x", "",
        "akam/akam-0f0f/volume/setx", "homeassistant/status" };
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++)
        CHECK(mqtt_ha_command(&h, other[i], strlen(other[i]), (const uint8_t *)"10", 2, 0) == -2);
    CHECK(h.invalid == inv && g_set_calls == 0);

    /* payload hygiene: NUL, control and high bytes, length bounds */
    static const uint8_t nul[] = { '1', 0, '0' };
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, nul, 3, 0) == -1);
    static const uint8_t hi[] = { 0xc3, 0xa9 };
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/status_led/set", 29, hi, 2, 0) == -1);
    static const uint8_t ctl[] = { 'o', 'n', '\n' };
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/status_led/set", 29, ctl, 3, 0) == -1);
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, (const uint8_t *)"", 0, 0) == -1);
    uint8_t big[4096];
    memset(big, '1', sizeof(big));
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, big, sizeof(big), 0) == -1);
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, big, MQTT_HA_VAL_MAX, 0) == -1);
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, big, MQTT_HA_VAL_MAX - 1, 0) == -1);   /* printable, still not a number in range */
    CHECK(g_set_calls == 0);

    /* with a custom base topic */
    mqtt_ha_init(&h, &g_ops, "home/cam1", "", 1);
    g_set_calls = 0;
    CHECK(mqtt_ha_command(&h, "home/cam1/volume/set", 20, (const uint8_t *)"33", 2, 0) == (int)idx_of("volume") && g_set_calls == 1);
    CHECK(mqtt_ha_command(&h, "akam/akam-0f0f/volume/set", 25, (const uint8_t *)"33", 2, 0) == -2);
}

/* Random topics and payloads: the setter must only ever see an allow-listed key with a value of the right shape. */
static uint32_t g_rng = 99;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static int allowed_key(const char *k)
{
    static const char *const ok[] = { "led.status", "night.ir_led", "night.mode", "audio.out_volume", "isp.brightness",
        "isp.contrast", "isp.saturation", "isp.sharpness", "audio.talk_enable", "audio.out_test" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        if (!strcmp(k, ok[i])) return 1;
    return 0;
}

static void test_fuzz(void)
{
    mqtt_ha_t h;
    static const char *const ids[] = { "status_led", "ir_led", "night_mode", "volume", "brightness", "contrast",
        "saturation", "sharpness", "talk", "beep", "uptime", "reboot", "x" };
    static const char *const words[] = { "on", "off", "auto", "day", "night", "ON", "OFF", "PRESS", "0", "1", "-50", "50",
        "100", "101", "-51", "9", "x", "reboot", "1;reboot", "$(reboot)", "50\n", "5 0", "" };
    mqtt_ha_init(&h, &g_ops, "", "", 1);
    for (int it = 0; it < 40000; it++) {
        char topic[96], pl[40];
        size_t pn;
        if (rnd() % 4) {
            snprintf(topic, sizeof(topic), "akam/akam-0f0f/%s/set", ids[rnd() % (sizeof(ids) / sizeof(ids[0]))]);
            snprintf(pl, sizeof(pl), "%s", words[rnd() % (sizeof(words) / sizeof(words[0]))]);
            pn = strlen(pl);
            if (rnd() % 8 == 0) pl[rnd() % (pn + 1)] = (char)rnd();      /* one random byte */
        } else {
            size_t tn = rnd() % 60;
            for (size_t i = 0; i < tn; i++) topic[i] = (char)(rnd() % 3 ? "akam-0f0/volumeset"[rnd() % 18] : (char)rnd());
            topic[tn] = 0;
            pn = rnd() % 30;
            for (size_t i = 0; i < pn; i++) pl[i] = (char)rnd();
        }
        g_set_calls = 0;
        uint8_t *p = malloc(pn ? pn : 1);
        memcpy(p, pl, pn);
        int r = mqtt_ha_command(&h, topic, strlen(topic), p, pn, 0);
        free(p);
        if (g_set_calls) {
            CHECK(r >= 0 && allowed_key(g_set_key));
            for (const char *c = g_set_val; *c; c++) CHECK((*c >= '0' && *c <= '9') || *c == '-' || (*c >= 'a' && *c <= 'z'));
        } else {
            CHECK(r < 0);
        }
    }
}

int main(void)
{
    test_init();
    test_discovery();
    test_state();
    test_commands();
    test_fuzz();
    if (g_failed) { fprintf(stderr, "mqtt_ha_test: %d FAILED\n", g_failed); return 1; }
    printf("mqtt_ha_test: OK\n");
    return 0;
}
