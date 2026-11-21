/* Host unit test of hal_sys (pure /proc parsers, sampler on fixture files) and of the sys MQTT sensors of the 2K.
 * Prints "rot2k sys unit: N failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mqtt_cfg.h"
#include "hal_sys.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static const char STAT1[] = "cpu  100 0 100 800 0 0 0 0 0 0\ncpu0 100 0 100 800 0 0 0 0 0 0\nintr 1\n";
static const char STAT2[] = "cpu  150 0 150 900 50 0 0 0 0 0\ncpu0 1 2 3 4\n";
static const char MEM[] = "MemTotal:       60000 kB\nMemFree:         4000 kB\nMemAvailable:   21000 kB\nBuffers: 1 kB\n";
static const char MEM_OLD[] = "MemTotal: 60000 kB\nMemFree: 4000 kB\nBuffers: 1000 kB\nCached: 5000 kB\n";
static const char WL[] = "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
                         " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n"
                         " wlan0: 0000   48.  -62.  -256        0      0      0      0      0        0\n";

static void put(const char *root, const char *rel, const char *txt)
{
    char p[200];
    FILE *f;
    snprintf(p, sizeof p, "%s%s", root, rel);
    f = fopen(p, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
}

static void test_parsers(void)
{
    hal_sys_t st = { 0, 0, 0 };
    unsigned long long t, b;
    long a, m, u;
    int x, dbm = 0, q = 0;
    CHECK(hal_sys_parse_stat(STAT1, &t, &b) == 0 && t == 1000 && b == 200, "stat: total and busy (idle excluded)");
    CHECK(hal_sys_parse_stat("cpu0 1 2 3 4", &t, &b) == -1 && hal_sys_parse_stat("cpu  1 2", &t, &b) == -1, "stat: bad lines refused");
    CHECK(hal_sys_cpu_pct(&st, STAT1) == HAL_SYS_NA, "cpu: first sample has no delta");
    CHECK(hal_sys_cpu_pct(&st, STAT2) == 40, "cpu: 100 busy of 250 = 40 %");
    CHECK(hal_sys_cpu_pct(&st, STAT2) == HAL_SYS_NA, "cpu: no tick elapsed = NA");
    CHECK(hal_sys_cpu_pct(&st, STAT1) == HAL_SYS_NA, "cpu: counters going back = NA");
    CHECK(hal_sys_parse_load1("0.52 0.40 0.31 1/60 123\n", &x) == 0 && x == 52, "load1 0.52");
    CHECK(hal_sys_parse_load1("12.5 1 1", &x) == 0 && x == 1250, "load1 12.5");
    CHECK(hal_sys_parse_load1("x", &x) == -1, "load1 junk refused");
    CHECK(hal_sys_parse_mem(MEM, &m, &a) == 0 && m == 60000 && a == 21000, "mem: MemAvailable");
    CHECK(hal_sys_parse_mem(MEM_OLD, &m, &a) == 0 && a == 10000, "mem: fallback Free+Buffers+Cached");
    CHECK(hal_sys_parse_mem("MemTotal: 5 kB\n", &m, &a) == -1, "mem: nothing available = refused");
    CHECK(hal_sys_parse_uptime("12345.67 9999.00\n", &u) == 0 && u == 12345 && hal_sys_parse_uptime("", &u) == -1, "uptime");
    CHECK(hal_sys_parse_wireless(WL, "wlan0", &dbm, &q) == 0 && dbm == -62 && q == 48, "wireless: the 2K line, -62 dBm, quality 48");
    CHECK(hal_sys_parse_wireless(WL, "wlan1", &dbm, &q) == -1, "wireless: other interface = absent");
    CHECK(hal_sys_parse_wireless(" wlan0: 0000 48. 62. 0\n", "wlan0", &dbm, &q) == -1, "wireless: positive level is not dBm");
    CHECK(hal_sys_parse_wireless(" wlan0: 0000 48 -70 0\n", "wlan0", &dbm, &q) == 0 && dbm == -70, "wireless: no trailing dot");
}

static void test_sample(void)
{
    char root[] = "/tmp/rot2k_sys_XXXXXX", p[200];
    hal_sys_t st = { 0, 0, 0 };
    hal_sys_sample_t s;
    if (!mkdtemp(root)) {
        CHECK(0, "mkdtemp");
        return;
    }
    snprintf(p, sizeof p, "%s/proc", root);
    mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/proc/net", root);
    mkdir(p, 0700);
    put(root, "/proc/stat", STAT1);
    put(root, "/proc/loadavg", "0.52 0.40 0.31 1/60 123\n");
    put(root, "/proc/meminfo", MEM);
    put(root, "/proc/uptime", "300.10 200.00\n");
    put(root, "/proc/net/wireless", WL);
    hal_sys_sample(&st, root, "wlan0", &s);
    CHECK(s.cpu_pct == HAL_SYS_NA && s.load1_x100 == 52 && s.mem_avail_kb == 21000 && s.mem_total_kb == 60000 &&
          s.uptime_s == 300 && s.rssi_dbm == -62 && s.link_quality == 48, "sample 1: all fields, cpu NA");
    CHECK(s.temp_c10 == HAL_SYS_NA, "temperature is absent on this board");
    put(root, "/proc/stat", STAT2);
    snprintf(p, sizeof p, "%s/proc/net/wireless", root);
    unlink(p);
    hal_sys_sample(&st, root, "wlan0", &s);
    CHECK(s.cpu_pct == 40 && s.rssi_dbm == HAL_SYS_NA && s.link_quality == HAL_SYS_NA, "sample 2: cpu delta, no wireless file = NA");
    hal_sys_sample(&st, "/nonexistent", "wlan0", &s);
    CHECK(s.cpu_pct == HAL_SYS_NA && s.mem_avail_kb == HAL_SYS_NA && s.uptime_s == HAL_SYS_NA && s.load1_x100 == HAL_SYS_NA,
          "no /proc at all: everything NA");
    snprintf(p, sizeof p, "rm -rf %s", root);
    if (system(p)) {
    }
}

static void test_mqtt(void)
{
    a2k_mqtt_cfg_t cfg;
    a2k_mqtt_ids_t ids;
    struct a2k_mqtt_det d;
    char t[160], v[64], doc[1024];
    memset(&cfg, 0, sizeof cfg);
    memset(&d, 0, sizeof d);
    snprintf(cfg.base, sizeof cfg.base, "akam/test");
    snprintf(cfg.prefix, sizeof cfg.prefix, "ha");
    CHECK(a2k_mqtt_ids_init(&ids, &cfg, "cam", "aa:bb:cc:dd:ee:ff", "0.1.0") == 0, "ids");
    CHECK(A2K_E_SYS0 == A2K_E_NIGHT + 1, "sys entities follow the night index");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_SYS0, t, sizeof t, doc, sizeof doc) == -1, "sys off: no document");
    ids.sys = 1;
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_SYS0 + 2, t, sizeof t, doc, sizeof doc) > 0 &&
          !strcmp(t, "ha/sensor/akam_aabbccddeeff/sys_wifi_rssi/config"), "rssi discovery topic");
    CHECK(strstr(doc, "\"device_class\":\"signal_strength\"") && strstr(doc, "\"unit_of_measurement\":\"dBm\"") &&
          strstr(doc, "\"entity_category\":\"diagnostic\"") && strstr(doc, "\"state_class\":\"measurement\"") &&
          strstr(doc, "\"state_topic\":\"akam/test/sys_wifi_rssi/state\"") && !strstr(doc, "command_topic"),
          "rssi document: signal_strength, dBm, diagnostic, no command topic");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_SYS0 + 3, t, sizeof t, doc, sizeof doc) > 0 && !strstr(doc, "state_class"),
          "uptime has no state_class measurement");
    CHECK(a2k_mqtt_discovery(&ids, A2K_E_SYS0, t, sizeof t, doc, 40) == -1, "tiny buffer is refused, not truncated");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0, &d, -1, t, sizeof t, v, sizeof v) == 0, "no sample yet: no state");
    d.sys_valid = 1;
    d.sys.cpu_pct = 37;
    d.sys.load1_x100 = 105;
    d.sys.mem_avail_kb = 21000;
    d.sys.uptime_s = 300;
    d.sys.rssi_dbm = -62;
    d.sys.link_quality = 48;
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(t, "akam/test/sys_cpu/state") && !strcmp(v, "37"), "cpu state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 1, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "21000"), "mem state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 2, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "-62"), "rssi state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 3, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "300"), "uptime state");
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 4, &d, -1, t, sizeof t, v, sizeof v) == 1 && !strcmp(v, "1.05"), "load state");
    CHECK(a2k_mqtt_attrs(&ids, A2K_E_SYS0 + 2, &d, "", 554, t, sizeof t, doc, sizeof doc) == 1 &&
          !strcmp(t, "akam/test/sys_wifi_rssi/attributes") && !strcmp(doc, "{\"link_quality\":48}"), "rssi attributes: link quality (valid JSON)");
    d.sys.rssi_dbm = HAL_SYS_NA;
    d.sys.cpu_pct = HAL_SYS_NA;
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 2, &d, -1, t, sizeof t, v, sizeof v) == 0 &&
          a2k_mqtt_state(&ids, A2K_E_SYS0, &d, -1, t, sizeof t, v, sizeof v) == 0, "an absent value is not published");
    ids.sys = 0;
    CHECK(a2k_mqtt_state(&ids, A2K_E_SYS0 + 1, &d, -1, t, sizeof t, v, sizeof v) == 0, "sys off: no state");
}

int main(void)
{
    test_parsers();
    test_sample();
    test_mqtt();
    printf("rot2k sys unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
