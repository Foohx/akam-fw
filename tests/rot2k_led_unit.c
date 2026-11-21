/* Host unit test of the status LED of the 2K: the pattern parser, pattern timing on a fake clock, the rule priority and the engine on
 * a fake hal_light driver (leds.c), the route parser behind the "no Wi-Fi" rule (hal_sys.c) and the Home Assistant side of
 * the controls (discovery, state, command parsing in mqtt_cfg.c). Prints "rot2k led unit: N failed". */
#include <stdio.h>
#include <string.h>

#include "hal.h"
#include "leds.h"
#include "mqtt_cfg.h"
#include "hal_sys.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int lvl[2] = { -1, -1 }, writes[2];
static int fake_set(const hal_light_emitter_t *e, int on)
{
    int i = (int)(e - a2k_light_desc.emitters);
    lvl[i] = on;
    writes[i]++;
    return 0;
}
static const hal_light_ops_t fake_drv = { fake_set };

static a2k_leds_in_t g_in;
static void fake_inputs(void *u, a2k_leds_in_t *in)
{
    (void)u;
    *in = g_in;
}
static const a2k_leds_ops_t fake_ops = { fake_inputs, 0 };

static void test_parse(void)
{
    a2k_pat_t p;
    char t[A2K_PAT_MAX];
    CHECK(a2k_pat_parse("on", &p) == 0 && p.kind == A2K_PAT_ON && a2k_pat_parse("off", &p) == 0 && p.kind == A2K_PAT_OFF, "on / off");
    CHECK(a2k_pat_parse("3:250:250:5000", &p) == 0 && p.kind == A2K_PAT_BLINK && p.n == 3 && p.on_ms == 250 && p.off_ms == 250 && p.pause_ms == 5000, "3:250:250:5000");
    a2k_pat_format(&p, t, sizeof t);
    CHECK(!strcmp(t, "3:250:250:5000"), "format round trip");
    CHECK(a2k_pat_parse("1:100:100:0", &p) == 0 && a2k_pat_parse("1:250:250:0", &p) == 0, "the defaults with pause 0 are valid");
    CHECK(a2k_pat_parse("10:50:50:50", &p) == 0 && a2k_pat_parse("1:60000:50:0", &p) == 0 && a2k_pat_parse("1:50:60000:59950", &p) == 0, "bounds: N 10, 50 ms, 60000 ms, cycle exactly 120000 are valid");
    CHECK(a2k_pat_parse("0:250:250:0", &p) == -1 && a2k_pat_parse("11:250:250:0", &p) == -1, "N 0 and 11 refused");
    CHECK(a2k_pat_parse("1:49:250:0", &p) == -1 && a2k_pat_parse("1:250:49:0", &p) == -1 && a2k_pat_parse("1:60001:250:0", &p) == -1 &&
              a2k_pat_parse("1:250:60001:0", &p) == -1 && a2k_pat_parse("1:250:250:60001", &p) == -1 && a2k_pat_parse("1:250:250:49", &p) == -1,
          "on / off / pause out of 50..60000 refused (pause 0 allowed)");
    CHECK(a2k_pat_parse("3:30000:30000:1000", &p) == -1 && a2k_pat_parse("2:30000:30000:0", &p) == 0 && a2k_pat_parse("2:30000:30000:1", &p) == -1,
          "cycle > 120000 ms refused");
    CHECK(a2k_pat_parse("", &p) == -1 && a2k_pat_parse("ON", &p) == -1 && a2k_pat_parse("blink", &p) == -1 && a2k_pat_parse(" on", &p) == -1 &&
              a2k_pat_parse("on ", &p) == -1 && a2k_pat_parse("1:2:3", &p) == -1 && a2k_pat_parse("1:250:250:0:0", &p) == -1 &&
              a2k_pat_parse("1:250:250:", &p) == -1 && a2k_pat_parse("1::250:0", &p) == -1 && a2k_pat_parse("+1:250:250:0", &p) == -1 &&
              a2k_pat_parse("1:2.5:250:0", &p) == -1 && a2k_pat_parse("-1:250:250:0", &p) == -1 && a2k_pat_parse("1:250:250:0\n", &p) == -1 &&
              a2k_pat_parse("1:9999999:250:0", &p) == -1 && a2k_pat_parse(0, &p) == -1,
          "anything outside the syntax refused");
    CHECK(a2k_leds_parse_mode("auto") == A2K_LEDS_AUTO && a2k_leds_parse_mode("on") == A2K_LEDS_ON && a2k_leds_parse_mode("off") == A2K_LEDS_OFF &&
              a2k_leds_parse_mode("Auto") == -1 && a2k_leds_parse_mode("") == -1,
          "master mode words");
}

