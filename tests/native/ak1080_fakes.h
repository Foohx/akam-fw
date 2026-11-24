/* Fake modules of the 1080p for the host tests of the registry (ak1080_settings_test.c) and of the signed API (ak1080_api_server.c):
 * what the rows are built on (the 8091 setters, video_ctl, ctl_sys, the ONVIF and MQTT services, night) behaves like the real
 * ones at the level the registry cares about, and the LED is the REAL engine of leds.c on a fake sysfs directory. Included by one
 * program only: the functions are the external symbols ak1080_settings.c / ak1080_api.c call. */
#ifndef AK1080_FAKES_H
#define AK1080_FAKES_H
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "settings.h"
#include "api_keys.h"
#include "control.h"
#include "ctl_sys.h"
#include "leds.h"
#include "mqtt_svc.h"
#include "night.h"
#include "onvif_cfg.h"
#include "onvif_device.h"
#include "version.h"
#include "video_cfg.h"
#include "video_ctl.h"
#include "image_orient.h"

/* The ISP behind image_orient.c (the module is the REAL one; its ISP call is replaced by fake_orient_hw below). */
int isp_pipeline_ready(void) { return 1; }
void isp_cfg_lock(void) {}
void isp_cfg_unlock(void) {}
int isp_tunnel(uint32_t cmd, void *payload) { (void)cmd; (void)payload; return 0; }
__attribute__((unused)) static int g_orient_hw_calls, g_orient_hw_fail, g_orient_hw_f = -1, g_orient_hw_m = -1;
__attribute__((unused)) static int fake_orient_hw(int f, int m)
{
    g_orient_hw_calls++;
    if (g_orient_hw_fail) return -1;
    g_orient_hw_f = f;
    g_orient_hw_m = m;
    return 0;
}

/* ------------------------------------------------------------------ stubs: the modules the rows are built on */
static char g_root[80];
static int g_last_persist = -1;
static int g_vals[8];            /* isp.brightness contrast saturation sharpness denoise (0..4), see key_slot */
static char g_night_mode[8] = "auto";
static char g_led_status[8] = "on", g_ir[8] = "auto";
static int g_video[5] = { 1536, 15, 2, 28, 43 }; /* bitrate fps gop minqp maxqp */
static int g_rssi = -1;
static int g_fail_apply;
static int g_fail_save; /* the module applies the value but cannot write its file: "(not saved: ...)" even for a saved write */
/* audio, white balance and the LAN opt-in of the text port (the modules behind control_get_value / control_apply_setting) */
static int g_audio_vol = 100, g_audio_out = 1, g_audio_talk = 1, g_audio_mic, g_beeps, g_lan;
static char g_wb[8] = "conf";

static int key_slot(const char *k)
{
    static const char *const names[] = { "isp.brightness", "isp.contrast", "isp.saturation", "isp.sharpness", "isp.denoise", "isp.wdr", "isp.nr2" };
    for (int i = 0; i < 7; i++)
        if (!strcmp(k, names[i])) return i;
    return -1;
}

int control_get_setting(const char *key, char *out, size_t cap)
{
    int s = key_slot(key);
    if (s >= 0) { snprintf(out, cap, "%d", g_vals[s]); return 0; }
    if (!strcmp(key, "night.mode")) { snprintf(out, cap, "%s", g_night_mode); return 0; }
    return -1;
}

