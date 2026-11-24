/* Host test of firmware/lsc-indoor-1080p-ak3918/settings.c, the settings registry of the 1080p (convergence P7): the table validates, every row reads
 * and writes through the module it is built on (here: stubs that behave like the 8091 code), the registry rules (strict canonical
 * text, ranges, options, read-only, all-or-nothing PATCH, cross-field QP rule), the 8091 adapter (old and new names, replies,
 * persistence flag), the LED patterns on the real engine (leds.c) and the ONVIF / MQTT switches. The schema must fit the API reply
 * buffer. No camera. */
#include "ak1080_fakes.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* the fake modules are in ak1080_fakes.h */
/* ------------------------------------------------------------------ helpers */
static const char *get(const char *key)
{
    static char v[STREG_VAL_MAX];
    int n = streg_get(ak1080_settings_reg(), ST_SRC_HTTP, key, v, sizeof v);
    if (n < 0) snprintf(v, sizeof v, "<%s>", streg_err_name(n));
    return v;
}

static const char *ctl_set(const char *key, const char *val, int persist)
{
    static char m[256];
    int rc = ak1080_settings_ctl_set(key, val, persist, m, sizeof m);
    if (rc < 0) return "(not a row)";
    return m;
}

static const char *slurp(const char *rel)
{
    static char b[600];
    char p[200];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    b[0] = '\0';
    FILE *f = fopen(p, "r");
    if (!f) return b;
    size_t n = fread(b, 1, sizeof b - 1, f);
    b[n] = '\0';
    fclose(f);
    return b;
}

static void put(const char *rel, const char *s)
{
    char p[200];
    snprintf(p, sizeof p, "%s/%s", g_root, rel);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(s, f);
    fclose(f);
}

/* ------------------------------------------------------------------ tests */
static void test_table(void)
{
    CHECK(ak1080_settings_reg() == NULL, "no registry before init");
    char line[64];
    CHECK(ak1080_settings_ctl_get("led.mode", line, sizeof line) == -1, "not ready: not a row");
    CHECK(ak1080_settings_init() == 0, "the table validates");
    CHECK(ak1080_settings_init() == 0, "init is idempotent");
    const streg_t *r = ak1080_settings_reg();
    CHECK(r && r->n == 56, "row count %zu", r ? r->n : 0);
    char *schema = malloc(12288);
    int n = streg_json_schema(r, schema, 12288);
    CHECK(n > 0 && n < 12000, "the schema fits the API reply buffer (%d bytes)", n);
    CHECK(strstr(schema, "\"led.mode\"") && strstr(schema, "\"video0.min_qp\"") && !strstr(schema, "ptz.") && !strstr(schema, "track."),
          "schema has the 1080p rows and no PTZ or tracking row");
    CHECK(strstr(schema, "\"restart_component\"") != NULL, "video rows say they apply at the next start");
    free(schema);
    /* every writable row has a setter and a default that its own parser accepts: streg_init checked it; the names are the registry's */
    const char *must[] = { "led.mode", "led.stream", "led.boot", "led.nowifi", "led.night", "led.idle", "night.ir_led", "night.mode",
                           "image.brightness", "image.denoise", "video0.kbps", "video0.fps", "video0.gop", "video0.min_qp",
                           "video0.max_qp", "onvif.enabled", "mqtt.enabled", "sys.cpu_pct", "net.wifi_rssi", "rtsp.clients", "night.active",
                           "api.keys" };
    for (unsigned i = 0; i < sizeof must / sizeof must[0]; i++) CHECK(streg_find(r, must[i]) != NULL, "row %s", must[i]);
    const char *absent[] = { "ptz.pan", "track.enabled", "detect.person", "net.wifi_band", "button.last_gesture", "zones.1" };
    for (unsigned i = 0; i < sizeof absent / sizeof absent[0]; i++) CHECK(streg_find(r, absent[i]) == NULL, "no row %s", absent[i]);
    for (size_t i = 0; i < r->n; i++) {
        const st_def_t *d = &r->defs[i];
        CHECK(!d->persist && !d->secret, "%s: no registry file, no secret", d->key);
        if (!(d->access & ST_ACC_WRITE)) CHECK(d->get != NULL, "%s: a state row has a getter", d->key);
    }
}

