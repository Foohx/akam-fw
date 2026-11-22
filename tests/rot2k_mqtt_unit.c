/* Host unit test of the Rotatable 2K app's MQTT module: mqtt_cfg.c (strict akam/mqtt parser, akam/mqtt_pass reader, secrets never printed,
 * topic and payload builders with golden JSON, fuzzed) and mqtt_bind.c (the client thread and its reconnect state machine,
 * run against a scripted stub socket with a fake clock, no network). The 1080p codec (src/net/mqtt/mqtt.c) is linked as is.
 * Build with -fsanitize=address,undefined for the fuzz parts. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api_events.h"
#include "mqtt_bind.h"
#include "orient.h"
#include "mqtt_cfg.h"
#include "zones.h"
#include "mqtt.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

#define SECRET "Pa55w0rd-never-in-a-log"
#define USER "mqttuser"

/* ------------------------------------------------------------------------ log capture (a2k_log stub) */
static pthread_mutex_t g_logmu = PTHREAD_MUTEX_INITIALIZER;
static char g_logbuf[16384];
static size_t g_loglen;

void a2k_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_logmu);
    size_t n = strlen(line);
    if (g_loglen + n + 2 < sizeof g_logbuf) {
        memcpy(g_logbuf + g_loglen, line, n);
        g_logbuf[g_loglen + n] = '\n';
        g_loglen += n + 1;
        g_logbuf[g_loglen] = 0;
    }
    pthread_mutex_unlock(&g_logmu);
}

/* ------------------------------------------------------------------------ stub network */
#define MAXSENT 400
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_sent[MAXSENT][1200];     /* "topic|payload|retain" or "#CONNECT", "#PINGREQ", "#DISCONNECT" */
static int g_nsent;
static uint8_t g_connect_pkt[300];
static size_t g_connect_len;
static uint8_t g_in[512];
static size_t g_inlen;
static volatile int64_t g_fake;
static unsigned g_sleeps[64];
static int g_nsleeps;
static int g_slow_clock;
static int g_dials, g_dial_fail, g_connack_rc, g_mute_ping, g_drop_at_ms, g_garbage, g_block_send, g_closes;
static unsigned g_seed;

static unsigned rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static void reset_stub(void)
{
    pthread_mutex_lock(&g_mu);
    g_nsent = 0;
    g_inlen = 0;
    g_nsleeps = 0;
    g_slow_clock = g_dials = g_dial_fail = g_connack_rc = g_mute_ping = g_garbage = g_block_send = g_closes = 0;
    g_drop_at_ms = -1;
    g_fake = 1000;
    g_connect_len = 0;
    pthread_mutex_unlock(&g_mu);
}

static int s_dial(const a2k_mqtt_cfg_t *cfg, char *ip, size_t cap, const char **reason)
{
    (void)cfg;
    pthread_mutex_lock(&g_mu);
    int n = ++g_dials, fail = n <= g_dial_fail;
    g_inlen = 0;
    pthread_mutex_unlock(&g_mu);
    if (fail) {
        *reason = "connect";
        return -1;
    }
    snprintf(ip, cap, "192.0.2.50");
    return 7;
}

static void queue_in(const void *b, size_t n)
{
    if (g_inlen + n <= sizeof g_in) {
        memcpy(g_in + g_inlen, b, n);
        g_inlen += n;
    }
}

static int s_send(int h, const uint8_t *b, size_t n)
{
    mqtt_pkt_t p;
    (void)h;
    while (g_block_send)
        usleep(1000);
    pthread_mutex_lock(&g_mu);
    if (mqtt_frame(b, n, n, &p) > 0 && g_nsent < MAXSENT) {
        if (p.type == MQTT_CONNECT) {
            memcpy(g_connect_pkt, b, n < sizeof g_connect_pkt ? n : sizeof g_connect_pkt);
            g_connect_len = n;
            snprintf(g_sent[g_nsent++], sizeof g_sent[0], "#CONNECT");
            if (g_garbage) {
                uint8_t junk[64];
                size_t k = rnd() % sizeof junk + 1;
                for (size_t i = 0; i < k; i++)
                    junk[i] = (uint8_t)rnd();
                if (rnd() % 2) { junk[0] = 0x20; junk[1] = 2; junk[2] = 0; junk[3] = 0; } /* sometimes a valid CONNACK first */
                queue_in(junk, k);
            } else {
                uint8_t ack[4] = { 0x20, 2, 0, (uint8_t)g_connack_rc };
                queue_in(ack, 4);
            }
        } else if (p.type == MQTT_PUBLISH) {
            mqtt_pub_t m;
            if (mqtt_dec_publish(&p, &m) == 0)
                snprintf(g_sent[g_nsent++], sizeof g_sent[0], "%.*s|%.*s|%d", (int)m.tlen, m.topic, (int)m.plen,
                         (const char *)m.payload, m.retain);
        } else if (p.type == MQTT_PINGREQ) {
            snprintf(g_sent[g_nsent++], sizeof g_sent[0], "#PINGREQ");
            if (!g_mute_ping) {
                static const uint8_t pr[2] = { 0xd0, 0 };
                queue_in(pr, 2);
            }
        } else if (p.type == MQTT_DISCONNECT) {
            snprintf(g_sent[g_nsent++], sizeof g_sent[0], "#DISCONNECT");
        }
    }
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int s_recv(int h, uint8_t *b, size_t cap, int timeout_ms)
{
    int r = 0;
    (void)h;
    pthread_mutex_lock(&g_mu);
    if (g_inlen) {
        size_t n = g_inlen < cap ? g_inlen : cap;
        memcpy(b, g_in, n);
        memmove(g_in, g_in + n, g_inlen - n);
        g_inlen -= n;
        r = (int)n;
    } else if (g_drop_at_ms >= 0 && g_fake >= g_drop_at_ms) {
        g_drop_at_ms = -1;
        r = -1;
    } else {
        g_fake += g_slow_clock ? 1 : timeout_ms > 1000 ? 1000 : timeout_ms; /* time passes only while the thread waits */
    }
    pthread_mutex_unlock(&g_mu);
    if (r == 0)
        usleep(300);
    return r;
}

static void s_close(int h) { (void)h; pthread_mutex_lock(&g_mu); g_closes++; pthread_mutex_unlock(&g_mu); }
static int64_t s_now(void) { return g_fake; }

static void s_sleep(unsigned ms)
{
    pthread_mutex_lock(&g_mu);
    if (g_nsleeps < 64)
        g_sleeps[g_nsleeps++] = ms;
    g_fake += ms;
    pthread_mutex_unlock(&g_mu);
    usleep(300);
}

static const a2k_mqtt_net_t g_stub = { s_dial, s_send, s_recv, s_close, s_now, s_sleep };

static int sent_count(const char *prefix)
{
    int n = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nsent; i++)
        n += strncmp(g_sent[i], prefix, strlen(prefix)) == 0;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static int wait_sent(const char *prefix, int count)
{
    for (int i = 0; i < 4000; i++) {
        if (sent_count(prefix) >= count)
            return 1;
        usleep(1000);
    }
    return 0;
}

static int wait_state(const char *want)
{
    char s[48];
    for (int i = 0; i < 4000; i++) {
        a2k_mqtt_conn_state(s, sizeof s);
        if (!strcmp(s, want))
            return 1;
        usleep(1000);
    }
    return 0;
}

static int log_has(const char *what)
{
    int r;
    pthread_mutex_lock(&g_logmu);
    r = strstr(g_logbuf, what) != NULL;
    pthread_mutex_unlock(&g_logmu);
    return r;
}

/* ------------------------------------------------------------------------ files */
static const char *g_dir;
static char g_conf[200], g_pass[200], g_macf[200];

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fputs(text, f);
    fclose(f);
}

static void det(int present, int score, const char *status, int64_t epoch)
{
    a2k_det_t d;
    memset(&d, 0, sizeof d);
    d.present = present;
    d.last_box.cls = 0;
    d.last_box.score = score;
    d.last_box.x1 = 10; d.last_box.y1 = 20; d.last_box.x2 = 110; d.last_box.y2 = 150;
    d.heads = 1;
    d.head_score = 640;
    a2k_mqtt_detection(&d, status, epoch);
}

static int viewers_fn(void) { return 2; }

static int start_with(const char *conf)
{
    write_file(g_conf, conf);
    write_file(g_pass, "" SECRET "\n"); /* the password lives in its own card file, next to the config */
    return a2k_mqtt_start(g_conf, g_macf, 554, viewers_fn);
}

static const char *FULL = "enabled=1\nhost=192.0.2.10\nport=1884\nuser=" USER "\nclient_id=akam-test\n"
                          "base_topic=akam/test\ndiscovery=on\ndiscovery_prefix=homeassistant\ninterval=30\n";

/* ------------------------------------------------------------------------ config tests */
static void test_cfg(void)
{
    a2k_mqtt_cfg_t c;
    char d[300];
    printf("== config parser\n");
    CHECK(a2k_mqtt_cfg_parse(FULL, strlen(FULL), &c) == 0 && c.enabled && c.port == 1884 &&
          !strcmp(c.host, "192.0.2.10") && !strcmp(c.user, USER) &&
          !strcmp(c.client_id, "akam-test") && !strcmp(c.base, "akam/test") && c.discovery && c.interval == 30,
          "full file parses");
    CHECK(a2k_mqtt_cfg_parse("enabled=0\nhost=broker.lan\n", 25, &c) == 0 && !c.enabled && c.port == 1883 &&
          c.discovery && c.interval == 30 && !c.base[0] && !c.prefix[0],
          "minimal file: defaults (port 1883, discovery on, interval 30, base and id empty)");
    CHECK(a2k_mqtt_cfg_parse("enabled=1\r\nhost=a.b\r\n", 20, &c) == 0, "CRLF tolerated");
    static const char *const bad[] = {
        "host=a\n", "enabled=1\n", "enabled=2\nhost=a\n", "enabled=1\nhost=a\nhost=b\n", "enabled=1\nhost=a\nfoo=1\n",
        "enabled=1\nhost=-a\n", "enabled=1\nhost=a b\n", "enabled=1\nhost=a\nport=0\n", "enabled=1\nhost=a\nport=65536\n",
        "enabled=1\nhost=a\nport=1x\n", "enabled=1\nhost=a\nport=\n", "enabled=1\nhost=a\nuser=a b\n",
        "enabled=1\nhost=a\npass=x\n" /* the password is not a key of this file */, "enabled=1\nhost=a\nuser=-\n",
        "enabled=1\nhost=a\nclient_id=a/b\n", "enabled=1\nhost=a\nbase_topic=/x\n", "enabled=1\nhost=a\nbase_topic=x//y\n",
        "enabled=1\nhost=a\nbase_topic=x/\n", "enabled=1\nhost=a\nbase_topic=x#\n", "enabled=1\nhost=a\ndiscovery=yes\n",
        "enabled=1\nhost=a\ninterval=9\n", "enabled=1\nhost=a\ninterval=301\n", "enabled=1\nhost=a\n\n", "enabled=1\nhost\n",
        "enabled=1\nhost=a\ndiscovery_prefix=a b\n", " enabled=1\nhost=a\n", "enabled =1\nhost=a\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char msg[96];
        snprintf(msg, sizeof msg, "invalid file %zu refused", i);
        CHECK(a2k_mqtt_cfg_parse(bad[i], strlen(bad[i]), &c) == -1, msg);
    }
    {
        char big[A2K_MQTT_CFG_MAX + 40];
        memset(big, 'a', sizeof big);
        CHECK(a2k_mqtt_cfg_parse(big, A2K_MQTT_CFG_MAX + 1, &c) == -1, "over-long file refused");
    }
    CHECK(a2k_mqtt_cfg_parse("enabled=1\nho\0st=a\n", 18, &c) == -1, "NUL byte refused");
    printf("== secrets are never printed\n");
    a2k_mqtt_cfg_parse(FULL, strlen(FULL), &c);
    snprintf(c.pass, sizeof c.pass, "%s", SECRET);
    a2k_mqtt_cfg_describe(&c, d, sizeof d);
    CHECK(!strstr(d, SECRET) && !strstr(d, USER) && strstr(d, "password set"), "describe() omits password and user name");
    a2k_mqtt_cfg_wipe(&c);
    CHECK(c.pass[0] == 0 && c.pass[100] == 0 && c.host[0], "wipe zeroes the password only");
    CHECK(a2k_mqtt_cfg_load("/nonexistent/akam/mqtt", &c) == 1 && !c.enabled, "absent file: off (1)");
    write_file(g_conf, "enabled=1\nhost=a\nbogus=1\n");
    CHECK(a2k_mqtt_cfg_load(g_conf, &c) == -1 && !c.host[0] && !c.pass[0], "invalid file: -1 and the struct is zeroed");
    write_file(g_conf, FULL);
    CHECK(a2k_mqtt_cfg_load(g_conf, &c) == 0 && !strcmp(c.user, USER) && !c.pass[0], "file loads (the password is not in it)");
    printf("== password file akam/mqtt_pass (same rules as the 1080p mqtt_pass_load)\n");
    CHECK(a2k_mqtt_pass_path("/mnt/sdcard/akam/mqtt", d, sizeof d) == 0 && !strcmp(d, "/mnt/sdcard/akam/mqtt_pass"),
          "the password file sits next to the config");
    CHECK(a2k_mqtt_pass_path("mqtt", d, sizeof d) == 0 && !strcmp(d, "mqtt_pass"), "bare file name: current directory");
    CHECK(a2k_mqtt_pass_path("/a/b/mqtt", d, 8) == -1, "a path that does not fit is refused");
    {
        char pw[A2K_MQTT_PASS_MAX + 1], big[300];
        CHECK(a2k_mqtt_pass_load("/nonexistent/akam/mqtt_pass", pw, sizeof pw) == 0 && !pw[0], "absent: no password (0)");
        write_file(g_pass, "hunter2 x=y\n");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == 0 && !strcmp(pw, "hunter2 x=y"), "one LF trimmed, spaces kept");
        write_file(g_pass, "abc");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == 0 && !strcmp(pw, "abc"), "no trailing LF needed");
        write_file(g_pass, "");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == 0 && !pw[0], "empty file: empty password");
        write_file(g_pass, "abc\r\n");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == -1 && !pw[0], "a CR is a control character: refused");
        write_file(g_pass, "a\tb\n");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == -1, "a tab is refused");
        write_file(g_pass, "ab\x7f\n");
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == -1, "DEL is refused");
        memset(big, 'p', sizeof big);
        big[128] = '\n';
        big[129] = 0;
        write_file(g_pass, big);
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == 0 && strlen(pw) == 128, "128 characters accepted");
        memset(big, 'p', sizeof big);
        big[129] = '\n';
        big[130] = 0;
        write_file(g_pass, big);
        CHECK(a2k_mqtt_pass_load(g_pass, pw, sizeof pw) == -1 && !pw[0], "129 characters refused");
        CHECK(a2k_mqtt_pass_load(g_dir, pw, sizeof pw) == -1, "a directory is not a password file");
        unlink(g_pass);
    }
}