static void test_timing(void)
{
    a2k_pat_t p;
    long t;
    int ok = 1;
    a2k_pat_parse("3:250:250:5000", &p);
    /* cycle = 3 * 500 + 5000 = 6500 ms: lit [0,250) [500,750) [1000,1250), dark until 6500, then again */
    for (t = 0; t < 13000; t++) {
        long m = t % 6500;
        int want = m < 1500 && m % 500 < 250;
        if (a2k_pat_level(&p, t) != want)
            ok = 0;
    }
    CHECK(ok, "3:250:250:5000: 3 blinks of 250/250, then 5 s dark, repeating (every ms over two cycles)");
    CHECK(a2k_pat_level(&p, 0) == 1 && a2k_pat_level(&p, 249) == 1 && a2k_pat_level(&p, 250) == 0 && a2k_pat_level(&p, 500) == 1 &&
              a2k_pat_level(&p, 1250) == 0 && a2k_pat_level(&p, 6499) == 0 && a2k_pat_level(&p, 6500) == 1,
          "edges");
    a2k_pat_parse("1:250:250:0", &p);
    CHECK(a2k_pat_level(&p, 0) == 1 && a2k_pat_level(&p, 250) == 0 && a2k_pat_level(&p, 500) == 1, "pause 0: plain blinking");
}

static void test_priority(void)
{
    a2k_leds_in_t in;
    memset(&in, 0, sizeof in);
    CHECK(a2k_leds_pick(&in) == A2K_RULE_IDLE, "nothing true: idle");
    in.night = 1;
    CHECK(a2k_leds_pick(&in) == A2K_RULE_NIGHT, "night");
    in.stream = 1;
    CHECK(a2k_leds_pick(&in) == A2K_RULE_STREAM, "stream beats night");
    in.person = 1;
    CHECK(a2k_leds_pick(&in) == A2K_RULE_PERSON, "person beats stream");
    in.nowifi = 1;
    CHECK(a2k_leds_pick(&in) == A2K_RULE_NOWIFI, "no Wi-Fi beats person");
    in.boot = 1;
    CHECK(a2k_leds_pick(&in) == A2K_RULE_BOOT, "boot beats everything");
    CHECK(!strcmp(a2k_rule_key(A2K_RULE_BOOT), "led_boot") && !strcmp(a2k_rule_key(A2K_RULE_IDLE), "led_idle") &&
              !strcmp(a2k_rule_default(A2K_RULE_BOOT), "1:100:100:0") && !strcmp(a2k_rule_default(A2K_RULE_NOWIFI), "1:250:250:0") &&
              !strcmp(a2k_rule_default(A2K_RULE_IDLE), "on"),
          "rule keys and defaults");
}

