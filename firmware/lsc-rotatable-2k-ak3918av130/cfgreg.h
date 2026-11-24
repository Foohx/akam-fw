/* Registry rows of the options that live in files of the user (MQTT, ONVIF, the stream password, NTP, the microphone) on the 2K, so that the
 * signed API and the assistant can set them like on the 1080p. DATA, VALIDATION AND A BACKEND: no I/O of its own (the app never spawns a process
 * and these rows are checked on the host without a camera). settings.c installs the backend with a2k_cfgreg_set_backend(): the getters read the
 * files (/mnt/sdcard/akam/akam.conf, mqtt, rtsp.conf, ntp: the files the supervisor and the modules read) and the setters queue the changes of ONE
 * PATCH into a request file in RAM, which the supervisor (own_app_run.sh, cfg_request) validates again and writes into the right place (flash
 * /config/akam in the card-less image, the card otherwise). Nothing is written to the flash by the app, and the supervisor writes only a file
 * whose content changes.
 *
 * Rows (all need an admin key, none is persisted by the registry, all apply at the next restart of the app):
 *   audio.mic_enable      bool    akam.conf audio=       (opt-in, default off)
 *   mqtt.enabled/host/port/user   the file mqtt             mqtt.password: WRITE-ONLY, the file mqtt_pass
 *   onvif.enabled/port    akam.conf onvif= / onvif_port=  (needs a stream password)
 *   rtsp.user             the user of rtsp.conf ("" = no password)   rtsp.password: WRITE-ONLY, becomes the Digest HA1 of rtsp.conf
 *   ntp.servers           1 to 3 servers, space separated ("" = off)
 *   rtsp.auth, cfg.apply  read only: off | on | invalid, and idle | queued | ok | refused of the last request.
 * A write-only row is never read, never listed, never echoed in a PATCH reply and never logged. */
#ifndef AKAM2K_CFGREG_H
#define AKAM2K_CFGREG_H
#include <stddef.h>

#include "settings_reg.h"

#define A2K_CFGREG_N 13

extern const st_def_t a2k_cfgreg_defs[A2K_CFGREG_N];

typedef struct {
    /* the configured value (the file's), its length, or < 0 */
    int (*get)(const char *key, char *out, size_t cap);
    /* queue key=val for the supervisor; the PATCH is closed with flush; 0 or < 0 */
    int (*set)(const char *key, const char *val);
} a2k_cfgreg_backend_t;

void a2k_cfgreg_set_backend(const a2k_cfgreg_backend_t *b);

/* the grammars the supervisor enforces too (public for the host tests) */
int a2k_cfgreg_user_ok(const char *s);     /* 1 to 32 of A-Z a-z 0-9 . _ @ + -, not a lone - */
int a2k_cfgreg_host_ok(const char *s);     /* 1 to 63 of A-Z a-z 0-9 . - */
int a2k_cfgreg_ntp_ok(const char *list);   /* 1 to 3 servers separated by single spaces, each a host name or IPv4 address */
int a2k_cfgreg_secret_ok(const char *s, size_t min); /* min to 128 printable characters, no control character */

#endif