int control_apply_setting(const char *key, const char *value, int persist, char *msg, size_t cap)
{
    g_last_persist = persist;
    msg[0] = '\0';
    if (g_fail_apply) { snprintf(msg, cap, "ERR %s refused", key); return -1; }
    int s = key_slot(key);
    if (s >= 0) {
        g_vals[s] = atoi(value);
        snprintf(msg, cap, "OK %s=%d%s", key, g_vals[s], (persist && !g_fail_save) ? "" : " (not saved: stub)");
        return 0;
    }
    if (!strcmp(key, "night.mode")) { snprintf(g_night_mode, sizeof g_night_mode, "%s", value); snprintf(msg, cap, "OK night.mode=%s", value); return 0; }
    if (!strcmp(key, "led.status")) { snprintf(g_led_status, sizeof g_led_status, "%s", value); snprintf(msg, cap, "OK led.status=%s", value); return 0; }
    if (!strcmp(key, "night.ir_led")) { snprintf(g_ir, sizeof g_ir, "%s", value); snprintf(msg, cap, "OK night.ir_led=%s", value); return 0; }
    if (!strcmp(key, "audio.out_volume")) { g_audio_vol = atoi(value); snprintf(msg, cap, "OK %s=%d", key, g_audio_vol); return 0; }
    if (!strcmp(key, "audio.out_enable")) { g_audio_out = atoi(value); snprintf(msg, cap, "OK %s=%d%s", key, g_audio_out, g_audio_out ? " (restart to apply)" : ""); return 0; }
    if (!strcmp(key, "audio.talk_enable")) { g_audio_talk = atoi(value); snprintf(msg, cap, "OK %s=%d", key, g_audio_talk); return 0; }
    if (!strcmp(key, "audio.mic_enable")) { g_audio_mic = atoi(value); snprintf(msg, cap, "OK %s=%d (restart to apply)", key, g_audio_mic); return 0; }
    if (!strcmp(key, "audio.out_test")) { g_beeps++; snprintf(msg, cap, "OK audio.out_test=1"); return 0; }
    if (!strcmp(key, "img.wb")) { snprintf(g_wb, sizeof g_wb, "%s", value); snprintf(msg, cap, "OK img.wb=%s", value); return 0; }
    if (!strncmp(key, "video.", 6)) {
        int i = !strcmp(key, "video.bitrate_cfg") ? 0 : !strcmp(key, "video.fps_cfg") ? 1 : !strcmp(key, "video.gop_cfg") ? 2
                : !strcmp(key, "video.minqp_cfg") ? 3 : 4;
        g_video[i] = atoi(value);
        snprintf(msg, cap, "OK %s=%s (restart to apply)", key, value);
        return 0;
    }
    snprintf(msg, cap, "ERR unknown param %s", key);
    return -1;
}

int control_get_value(const char *key, char *out, size_t cap)
{
    if (!strcmp(key, "audio.out_volume")) return snprintf(out, cap, "%d", g_audio_vol), 0;
    if (!strcmp(key, "audio.out_enable")) return snprintf(out, cap, "%d", g_audio_out), 0;
    if (!strcmp(key, "audio.talk_enable")) return snprintf(out, cap, "%d", g_audio_talk), 0;
    if (!strcmp(key, "audio.mic_enable")) return snprintf(out, cap, "%d", g_audio_mic), 0;
    if (!strcmp(key, "audio.out_state")) return snprintf(out, cap, "idle"), 0;
    if (!strcmp(key, "audio.out_underruns")) return snprintf(out, cap, "3"), 0;
    if (!strcmp(key, "audio.out_drops")) return snprintf(out, cap, "1"), 0;
    if (!strcmp(key, "led.state")) return snprintf(out, cap, "solid"), 0;
    if (!strcmp(key, "img.wb")) return snprintf(out, cap, "%s", g_wb), 0;
    return -1;
}
int control_lan_get(void) { return g_lan; }
int control_lan_set(int on) { g_lan = on ? 1 : 0; return 0; }

int video_ctl_get(const char *key, char *out, size_t cap)
{
    if (!strcmp(key, "video.cfg_pending")) { snprintf(out, cap, "%s=1", key); return 0; }
    int i = !strcmp(key, "video.bitrate_cfg") ? 0 : !strcmp(key, "video.fps_cfg") ? 1 : !strcmp(key, "video.gop_cfg") ? 2
            : !strcmp(key, "video.minqp_cfg") ? 3 : !strcmp(key, "video.maxqp_cfg") ? 4 : -1;
    if (i < 0) return -1;
    snprintf(out, cap, "%s=%d", key, g_video[i]);
    return 0;
}

/* the LED: the REAL engine on a fake sysfs directory, behind the ledsys_* names of the singleton */
static leds_t g_leds;
static void leds_setup(void)
{
    char led[120], conf[120];
    snprintf(led, sizeof led, "%s/led", g_root);
    snprintf(conf, sizeof conf, "%s/conf", g_root);
    mkdir(led, 0755);
    leds_cfg_t cfg = { led, conf, "lo", "/nonexistent", 0 };
    leds_init(&g_leds, NULL, &cfg);
}
int ledsys_ctl_get(const char *key, char *out, size_t cap)
{
    if (!strcmp(key, "led.status")) { snprintf(out, cap, "led.status=%s", g_led_status); return 0; }
    if (!strcmp(key, "night.ir_led")) { snprintf(out, cap, "night.ir_led=%s", g_ir); return 0; }
    return -1;
}
int ledsys_pat_get(int rule, char *out, size_t cap) { return leds_pat_get(&g_leds, rule, out, cap); }
int ledsys_pat_set(int rule, const char *text) { return leds_pat_set(&g_leds, rule, text); }
void ledsys_pat_default(int rule, char *out, size_t cap) { leds_pat_default(&g_leds, rule, out, cap); }