static void test_engine(void)
{
    a2k_leds_t l;
    char t[A2K_PAT_MAX];
    long now;
    memset(&g_in, 0, sizeof g_in);
    lvl[0] = lvl[1] = -1;
    writes[0] = writes[1] = 0;
    CHECK(a2k_leds_init(&l, &a2k_light_desc, &fake_drv, &fake_ops, 0) == 0, "engine init on the 2K descriptor (blue)");
    a2k_leds_set_mode(&l, A2K_LEDS_ON);
    a2k_leds_tick(&l, 0);
    a2k_leds_tick(&l, 100);
    CHECK(writes[1] == 0 && writes[0] == 0, "mode on, never taken: the LED is not written (stock state kept)");
    a2k_leds_set_mode(&l, A2K_LEDS_AUTO);
    a2k_leds_tick(&l, 1000); /* wifi up at once: boot ends, idle = on */
    CHECK(writes[1] == 0, "auto, all defaults, Wi-Fi up: still not written");
    /* no Wi-Fi: 1:250:250:0, after the boot phase */
    g_in.nowifi = 1;
    a2k_leds_tick(&l, 30000);
    CHECK(lvl[1] == 1 && a2k_leds_last_rule(&l) == A2K_RULE_NOWIFI, "no Wi-Fi: blinking starts lit");
    a2k_leds_tick(&l, 30250);
    CHECK(lvl[1] == 0, "no Wi-Fi: dark at +250 ms");
    a2k_leds_tick(&l, 30500);
    CHECK(lvl[1] == 1, "no Wi-Fi: lit at +500 ms");
    g_in.nowifi = 0;
    a2k_leds_tick(&l, 30600);
    CHECK(lvl[1] == 1 && a2k_leds_last_rule(&l) == A2K_RULE_IDLE, "Wi-Fi back: idle on");
    /* a user pattern on person: 3:250:250:5000 over the fake clock */
    CHECK(a2k_leds_set_pattern(&l, A2K_RULE_PERSON, "3:250:250:5000") == 0, "set the person pattern");
    CHECK(a2k_leds_set_pattern(&l, A2K_RULE_PERSON, "3:10:250:5000") == -1, "an invalid pattern is refused");
    a2k_leds_get_pattern(&l, A2K_RULE_PERSON, t, sizeof t);
    CHECK(!strcmp(t, "3:250:250:5000"), "the refused pattern left the old one");
    g_in.person = 1;
    now = 40000;
    a2k_leds_tick(&l, now);
    CHECK(lvl[1] == 1, "person: lit at 0");
    a2k_leds_tick(&l, now + 250);
    CHECK(lvl[1] == 0, "person: dark at 250");
    a2k_leds_tick(&l, now + 1000);
    CHECK(lvl[1] == 1, "person: third blink lit at 1000");
    a2k_leds_tick(&l, now + 1250);
    CHECK(lvl[1] == 0, "person: dark after the third blink");
    a2k_leds_tick(&l, now + 3000);
    a2k_leds_tick(&l, now + 6000);
    CHECK(lvl[1] == 0, "person: still in the 5 s pause at +6000");
    a2k_leds_tick(&l, now + 6500);
    CHECK(lvl[1] == 1, "person: next cycle lit at +6500");
    g_in.stream = 1;
    g_in.person = 0;
    a2k_leds_tick(&l, now + 7000);
    CHECK(a2k_leds_last_rule(&l) == A2K_RULE_STREAM && lvl[1] == 1, "person ends, stream plays: stream rule (on)");
    a2k_leds_set_mode(&l, A2K_LEDS_OFF);
    a2k_leds_tick(&l, now + 7100);
    CHECK(lvl[1] == 0 && a2k_leds_last_rule(&l) == -1, "forced off: dark, rules ignored");
    g_in.nowifi = 1;
    a2k_leds_tick(&l, now + 9000);
    CHECK(lvl[1] == 0, "forced off stays dark even without Wi-Fi");
    a2k_leds_set_mode(&l, A2K_LEDS_ON);
    a2k_leds_tick(&l, now + 9100);
    CHECK(lvl[1] == 1, "forced on: lit");
    a2k_leds_set_mode(&l, A2K_LEDS_OFF);
    a2k_leds_tick(&l, now + 9200);
    a2k_leds_release(&l);
    CHECK(lvl[1] == 1 && lvl[0] == -1, "release: back to on (stock), the IR emitter never touched");
    CHECK(a2k_leds_set_mode(&l, 7) == -1, "bad mode refused");
    a2k_leds_destroy(&l);
}