/* ------------------------------------------------------------------------ builders */
static void test_builders(void)
{
    a2k_mqtt_cfg_t c;
    a2k_mqtt_ids_t ids;
    char t[200], doc[1100], v[80];
    struct a2k_mqtt_det d;
    memset(&c, 0, sizeof c);
    printf("== topics and payloads (golden)\n");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "", "0.1.0") == -2, "no MAC: -2 (retried later)");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "AA:BB:CC:DD:EE:FF", "0.1.0") == -2, "upper-case MAC refused");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0 &&
          !strcmp(ids.base, "akam/akam-rot2k") && !strcmp(ids.prefix, "homeassistant") &&
          !strcmp(ids.node, "akam_aabbccddeeff"), "defaults: akam/<hostname>, homeassistant, akam_<mac>");
    strcpy(c.base, "akam/test");
    strcpy(c.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids with custom topics");
    CHECK(a2k_mqtt_status_topic(&ids, t, sizeof t) > 0 && !strcmp(t, "akam/test/status"), "status topic");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PERSON, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/binary_sensor/akam_aabbccddeeff/person/config"), "person discovery topic");
    CHECK(!strcmp(doc,
        "{\"name\":\"Person\",\"unique_id\":\"akam_aabbccddeeff_person\",\"availability_topic\":\"akam/test/status\","
        "\"state_topic\":\"akam/test/person/state\",\"json_attributes_topic\":\"akam/test/person/attributes\","
        "\"device_class\":\"occupancy\",\"device\":{\"identifiers\":[\"akam_aabbccddeeff\"],\"connections\":"
        "[[\"mac\",\"aa:bb:cc:dd:ee:ff\"]],\"name\":\"akam-rot2k\",\"manufacturer\":\"Akam\",\"model\":"
        "\"lsc-rotatable-2k-ak3918av130\",\"sw_version\":\"0.1.0\"}}"), "person discovery document (golden)");
    a2k_mqtt_discovery(&ids, A2K_E_SCORE, t, sizeof t, doc, sizeof doc);
    CHECK(!strcmp(t, "ha/sensor/akam_aabbccddeeff/person_score/config") &&
          strstr(doc, "\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"device\":"), "score sensor discovery");
    a2k_mqtt_discovery(&ids, A2K_E_LAST, t, sizeof t, doc, sizeof doc);
    CHECK(strstr(doc, "\"device_class\":\"timestamp\""), "last detection is a timestamp sensor");
    a2k_mqtt_discovery(&ids, A2K_E_VIEWERS, t, sizeof t, doc, sizeof doc);
    CHECK(strstr(doc, "\"json_attributes_topic\":\"akam/test/viewers/attributes\""), "viewers carry the stream attributes");
    a2k_mqtt_discovery(&ids, A2K_E_VERSION, t, sizeof t, doc, sizeof doc);
    CHECK(strstr(doc, "\"entity_category\":\"diagnostic\""), "version is diagnostic");
    CHECK(!strstr(doc, "command_topic") && !strstr(doc, SECRET), "no command topic in any document");

    /* zones and the snapshot image */
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_IMAGE, t, sizeof t, doc, sizeof doc) == -1, "no snapshot: no image document");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_ZONE0, t, sizeof t, doc, sizeof doc) == -1, "no zone: no zone document");
    ids.nzones = 2;
    strcpy(ids.zone[0], "porch");
    strcpy(ids.zone[1], "drive_2");
    ids.snapshot = 1;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_ZONE0, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/binary_sensor/akam_aabbccddeeff/person_porch/config"), "zone discovery topic");
    CHECK(!strcmp(doc,
        "{\"name\":\"Person porch\",\"unique_id\":\"akam_aabbccddeeff_person_porch\",\"availability_topic\":\"akam/test/status\","
        "\"state_topic\":\"akam/test/person_porch/state\",\"device_class\":\"occupancy\",\"device\":{\"identifiers\":"
        "[\"akam_aabbccddeeff\"],\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:ff\"]],\"name\":\"akam-rot2k\",\"manufacturer\":"
        "\"Akam\",\"model\":\"lsc-rotatable-2k-ak3918av130\",\"sw_version\":\"0.1.0\"}}"), "zone discovery document (golden)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_ZONE0 + 1, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/binary_sensor/akam_aabbccddeeff/person_drive_2/config") && strstr(doc, "\"name\":\"Person drive_2\""),
          "second zone");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_ZONE0 + 2, t, sizeof t, doc, sizeof doc) == -1, "a zone past nzones has no document");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_IMAGE, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/image/akam_aabbccddeeff/snapshot/config"), "image discovery topic");
    CHECK(!strcmp(doc,
        "{\"name\":\"Person snapshot\",\"unique_id\":\"akam_aabbccddeeff_snapshot\",\"availability_topic\":\"akam/test/status\","
        "\"image_topic\":\"akam/test/snapshot\",\"content_type\":\"image/jpeg\",\"device\":{\"identifiers\":"
        "[\"akam_aabbccddeeff\"],\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:ff\"]],\"name\":\"akam-rot2k\",\"manufacturer\":"
        "\"Akam\",\"model\":\"lsc-rotatable-2k-ak3918av130\",\"sw_version\":\"0.1.0\"}}"), "image discovery document (golden)");
    CHECK(!strstr(doc, "command_topic") && !strstr(doc, "retain"), "the image entity has no command topic");
    CHECK(a2k_mqtt_snapshot_topic(&ids, t, sizeof t) > 0 && !strcmp(t, "akam/test/snapshot"), "snapshot topic");
    CHECK(a2k_mqtt_detections_topic(&ids, t, sizeof t) > 0 && !strcmp(t, "akam/test/detections"), "detections topic");
    memset(&d, 0, sizeof d);
    CHECK(a2k_mqtt_state(&ids, A2K_E_ZONE0, &d, -1, t, sizeof t, v, sizeof v) == 0, "no report yet: no zone state");
    d.valid = 1;
    d.zmask = 2; /* zone 1 only */
    CHECK(a2k_mqtt_state(&ids, A2K_E_ZONE0, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(t, "akam/test/person_porch/state") &&
          !strcmp(v, "OFF"), "zone 0 OFF");
    CHECK(a2k_mqtt_state(&ids, A2K_E_ZONE0 + 1, &d, -1, t, sizeof t, v, sizeof v) == 1 &&
          !strcmp(t, "akam/test/person_drive_2/state") && !strcmp(v, "ON"), "zone 1 ON");
    CHECK(a2k_mqtt_state(&ids, A2K_E_ZONE0 + 2, &d, -1, t, sizeof t, v, sizeof v) == 0, "a zone past nzones has no state");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_ZONE0, &d, "", 554, t, sizeof t, doc, sizeof doc) == 0, "zones have no attributes");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_ZONE0, t, 10, doc, sizeof doc) == -1 &&
          a2k_mqtt_discovery(&ids, A2K_E_ZONE0, t, sizeof t, doc, 100) == -1, "small buffers: -1 for zone documents too");

    memset(&d, 0, sizeof d);
    CHECK(a2k_mqtt_state(&ids, A2K_E_PERSON, &d, -1, t, sizeof t, v, sizeof v) == 0, "no report yet: no person state");
    d.valid = 1; d.present = 1; d.score = 812; d.cls = 0; d.x1 = 10; d.y1 = 20; d.x2 = 110; d.y2 = 150; d.heads = 1;
    d.head_score = 640; d.last_on_epoch = 1763200000; strcpy(d.status, "run");
    CHECK(a2k_mqtt_state(&ids, A2K_E_PERSON, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(t, "akam/test/person/state") &&
          !strcmp(v, "ON"), "person ON");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SCORE, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "81.2"), "score 812 = 81.2 %");
    CHECK(a2k_mqtt_state(&ids, A2K_E_LAST, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "2025-11-15T09:46:40+00:00"),
          "last detection ISO 8601 UTC");
    d.last_on_epoch = 0;
    CHECK(a2k_mqtt_state(&ids, A2K_E_LAST, &d, -1, t, sizeof t, v, sizeof v) == 0, "clock unknown: no last detection");
    d.last_on_epoch = 1763200000;
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_PERSON, &d, "", 554, t, sizeof t, doc, sizeof doc) == 1 &&
          !strcmp(t, "akam/test/person/attributes") &&
          !strcmp(doc, "{\"score\":812,\"class\":0,\"box\":[10,20,110,150],\"heads\":1,\"head_score\":640,"
                       "\"detection\":\"run\",\"last_detection\":\"2025-11-15T09:46:40+00:00\"}"), "person attributes (golden)");
    d.present = 0; d.score = 0; d.x1 = d.y1 = d.x2 = d.y2 = 0;
    CHECK(a2k_mqtt_state(&ids, A2K_E_PERSON, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "OFF"), "person OFF");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_PERSON, &d, "", 554, t, sizeof t, doc, sizeof doc) == 1 &&
          strstr(doc, "\"score\":0,\"class\":0,\"box\":[0,0,0,0]"), "OFF attributes: score and box zero");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_VIEWERS, &d, "192.0.2.50", 554, t, sizeof t, doc, sizeof doc) == 1 &&
          !strcmp(doc, "{\"stream_main\":\"rtsp://192.0.2.50:554/stream0\",\"stream_sub\":\"rtsp://192.0.2.50:554/stream1\"}"),
          "stream URLs attributes (golden, no credentials)");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_VIEWERS, &d, "", 554, t, sizeof t, doc, sizeof doc) == 0, "unknown IP: no URL attributes");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_VIEWERS, &d, "1.2.3.4\"x", 554, t, sizeof t, doc, sizeof doc) == 0, "hostile IP text refused");
    CHECK(a2k_mqtt_state(&ids, A2K_E_VIEWERS, &d, 3, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "3"), "viewers state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_VERSION, &d, 0, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "0.1.0"), "version state");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PERSON, t, 10, doc, sizeof doc) == -1, "small topic buffer: -1");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PERSON, t, sizeof t, doc, 100) == -1, "small payload buffer: -1");

    /* the day/night binary_sensor */
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_NIGHT, t, sizeof t, doc, sizeof doc) == -1, "night off: no night document");
    CHECK(A2K_E_NIGHT > A2K_E_ZONE0 + A2K_ZONE_MAX - 1 && A2K_E_NIGHT != A2K_E_IMAGE, "the night entity index is outside the zone range");
    ids.night = 1;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_NIGHT, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/binary_sensor/akam_aabbccddeeff/night/config"), "night discovery topic");
    CHECK(!strcmp(doc,
        "{\"name\":\"Night mode\",\"unique_id\":\"akam_aabbccddeeff_night\",\"availability_topic\":\"akam/test/status\","
        "\"state_topic\":\"akam/test/night/state\",\"json_attributes_topic\":\"akam/test/night/attributes\","
        "\"icon\":\"mdi:weather-night\",\"device\":{\"identifiers\":"
        "[\"akam_aabbccddeeff\"],\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:ff\"]],\"name\":\"akam-rot2k\",\"manufacturer\":"
        "\"Akam\",\"model\":\"lsc-rotatable-2k-ak3918av130\",\"sw_version\":\"0.1.0\"}}"), "night discovery document (golden)");
    CHECK(!strstr(doc, "command_topic"), "the night entity has no command topic");
    memset(&d, 0, sizeof d);
    CHECK(a2k_mqtt_state(&ids, A2K_E_NIGHT, &d, -1, t, sizeof t, v, sizeof v) == 0 &&
          a2k_mqtt_attrs(&ids, A2K_E_NIGHT, &d, "", 554, t, sizeof t, doc, sizeof doc) == 0, "no day/night report yet: no state, no attributes");
    d.dn.valid = 1;
    d.dn.night = 1;
    d.dn.mode = 0;
    d.dn.ir_on = 1;
    d.dn.switches = 2;
    d.dn.last_switch_epoch = 1763200000;
    snprintf(d.dn.why, sizeof d.dn.why, "night: the vendor day/night decision held 3 s");
    CHECK(a2k_mqtt_state(&ids, A2K_E_NIGHT, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(t, "akam/test/night/state") && !strcmp(v, "ON"),
          "night state ON");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_NIGHT, &d, "", 554, t, sizeof t, doc, sizeof doc) == 1 && !strcmp(t, "akam/test/night/attributes") &&
          !strcmp(doc, "{\"mode\":\"auto\",\"ir_led\":\"on\",\"automatic\":\"ok\",\"switches\":2,\"last_switch\":"
                       "\"2025-11-15T09:46:40+00:00\",\"reason\":\"night: the vendor day/night decision held 3 s\"}"),
          "night attributes (golden)");
    d.dn.night = 0;
    d.dn.mode = 2;
    d.dn.ir_on = 0;
    d.dn.auto_off = 1;
    d.dn.last_switch_epoch = 0;
    snprintf(d.dn.why, sizeof d.dn.why, "bad \"quote\" \\ and\ncontrol");
    CHECK(a2k_mqtt_state(&ids, A2K_E_NIGHT, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "OFF"), "night state OFF");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_NIGHT, &d, "", 554, t, sizeof t, doc, sizeof doc) == 1 && strstr(doc, "\"mode\":\"night\"") &&
          strstr(doc, "\"automatic\":\"gave_up\"") && !strstr(doc, "last_switch") && strstr(doc, "\"reason\":\"bad \\\"quote\\\" \\\\ andcontrol\""),
          "attributes: forced mode, gave up, no time when the clock is unknown, the reason text is escaped");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_NIGHT, &d, "", 554, t, sizeof t, doc, 30) == 0, "a small attributes buffer: nothing");
    ids.night = 0;
    CHECK(a2k_mqtt_state(&ids, A2K_E_NIGHT, &d, -1, t, sizeof t, v, sizeof v) == 0, "night off: no state");
}