static void test_state(void)
{
    CHECK(!strcmp(get("sys.cpu_pct"), "37") && !strcmp(get("sys.load1_x100"), "12") && !strcmp(get("sys.mem_avail_kb"), "21000") &&
              !strcmp(get("sys.uptime_s"), "86400") && !strcmp(get("net.wifi_lq"), "70"),
          "sys rows come from ctl_sys");
    CHECK(!strcmp(get("net.wifi_rssi"), "-1000000"), "unknown becomes -1000000, as on the 2K (got %s)", get("net.wifi_rssi"));
    CHECK(!strcmp(get("video0.fps_x10"), "-1000000") && !strcmp(get("video0.bitrate_kbps"), "0"), "stream measures");
    CHECK(!strcmp(get("rtsp.clients"), "1") && !strcmp(get("rtsp.auth"), "on") && !strcmp(get("rtsp.skipped_frames"), "2"), "rtsp rows");
    CHECK(!strcmp(get("sys.time_set"), "1") && !strcmp(get("sys.version"), "b1-test") && !strcmp(get("net.hostname"), "akam-test"), "identity rows");
    CHECK(!strcmp(get("night.active"), "0"), "night.active day");
    g_night = 1;
    CHECK(!strcmp(get("night.active"), "1"), "night.active night");
    g_night = 0;
    CHECK(!strcmp(get("api.keys"), "0"), "api.keys");
    ak1080_settings_set_keys(3);
    CHECK(!strcmp(get("api.keys"), "3"), "api.keys follows");
    ak1080_ntp_path = "/nonexistent/ntp";
    CHECK(!strcmp(get("net.ntp"), ""), "no NTP file: empty");
    CHECK(!strcmp(get("video0.cfg_pending"), "1"), "cfg_pending");
    CHECK(!strcmp(get("onvif.state"), "off") && !strcmp(get("mqtt.state"), "off"), "service states");
    /* a state row cannot be written */
    char m[100];
    CHECK(ak1080_settings_ctl_set("sys.cpu_pct", "5", 1, m, sizeof m) == 1 && !strcmp(m, "ERR sys.cpu_pct is read-only"), "read-only: '%s'", m);
}