static void test_boot(void)
{
    a2k_leds_t l;
    memset(&g_in, 0, sizeof g_in);
    lvl[1] = -1;
    a2k_leds_init(&l, &a2k_light_desc, &fake_drv, &fake_ops, 1000);
    g_in.nowifi = 1;
    a2k_leds_tick(&l, 1000);
    CHECK(a2k_leds_last_rule(&l) == A2K_RULE_BOOT && lvl[1] == 1, "boot rule first (1:100:100:0)");
    a2k_leds_tick(&l, 1100);
    CHECK(lvl[1] == 0, "boot: 100 ms blinks");
    a2k_leds_tick(&l, 1000 + A2K_LEDS_BOOT_MS - 1);
    CHECK(a2k_leds_last_rule(&l) == A2K_RULE_BOOT, "boot lasts 20 s without Wi-Fi");
    a2k_leds_tick(&l, 1000 + A2K_LEDS_BOOT_MS);
    CHECK(a2k_leds_last_rule(&l) == A2K_RULE_NOWIFI, "then the no-Wi-Fi rule");
    a2k_leds_destroy(&l);
    a2k_leds_init(&l, &a2k_light_desc, &fake_drv, &fake_ops, 0);
    g_in.nowifi = 1;
    a2k_leds_tick(&l, 0);
    g_in.nowifi = 0;
    a2k_leds_tick(&l, 3000);
    g_in.nowifi = 1;
    a2k_leds_tick(&l, 5000);
    CHECK(a2k_leds_last_rule(&l) == A2K_RULE_NOWIFI, "boot ends for good once the Wi-Fi was up");
    a2k_leds_destroy(&l);
}

static void test_route(void)
{
    static const char R1[] = "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                             "wlan0\t000000E0\t00000000\t0001\t0\t0\t0\t000000F0\t0\t0\t0\n";
    static const char R2[] = "Iface\tDestination\tGateway \tFlags\n"
                             "wlan0\t00000000\t0100A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
                             "wlan0\t0000A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n";
    CHECK(hal_sys_parse_route(R1, "wlan0") == 0, "route: only the multicast route is not an IPv4 address");
    CHECK(hal_sys_parse_route(R2, "wlan0") == 1 && hal_sys_parse_route(R2, "eth0") == 0 && hal_sys_parse_route("", "wlan0") == 0, "route: default + connected route = up");
    CHECK(hal_sys_net_ipv4("/nonexistent/rot2k", "wlan0") == 0, "route: unreadable = no Wi-Fi");
}

static void test_mqtt(void)
{
    a2k_mqtt_ids_t ids;
    a2k_mqtt_cfg_t cfg;
    char topic[200], doc[1024], val[A2K_LED_VAL], f[100];
    struct a2k_mqtt_det d;
    int e = -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.discovery = 1;
    CHECK(a2k_mqtt_ids_init(&ids, &cfg, "cam", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_LED0, topic, sizeof topic, doc, sizeof doc) == -1, "no LED entity unless ids.leds");
    ids.leds = 1;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_LED0, topic, sizeof topic, doc, sizeof doc) > 0 &&
              !strcmp(topic, "homeassistant/select/akam_aabbccddeeff/status_led/config"),
          "Status LED: select discovery topic");
    CHECK(strstr(doc, "\"name\":\"Status LED\"") && strstr(doc, "\"options\":[\"on\",\"off\",\"auto\"]") && strstr(doc, "/status_led/set\"") &&
              strstr(doc, "/status_led/state\"") && strstr(doc, "\"entity_category\":\"config\""),
          "Status LED: name, options on/off/auto, command and state topics");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_LED0 + 1, topic, sizeof topic, doc, sizeof doc) > 0 && strstr(topic, "/select/") && strstr(topic, "/ir_led/config") &&
              strstr(doc, "\"name\":\"IR LED\"") && strstr(doc, "\"options\":[\"auto\",\"off\"]"),
          "IR LED: select auto/off");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_LED0 + 4, topic, sizeof topic, doc, sizeof doc) > 0 && strstr(topic, "/text/") && strstr(topic, "/led_person/config") &&
              strstr(doc, "/led_person/set\"") && strstr(doc, "\"max\":31"),
          "a rule: text entity with a command topic");
    for (int k = 0; k < A2K_LED_N; k++)
        if (a2k_mqtt_discovery(&ids, A2K_E_LED0 + k, topic, sizeof topic, doc, sizeof doc) <= 0)
            e = -2;
    CHECK(e == -2 ? 0 : 1, "all eight discovery documents build");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_LED0 + A2K_LED_N, topic, sizeof topic, doc, sizeof doc) == -1, "beyond the table: nothing");
    memset(&d, 0, sizeof d);
    CHECK(!a2k_mqtt_state(&ids, A2K_E_LED0, &d, -1, topic, sizeof topic, val, sizeof val), "no state before a read");
    d.led_valid = 1;
    snprintf(d.led[0], A2K_LED_VAL, "auto");
    snprintf(d.led[2], A2K_LED_VAL, "3:250:250:5000");
    CHECK(a2k_mqtt_state(&ids, A2K_E_LED0, &d, -1, topic, sizeof topic, val, sizeof val) && !strcmp(val, "auto") && !strcmp(topic, "akam/cam/status_led/state"),
          "state: select value on BASE/status_led/state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_LED0 + 2, &d, -1, topic, sizeof topic, val, sizeof val) && !strcmp(val, "3:250:250:5000") && !strcmp(topic, "akam/cam/led_boot/state"),
          "state: pattern text");
    CHECK(!a2k_mqtt_state(&ids, A2K_E_LED0 + 1, &d, -1, topic, sizeof topic, val, sizeof val), "state: an unread control publishes nothing");
    CHECK(a2k_mqtt_led_filter(&ids, f, sizeof f) == 0 && !strcmp(f, "akam/cam/+/set"), "subscription filter BASE/+/set");