/* ------------------------------------------------------------------------ fuzz */
static void test_fuzz(void)
{
    a2k_mqtt_cfg_t c;
    char buf[A2K_MQTT_CFG_MAX + 64];
    size_t base = strlen(FULL);
    int ok = 0, bad = 0;
    printf("== parser fuzz (ASan/UBSan: a crash is the failure)\n");
    g_seed = 12345;
    for (int it = 0; it < 300000; it++) {
        size_t n;
        if (it % 3 == 0) { /* random bytes */
            n = rnd() % sizeof buf;
            for (size_t i = 0; i < n; i++)
                buf[i] = (char)rnd();
        } else { /* mutated valid file */
            memcpy(buf, FULL, base);
            n = base;
            for (int m = 1 + (int)(rnd() % 4); m > 0; m--) {
                size_t p = rnd() % (n + 1);
                switch (rnd() % 3) {
                case 0: if (n) buf[p % n] = (char)rnd(); break;
                case 1: if (n + 1 < sizeof buf) { memmove(buf + p + 1, buf + p, n - p); buf[p] = "=\n\r a#/\0z9"[rnd() % 10]; n++; } break;
                default: if (n && p < n) { memmove(buf + p, buf + p + 1, n - p - 1); n--; } break;
                }
            }
        }
        if (a2k_mqtt_cfg_parse(buf, n, &c) == 0) {
            ok++;
            if (strlen(c.host) < 1 || strlen(c.host) > 63 || c.port < 1 || c.port > 65535 || c.interval < 10 ||
                c.interval > 300 || (c.base[0] && !a2k_mqtt_topic_ok(c.base, 64))) {
                CHECK(0, "a parsed file broke an invariant");
                return;
            }
        } else {
            bad++;
        }
    }
    CHECK(ok > 100 && bad > 1000, "fuzz ran with both accepted and refused inputs");
    printf("         (%d accepted, %d refused)\n", ok, bad);
}

/* ------------------------------------------------------------------------ client */
static int in_range(unsigned v, unsigned lo, unsigned hi) { return v >= lo && v <= hi; }

static void test_client(void)
{
    char s[48];
    printf("== client: disabled cases\n");
    a2k_mqtt_test_set_net(&g_stub);
    reset_stub();
    CHECK(a2k_mqtt_start("/nonexistent/mqtt", g_macf, 554, NULL) == 1 && g_dials == 0, "no file: off, no dial, no thread");
    CHECK(start_with("enabled=0\nhost=192.0.2.10\n") == 1 && g_dials == 0, "enabled=0: off");
    CHECK(start_with("enabled=1\nhost=192.0.2.10\nbogus=1\nuser=u\n") == -1 && g_dials == 0,
          "invalid file: refused, no dial");
    CHECK(!log_has(SECRET), "the invalid file's content is not logged");
    write_file(g_conf, "enabled=1\nhost=192.0.2.10\nuser=u\n");
    write_file(g_pass, "bad\001pass\n");
    CHECK(a2k_mqtt_start(g_conf, g_macf, 554, NULL) == -1 && g_dials == 0, "unusable password file: MQTT stays off, no dial");
    CHECK(!log_has("bad") && log_has("mqtt_pass"), "the log names the file, never its content");
    a2k_mqtt_stop();

    printf("== client: first connection after failed dials (backoff)\n");
    reset_stub();
    g_dial_fail = 3;
    CHECK(start_with(FULL) == 0, "started");
    CHECK(wait_state("connected"), "connected after 3 failed dials");
    CHECK(g_nsleeps >= 3 && in_range(g_sleeps[0], 4000, 6000) && in_range(g_sleeps[1], 8000, 12000) &&
          in_range(g_sleeps[2], 16000, 24000), "backoff 5 s, 10 s, 20 s (+-20 %)");
    CHECK(wait_sent("akam/test/status|online|1", 1), "availability online published, retained");
    pthread_mutex_lock(&g_mu);
    {
        /* CONNECT: will on the status topic, retained, user and password present on the wire (it IS the login) */
        const uint8_t *p = g_connect_pkt;
        size_t n = g_connect_len;
        int will = 0, user = 0, pass = 0;
        for (size_t i = 0; i + 16 < n; i++) {
            will |= !memcmp(p + i, "akam/test/status", 16);
        }
        for (size_t i = 0; i + strlen(USER) <= n; i++) user |= !memcmp(p + i, USER, strlen(USER));
        for (size_t i = 0; i + strlen(SECRET) <= n; i++) pass |= !memcmp(p + i, SECRET, strlen(SECRET));
        CHECK(will && user && pass && (p[9] & 0x24) == 0x24 && (p[9] & 0xc0) == 0xc0, "CONNECT carries will (retained), user, password");
    }
    CHECK(strcmp(g_sent[0], "#CONNECT") == 0 && strcmp(g_sent[1], "akam/test/status|online|1") == 0, "CONNECT then online");
    pthread_mutex_unlock(&g_mu);
    CHECK(wait_sent("homeassistant/", 6), "5 discovery documents published, plus the empty retained snapshot config (snapshots off)");
    pthread_mutex_lock(&g_mu);
    {
        int retained = 1, found = 0;
        for (int i = 0; i < g_nsent; i++)
            if (!strncmp(g_sent[i], "homeassistant/", 14)) {
                found++;
                retained &= g_sent[i][strlen(g_sent[i]) - 1] == '1';
            }
        CHECK(found == 6 && retained, "discovery documents are retained");
    }
    pthread_mutex_unlock(&g_mu);
    CHECK(sent_count("homeassistant/image/akam_aabbccddeeff/snapshot/config||1") == 1,
          "snapshots off: an empty retained config removes an image entity of an earlier run");
    CHECK(sent_count("homeassistant/binary_sensor/akam_aabbccddeeff/person_") == 0, "no zone: no zone entity");
    CHECK(wait_sent("akam/test/viewers/state|2|0", 1) && wait_sent("akam/test/viewers/attributes|{\"stream_main\":\"rtsp://192.0.2.50:554/stream0\"", 1),
          "viewers and the stream URLs (local address of the socket) published");
    CHECK(wait_sent("akam/test/app_version/state|0.1.0|0", 1), "version published");

    printf("== client: detection transitions\n");
    det(0, 0, "run", 1763200000);
    CHECK(wait_sent("akam/test/person/state|OFF|0", 1), "first report: OFF published");
    det(1, 812, "run", 1763200100);
    CHECK(wait_sent("akam/test/person/state|ON|0", 1), "ON published");
    CHECK(wait_sent("akam/test/person/attributes|{\"score\":812,\"class\":0,\"box\":[10,20,110,150],\"heads\":1,\"head_score\":640,\"detection\":\"run\",\"last_detection\":\"2025-11-15T09:48:20+00:00\"}|0", 1),
          "ON attributes: score, class, box, heads, last detection");
    CHECK(wait_sent("akam/test/person_score/state|81.2|0", 1) && wait_sent("akam/test/last_detection/state|2025-11-15T09:48:20+00:00|0", 1),
          "score and last detection states");
    det(1, 850, "run", 1763200101); /* same state, fresher score: not a transition */
    det(0, 0, "run", 1763200200);
    CHECK(wait_sent("akam/test/person/state|OFF|0", 2), "OFF published");
    CHECK(sent_count("akam/test/person/state|ON|0") == 1, "a score change without a transition publishes nothing extra");
    det(0, 0, "failed", 1763200300);
    CHECK(wait_sent("akam/test/person/attributes|{\"score\":0,\"class\":0,\"box\":[0,0,0,0],\"heads\":1,\"head_score\":640,\"detection\":\"failed\"", 1),
          "detection failure is visible in the attributes");

    printf("== client: periodic republish and keepalive\n");
    pthread_mutex_lock(&g_mu);
    g_mute_ping = 0;
    pthread_mutex_unlock(&g_mu);
    CHECK(wait_sent("akam/test/viewers/state|2|0", 3), "states republished every interval");
    CHECK(wait_sent("#PINGREQ", 1) && wait_state("connected"), "PINGREQ answered: stays connected");

    printf("== client: stop says offline then DISCONNECT\n");
    a2k_mqtt_stop();
    pthread_mutex_lock(&g_mu);
    CHECK(g_nsent >= 2 && !strcmp(g_sent[g_nsent - 2], "akam/test/status|offline|1") && !strcmp(g_sent[g_nsent - 1], "#DISCONNECT"),
          "offline (retained) then DISCONNECT");
    pthread_mutex_unlock(&g_mu);
    a2k_mqtt_conn_state(s, sizeof s);
    CHECK(!strcmp(s, "off"), "state off after stop");

    printf("== client: connection loss, backoff reset only after a stable session\n");
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected");
    g_drop_at_ms = (int)(g_fake + 100); /* drops after 100 ms of session: not stable */
    CHECK(wait_sent("#CONNECT", 2), "reconnects after a drop");
    CHECK(g_nsleeps >= 1 && in_range(g_sleeps[0], 4000, 6000), "first retry after 5 s");
    g_drop_at_ms = (int)(g_fake + 100);
    CHECK(wait_sent("#CONNECT", 3), "and again");
    CHECK(g_nsleeps >= 2 && in_range(g_sleeps[1], 8000, 12000), "the quick drop did not reset the backoff: 10 s");
    g_drop_at_ms = (int)(g_fake + 70000); /* a session that held 70 s */
    CHECK(wait_sent("#CONNECT", 4), "reconnects after a long session");
    CHECK(g_nsleeps >= 3 && in_range(g_sleeps[2], 4000, 6000), "a stable session restarts the backoff from 5 s");
    a2k_mqtt_stop();

    printf("== client: refused, silent broker\n");
    reset_stub();
    g_connack_rc = 5;
    CHECK(start_with(FULL) == 0 && wait_state("error:refused_not_authorized"), "CONNACK 5: error:refused_not_authorized");
    CHECK(sent_count("akam/") == 0, "nothing published to a refusing broker");
    a2k_mqtt_stop();
    reset_stub();
    g_connack_rc = 4;
    CHECK(start_with(FULL) == 0 && wait_state("error:refused_credentials"), "CONNACK 4: error:refused_credentials");
    a2k_mqtt_stop();
    reset_stub();
    g_mute_ping = 1;
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected (ping replies muted)");
    CHECK(wait_state("error:timeout"), "no PINGRESP within 10 s: error:timeout, reconnect");
    a2k_mqtt_stop();

    printf("== client: never blocks the detection thread, bounded queue\n");
    reset_stub();
    g_slow_clock = 1; /* no periodic republish in this scenario: only transitions are counted */
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("akam/test/app_version/state", 1), "connected");
    det(0, 0, "run", 1763200000);
    wait_sent("akam/test/person/state|OFF|0", 1);
    pthread_mutex_lock(&g_mu);
    g_block_send = 1; /* the broker stops reading: the thread is stuck in send */
    pthread_mutex_unlock(&g_mu);
    {
        struct timespec a, b;
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &a);
        usleep(20000);
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 100; i++)
            det(i & 1, 700 + i, "run", 1763300000 + i);
        det(1, 900, "run", 1763300500); /* final state: ON */
        clock_gettime(CLOCK_MONOTONIC, &b);
        ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        CHECK(ms < 200, "101 reports while the send is stuck: the caller never waited");
    }
    int before = sent_count("akam/test/person/state|");
    g_block_send = 0;
    CHECK(wait_sent("akam/test/person/state|ON|0", 2), "released: the newest state is published");
    usleep(50000);
    {
        int after = sent_count("akam/test/person/state|") - before;
        CHECK(after <= A2K_MQTT_RING + 2, "at most the ring size of transitions were queued (the oldest were dropped)");
        printf("         (%d state publishes after the release)\n", after);
    }
    a2k_mqtt_stop();

    printf("== client: hostile broker bytes (fuzz)\n");
    reset_stub();
    g_garbage = 1;
    g_seed = 777;
    CHECK(start_with(FULL) == 0, "started against a garbage broker");
    for (int i = 0; i < 4000 && g_dials < 300; i++)
        usleep(1000);
    CHECK(g_dials >= 300, "300 sessions of random broker bytes: no crash, each ended with a protocol-style error and a retry");
    a2k_mqtt_stop();

    printf("== client: the password never reaches the log\n");
    CHECK(!log_has(SECRET) && !log_has(USER), "no log line holds the password or the user name");
    CHECK(log_has("password set (not logged)"), "the log says a password is set, not which");
}

