#pragma once
#include <stddef.h>

/* TCP tuning/debug control server (port 8091).
 *
 * Text protocol (newline-delimited), up to 4 clients at once, one slot kept for loopback peers
 * (a remote peer gets at most 3, dropped after 120 s idle; otherwise "ERR busy"):
 *   LIST                  -> one "name=value" line per known param (then metrics, ctl.clients, ctl.debug), then "."
 *   GET <param>           -> "<param>=<value>" or "ERR ..."
 *   SET <param> <value>   -> "OK <param>=<value>" or "ERR ..."
 *   DEBUG on [min]|off|status  (loopback only) -> remote writes allowed for min minutes
 *   ONVIF reload          (loopback only) -> "OK onvif.state=<state>": re-read /etc/config/akam/onvif and start/stop
 *                         the ONVIF HTTP server and WS-Discovery (onvif_cfg.h); GET onvif.state is readable by all
 *   MQTT reload|test      (loopback only) -> "OK mqtt.state=<state>" re-reads /etc/config/akam/mqtt and starts/stops the
 *                         MQTT client (mqtt_svc.h); "OK mqtt.test=running" starts a one-shot broker test whose result
 *                         is "GET mqtt.test" (running|ok|error:<reason>); GET mqtt.state and mqtt.stats are readable by all
 * Read-only by default: SET (and SAVE/RESET) from a non-loopback peer answer
 * "ERR read-only mode, see akam_debug.sh" unless DEBUG is on (RAM-only flag, off at start,
 * auto-off after 60 min by default). Loopback peers always may write.
 * Async log lines (tee'd from stdout) are pushed on the same connection
 * (loopback peers only, or any peer while DEBUG is on; a remote reader without DEBUG gets none, and
 * reads net.ssid and net.mac as "hidden") prefixed "LOG " whenever they occur, interleaved with command responses —
 * a client tells them apart by the "LOG " prefix, not by strict
 * request/response framing.
 *
 * See docs/hacking.md for the full
 * parameter list and the PC-side WinUI3 tuning tab that talks this protocol.
 *
 * Call control_start() as the very first thing in main() (before any other
 * printf-producing init) so the stdout tee captures the full boot log, not
 * just ae/night's own logging. control_stop() on shutdown. */

int  control_start(void);
void control_stop(void);

/* After the ISP is up (once, from main): apply the saved isp.* picture settings (isp_tune.h). */
void control_apply_saved_isp(void);

/* Shared setter/getter for the other control paths (ONVIF Imaging, onvif_imaging.c). control_apply_setting runs
 * the same code as an 8091 SET under the same lock; persist != 0 writes flash like a loopback SET, 0 is RAM only.
 * Returns 0 when msg holds an "OK ..." answer, else -1 (msg holds "ERR ..."). control_get_setting answers the
 * isp.brightness/contrast/saturation/sharpness values and night.mode as text (0, or -1 for another key).
 * control_persist_picture writes the four isp.* values to flash now (0, or -1). */
int  control_apply_setting(const char *key, const char *value, int persist, char *msg, size_t cap);
int  control_get_setting(const char *key, char *out, size_t cap);
int  control_persist_picture(void);
/* Value text of any readable key (settings, night.state, LED, audio, metrics), for the MQTT module. 0, or -1. */
int  control_get_value(const char *key, char *out, size_t cap);

/* The settings registry behind the text port (ak1080_settings.h, convergence P7). The old code of this file stays the owner of every
 * key it knows (names, replies, errors and the write rule are byte-identical); a key it does NOT know is offered to the registry,
 * which has the rows under their registry names (led.mode, led.stream, image.brightness, video0.kbps, onvif.enabled, ...). Not in
 * LIST (the order and content of LIST are unchanged). get: 0 and "key=value" in line, -1 not a row. set: -1 not a row, 0 and an
 * "OK ..." line, 1 and an "ERR ..." line. NULL ops (the default) = no registry, "ERR unknown param" as before. Call before control_start(). */
typedef struct {
    int (*get)(const char *key, char *line, size_t cap);
    int (*set)(const char *key, const char *val, int persist, char *msg, size_t cap);
} control_reg_ops_t;
void control_set_registry(const control_reg_ops_t *ops);

/* 1 while the 8091 DEBUG mode is on (RAM flag, auto-off): the switch for chatty log lines (per-frame, per-AE-window).
 * Thread safe, cheap enough for once a second; do not call per video frame without a rate condition first. */
int  control_debug_on(void);