int ctl_sys_get(const char *key, char *out, size_t cap)
{
    static const struct { const char *k, *v; } t[] = {
        { "sys.cpu_pct", "37" }, { "sys.load1_x100", "12" }, { "sys.mem_total_kb", "57344" }, { "sys.mem_avail_kb", "21000" },
        { "sys.uptime_s", "86400" }, { "sys.time_set", "1" }, { "net.wifi_lq", "70" }, { "net.ssid", "my net" }, { "rtsp.clients", "1" }, { "rtsp.auth", "on" },
        { "rtsp.skipped_frames", "2" }, { "video.fps_actual", "unknown" }, { "video.bitrate", "0" },
    };
    if (!strcmp(key, "net.wifi_rssi")) { if (g_rssi >= 0) return -1; snprintf(out, cap, "unknown"); return 0; }
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(key, t[i].k)) { snprintf(out, cap, "%s", t[i].v); return 0; }
    return -1;
}

static int g_night;
int night_is_night(void) { return g_night; }
const char *firmware_version(void) { return "b1-test"; }
void onvif_hostname_get(char *out) { strcpy(out, "akam-test"); }

/* ONVIF: the flag file with the same two lines as the real parser; the service "starts" only when a password is set */
const char *onvif_cfg_path;
static int g_have_password = 1, g_onvif_reloads;
static char g_onvif_state[ONVIF_STATE_MAX] = "off";
int onvif_cfg_load(onvif_cfg_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(onvif_cfg_path, "r");
    if (!f) return 1;
    int en = 0, port = 0;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "enabled=%d", &en) == 1) continue;
        (void)sscanf(line, "port=%d", &port);
    }
    fclose(f);
    out->enabled = en;
    out->port = port;
    return 0;
}
int onvif_svc_reload(char *s, size_t c)
{
    onvif_cfg_t cfg;
    g_onvif_reloads++;
    (void)onvif_cfg_load(&cfg);
    if (!cfg.enabled) snprintf(g_onvif_state, sizeof g_onvif_state, "off");
    else if (!g_have_password) snprintf(g_onvif_state, sizeof g_onvif_state, "no_password");
    else snprintf(g_onvif_state, sizeof g_onvif_state, "on:%d", cfg.port);
    snprintf(s, c, "%s", g_onvif_state);
    return cfg.enabled && !g_have_password ? -1 : 0;
}
void onvif_svc_state(char *s, size_t c) { snprintf(s, c, "%s", g_onvif_state); }

/* MQTT: a file with host= is "valid" */
const char *mqtt_cfg_path;
static char g_mqtt_state[MQTT_STATE_MAX] = "off";
static int g_mqtt_fail_start;
int mqtt_cfg_parse(const char *text, size_t len, mqtt_cfg_t *out)
{
    memset(out, 0, sizeof *out);
    char *t = strndup(text, len);
    int ok = t && strstr(t, "host=") && strstr(t, "enabled=");
    out->enabled = t && strstr(t, "enabled=1") != NULL;
    free(t);
    return ok ? 0 : -1;
}
int mqtt_cfg_load(mqtt_cfg_t *out)
{
    char buf[600];
    memset(out, 0, sizeof *out);
    FILE *f = fopen(mqtt_cfg_path, "r");
    if (!f) return 1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return mqtt_cfg_parse(buf, n, out);
}
int mqtt_svc_reload(char *s, size_t c)
{
    mqtt_cfg_t cfg;
    (void)mqtt_cfg_load(&cfg);
    if (cfg.enabled && g_mqtt_fail_start) { snprintf(g_mqtt_state, sizeof g_mqtt_state, "error:thread"); snprintf(s, c, "%s", g_mqtt_state); return -1; }
    snprintf(g_mqtt_state, sizeof g_mqtt_state, "%s", cfg.enabled ? "connecting" : "off");
    snprintf(s, c, "%s", g_mqtt_state);
    return 0;
}
void mqtt_svc_state(char *s, size_t c) { snprintf(s, c, "%s", g_mqtt_state); }


/* what ak1080_api.c adds */
int ledsys_blip(const char *pattern, long dur_ms) { return leds_blip(&g_leds, pattern, dur_ms); }
void ledsys_blip_clear(void) { leds_blip_clear(&g_leds); }
static void (*g_night_cb)(int, int);
void night_set_notify(void (*cb)(int night, int ir_on)) { g_night_cb = cb; }

#endif