static void det_z(int present, int score, const char *status, int64_t epoch, unsigned zmask)
{
    a2k_det_t d;
    memset(&d, 0, sizeof d);
    d.present = present;
    d.last_box.score = score;
    d.last_box.x1 = 10; d.last_box.y1 = 20; d.last_box.x2 = 110; d.last_box.y2 = 150;
    d.zone_mask = zmask;
    a2k_mqtt_detection(&d, status, epoch);
}

static void test_zones_snapshot(void)
{
    a2k_zones_t zs;
    static uint8_t big[A2K_MQTT_SNAP_MAX + 16];
    unsigned sent, dropped;
    printf("== client: default client id, zones, snapshot\n");
    reset_stub();
    write_file(g_conf, "enabled=1\nhost=192.0.2.10\n");
    write_file(g_pass, "\n");
    CHECK(a2k_mqtt_start(g_conf, g_macf, 554, NULL) == 0 && wait_state("connected"), "connected without client_id");
    pthread_mutex_lock(&g_mu);
    {
        int found = 0;
        for (size_t i = 0; i + 10 <= g_connect_len; i++)
            found |= !memcmp(g_connect_pkt + i, "akam-rot2k", 10);
        CHECK(found, "the default client id is akam-rot2k (not the stock host name)");
    }
    pthread_mutex_unlock(&g_mu);
    CHECK(wait_sent("homeassistant/", 6), "announced");
    a2k_mqtt_stop();

    reset_stub();
    CHECK(a2k_zones_parse("porch=0,40,50,100\ndrive=50,40,100,100\n", 38, &zs) == 0 && zs.n == 2, "two zones parsed");
    a2k_mqtt_set_zones(&zs);
    a2k_mqtt_set_snapshot(1);
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with zones and snapshots");
    CHECK(wait_sent("homeassistant/", 8), "discovery: 5 fixed, the image and two zones");
    CHECK(sent_count("homeassistant/binary_sensor/akam_aabbccddeeff/person_porch/config|{") == 1 &&
          sent_count("homeassistant/binary_sensor/akam_aabbccddeeff/person_drive/config|{") == 1 &&
          sent_count("homeassistant/image/akam_aabbccddeeff/snapshot/config|{\"name\":\"Person snapshot\"") == 1,
          "zone and image documents published");
    {
        int retained = 1;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_nsent; i++)
            if (!strncmp(g_sent[i], "homeassistant/", 14))
                retained &= g_sent[i][strlen(g_sent[i]) - 1] == '1';
        pthread_mutex_unlock(&g_mu);
        CHECK(retained && sent_count("homeassistant/image/akam_aabbccddeeff/snapshot/config||1") == 0,
              "all retained, no empty image config while snapshots are on");
    }
    det_z(0, 0, "run", 1763200000, 0);
    CHECK(wait_sent("akam/test/person_porch/state|OFF|0", 1) && wait_sent("akam/test/person_drive/state|OFF|0", 1),
          "first report: both zones OFF");
    det_z(1, 800, "run", 1763200100, 1);
    CHECK(wait_sent("akam/test/person_porch/state|ON|0", 1) && wait_sent("akam/test/person/state|ON|0", 1),
          "zone 0 ON with the global person");
    CHECK(sent_count("akam/test/person_drive/state|ON|0") == 0, "zone 1 stays OFF");
    det_z(1, 800, "run", 1763200110, 3);
    CHECK(wait_sent("akam/test/person_drive/state|ON|0", 1), "a zone change alone (global still ON) is a transition");
    det_z(1, 800, "run", 1763200120, 2);
    CHECK(wait_sent("akam/test/person_porch/state|OFF|0", 2), "zone 0 leaves while the global stays ON");
    CHECK(sent_count("akam/test/person_drive/state|ON|0") >= 1 && sent_count("akam/test/person_porch/state|ON|0") >= 1,
          "zone states are published with every transition");
    /* the snapshot */
    a2k_mqtt_snapshot((const uint8_t *)"JFIFdata", 8);
    CHECK(wait_sent("akam/test/snapshot|JFIFdata|0", 1), "snapshot published raw on BASE/snapshot, not retained");
    memset(big, 0x5a, sizeof big);
    a2k_mqtt_snapshot(big, A2K_MQTT_SNAP_MAX + 1);
    a2k_mqtt_snapshot(big, 0);
    a2k_mqtt_snapshot(NULL, 10);
    usleep(50000);
    a2k_mqtt_snap_stats(&sent, &dropped);
    CHECK(sent == 1 && dropped >= 3 && sent_count("akam/test/snapshot|") == 1, "oversized, empty and NULL snapshots are skipped and counted");
    memset(big, 0x5a, 4096);
    a2k_mqtt_snapshot(big, A2K_MQTT_SNAP_MAX);
    {
        int ok = 0;
        for (int i = 0; i < 4000 && !ok; i++) {
            a2k_mqtt_snap_stats(&sent, &dropped);
            ok = sent == 2;
            usleep(1000);
        }
        CHECK(ok, "a snapshot of exactly the cap is published (200 KB through the stub socket)");
    }
    /* the detections list: BASE/detections, QoS 0, not retained, no discovery entity */
    {
        static const char DJ[] = "{\"ts_ms\":1700000000123,\"w\":320,\"h\":192,\"boxes\":[{\"x\":312,\"y\":104,\"w\":125,\"h\":520,"
                                 "\"score\":900,\"id\":0,\"target\":false,\"ignored\":false}]}";
        char longj[A2K_MQTT_DET_MAX + 8];
        unsigned dsent, ddropped;
        a2k_mqtt_det_stats(&dsent, &ddropped);
        CHECK(dsent == 0 && ddropped == 0, "no detections list yet");
        a2k_mqtt_detections(DJ);
        {
            char want[400]; /* the stub keeps a record of the first bytes only: compare the head of the JSON */
            snprintf(want, sizeof want, "akam/test/detections|%.90s", DJ);
            CHECK(wait_sent(want, 1), "detections list published on BASE/detections");
        }
        a2k_mqtt_detections("{\"ts_ms\":1,\"boxes\":[]}");
        CHECK(wait_sent("akam/test/detections|{\"ts_ms\":1,\"boxes\":[]}|0", 1), "the empty list is published too");
        memset(longj, 'x', sizeof longj - 1);
        longj[sizeof longj - 1] = '\0';
        a2k_mqtt_detections(longj);
        a2k_mqtt_detections("");
        a2k_mqtt_detections(NULL);
        usleep(50000);
        a2k_mqtt_det_stats(&dsent, &ddropped);
        CHECK(dsent == 2 && ddropped >= 3 && sent_count("akam/test/detections|") == 2, "oversized, empty and NULL lists are dropped and counted");
    }
    a2k_mqtt_stop();
    {
        unsigned dsent, ddropped, dsent2, ddropped2;
        a2k_mqtt_det_stats(&dsent, &ddropped);
        a2k_mqtt_detections("{\"ts_ms\":2,\"boxes\":[]}");
        a2k_mqtt_det_stats(&dsent2, &ddropped2);
        CHECK(dsent2 == dsent && ddropped2 == ddropped && sent_count("akam/test/detections|{\"ts_ms\":2") == 0, "MQTT stopped: a list is a no-op, nothing queued");
    }
    a2k_mqtt_snapshot((const uint8_t *)"JFIFdata", 8);
    a2k_mqtt_snap_stats(&sent, &dropped);
    CHECK(dropped >= 1 && sent == 2, "MQTT stopped: a snapshot is dropped, nothing queued");
    a2k_mqtt_set_zones(NULL);
    a2k_mqtt_set_snapshot(0);
}

static void test_night_client(void)
{
    struct a2k_mqtt_night n;
    printf("== client: the day/night binary_sensor\n");
    reset_stub();
    a2k_mqtt_set_night(1);
    memset(&n, 0, sizeof n);
    n.valid = 1;
    n.mode = 0;
    snprintf(n.why, sizeof n.why, "automatic, start in day");
    a2k_mqtt_night(&n); /* before the start: kept as the current state */
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the night entity");
    CHECK(wait_sent("homeassistant/binary_sensor/akam_aabbccddeeff/night/config|{\"name\":\"Night mode\"", 1), "night discovery published");
    CHECK(wait_sent("akam/test/night/state|OFF|0", 1), "the state known before the connection is published at once");
    CHECK(wait_sent("akam/test/night/attributes|{\"mode\":\"auto\"", 1), "attributes published");
    n.night = 1;
    n.ir_on = 1;
    n.switches = 1;
    a2k_mqtt_night(&n);
    CHECK(wait_sent("akam/test/night/state|ON|0", 1), "a switch to night is published at once, not retained");
    det_z(1, 800, "run", 1763200000, 0);
    CHECK(wait_sent("akam/test/person/state|ON|0", 1) && sent_count("akam/test/night/state|OFF|0") == 1, "a detection report does not reset the night state");
    a2k_mqtt_stop();
    a2k_mqtt_set_night(0);
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("homeassistant/", 5), "connected without the night entity");
    CHECK(sent_count("homeassistant/binary_sensor/akam_aabbccddeeff/night/config") == 0 && sent_count("akam/test/night/") == 0,
          "night off: nothing about it is published");
    a2k_mqtt_stop();
}

static int g_press_n, g_press_cmd[64];
static int g_vel_n, g_vstop_n;
static char g_vel[64][32];
static int vel_fn(int sx, int sy, int dx, int dy, int lease)
{
    pthread_mutex_lock(&g_mu);
    if (g_vel_n < 64)
        snprintf(g_vel[g_vel_n], sizeof g_vel[0], "%d %d %d %d %d", sx, sy, dx, dy, lease);
    g_vel_n++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}
static int vstop_fn(void)
{
    pthread_mutex_lock(&g_mu);
    g_vstop_n++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}