static void test_settings(void)
{
    char line[100];
    CHECK(!strcmp(get("led.mode"), "on") && !strcmp(get("night.mode"), "auto") && !strcmp(get("image.sharpness"), "0"), "initial values");
    CHECK(!strcmp(get("led.stream"), "1:1000:1000:0") && !strcmp(get("led.idle"), "on"), "pattern defaults from the engine");
    /* the 8091 adapter */
    CHECK(!strcmp(ctl_set("led.mode", "auto", 1), "OK led.mode=auto") && !strcmp(g_led_status, "auto"), "led.mode reaches led.status");
    CHECK(g_last_persist == 1, "a loopback write persists");
    CHECK(!strcmp(ctl_set("image.contrast", "-12", 0), "OK image.contrast=-12 (not saved: stub)") && g_vals[1] == -12, "network write: RAM only, note kept");
    CHECK(g_last_persist == 0, "persist flag 0 reached the module");
    CHECK(!strcmp(ctl_set("onvif.enabled", "1", 0), "ERR onvif.enabled can only be set from the camera") &&
              !strncmp(ctl_set("mqtt.enabled", "1", 0), "ERR mqtt.enabled can only", 25) &&
              !strncmp(ctl_set("led.stream", "on", 0), "ERR led.stream can only", 23),
          "a network (DEBUG) write never reaches a row that has no RAM-only mode");
    CHECK(!strncmp(ctl_set("image.contrast", "51", 1), "ERR invalid image.contrast value 51 (range)", 41) && g_vals[1] == -12, "out of range: nothing applied");
    CHECK(!strncmp(ctl_set("image.contrast", "+5", 1), "ERR invalid image.contrast value +5 (format)", 43), "strict canonical text");
    CHECK(!strncmp(ctl_set("led.mode", "blink", 1), "ERR invalid led.mode value blink (range)", 40), "enum option");
    CHECK(!strcmp(ctl_set("video0.kbps", "2000", 1), "OK video0.kbps=2000 (restart to apply)") && g_video[0] == 2000, "video: applies at the next start");
    CHECK(!strncmp(ctl_set("video0.kbps", "400", 1), "ERR invalid video0.kbps value 400 (range)", 40), "kbps range");
    CHECK(!strncmp(ctl_set("video0.fps", "30", 1), "ERR invalid video0.fps value 30 (range)", 38), "fps option (30 is refused, see video_cfg.h)");
    CHECK(!strcmp(ctl_set("video0.fps", "25", 1), "OK video0.fps=25 (restart to apply)") && g_video[1] == 25, "fps 25");
    CHECK(!strcmp(ctl_set("not.a.row", "1", 1), "(not a row)"), "unknown key is not ours");
    CHECK(ak1080_settings_ctl_get("not.a.row", line, sizeof line) == -1, "get: not a row");
    CHECK(ak1080_settings_ctl_get("image.contrast", line, sizeof line) == 0 && !strcmp(line, "image.contrast=-12"), "get: %s", line);
    CHECK(ak1080_settings_ctl_get("video0.gop", line, sizeof line) == 0 && !strcmp(line, "video0.gop=2"), "get video gop: %s", line);
    g_fail_apply = 1;
    CHECK(!strncmp(ctl_set("night.mode", "night", 1), "ERR invalid night.mode value night (setter)", 42), "a module refusal is a setter error");
    g_fail_apply = 0;
    CHECK(!strcmp(ctl_set("night.mode", "night", 1), "OK night.mode=night"), "night.mode");

    /* LED patterns on the real engine */
    CHECK(!strcmp(ctl_set("led.nowifi", "3:250:250:2000", 1), "OK led.nowifi=3:250:250:2000") && !strcmp(get("led.nowifi"), "3:250:250:2000"), "pattern set");
    CHECK(!strncmp(ctl_set("led.nowifi", "1:10:10:0", 1), "ERR invalid led.nowifi value 1:10:10:0 (check)", 45), "pattern floor 50 ms");
    CHECK(!strncmp(ctl_set("led.nowifi", "x", 1), "ERR invalid led.nowifi value x (", 32), "pattern grammar");
    CHECK(!strcmp(slurp("conf/leds"), "status=on\nir_led=auto\nled_nowifi=3:250:250:2000\n"), "pattern saved by the LED module: '%s'", slurp("conf/leds"));
}

static const char *patch(size_t n, const char *const *k, const char *const *v, st_result_t *res, int *http)
{
    static char out[600];
    *http = ak1080_settings_patch(AKEY_WRITE, n, k, v, res);
    int len = streg_json_patch_reply(ak1080_settings_reg(), n, k, v, res, out, sizeof out);
    if (len < 0) snprintf(out, sizeof out, "(reply too big)");
    return out;
}

static void test_patch(void)
{
    st_result_t res[8];
    int http;
    {   /* the QP rule is checked over the whole patch */
        const char *k[] = { "video0.min_qp", "video0.max_qp" }, *v[] = { "45", "50" };
        patch(2, k, v, res, &http);
        CHECK(http == 200 && g_video[3] == 45 && g_video[4] == 50, "min and max raised together (http %d)", http);
    }
    {
        const char *k[] = { "video0.min_qp", "image.contrast" }, *v[] = { "51", "3" };
        int before = g_vals[1];
        const char *o = patch(2, k, v, res, &http);
        CHECK(http == 422 && g_vals[1] == before && g_video[3] == 45, "min_qp 51 with max 50 refuses the whole patch (http %d) %s", http, o);
        CHECK(res[0].err == ST_E_CHECK && res[1].err == ST_E_NOTAPPLIED, "check, not_applied");
    }
    {
        const char *k[] = { "image.brightness", "night.active" }, *v[] = { "5", "1" };
        int before = g_vals[0];
        patch(2, k, v, res, &http);
        CHECK(http == 422 && res[1].err == ST_E_READONLY && g_vals[0] == before, "a read-only row refuses the patch, the valid row is not applied");
    }
    {
        const char *k[] = { "image.brightness", "image.saturation", "led.mode" }, *v[] = { "5", "-5", "off" };
        const char *o = patch(3, k, v, res, &http);
        CHECK(http == 200 && g_vals[0] == 5 && g_vals[2] == -5 && !strcmp(g_led_status, "off"), "three rows in one patch (%s)", o);
        CHECK(g_last_persist == 1, "an authenticated PATCH persists");
        CHECK(strstr(o, "\"led.mode\":{\"ok\":true,\"apply\":\"applied\"") != NULL, "reply says applied: %s", o);
    }
    {
        const char *k[] = { "video0.gop" }, *v[] = { "3" };
        const char *o = patch(1, k, v, res, &http);
        CHECK(http == 200 && strstr(o, "restart_component") != NULL, "video apply mode in the reply: %s", o);
    }
}

