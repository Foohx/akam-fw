#pragma once
/* The settings registry of the 1080p (specs/control-api.md sections 2 and 11, docs/archive/convergence-plan.md step P7): one row per
 * user-visible setting or state value, under the same key names as the Rotatable 2K wherever the concept is the same (led.mode,
 * led.<rule>, night.ir_led, image.*, video0.*, sys.*, net.wifi_*, rtsp.clients, night.active, api.keys), a different scale
 * being told by the schema (bounds, options, default), not by the name. DATA and glue only: every row calls the module that
 * already owns the setting (the 8091 control port code in control.c, leds, video_ctl, ctl_sys, onvif_cfg, mqtt_svc), so a value
 * has ONE writer, ONE validation of its own and ONE persistence, whichever door it came through (the 8091 text port, the signed
 * HTTP API, a later MQTT layer). The registry file of the 2K (/config/akam/settings) does not exist here: every row has
 * persist=0, the module of the row keeps its own file under /etc/config/akam, written only on a user change.
 *
 * Rows (docs/porting/lsc-1080p/control-api.md has the table with ranges and defaults):
 *   led.mode auto|on|off (8091: led.status), led.stream led.boot led.nowifi led.night led.idle (LED patterns), night.ir_led
 *   night.mode auto|day|night, image.brightness/contrast/saturation/sharpness -50..50, image.denoise 0..4
 *   video0.kbps/fps/gop/min_qp/max_qp (saved, applied at the next start of akamd: apply = "restart_component": akamd reads them at its next start)
 *   onvif.enabled, mqtt.enabled (live: the service is reloaded)
 *   img.wb conf|manual, audio.out_volume/out_enable/talk_enable/mic_enable (the speaker and microphone, same names as the old text port), control.lan (the
 *   opt-in that opens the text port 8091 to the LAN: bound when akamd starts)
 *   read-only state: audio.out_state/underruns/drops led.state net.ssid net.wifi_rate sys.* net.wifi_rssi net.wifi_lq rtsp.clients rtsp.auth rtsp.skipped_frames video0.fps_x10 video0.bitrate_kbps
 *   video0.cfg_pending night.active onvif.state mqtt.state sys.time_set net.hostname net.ntp sys.version api.keys
 * Not here on purpose: ptz.* track.* detect.* zones.* button.* net.wifi_band/country (no such hardware or radio), ae.* and the
 * night thresholds (tuning, stay on 8091), anything that holds a secret (RTSP and MQTT passwords stay write-only files). */
#include <stddef.h>

#include "settings_reg.h"

/* The NTP server file the net.ntp row reads (/etc/config/akam/ntp, written by akam_ntp.sh); tests point it elsewhere. */
extern const char *ak1080_ntp_path;

/* Builds and validates the table. 0, or -1 (logged). Idempotent. */
int ak1080_settings_init(void);
const streg_t *ak1080_settings_reg(void);

/* The number of API keys, for the api.keys state row (set by the API layer). */
void ak1080_settings_set_keys(int n);

/* ---- the 8091 adapter (control.c): the rows that the old 8091 code does not know under that name ---- */
/* 0 and "key=value" in line; -1 when the key is not a registry row. */
int ak1080_settings_ctl_get(const char *key, char *line, size_t cap);
/* -1 not a registry row (msg untouched); 0 done, msg "OK key=value[ (restart to apply)]"; 1 refused, msg "ERR ...".
 * persist: 1 for a loopback peer (the setting is saved), 0 for a network peer in DEBUG mode (RAM only where the module allows). */
int ak1080_settings_ctl_set(const char *key, const char *val, int persist, char *msg, size_t cap);

/* ---- the signed API (ak1080_api.c) ---- */
/* Applies the rows of a PATCH with the persistence of an authenticated write (saved). Returns the HTTP status of streg_patch. */
int ak1080_settings_patch(int scope, size_t n, const char *const *keys, const char *const *vals, st_result_t *res);