static int press_fn(int cmd)
{
    pthread_mutex_lock(&g_mu);
    if (g_press_n < 64)
        g_press_cmd[g_press_n++] = cmd;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int presses(void)
{
    int n;
    pthread_mutex_lock(&g_mu);
    n = g_press_n;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void inject(const char *topic, const char *payload, int retain)
{
    uint8_t b[300];
    int n = mqtt_enc_publish(b, sizeof b, topic, payload, strlen(payload), retain);
    pthread_mutex_lock(&g_mu);
    if (n > 0)
        queue_in(b, (size_t)n);
    pthread_mutex_unlock(&g_mu);
}

static void test_ptz(void)
{
    a2k_mqtt_cfg_t c;
    a2k_mqtt_ids_t ids;
    char t[200], doc[1100], v[80];
    struct a2k_mqtt_det d;
    static const a2k_mqtt_ptz_t ops = { press_fn, NULL, NULL };
    int cmd = -1;
    static const char *const ids_name[] = { "ptz_left", "ptz_right", "ptz_up", "ptz_down", "ptz_home", "ptz_park", "ptz_recal" };
    printf("== PTZ: discovery, state, command parsing\n");
    memset(&c, 0, sizeof c);
    strcpy(c.base, "akam/test");
    strcpy(c.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PTZ0, t, sizeof t, doc, sizeof doc) == -1 && a2k_mqtt_discovery(&ids, A2K_E_PTZ_STATE, t, sizeof t, doc, sizeof doc) == -1,
          "no PTZ entity unless ids.ptz");
    ids.ptz = 1;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PTZ0 + 4, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/button/akam_aabbccddeeff/ptz_home/config") &&
          !strcmp(doc, "{\"name\":\"PTZ home\",\"unique_id\":\"akam_aabbccddeeff_ptz_home\",\"availability_topic\":\"akam/test/status\","
                       "\"command_topic\":\"akam/test/ptz_home/set\",\"payload_press\":\"PRESS\",\"icon\":\"mdi:home-circle\",\"device\":{\"identifiers\":"
                       "[\"akam_aabbccddeeff\"],\"connections\":[[\"mac\",\"aa:bb:cc:dd:ee:ff\"]],\"name\":\"akam-rot2k\",\"manufacturer\":\"Akam\","
                       "\"model\":\"lsc-rotatable-2k-ak3918av130\",\"sw_version\":\"0.1.0\"}}"), "button ptz_home discovery (golden)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PTZ_STATE, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/sensor/akam_aabbccddeeff/ptz_state/config") &&
          strstr(doc, "\"state_topic\":\"akam/test/ptz_state/state\"") && strstr(doc, "\"device_class\":\"enum\"") &&
          strstr(doc, "\"options\":[\"home\",\"parked\",\"moved\",\"calibrating\",\"unknown\"]") && !strstr(doc, "command_topic"),
          "ptz_state: enum sensor, no command topic");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_PTZ_PAN, t, sizeof t, doc, sizeof doc) > 0 && strstr(doc, "\"entity_category\":\"diagnostic\"") &&
          strstr(doc, "\"unit_of_measurement\":\"steps\"") && a2k_mqtt_discovery(&ids, A2K_E_PTZ_TILT, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/sensor/akam_aabbccddeeff/ptz_tilt/config"), "pan and tilt are diagnostic sensors in steps");
    for (int k = 0; k < A2K_PTZ_BTN; k++) {
        a2k_mqtt_discovery(&ids, A2K_E_PTZ0 + k, t, sizeof t, doc, sizeof doc);
        snprintf(v, sizeof v, "ha/button/akam_aabbccddeeff/%s/config", ids_name[k]);
        CHECK(!strcmp(t, v) && strstr(doc, "\"payload_press\":\"PRESS\""), ids_name[k]);
    }
    memset(&d, 0, sizeof d);
    CHECK(!a2k_mqtt_state(&ids, A2K_E_PTZ_STATE, &d, -1, t, sizeof t, v, sizeof v), "no state before the first report");
    d.ptz.valid = 1;
    snprintf(d.ptz.state, sizeof d.ptz.state, "calibrating");
    CHECK(a2k_mqtt_state(&ids, A2K_E_PTZ_STATE, &d, -1, t, sizeof t, v, sizeof v) && !strcmp(t, "akam/test/ptz_state/state") && !strcmp(v, "calibrating") &&
          !a2k_mqtt_state(&ids, A2K_E_PTZ_PAN, &d, -1, t, sizeof t, v, sizeof v) && !a2k_mqtt_state(&ids, A2K_E_PTZ_TILT, &d, -1, t, sizeof t, v, sizeof v),
          "calibrating: state only, no pan / tilt while the position is unknown");
    d.ptz.known = 1;
    d.ptz.pan = 1275;
    d.ptz.tilt = 1750;
    CHECK(a2k_mqtt_state(&ids, A2K_E_PTZ_PAN, &d, -1, t, sizeof t, v, sizeof v) && !strcmp(t, "akam/test/ptz_pan/state") && !strcmp(v, "1275") &&
          a2k_mqtt_state(&ids, A2K_E_PTZ_TILT, &d, -1, t, sizeof t, v, sizeof v) && !strcmp(v, "1750") && !a2k_mqtt_state(&ids, A2K_E_PTZ0, &d, -1, t, sizeof t, v, sizeof v),
          "pan / tilt when known; a button has no state");
    ids.ptz = 0;
    CHECK(!a2k_mqtt_state(&ids, A2K_E_PTZ_STATE, &d, -1, t, sizeof t, v, sizeof v), "ptz off: no state");
    ids.ptz = 1;
#define PCMD(tp, pl) a2k_mqtt_ptz_command(&ids, tp, strlen(tp), (const uint8_t *)(pl), strlen(pl), &cmd)
    for (int k = 0; k < A2K_PTZ_BTN; k++) {
        snprintf(t, sizeof t, "akam/test/%s/set", ids_name[k]);
        cmd = -1;
        CHECK(PCMD(t, "PRESS") == 1 && cmd == k, ids_name[k]);
    }
    CHECK(PCMD("akam/test/ptz_home/set", "press") == 0 && PCMD("akam/test/ptz_home/set", "PRESS ") == 0 && PCMD("akam/test/ptz_home/set", "PRESSX") == 0 &&
          PCMD("akam/test/ptz_home/set", "") == 0 && PCMD("akam/test/ptz_home/set", "ON") == 0 && PCMD("akam/test/ptz_home/set", "1") == 0,
          "only the exact payload PRESS is a command");
    CHECK(PCMD("akam/test/ptz_state/set", "PRESS") == 0 && PCMD("akam/test/ptz_pan/set", "PRESS") == 0 && PCMD("akam/test/ptz_home/state", "PRESS") == 0 &&
          PCMD("akam/test/ptz_hom/set", "PRESS") == 0 && PCMD("akam/other/ptz_home/set", "PRESS") == 0 && PCMD("akam/test/status_led/set", "PRESS") == 0 &&
          PCMD("akam/test/ptz_home/set/x", "PRESS") == 0 && PCMD("akam/test/ptz_home", "PRESS") == 0, "sensors, other topics and near misses are not commands");
    ids.ptz = 0;
    CHECK(PCMD("akam/test/ptz_home/set", "PRESS") == 0, "ptz off: no command");
    ids.ptz = 1;
    ids.leds = 1;
    CHECK(a2k_mqtt_led_command(&ids, "akam/test/ptz_home/set", 22, (const uint8_t *)"PRESS", 5, &cmd, v, sizeof v) == 0 &&
          a2k_mqtt_led_command(&ids, "akam/test/ptz_home/set", 22, (const uint8_t *)"auto", 4, &cmd, v, sizeof v) == 0, "the LED parser never takes a PTZ topic");
    CHECK(PCMD("akam/test/status_led/set", "auto") == 0, "and the PTZ parser never takes an LED command");
#undef PCMD

    printf("== PTZ: client (commands, retained ignored, rate limit, states)\n");
    reset_stub();
    pthread_mutex_lock(&g_mu);
    g_press_n = 0;
    pthread_mutex_unlock(&g_mu);
    a2k_mqtt_set_ptz(&ops);
    a2k_mqtt_ptz("calibrating", 0, 0, 0); /* before the start: kept as the current state */
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the PTZ entities");
    CHECK(wait_sent("homeassistant/button/akam_aabbccddeeff/ptz_", 6) && wait_sent("homeassistant/sensor/akam_aabbccddeeff/ptz_", 3), "discovery: six buttons, three sensors");
    CHECK(wait_sent("akam/test/ptz_state/state|calibrating|0", 1) && sent_count("akam/test/ptz_pan/state|") == 0, "the state known before the connection is published, no pan while unknown");
    a2k_mqtt_ptz("home", 1, 1275, 1300);
    CHECK(wait_sent("akam/test/ptz_state/state|home|0", 1) && wait_sent("akam/test/ptz_pan/state|1275|0", 1) && wait_sent("akam/test/ptz_tilt/state|1300|0", 1),
          "a state change is published at once (not retained)");
    inject("akam/test/ptz_left/set", "PRESS", 0);
    CHECK(({ for (int i = 0; i < 2000 && presses() < 1; i++) usleep(1000); presses() == 1; }) && g_press_cmd[0] == 0, "a PRESS on ptz_left reaches the controller (command 0)");
    inject("akam/test/ptz_park/set", "PRESS", 1);
    inject("akam/test/ptz_park/set", "press", 0);
    inject("akam/test/ptz_state/set", "PRESS", 0);
    inject("akam/test/ptz_home/set", "PRESS", 0);
    CHECK(({ for (int i = 0; i < 2000 && presses() < 2; i++) usleep(1000); presses() == 2; }) && g_press_cmd[1] == 4, "retained, wrong-case and sensor commands are ignored; the next valid one (home) is obeyed");
    inject("akam/test/ptz_recal/set", "PRESS", 0);
    CHECK(({ for (int i = 0; i < 2000 && presses() < 3; i++) usleep(1000); presses() == 3; }) && g_press_cmd[2] == 6, "a PRESS on ptz_recal (Recalibrate PTZ) reaches the controller (command 6)");
    for (int k = 0; k < 40; k++)
        inject("akam/test/ptz_up/set", "PRESS", 0);
    usleep(300000);
    CHECK(presses() <= 3 + A2K_MQTT_CMD_MAX, "rate limited: at most A2K_MQTT_CMD_MAX per window");
    a2k_mqtt_stop();
    printf("== PTZ velocity command (BASE/ptz_vel/set): mapping, lease, stop, junk ignored, own budget\n");
    {
        static const a2k_mqtt_ptz_t ops_v = { press_fn, vel_fn, vstop_fn };
        a2k_mqtt_set_ptz(&ops_v);
        reset_stub();
        g_vel_n = g_vstop_n = 0;
        CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the velocity command");
        inject("akam/test/ptz_vel/set", "0.5 -0.5 300", 0);
        CHECK(({ for (int i = 0; i < 2000 && g_vel_n < 1; i++) usleep(1000); g_vel_n == 1; }) && !strcmp(g_vel[0], "1 -1 4 4 300"), "\"0.5 -0.5 300\" -> right, down, divisors 4 / 4, lease 300 ms");
        inject("akam/test/ptz_vel/set", "1,0", 0);
        CHECK(({ for (int i = 0; i < 2000 && g_vel_n < 2; i++) usleep(1000); g_vel_n == 2; }) && !strcmp(g_vel[1], "1 0 2 0 450"), "a comma works and the lease defaults to 450 ms");
        inject("akam/test/ptz_vel/set", "1 1", 0);
        CHECK(({ for (int i = 0; i < 2000 && g_vel_n < 3; i++) usleep(1000); g_vel_n == 3; }) && !strcmp(g_vel[2], "1 1 3 3 450"), "a full diagonal is capped at divisor 3");
        inject("akam/test/ptz_vel/set", "stop", 0);
        inject("akam/test/ptz_vel/set", "STOP", 0);
        inject("akam/test/ptz_vel/set", "0 0.02", 0);
        CHECK(({ for (int i = 0; i < 2000 && g_vstop_n < 3; i++) usleep(1000); g_vstop_n == 3; }) && g_vel_n == 3, "stop, STOP and a velocity inside the dead zone are stops");
        inject("akam/test/ptz_vel/set", "1 0 300", 1);
        inject("akam/test/ptz_vel/set", "fast 0", 0);
        inject("akam/test/ptz_vel/set", "1", 0);
        inject("akam/test/ptz_vel/set", "1 0 100", 0);
        inject("akam/test/ptz_vel/set", "1 0 3000", 0);
        inject("akam/test/ptz_vel/set", "1 0 300 7", 0);
        inject("akam/test/ptz_vel/set", "2 0", 0);
        inject("akam/test/ptz_vel/set", "1e0 0", 0);
        inject("akam/test/ptz_vel/state", "1 0", 0);
        inject("akam/test/ptz_vel/set/x", "1 0", 0);
        inject("akam/other/ptz_vel/set", "1 0", 0);
        inject("akam/test/ptz_home/set", "1 0", 0);
        usleep(150000);
        CHECK(g_vel_n == 3 && g_vstop_n == 3, "retained, malformed, out-of-range and mis-addressed messages are ignored");
        {
            int n0 = g_vel_n;
            for (int k = 0; k < 40; k++) /* one burst: the thread reads it without waiting, so it is ONE window of the budget (the fake clock only runs while the thread idles) */
                inject("akam/test/ptz_vel/set", "0.5 0", 0);
            usleep(60000);
            CHECK(g_vel_n - n0 >= 1 && g_vel_n - n0 <= A2K_MQTT_VEL_MAX, "own budget: a burst of 40 is cut to at most A2K_MQTT_VEL_MAX (5) commands; the general 20 per 10 s is not what limits it");
        }
        CHECK(sent_count("homeassistant/button/") >= 6 && sent_count("homeassistant/") > 0, "no Home Assistant entity was added for it (still the six buttons)");
        a2k_mqtt_stop();
    }
    a2k_mqtt_set_ptz(NULL);
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("homeassistant/", 5), "connected without the PTZ entities");
    CHECK(sent_count("homeassistant/button/") == 0 && sent_count("akam/test/ptz_") == 0, "no ptz: nothing about it is published");
    a2k_mqtt_stop();
}