static void test_switches(void)
{
    char onvif[200], mqtt[200];
    snprintf(onvif, sizeof onvif, "%s/onvif", g_root);
    snprintf(mqtt, sizeof mqtt, "%s/mqtt", g_root);
    onvif_cfg_path = onvif;
    mqtt_cfg_path = mqtt;
    CHECK(!strcmp(get("onvif.enabled"), "0") && !strcmp(get("mqtt.enabled"), "0"), "both off without a file");

    /* ONVIF: a password is set */
    CHECK(!strcmp(ctl_set("onvif.enabled", "1", 1), "OK onvif.enabled=1"), "enable");
    CHECK(!strcmp(slurp("onvif"), "enabled=1\nport=80\n"), "flag file: '%s'", slurp("onvif"));
    CHECK(!strcmp(get("onvif.enabled"), "1") && !strcmp(get("onvif.state"), "on:80"), "enabled and running (%s)", get("onvif.state"));
    int reloads = g_onvif_reloads;
    CHECK(!strcmp(ctl_set("onvif.enabled", "1", 1), "OK onvif.enabled=1") && g_onvif_reloads == reloads, "same value: no write, no reload");
    CHECK(!strcmp(ctl_set("onvif.enabled", "0", 1), "OK onvif.enabled=0") && !strcmp(slurp("onvif"), "enabled=0\nport=80\n"), "disable keeps the port");
    CHECK(!strcmp(get("onvif.state"), "off"), "stopped");
    put("onvif", "enabled=0\nport=8080\n");
    CHECK(!strncmp(ctl_set("onvif.enabled", "1", 1), "OK", 2) && !strcmp(slurp("onvif"), "enabled=1\nport=8080\n"), "the saved port is kept");
    CHECK(!strcmp(get("onvif.state"), "on:8080"), "on the saved port");
    /* no stream password: refused, the old file is back (as akam_onvif.sh refuses) */
    ctl_set("onvif.enabled", "0", 1);
    g_have_password = 0;
    CHECK(!strncmp(ctl_set("onvif.enabled", "1", 1), "ERR invalid onvif.enabled value 1 (setter)", 42), "no password: refused");
    CHECK(!strcmp(slurp("onvif"), "enabled=0\nport=8080\n") && !strcmp(get("onvif.state"), "off"), "the previous flag is back ('%s', %s)", slurp("onvif"), get("onvif.state"));
    unlink(onvif);
    CHECK(!strncmp(ctl_set("onvif.enabled", "1", 1), "ERR", 3) && access(onvif, F_OK) != 0, "no file before: no file after a refusal");
    g_have_password = 1;

    /* MQTT: only flips enabled= in an existing valid file */
    CHECK(!strncmp(ctl_set("mqtt.enabled", "1", 1), "ERR invalid mqtt.enabled value 1 (setter)", 42), "no file: refused (configure the broker first)");
    CHECK(access(mqtt, F_OK) != 0, "nothing created");
    put("mqtt", "enabled=0\nhost=broker.example\nport=1883\n");
    CHECK(!strcmp(ctl_set("mqtt.enabled", "1", 1), "OK mqtt.enabled=1"), "enable");
    CHECK(!strcmp(slurp("mqtt"), "enabled=1\nhost=broker.example\nport=1883\n"), "only enabled= changed: '%s'", slurp("mqtt"));
    CHECK(!strcmp(get("mqtt.enabled"), "1") && !strcmp(get("mqtt.state"), "connecting"), "running");
    put("mqtt", "host=broker.example\nuser=cam\n");
    CHECK(!strcmp(ctl_set("mqtt.enabled", "1", 1), "OK mqtt.enabled=1") && !strcmp(slurp("mqtt"), "host=broker.example\nuser=cam\nenabled=1\n"),
          "a missing enabled= line is added: '%s'", slurp("mqtt"));
    ctl_set("mqtt.enabled", "0", 1);
    g_mqtt_fail_start = 1;
    CHECK(!strncmp(ctl_set("mqtt.enabled", "1", 1), "ERR", 3) && strstr(slurp("mqtt"), "enabled=0") && !strcmp(get("mqtt.state"), "off"),
          "the thread cannot start: the old file is back ('%s', %s)", slurp("mqtt"), get("mqtt.state"));
    g_mqtt_fail_start = 0;
    put("mqtt", "garbage only\n");
    CHECK(!strncmp(ctl_set("mqtt.enabled", "1", 1), "ERR", 3) && !strcmp(slurp("mqtt"), "garbage only\n"), "an invalid file is never rewritten");

    /* NTP: the servers of the file */
    char ntp[200];
    snprintf(ntp, sizeof ntp, "%s/ntp", g_root);
    ak1080_ntp_path = ntp;
    put("ntp", "server=pool.ntp.org\nserver=192.0.2.1\nfoo=bar\nserver=\n");
    CHECK(!strcmp(get("net.ntp"), "pool.ntp.org 192.0.2.1"), "net.ntp lists the servers: '%s'", get("net.ntp"));
}

