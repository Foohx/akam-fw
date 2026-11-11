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
 * Read-only by default: SET (and SAVE/RESET) from a non-loopback peer answer
 * "ERR read-only mode, see akam_debug.sh" unless DEBUG is on (RAM-only flag, off at start,
 * auto-off after 60 min by default). Loopback peers always may write.
 * Async log lines (tee'd from stdout) are pushed on the same connection
 * prefixed "LOG " whenever they occur, interleaved with command responses —
 * a client tells them apart by the "LOG " prefix, not by strict
 * request/response framing.
 *
 * See docs/serial_console_and_camera_tuning.md for the full
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