/* ---- ctl group: the tracking switch, zone texts, tracking areas, button event ---- */
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER;
static char g_ctl_track[4] = "OFF", g_ctl_zones[260] = "porch=0,0,50,50", g_ctl_areas[3][A2K_ZONE_NAME_MAX + 1];
static int g_ctl_na, g_ctl_setn, g_ctl_last_k = -1, g_ctl_orient;
static char g_ctl_last_v[A2K_CTL_VAL];
static int ctl_get_fn(int k, char *out, size_t cap)
{
    int rc = 0;
    pthread_mutex_lock(&g_cmu);
    if (k == A2K_CTL_TRACK) snprintf(out, cap, "%s", g_ctl_track);
    else if (k == A2K_CTL_ZONES) snprintf(out, cap, "%s", g_ctl_zones);
    else if (g_ctl_orient && k >= A2K_CTL_FLIP && k <= A2K_CTL_CEILING) { /* only while a test follows the orientation (the sent-message log is bounded) */
        int f, m;
        a2k_orient_get(&f, &m);
        snprintf(out, cap, "%s", (k == A2K_CTL_FLIP ? f : k == A2K_CTL_MIRROR ? m : f && m) ? "ON" : "OFF");
    }
    else if (k == A2K_CTL_IGNORE) snprintf(out, cap, "%s", "door=1,1,2,2");
    else if (k == A2K_CTL_AREAS) snprintf(out, cap, "%s", g_ctl_na ? "door=500:1500:500:1500" : "none");
    else if (k >= A2K_CTL_SLOT0 && (k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT == 0 && (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT < g_ctl_na) snprintf(out, cap, "ON");
    else rc = -1;
    pthread_mutex_unlock(&g_cmu);
    return rc;
}
static int ctl_set_fn(int k, const char *val)
{
    pthread_mutex_lock(&g_cmu);
    g_ctl_setn++;
    g_ctl_last_k = k;
    snprintf(g_ctl_last_v, sizeof g_ctl_last_v, "%s", val);
    if (k == A2K_CTL_TRACK) snprintf(g_ctl_track, sizeof g_ctl_track, "%s", val);
    if (k == A2K_CTL_ZONES) {
        if (!strcmp(val, "bad")) { pthread_mutex_unlock(&g_cmu); return -1; } /* refused: the old value stays and is published back */
        snprintf(g_ctl_zones, sizeof g_ctl_zones, "%s", val);
    }
    if (k == A2K_CTL_NEWAREA && g_ctl_na < 3) snprintf(g_ctl_areas[g_ctl_na++], sizeof g_ctl_areas[0], "%s", val);
    pthread_mutex_unlock(&g_cmu);
    if (k == A2K_CTL_NEWAREA)
        a2k_mqtt_resync();
    return 0;
}
static int ctl_areas_fn(char names[][A2K_ZONE_NAME_MAX + 1])
{
    int n;
    pthread_mutex_lock(&g_cmu);
    n = g_ctl_na;
    for (int i = 0; i < n; i++)
        snprintf(names[i], A2K_ZONE_NAME_MAX + 1, "%s", g_ctl_areas[i]);
    pthread_mutex_unlock(&g_cmu);
    return n;
}
static int ctl_sets(void)
{
    int n;
    pthread_mutex_lock(&g_cmu);
    n = g_ctl_setn;
    pthread_mutex_unlock(&g_cmu);
    return n;
}

static void test_ctl(void)
{
    a2k_mqtt_cfg_t c;
    a2k_mqtt_ids_t ids;
    char t[200], doc[1100], v[A2K_CTL_VAL];
    static const a2k_mqtt_ctl_t ops = { ctl_get_fn, ctl_set_fn, ctl_areas_fn };
    int k = -1;
    printf("== ctl group: discovery, state, command parsing\n");
    memset(&c, 0, sizeof c);
    strcpy(c.base, "akam/test");
    strcpy(c.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0, t, sizeof t, doc, sizeof doc) == -1 && a2k_mqtt_discovery(&ids, A2K_E_BUTTON, t, sizeof t, doc, sizeof doc) == -1,
          "no ctl entity unless ids.ctl");
    ids.ctl = 1;
    CHECK(A2K_E_CTL0 == A2K_E_PTZ0 + A2K_PTZ_N && A2K_E_BUTTON == A2K_E_CTL0 + A2K_CTL_N && A2K_CTL_N == 25 && A2K_ZONE_MAX == 8, "entity index layout (25 = 9 fixed ctl entities incl. the three orientation switches + 4 slots x 4)");
    {
        static const char *const id[3] = { "image_flip", "image_mirror", "ceiling_mount" }, *const nm[3] = { "Image flip", "Image mirror", "Ceiling mount" };
        int k, ok = 1;
        for (k = 0; k < 3; k++) {
            char want_t[80], want_c[80], want_n[40];
            snprintf(want_t, sizeof want_t, "ha/switch/akam_aabbccddeeff/%s/config", id[k]);
            snprintf(want_c, sizeof want_c, "\"command_topic\":\"akam/test/%s/set\"", id[k]);
            snprintf(want_n, sizeof want_n, "\"name\":\"%s\"", nm[k]);
            ok &= a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_FLIP + k, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, want_t) && strstr(doc, want_c) && strstr(doc, want_n) &&
                  strstr(doc, "\"payload_on\":\"ON\"") && strstr(doc, "\"payload_off\":\"OFF\"");
        }
        CHECK(ok && A2K_CTL_FLIP == 6 && A2K_CTL_MIRROR == 7 && A2K_CTL_CEILING == 8 && A2K_CTL_SLOT0 == 9, "the orientation switches (flip, mirror, ceiling mount): switch entities with ON / OFF commands, in the ctl group before the tracking-area slots");
    }
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_TRACK, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/switch/akam_aabbccddeeff/track/config") &&
              strstr(doc, "\"name\":\"Person tracking\"") && strstr(doc, "\"command_topic\":\"akam/test/track/set\"") && strstr(doc, "\"state_topic\":\"akam/test/track/state\"") &&
              strstr(doc, "\"payload_on\":\"ON\"") && strstr(doc, "\"payload_off\":\"OFF\""),
          "switch 'Person tracking'");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_PANONLY, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/switch/akam_aabbccddeeff/pan_only/config") && strstr(doc, "\"name\":\"Pan only tracking\"") && strstr(doc, "\"command_topic\":\"akam/test/pan_only/set\""), "switch 'Pan only tracking' (track.tilt 0)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_ZONES, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/text/akam_aabbccddeeff/zones/config") &&
              strstr(doc, "\"max\":255") && strstr(doc, "\"name\":\"Detection zones\""),
          "text 'Detection zones'");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_IGNORE, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/text/akam_aabbccddeeff/ignore_zones/config"), "text 'Ignore zones'");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_AREAS, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/sensor/akam_aabbccddeeff/track_areas/config") &&
              !strstr(doc, "command_topic"),
          "sensor 'Tracking areas' (no command)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_NEWAREA, t, sizeof t, doc, sizeof doc) > 0 && strstr(doc, "\"command_topic\":\"akam/test/track_area_new/set\"") &&
              !strstr(doc, "state_topic"),
          "text 'New tracking area name' (command only)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0, t, sizeof t, doc, sizeof doc) == -1, "no area slot entity without an area");
    ids.ntarea = 2;
    strcpy(ids.tarea[0], "door");
    strcpy(ids.tarea[1], "yard");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/switch/akam_aabbccddeeff/tarea0/config") &&
              strstr(doc, "\"name\":\"Tracking area door\"") && strstr(doc, "\"command_topic\":\"akam/test/tarea0/set\"") && strstr(doc, "\"unique_id\":\"akam_aabbccddeeff_tarea0\""),
          "area switch 'Tracking area door'");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0 + 1, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/button/akam_aabbccddeeff/tarea0_a/config") &&
              strstr(doc, "\"name\":\"Set tracking area door corner A\"") && strstr(doc, "\"payload_press\":\"PRESS\""),
          "area button 'Set tracking area door corner A'");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0 + A2K_CTL_PER_SLOT + 2, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/button/akam_aabbccddeeff/tarea1_b/config") &&
              strstr(doc, "corner B"), "slot 1 corner B");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0 + 3, t, sizeof t, doc, sizeof doc) > 0 && strstr(doc, "\"name\":\"Delete tracking area door\"") &&
              !strcmp(t, "ha/button/akam_aabbccddeeff/tarea0_del/config"), "area delete button");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_CTL0 + A2K_CTL_SLOT0 + 2 * A2K_CTL_PER_SLOT, t, sizeof t, doc, sizeof doc) == -1 &&
              a2k_mqtt_ctl_config_topic(&ids, A2K_CTL_SLOT0 + 2 * A2K_CTL_PER_SLOT, t, sizeof t) == 0 && !strcmp(t, "ha/switch/akam_aabbccddeeff/tarea2/config"),
          "slot 2 has no entity, but its config topic is known (to clear it)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_BUTTON, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/event/akam_aabbccddeeff/button/config") &&
              strstr(doc, "\"name\":\"Button\"") && strstr(doc, "\"state_topic\":\"akam/test/button/event\"") && strstr(doc, "\"event_types\":[\"press\"]"),
          "event entity 'Button' (event_types press)");
    CHECK(a2k_mqtt_button_topic(&ids, t, sizeof t) == 0 && !strcmp(t, "akam/test/button/event") && !strcmp(a2k_mqtt_button_payload, "{\"event_type\":\"press\"}"), "event topic and payload");
    CHECK(a2k_mqtt_zone_config_topic(&ids, "porch", t, sizeof t) == 0 && !strcmp(t, "ha/binary_sensor/akam_aabbccddeeff/person_porch/config"), "zone config topic (to clear a removed zone)");
    CHECK(a2k_mqtt_ctl_state(&ids, A2K_CTL_TRACK, "ON", t, sizeof t) == 1 && !strcmp(t, "akam/test/track/state") && a2k_mqtt_ctl_state(&ids, A2K_CTL_NEWAREA, "x", t, sizeof t) == 0 &&
              a2k_mqtt_ctl_state(&ids, A2K_CTL_SLOT0 + 1, "x", t, sizeof t) == 0 && a2k_mqtt_ctl_state(&ids, A2K_CTL_SLOT0 + 2 * A2K_CTL_PER_SLOT, "ON", t, sizeof t) == 0,
          "state topics: switch / text / sensor yes; button, command-only text, missing slot no");