/* The rows the assistant needs now that the text port is closed to the network: audio, white balance, the LAN opt-in, their state. */
static void test_audio_lan(void)
{
    const streg_t *r = ak1080_settings_reg();
    const char *rows[] = { "audio.out_volume", "audio.out_enable", "audio.talk_enable", "audio.mic_enable", "img.wb", "control.lan" };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) CHECK(streg_find(r, rows[i]) && (streg_find(r, rows[i])->access & ST_ACC_WRITE), "writable row %s", rows[i]);
    CHECK(!strcmp(get("net.ssid"), "my net") && streg_find(r, "net.mac") == NULL && streg_find(r, "net.ip") == NULL, "net.ssid is a state row, the MAC and the address are not");
    const char *state[] = { "audio.out_state", "audio.out_underruns", "audio.out_drops", "led.state", "net.wifi_rate" };
    for (unsigned i = 0; i < sizeof state / sizeof state[0]; i++) CHECK(streg_find(r, state[i]) && !(streg_find(r, state[i])->access & ST_ACC_WRITE), "read-only row %s", state[i]);
    CHECK(!strcmp(get("audio.out_volume"), "100") && !strcmp(get("audio.out_enable"), "1") && !strcmp(get("audio.mic_enable"), "0"), "audio values come from the module");
    CHECK(!strcmp(get("audio.out_state"), "idle") && !strcmp(get("audio.out_underruns"), "3") && !strcmp(get("led.state"), "solid"), "audio and LED state");
    CHECK(!strcmp(get("net.wifi_rate"), "-1000000") || get("net.wifi_rate")[0] == '<', "wifi_rate is a number or unknown");
    CHECK(!strcmp(ctl_set("audio.out_volume", "40", 1), "OK audio.out_volume=40") && g_audio_vol == 40 && !strcmp(get("audio.out_volume"), "40"), "volume");
    CHECK(!strncmp(ctl_set("audio.out_volume", "101", 1), "ERR invalid audio.out_volume", 28), "volume range 0..100 refused by the registry");
    CHECK(!strcmp(ctl_set("img.wb", "manual", 1), "OK img.wb=manual") && !strcmp(get("img.wb"), "manual"), "img.wb");
    CHECK(!strncmp(ctl_set("img.wb", "auto", 1), "ERR invalid img.wb", 18), "img.wb options");
    /* a module that says "restart to apply" makes the write a restart_component one, whatever the row says */
    const char *k[] = { "audio.out_enable" }, *v[] = { "1" };
    st_result_t res[1];
    int http;
    patch(1, k, v, res, &http);
    CHECK(http == 200 && res[0].apply == AP_RESTART_COMPONENT, "out_enable: restart note becomes apply=restart_component (http %d apply %d)", http, res[0].apply);
    v[0] = "0";
    patch(1, k, v, res, &http);
    CHECK(http == 200 && res[0].apply == AP_LIVE, "out_enable off: live");
    /* the opt-in of the text port */
    CHECK(!strcmp(get("control.lan"), "0"), "closed by default");
    k[0] = "control.lan"; v[0] = "1";
    patch(1, k, v, res, &http);
    CHECK(http == 200 && res[0].apply == AP_RESTART_COMPONENT && g_lan == 1 && !strcmp(get("control.lan"), "1"), "control.lan on is saved and applies at the next start");
    v[0] = "0";
    patch(1, k, v, res, &http);
    CHECK(http == 200 && g_lan == 0, "control.lan off");
    v[0] = "2";
    patch(1, k, v, res, &http);
    CHECK(http != 200 && g_lan == 0, "control.lan takes 0 or 1 only");
}