#define CMD(t, p) a2k_mqtt_led_command(&ids, t, strlen(t), (const uint8_t *)(p), strlen(p), &e, val, sizeof val)
    CHECK(CMD("akam/cam/status_led/set", "off") == 1 && e == 0 && !strcmp(val, "off"), "command: status_led off");
    CHECK(CMD("akam/cam/status_led/set", "auto") == 1 && CMD("akam/cam/status_led/set", "on") == 1, "command: status_led auto, on");
    CHECK(CMD("akam/cam/status_led/set", "blink") == 0 && CMD("akam/cam/status_led/set", "ON") == 0 && CMD("akam/cam/status_led/set", "of") == 0 &&
              CMD("akam/cam/status_led/set", "on,off") == 0 && CMD("akam/cam/status_led/set", "") == 0,
          "command: only the exact options");
    CHECK(CMD("akam/cam/ir_led/set", "off") == 1 && e == 1 && CMD("akam/cam/ir_led/set", "auto") == 1 && CMD("akam/cam/ir_led/set", "on") == 0,
          "command: ir_led auto / off, never a forced on");
    CHECK(CMD("akam/cam/led_person/set", "3:250:250:5000") == 1 && e == 4 && !strcmp(val, "3:250:250:5000"), "command: a pattern text");
    CHECK(CMD("akam/cam/led_idle/set", "on") == 1 && e == 7, "command: led_idle");
    CHECK(CMD("akam/cam/led_person/set", "1;rm -x") == 0 && CMD("akam/cam/led_person/set", "A") == 0 && CMD("akam/cam/led_person/set", "1 2") == 0,
          "command: characters outside [0-9a-z:] refused");
    CHECK(CMD("akam/cam/led_person/set", "1111111111111111111111111111111:") == 0, "command: over 31 bytes refused");
    CHECK(CMD("akam/cam/night/set", "on") == 0 && CMD("akam/cam/person/set", "off") == 0 && CMD("akam/other/status_led/set", "on") == 0 &&
              CMD("akam/cam/status_led/state", "on") == 0 && CMD("akam/cam/status_led/set/x", "on") == 0 && CMD("akam/cam/status_led", "on") == 0,
          "command: other topics are never obeyed");
    ids.leds = 0;
    CHECK(CMD("akam/cam/status_led/set", "on") == 0, "command: nothing is obeyed unless the controls are enabled");
}

int main(void)
{
    test_parse();
    test_timing();
    test_priority();
    test_engine();
    test_boot();
    test_route();
    test_mqtt();
    printf("rot2k led unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