#define CCMD(tp, pl) a2k_mqtt_ctl_command(&ids, tp, strlen(tp), (const uint8_t *)(pl), strlen(pl), &k, v, sizeof v)
    CHECK(CCMD("akam/test/track/set", "ON") == 1 && k == A2K_CTL_TRACK && !strcmp(v, "ON") && CCMD("akam/test/track/set", "OFF") == 1, "switch ON / OFF");
    CHECK(CCMD("akam/test/pan_only/set", "ON") == 1 && k == A2K_CTL_PANONLY && !strcmp(v, "ON"), "switch pan_only ON / OFF command");
    CHECK(CCMD("akam/test/track/set", "on") == 0 && CCMD("akam/test/track/set", "1") == 0 && CCMD("akam/test/track/set", "") == 0 && CCMD("akam/test/track/set", "ONN") == 0, "switch: exact payloads only");
    CHECK(CCMD("akam/test/tarea1_a/set", "PRESS") == 1 && k == A2K_CTL_SLOT0 + A2K_CTL_PER_SLOT + 1 && CCMD("akam/test/tarea1_a/set", "press") == 0 && CCMD("akam/test/tarea1_a/set", "ON") == 0,
          "button: exactly PRESS");
    CHECK(CCMD("akam/test/tarea2_a/set", "PRESS") == 0 && CCMD("akam/test/tarea0_del/set", "PRESS") == 1, "a button of a missing slot is not a command");
    CHECK(CCMD("akam/test/zones/set", "porch=0,0,50,50;zone_mode=all") == 1 && k == A2K_CTL_ZONES && !strcmp(v, "porch=0,0,50,50;zone_mode=all"), "zones text command");
    CHECK(CCMD("akam/test/zones/set", "porch=0,0,50,50@screen;drive=50,0,100,50@position") == 1 && !strcmp(v, "porch=0,0,50,50@screen;drive=50,0,100,50@position"),
          "zones text with anchor suffixes (@ allowed)");
    CHECK(CCMD("akam/test/zones/set", "") == 1 && v[0] == '\0' && CCMD("akam/test/ignore_zones/set", "none") == 1 && CCMD("akam/test/track_area_new/set", "") == 0 &&
              CCMD("akam/test/track_area_new/set", "gate") == 1 && k == A2K_CTL_NEWAREA,
          "an empty zones text clears; an empty area name is not a command");
    CHECK(CCMD("akam/test/zones/set", "Porch=1") == 0 && CCMD("akam/test/zones/set", "a b") == 0 && CCMD("akam/test/zones/set", "a\n") == 0 && CCMD("akam/test/zones/set", "a\"b") == 0,
          "characters outside [0-9a-z_=,;:.@-] are refused");
    {
        char big[300];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(CCMD("akam/test/zones/set", big) == 0, "a payload over the limit is refused");
    }
    CHECK(CCMD("akam/test/track_areas/set", "x") == 0 && CCMD("akam/test/track/state", "ON") == 0 && CCMD("akam/other/track/set", "ON") == 0 && CCMD("akam/test/track/set/x", "ON") == 0,
          "the sensor, state topics and near misses are not commands");
    ids.ctl = 0;
    CHECK(CCMD("akam/test/track/set", "ON") == 0, "ctl off: no command");
    ids.ctl = 1;
    CHECK(a2k_mqtt_led_command(&ids, "akam/test/track/set", 19, (const uint8_t *)"on", 2, &k, v, sizeof v) == 0, "the LED parser never takes a ctl topic");
#undef CCMD

    printf("== ctl group: client (commands, values back, retained ignored, resync, button event)\n");
    reset_stub();
    a2k_mqtt_set_ctl(&ops);
    pthread_mutex_lock(&g_cmu);
    g_ctl_na = 1;
    strcpy(g_ctl_areas[0], "door");
    g_ctl_setn = 0;
    pthread_mutex_unlock(&g_cmu);
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the ctl entities");
    CHECK(wait_sent("homeassistant/switch/akam_aabbccddeeff/track/config", 1) && wait_sent("homeassistant/text/akam_aabbccddeeff/zones/config", 1) &&
              wait_sent("homeassistant/event/akam_aabbccddeeff/button/config", 1) && wait_sent("homeassistant/switch/akam_aabbccddeeff/tarea0/config", 1) &&
              wait_sent("homeassistant/button/akam_aabbccddeeff/tarea0_a/config", 1),
          "discovery: switch, texts, event, the door area entities");
    CHECK(wait_sent("homeassistant/switch/akam_aabbccddeeff/tarea1/config|", 1), "the empty slots are cleared (an empty retained config)");
    CHECK(wait_sent("akam/test/track/state|OFF|0", 1) && wait_sent("akam/test/zones/state|porch=0,0,50,50|0", 1) && wait_sent("akam/test/track_areas/state|door=500:1500:500:1500|0", 1) &&
              wait_sent("akam/test/tarea0/state|ON|0", 1),
          "the values are published (not retained)");
    inject("akam/test/track/set", "ON", 0);
    CHECK(({ for (int i = 0; i < 2000 && ctl_sets() < 1; i++) usleep(1000); ctl_sets() == 1; }) && wait_sent("akam/test/track/state|ON|0", 1), "ON reaches the app and the real value is published back");
    inject("akam/test/zones/set", "bad", 0);
    CHECK(({ for (int i = 0; i < 2000 && ctl_sets() < 2; i++) usleep(1000); ctl_sets() == 2; }) && wait_sent("akam/test/zones/state|porch=0,0,50,50|0", 2),
          "an invalid zones text: the old value is published back");
    inject("akam/test/track/set", "OFF", 1);
    inject("akam/test/track/set", "maybe", 0);
    inject("akam/test/tarea0_a/set", "PRESS", 0);
    CHECK(({ for (int i = 0; i < 2000 && ctl_sets() < 3; i++) usleep(1000); ctl_sets() == 3; }) && g_ctl_last_k == A2K_CTL_SLOT0 + 1,
          "a retained command and a bad payload are ignored; a corner press is obeyed");
    inject("akam/test/track_area_new/set", "gate", 0);
    CHECK(wait_sent("homeassistant/switch/akam_aabbccddeeff/tarea1/config|{", 1) && wait_sent("homeassistant/button/akam_aabbccddeeff/tarea1_del/config|{", 1),
          "a new area is announced (resync): its four entities appear");
    CHECK(wait_sent("akam/test/track_areas/state|", 2), "the areas sensor is republished");
    {
        a2k_zones_t z;
        memset(&z, 0, sizeof z);
        strcpy(z.z[0].name, "drive");
        z.z[0].x2 = z.z[0].y2 = 50;
        z.n = 1;
        a2k_mqtt_zones_changed(&z);
    }
    CHECK(wait_sent("homeassistant/binary_sensor/akam_aabbccddeeff/person_drive/config|{", 1), "a zone added at run time is announced");
    {
        a2k_zones_t z;
        memset(&z, 0, sizeof z);
        a2k_mqtt_zones_changed(&z);
    }
    CHECK(wait_sent("homeassistant/binary_sensor/akam_aabbccddeeff/person_drive/config|", 2), "a zone removed at run time: its config is cleared");
    a2k_mqtt_button_event("press");
    CHECK(wait_sent("akam/test/button/event|{\"event_type\":\"press\"}|0", 1), "the button press is published as an event, not retained");
    a2k_mqtt_button_event("long");
    usleep(150000);
    CHECK(sent_count("akam/test/button/event|") == 1, "only 'press' is an event");
    for (int i = 0; i < 40; i++)
        inject("akam/test/track/set", "ON", 0);
    usleep(300000);
    CHECK(ctl_sets() <= 4 + A2K_MQTT_CMD_MAX + 1, "rate limited like the other controls");
    a2k_mqtt_stop();
    a2k_mqtt_set_ctl(NULL);
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("homeassistant/", 5), "connected without the ctl entities");
    CHECK(sent_count("homeassistant/switch/") == 0 && sent_count("homeassistant/event/") == 0 && sent_count("akam/test/track") == 0, "no ctl: nothing about it is published");
    a2k_mqtt_button_event("press");
    usleep(100000);
    CHECK(sent_count("akam/test/button/event|") == 0, "no ctl: no event");
    a2k_mqtt_stop();
}

static char g_vid_val[A2K_VID_N][16];
static int g_vid_setn, g_vid_last_k = -1;
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
static int vid_get_fn(int k, char *out, size_t cap)
{
    pthread_mutex_lock(&g_vmu);
    snprintf(out, cap, "%s", g_vid_val[k]);
    pthread_mutex_unlock(&g_vmu);
    return 0;
}
static int vid_set_fn(int k, const char *val)
{
    pthread_mutex_lock(&g_vmu);
    g_vid_setn++;
    g_vid_last_k = k;
    if (strcmp(val, "7777")) /* "7777" = refused by the app (an inconsistent set): the old value stays */
        snprintf(g_vid_val[k], sizeof g_vid_val[k], "%s", val);
    pthread_mutex_unlock(&g_vmu);
    return 0;
}
static int vid_sets(void)
{
    int n;
    pthread_mutex_lock(&g_vmu);
    n = g_vid_setn;
    pthread_mutex_unlock(&g_vmu);
    return n;
}

static void test_vid(void)
{
    a2k_mqtt_cfg_t c;
    a2k_mqtt_ids_t ids;
    char t[200], doc[1100], v[16];
    static const a2k_mqtt_vid_t ops = { vid_get_fn, vid_set_fn };
    int k = -1;
    printf("== video settings: discovery, state, command parsing, client\n");
    memset(&c, 0, sizeof c);
    strcpy(c.base, "akam/test");
    strcpy(c.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "cam", "aa:bb:cc:dd:ee:ff", "1.0") == 0, "ids");
    ids.vid = 1;
    CHECK(A2K_VID_N == 14 && A2K_E_VID0 == A2K_E_BUTTON + 1, "video entity layout");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_VID0 + A2K_VP_KBPS, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/number/akam_aabbccddeeff/video0_kbps/config") &&
              strstr(doc, "\"name\":\"Video main bit rate\"") && strstr(doc, "\"command_topic\":\"akam/test/video0_kbps/set\"") &&
              strstr(doc, "\"state_topic\":\"akam/test/video0_kbps/state\"") && strstr(doc, "\"min\":64,\"max\":8000,\"step\":1") && strstr(doc, "\"mode\":\"box\"") &&
              strstr(doc, "\"unit_of_measurement\":\"kbit/s\"") && strstr(doc, "\"entity_category\":\"config\""),
          "main bit rate: a config number, 64..8000 kbit/s");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_VID0 + A2K_VP_N + A2K_VP_MAXKBPS, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/number/akam_aabbccddeeff/video1_max_kbps/config") &&
              strstr(doc, "\"min\":0,\"max\":1500"),
          "sub peak bit rate: 0 (automatic) .. 1500");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_VID0 + A2K_VP_RC, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/select/akam_aabbccddeeff/video0_rc/config") &&
              strstr(doc, "\"options\":[\"vbr\",\"cbr\",\"avbr\"]") && !strstr(doc, "\"min\""),
          "rate control is a select");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_VID0 + A2K_VP_FPS, t, sizeof t, doc, sizeof doc) > 0 && strstr(doc, "\"min\":1,\"max\":15") && strstr(doc, "\"unit_of_measurement\":\"fps\""), "frame rate 1..15");
    ids.vid = 0;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_VID0, t, sizeof t, doc, sizeof doc) == -1, "no video entities without the group");
    ids.vid = 1;
    CHECK(a2k_mqtt_vid_state(&ids, 3, "32", t, sizeof t) == 1 && !strcmp(t, "akam/test/video0_min_qp/state") && a2k_mqtt_vid_state(&ids, A2K_VID_N, "1", t, sizeof t) == 0, "state topic");
#define VCMD(tp, pl) a2k_mqtt_vid_command(&ids, tp, strlen(tp), (const uint8_t *)(pl), strlen(pl), &k, v, sizeof v)
    CHECK(VCMD("akam/test/video0_kbps/set", "3000") == 1 && k == A2K_VP_KBPS && !strcmp(v, "3000") && VCMD("akam/test/video1_rc/set", "cbr") == 1 && k == A2K_VP_N + A2K_VP_RC,
          "a number and a select option");
    CHECK(VCMD("akam/test/video0_kbps/set", "63") == 0 && VCMD("akam/test/video0_kbps/set", "8001") == 0 && VCMD("akam/test/video1_kbps/set", "1001") == 0 &&
              VCMD("akam/test/video0_kbps/set", "") == 0 && VCMD("akam/test/video0_kbps/set", "-5") == 0 && VCMD("akam/test/video0_kbps/set", "12a") == 0 &&
              VCMD("akam/test/video0_kbps/set", "1e3") == 0 && VCMD("akam/test/video0_kbps/set", "100000") == 0,
          "numbers out of range or not plain digits are refused");
    CHECK(VCMD("akam/test/video0_max_kbps/set", "0") == 1 && VCMD("akam/test/video0_kbps/set", "0") == 0 && VCMD("akam/test/video0_fps/set", "16") == 0 &&
              VCMD("akam/test/video0_min_qp/set", "9") == 0 && VCMD("akam/test/video0_max_qp/set", "52") == 0 && VCMD("akam/test/video1_gop/set", "301") == 0,
          "0 only for the automatic peak; fps, QP and GOP ranges");
    CHECK(VCMD("akam/test/video0_rc/set", "VBR") == 0 && VCMD("akam/test/video0_rc/set", "2") == 0 && VCMD("akam/test/video0_rc/set", "vbr ") == 0 &&
              VCMD("akam/test/video0_rc/set", "avbrr") == 0 && VCMD("akam/test/video0_kbps/set", "vbr") == 0,
          "the select takes exactly one option; a number entity never a word");
    CHECK(VCMD("akam/test/video2_kbps/set", "1000") == 0 && VCMD("akam/other/video0_kbps/set", "1000") == 0 && VCMD("akam/test/video0_kbps/state", "1000") == 0 &&
              VCMD("akam/test/video0_kbps/set/x", "1000") == 0,
          "near misses are not commands");
    ids.vid = 0;
    CHECK(VCMD("akam/test/video0_kbps/set", "1000") == 0, "vid off: no command");