/* A save that fails is told in the PATCH reply ("saved":false), not only in the log: the value is applied but lost at the next reboot. */
static void test_unsaved_is_reported(void)
{
    const char *k[] = { "image.contrast", "led.mode" }, *v[] = { "7", "off" };
    st_result_t res[2];
    int http;
    const char *out = patch(1, k, v, res, &http);
    CHECK(http == 200 && !res[0].unsaved && !strstr(out, "saved"), "a clean save says nothing: %s", out);
    g_fail_save = 1;
    out = patch(2, k, v, res, &http);
    g_fail_save = 0;
    CHECK(http == 200 && res[0].ok && res[0].unsaved && !res[1].unsaved, "only the row whose file failed is marked (http %d)", http);
    CHECK(strstr(out, "\"image.contrast\":{\"ok\":true,\"apply\":\"applied\",\"saved\":false,\"value\":\"7\"}") != NULL &&
              !strstr(strstr(out, "\"led.mode\"") ? strstr(out, "\"led.mode\"") : "", "saved"), "the reply carries saved:false for that key only: %s", out);
    out = patch(1, k, v, res, &http);
    CHECK(!res[0].unsaved && !strstr(out, "saved"), "the flag does not stick to the next patch: %s", out);
}

static void test_orient(void)
{
    char path[160], txt[80];
    FILE *f;
    size_t n;
    snprintf(path, sizeof path, "%s/orient", g_root);
    image_orient_path = path;
    image_orient_hw = fake_orient_hw;
    CHECK(!strcmp(get("image.flip"), "0") && !strcmp(get("image.mirror"), "0") && !strcmp(get("image.ceiling"), "0"), "orientation off at first");
    CHECK(access(path, F_OK) != 0 && g_orient_hw_calls == 0, "nothing sent and no file while it stays off");
    CHECK(!strcmp(ctl_set("image.flip", "0", 1), "OK image.flip=0") && g_orient_hw_calls == 0, "writing the current value sends nothing");
    CHECK(!strcmp(ctl_set("image.flip", "1", 1), "OK image.flip=1") && g_orient_hw_f == 1 && g_orient_hw_m == 0, "flip reaches the ISP call (flip=1, mirror=0)");
    f = fopen(path, "r");
    n = f ? fread(txt, 1, sizeof txt - 1, f) : 0;
    if (f) fclose(f);
    txt[n] = '\0';
    CHECK(!strcmp(txt, "flip=1\nmirror=0\n"), "saved file: '%s'", txt);
    CHECK(!strcmp(ctl_set("image.mirror", "1", 1), "OK image.mirror=1") && g_orient_hw_f == 1 && g_orient_hw_m == 1 && !strcmp(get("image.ceiling"), "1"),
          "mirror on: both on, ceiling reads 1");
    CHECK(!strcmp(ctl_set("image.ceiling", "0", 1), "OK image.ceiling=0") && !strcmp(get("image.flip"), "0") && !strcmp(get("image.mirror"), "0"), "ceiling 0 turns both off");
    CHECK(access(path, F_OK) != 0, "both off: the file is removed");
    CHECK(!strcmp(ctl_set("image.ceiling", "1", 1), "OK image.ceiling=1") && !strcmp(get("image.flip"), "1") && !strcmp(get("image.mirror"), "1"), "ceiling 1 turns both on");
    /* the ISP refuses: nothing changes, nothing is saved */
    g_orient_hw_fail = 1;
    CHECK(!strncmp(ctl_set("image.ceiling", "0", 1), "ERR invalid image.ceiling value 0 (setter)", 41) && !strcmp(get("image.flip"), "1"), "ISP refusal: state kept");
    g_orient_hw_fail = 0;
    CHECK(!strncmp(ctl_set("image.flip", "2", 1), "ERR invalid image.flip value 2 (format)", 39) && !strncmp(ctl_set("image.flip", "on", 1), "ERR invalid image.flip", 22), "only 0 or 1");
    /* a network write in DEBUG mode (persist 0) is refused: the orientation always saves */
    CHECK(!strcmp(ctl_set("image.flip", "0", 0), "ERR image.flip can only be set from the camera"), "network door refused");
    /* the API door applies and saves it too */
    {
        const char *k[] = { "image.flip" }, *v[] = { "0" };
        st_result_t res[1];
        CHECK(ak1080_settings_patch(AKEY_ADMIN, 1, k, v, res) == 200 && !strcmp(get("image.flip"), "0") && !strcmp(get("image.mirror"), "1"), "PATCH flip=0 keeps mirror");
    }
    /* start-up: the saved file is applied once, never rewritten; a bad file means "not flipped" */
    {
        int calls;
        int rf, rm;
        put("orient", "flip=0\nmirror=1\n");
        calls = g_orient_hw_calls;
        CHECK(image_orient_apply_saved() == 0 && g_orient_hw_calls == calls + 1 && g_orient_hw_f == 0 && g_orient_hw_m == 1, "boot applies the saved file");
        image_orient_get(&rf, &rm);
        CHECK(rf == 0 && rm == 1, "boot state");
        put("orient", "flip=1\nflip=0\n");
        calls = g_orient_hw_calls;
        CHECK(image_orient_apply_saved() == 0 && g_orient_hw_calls == calls, "a bad file sends nothing");
        unlink(path);
        CHECK(image_orient_apply_saved() == 0 && g_orient_hw_calls == calls, "no file: nothing to apply");
    }
    /* the day / night switch replays the orientation: nothing while off, the current value while on, no flash write */
    {
        int calls;
        unlink(path);
        image_orient_path = path;
        ctl_set("image.ceiling", "0", 1);
        calls = g_orient_hw_calls;
        image_orient_replay_locked();
        CHECK(g_orient_hw_calls == calls, "replay with both off sends nothing");
        snprintf(path, sizeof path, "%s/orient2", g_root);
        CHECK(!strcmp(ctl_set("image.flip", "1", 1), "OK image.flip=1"), "flip on again");
        calls = g_orient_hw_calls;
        g_orient_hw_f = g_orient_hw_m = -1;
        image_orient_replay_locked();
        CHECK(g_orient_hw_calls == calls + 1 && g_orient_hw_f == 1 && g_orient_hw_m == 0, "replay sends the current orientation (1, 0)");
        {
            char again[160];
            snprintf(again, sizeof again, "%s/orient2", g_root);
            unlink(again);
            image_orient_replay_locked();
            CHECK(access(again, F_OK) != 0, "replay writes no file");
        }
        g_orient_hw_fail = 1;
        image_orient_replay_locked();
        g_orient_hw_fail = 0;
        CHECK(!strcmp(get("image.flip"), "1"), "a refused replay keeps the state");
        ctl_set("image.flip", "0", 1); /* back to off for the next case */
    }
    /* the file cannot be written: the picture turns, the answer says it was not saved */
    put("blocker", "x");
    snprintf(path, sizeof path, "%s/blocker/orient", g_root);
    CHECK(!strncmp(ctl_set("image.flip", "1", 1), "OK image.flip=1 (not saved:", 27), "unwritable file: applied, note kept");
}

int main(void)
{
    char tmpl[] = "/tmp/ak1080_settings.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    setvbuf(stdout, NULL, _IONBF, 0);
    leds_setup();
    test_table();
    test_state();
    test_settings();
    test_patch();
    test_switches();
    test_audio_lan();
    test_unsaved_is_reported();
    test_orient();
    leds_destroy(&g_leds);
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_root);
    if (system(cmd) != 0) { /* best effort */ }
    printf("ak1080_settings_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