#undef VCMD

    reset_stub();
    g_fake = 1000000; /* the command rate window of the ctl test (static) is long past */
    a2k_mqtt_set_vid(&ops);
    pthread_mutex_lock(&g_vmu);
    for (int i = 0; i < A2K_VID_N; i++)
        snprintf(g_vid_val[i], sizeof g_vid_val[i], "%d", 100 + i);
    strcpy(g_vid_val[A2K_VP_RC], "vbr");
    g_vid_setn = 0;
    pthread_mutex_unlock(&g_vmu);
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the video entities");
    CHECK(wait_sent("homeassistant/number/akam_aabbccddeeff/video0_kbps/config|{", 1) && wait_sent("homeassistant/select/akam_aabbccddeeff/video1_rc/config|{", 1) &&
              wait_sent("homeassistant/number/akam_aabbccddeeff/video1_gop/config|{", 1),
          "discovery of the 14 entities");
    CHECK(wait_sent("akam/test/video0_kbps/state|100|0", 1) && wait_sent("akam/test/video0_rc/state|vbr|0", 1) && wait_sent("akam/test/video1_gop/state|112|0", 1),
          "the values are published, not retained");
    inject("akam/test/video0_kbps/set", "3000", 0);
    CHECK(({ for (int i = 0; i < 2000 && vid_sets() < 1; i++) usleep(1000); vid_sets() == 1; }) && g_vid_last_k == A2K_VP_KBPS && wait_sent("akam/test/video0_kbps/state|3000|0", 1),
          "a command reaches the app and the real value comes back");
    inject("akam/test/video0_kbps/set", "7777", 0);
    inject("akam/test/video0_kbps/set", "99999", 0);
    inject("akam/test/video0_kbps/set", "4000", 1);
    CHECK(({ for (int i = 0; i < 2000 && vid_sets() < 2; i++) usleep(1000); vid_sets() == 2; }) && wait_sent("akam/test/video0_kbps/state|3000|0", 2),
          "a value the app refuses comes back unchanged; an out-of-range payload and a retained command never reach it");
    usleep(100000);
    CHECK(vid_sets() == 2, "nothing else was obeyed");
    a2k_mqtt_stop();
    a2k_mqtt_set_vid(NULL);
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("homeassistant/", 5), "connected without the video group");
    CHECK(sent_count("homeassistant/number/") == 0 && sent_count("homeassistant/select/") == 0 && sent_count("akam/test/video") == 0, "no video group: nothing about it is published");
    a2k_mqtt_stop();
}

/* ---- the programmable button: gesture publish + device triggers ---- */
static pthread_mutex_t g_bmu = PTHREAD_MUTEX_INITIALIZER;
static int g_btn_n;
static char g_btn_v[A2K_BTN_TRIG_N][A2K_BTN_NAME];
static int btn_list_fn(char names[][A2K_BTN_NAME])
{
    int n;
    pthread_mutex_lock(&g_bmu);
    n = g_btn_n;
    for (int i = 0; i < n; i++)
        snprintf(names[i], A2K_BTN_NAME, "%s", g_btn_v[i]);
    pthread_mutex_unlock(&g_bmu);
    return n;
}
static void btn_set_list(int n, const char *a, const char *b, const char *c, const char *d)
{
    const char *v[4] = { a, b, c, d };
    pthread_mutex_lock(&g_bmu);
    g_btn_n = n;
    for (int i = 0; i < n; i++)
        snprintf(g_btn_v[i], sizeof g_btn_v[0], "%s", v[i]);
    pthread_mutex_unlock(&g_bmu);
}

/* An orientation change that did not come from MQTT (API, button mode, the settings loaded at boot) republishes the switches (a2k_mqtt_follow_orientation). */
static void test_orient_follow(void)
{
    static const a2k_mqtt_ctl_t ops = { ctl_get_fn, ctl_set_fn, ctl_areas_fn };
    printf("== orientation switches follow a change made elsewhere\n");
    a2k_orient_test_reset();
    reset_stub();
    a2k_mqtt_set_ctl(&ops);
    g_ctl_orient = 1;
    CHECK(a2k_mqtt_follow_orientation() == 0, "the MQTT bind follows the orientation");
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the ctl entities");
    CHECK(wait_sent("akam/test/image_flip/state|OFF|0", 1) && wait_sent("akam/test/ceiling_mount/state|OFF|0", 1), "the orientation switches are published (off)");
    a2k_orient_set(1, 0);
    CHECK(wait_sent("akam/test/image_flip/state|ON|0", 1) && wait_sent("akam/test/image_mirror/state|OFF|0", 2) && wait_sent("akam/test/ceiling_mount/state|OFF|0", 2),
          "a change from another source republishes the switches with the new values");
    a2k_orient_set(1, 1);
    CHECK(wait_sent("akam/test/ceiling_mount/state|ON|0", 1) && wait_sent("akam/test/image_mirror/state|ON|0", 1), "the ceiling mount and the mirror follow too");
    a2k_mqtt_stop();
    g_ctl_orient = 0;
    a2k_orient_test_reset();
    a2k_mqtt_set_ctl(NULL);
}

static void test_btn(void)
{
    a2k_mqtt_cfg_t c;
    a2k_mqtt_ids_t ids;
    char t[200], doc[1100];
    static const a2k_mqtt_ctl_t ops = { ctl_get_fn, ctl_set_fn, ctl_areas_fn };
    static const char J1[] = "{\"gesture\":\"double\",\"actions\":[\"ptz.home\"],\"result\":\"ok\"}";
    static const char J2[] = "{\"gesture\":\"short\",\"actions\":[\"none\"],\"result\":\"ok\"}";
    printf("== button gestures: device triggers and the BASE/button publish\n");
    memset(&c, 0, sizeof c);
    strcpy(c.base, "akam/test");
    strcpy(c.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &c, "akam-rot2k", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids");
    CHECK(A2K_E_BTN0 == A2K_E_VID0 + A2K_VID_N && A2K_BTN_TRIG_N == 16 && A2K_MQTT_GESTURE_MAX == A2K_EVT_DATA, "layout: the triggers follow the video entities; 16 slots; the text limit is the event ring limit");
    ids.ctl = 1;
    ids.nbtn = 3;
    strcpy(ids.btn[0], "short");
    strcpy(ids.btn[1], "double");
    strcpy(ids.btn[2], "S S L");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_BTN0, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/device_automation/akam_aabbccddeeff/button_0/config") &&
              strstr(doc, "{\"automation_type\":\"trigger\",\"type\":\"short\",\"subtype\":\"button\",\"topic\":\"akam/test/button\",\"value_template\":\"{{ value_json.gesture }}\",\"payload\":\"short\","
                          "\"device\":{\"identifiers\":[\"akam_aabbccddeeff\"]") == doc,
          "device trigger of 'short' (type, subtype button, topic BASE/button, match on value_json.gesture, device)");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_BTN0 + 2, t, sizeof t, doc, sizeof doc) > 0 && !strcmp(t, "ha/device_automation/akam_aabbccddeeff/button_2/config") && strstr(doc, "\"type\":\"s_s_l\"") &&
              strstr(doc, "\"payload\":\"S S L\""),
          "a sequence: type s_s_l (lower case, _), payload the exact gesture name");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_BTN0 + 3, t, sizeof t, doc, sizeof doc) == -1 && a2k_mqtt_btn_config_topic(&ids, 3, t, sizeof t) == 0 && !strcmp(t, "ha/device_automation/akam_aabbccddeeff/button_3/config") &&
              a2k_mqtt_btn_config_topic(&ids, A2K_BTN_TRIG_N, t, sizeof t) == -1 && a2k_mqtt_gesture_topic(&ids, t, sizeof t) == 0 && !strcmp(t, "akam/test/button"),
          "no trigger past the configured ones, but their config topic is known (to clear it); the gesture topic");
    ids.ctl = 0;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_BTN0, t, sizeof t, doc, sizeof doc) == -1, "no trigger without the ctl group (MQTT stays opt-in)");
    ids.ctl = 1;

    reset_stub();
    pthread_mutex_lock(&g_cmu);
    g_ctl_na = 0;
    g_ctl_setn = 0;
    pthread_mutex_unlock(&g_cmu);
    btn_set_list(3, "short", "double", "S S L", "");
    a2k_mqtt_set_ctl(&ops);
    a2k_mqtt_set_buttons(btn_list_fn);
    CHECK(start_with(FULL) == 0 && wait_state("connected"), "connected with the ctl group and the gesture list");
    CHECK(wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_0/config|{\"automation_type\":\"trigger\"", 1) &&
              wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_1/config|{", 1) && wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_2/config|{", 1),
          "discovery: one device trigger per configured gesture");
    CHECK(wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_3/config|", 1) && sent_count("homeassistant/device_automation/akam_aabbccddeeff/button_3/config|{") == 0,
          "the slots past them are cleared (empty retained config)");
    a2k_mqtt_button_gesture("double", J1);
    CHECK(wait_sent("akam/test/button|{\"gesture\":\"double\",\"actions\":[\"ptz.home\"],\"result\":\"ok\"}|0", 1), "every gesture is published on BASE/button, not retained");
    usleep(100000);
    CHECK(sent_count("akam/test/button/event|") == 0, "a double press is not the legacy press event");
    a2k_mqtt_button_gesture("short", J2);
    CHECK(wait_sent("akam/test/button|{\"gesture\":\"short\"", 1) && wait_sent("akam/test/button/event|{\"event_type\":\"press\"}|0", 1),
          "a short gesture is published on BASE/button AND as the legacy Home Assistant event (the factory behaviour)");
    a2k_mqtt_button_gesture("long", "{\"gesture\":\"long\",\"actions\":[\"x\"],\"result\":\"failed\"}");
    a2k_mqtt_button_gesture("S L", "{\"gesture\":\"S L\",\"actions\":[],\"result\":\"unbound\"}");
    CHECK(wait_sent("akam/test/button|{\"gesture\":\"long\"", 1) && wait_sent("akam/test/button|{\"gesture\":\"S L\"", 1), "failed and unbound gestures are published too");
    {
        char huge[400];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        a2k_mqtt_button_gesture("long", huge);
        a2k_mqtt_button_gesture(NULL, J1);
        a2k_mqtt_button_gesture("long", NULL);
        usleep(100000);
        CHECK(sent_count("akam/test/button|xxx") == 0, "an oversized text or a NULL is dropped");
    }
    btn_set_list(4, "short", "double", "S S L", "very_long");
    a2k_mqtt_resync();
    CHECK(wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_3/config|{", 1), "a new binding (resync): its trigger is announced");
    btn_set_list(1, "short", "", "", "");
    a2k_mqtt_resync();
    CHECK(wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_1/config|", 2) && wait_sent("homeassistant/device_automation/akam_aabbccddeeff/button_3/config|", 3),
          "bindings removed: their triggers are cleared");
    a2k_mqtt_stop();
    a2k_mqtt_set_ctl(NULL);
    reset_stub();
    CHECK(start_with(FULL) == 0 && wait_state("connected") && wait_sent("homeassistant/", 5), "connected without the ctl group");
    a2k_mqtt_button_gesture("short", J2);
    usleep(100000);
    CHECK(sent_count("akam/test/button|") == 0 && sent_count("homeassistant/device_automation/") == 0, "no ctl (MQTT opt-in off): no trigger, no gesture message");
    a2k_mqtt_stop();
    a2k_mqtt_set_buttons(NULL);
}

int main(void)
{
    char dir[] = "/tmp/rot2k_mqtt_XXXXXX";
    char buf[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    g_dir = dir;
    snprintf(g_conf, sizeof g_conf, "%s/mqtt", dir);
    snprintf(g_pass, sizeof g_pass, "%s/mqtt_pass", dir);
    snprintf(g_macf, sizeof g_macf, "%s/address", dir);
    write_file(g_macf, "aa:bb:cc:dd:ee:ff\n");
    test_cfg();
    test_builders();
    test_fuzz();
    test_client();
    test_zones_snapshot();
    test_night_client();
    test_ptz();
    test_ctl();
    test_btn();
    test_vid();
    test_orient_follow();
    snprintf(buf, sizeof buf, "rm -rf %s", dir);
    if (system(buf) != 0) { /* best effort */ }
    printf("rot2k mqtt unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
