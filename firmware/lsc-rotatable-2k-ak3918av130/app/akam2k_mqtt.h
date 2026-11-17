/* Outbound MQTT 3.1.1 client of the Rotatable 2K own app: availability (LWT), Home Assistant discovery and the
 * person-detection state (akam2k_mqttcfg.h has the topics and payloads). Optional: no card file akam/mqtt (or
 * enabled=0, or an invalid file) means no thread, no socket, nothing.
 *
 * One thread of its own (64 KiB stack, below the video threads in priority). It never touches capture, encoding, the RTSP
 * server or the NPU: the detection thread only copies a small snapshot under a mutex (a2k_mqtt_detection, never blocks
 * on the network) into a bounded ring (8 transitions, the oldest is dropped when the broker is slow), and wakes the
 * thread through a pipe. A dead broker costs one failed connect per backoff period (5 s doubling to 60 s, spread by
 * 20 %, restarting from 5 s only after a session that held for 60 s), a dead resolver or a stuck send only this thread.
 * This file is the ONLY app file that may connect() (the anti-brick scan in tests/rot2k_overlay_test.sh). The broker
 * must be a LAN address or a name the LAN resolver knows: decloud blackholes the WAN. Plain TCP, no TLS. The password
 * is read by a2k_mqtt_cfg_load from the card file by this process itself, kept only in this thread's copy of the
 * config, wiped at stop, never logged, never put on a command line. */
#ifndef AKAM2K_MQTT_H
#define AKAM2K_MQTT_H
#include <stddef.h>
#include <stdint.h>

#include "akam2k_detstate.h"
#include "akam2k_mqttcfg.h"
#include "akam2k_zones.h"

#define A2K_MQTT_CONF_DEFAULT "/mnt/sdcard/akam/mqtt"
#define A2K_MQTT_MAC_PATH "/sys/class/net/wlan0/address"
#define A2K_MQTT_RING 8
#define A2K_MQTT_DET_MAX 800           /* the largest detections list published (api_events.h A2K_EVT_DDATA) */
#define A2K_MQTT_SNAP_MAX (200u << 10) /* the largest snapshot JPEG published; a bigger one is skipped */

/* The network, replaceable by the host unit test (tests/rot2k_mqtt_unit.c) with a scripted stub socket. */
typedef struct {
    /* connects to cfg->host:port; returns a handle >= 0 and fills the local IPv4 address, or -1 with a reason
     * ("dns", "connect", "timeout", "stopped") */
    int     (*dial)(const a2k_mqtt_cfg_t *cfg, char *local_ip, size_t lcap, const char **reason);
    int     (*send_all)(int h, const uint8_t *b, size_t n);               /* 0 or -1 */
    int     (*recv_some)(int h, uint8_t *b, size_t cap, int timeout_ms);  /* > 0 bytes, 0 timeout/wake, -1 closed, -2 stop */
    void    (*close_h)(int h);
    int64_t (*now_ms)(void);
    void    (*sleep_ms)(unsigned ms);                                     /* returns early on a stop */
} a2k_mqtt_net_t;

typedef int (*a2k_mqtt_viewers_fn)(void);

/* Reads conf_path and starts the thread. 0 started, 1 off (no file or enabled=0), -1 invalid file or start failure (logged,
 * the app goes on without MQTT). mac_path: file with the Wi-Fi MAC. rtsp_port: for the stream URL attributes. */
int a2k_mqtt_start(const char *conf_path, const char *mac_path, int rtsp_port, a2k_mqtt_viewers_fn viewers);
/* Before a2k_mqtt_start: the detection zones (one person_<zone> binary_sensor each; NULL or n = 0: none) and whether the JPEG
 * snapshot `image` entity exists. Copied. */
void a2k_mqtt_set_zones(const a2k_zones_t *z);
void a2k_mqtt_set_snapshot(int enabled);
/* Before a2k_mqtt_start: whether the day/night binary_sensor `night` exists. Its state arrives through a2k_mqtt_night. */
void a2k_mqtt_set_night(int enabled);
/* Before a2k_mqtt_start: whether the hal_sys diagnostic sensors exist. They are fed by a2k_mqtt_sys (cheap, thread safe, never
 * blocks; a no-op value store when MQTT is off) and published at the periodic republish, not on every sample. */
void a2k_mqtt_set_sys(int enabled);
void a2k_mqtt_sys(const hal_sys_sample_t *s);
/* Before a2k_mqtt_start: the status LED / IR LED controls (A2K_E_LED0.., akam2k_mqttcfg.h). NULL ops (the default) = no such
 * entities and no subscription (the app stays read-only). get: current text of control ent (0..A2K_LED_N-1), 0 or -1. set: apply a
 * validated text, 0 or -1 (the control's real value is published back either way). Both are called from the MQTT thread and
 * must be cheap and thread safe. At most A2K_MQTT_CMD_MAX commands per A2K_MQTT_CMD_WINDOW_MS are obeyed. */
#define A2K_MQTT_CMD_MAX 20
#define A2K_MQTT_CMD_WINDOW_MS 10000
typedef struct {
    int (*get)(int ent, char *out, size_t cap);
    int (*set)(int ent, const char *val);
} a2k_mqtt_leds_t;
void a2k_mqtt_set_leds(const a2k_mqtt_leds_t *ops);
/* Before a2k_mqtt_start: the PTZ buttons and sensors (A2K_E_PTZ0.., akam2k_mqttcfg.h). NULL press (the default) = no PTZ entities. press: queue
 * the command 0..A2K_PTZ_BTN-1 (the A2K_PTZ_* of akam2k_ptz.h), 0 or -1; called from the MQTT thread, must not block. Same filtering as the
 * LED controls: retained commands are ignored, at most A2K_MQTT_CMD_MAX per A2K_MQTT_CMD_WINDOW_MS are obeyed. */
typedef struct {
    int (*press)(int cmd);
} a2k_mqtt_ptz_t;
void a2k_mqtt_set_ptz(const a2k_mqtt_ptz_t *ops);
/* Before a2k_mqtt_start: the controls of person tracking, the zones and the push button (the ctl group of akam2k_mqttcfg.h, A2K_E_CTL0..).
 * NULL get (the default) = no such entities. get: the current text of ctl entity k (switch "ON" / "OFF", texts, the areas sensor), 0 or -1.
 * set: apply a validated payload ("PRESS", "ON" / "OFF" or the text), 0 or -1; the real value is published back either way. areas: the names
 * of the tracking areas (slot order), returns their count (0..A2K_CTL_SLOTS). Called from the MQTT thread: cheap, thread safe. Same
 * command filtering as the other controls (retained ignored, A2K_MQTT_CMD_MAX per A2K_MQTT_CMD_WINDOW_MS). */
typedef struct {
    int (*get)(int k, char *out, size_t cap);
    int (*set)(int k, const char *val);
    int (*areas)(char names[][A2K_ZONE_NAME_MAX + 1]);
} a2k_mqtt_ctl_t;
void a2k_mqtt_set_ctl(const a2k_mqtt_ctl_t *ops);
/* Before a2k_mqtt_start: the video settings (the vid group of akam2k_mqttcfg.h, A2K_E_VID0..): numbers and a select per stream. NULL get (the default) = no such
 * entities. get: the current text of entity k (0..A2K_VID_N-1), 0 or -1; set: apply a validated text, 0 or -1; the real value is published back either way.
 * Same command filtering as the other controls. */
typedef struct {
    int (*get)(int k, char *out, size_t cap);
    int (*set)(int k, const char *val);
} a2k_mqtt_vid_t;
void a2k_mqtt_set_vid(const a2k_mqtt_vid_t *ops);
/* Any thread, cheap, never blocks on the network: the zones or the tracking areas changed (names, count): the MQTT thread announces the
 * entities again (and clears the removed ones) and publishes the new values. The zones are the CONFIGURED ones (not the ones in force). */
void a2k_mqtt_zones_changed(const a2k_zones_t *z);
void a2k_mqtt_resync(void);
/* The push button was pressed briefly: publishes the Home Assistant event (type "press" only; others are ignored). Cheap, never blocks;
 * at most 4 presses wait for the broker, a dead broker drops them. */
void a2k_mqtt_button_event(const char *type);
/* Every gesture of the programmable button (akam2k_buttonrt.h): queued and published on BASE/button as json (the same text as the API event
 * "button": {"gesture","actions","result"}), not retained; a "short" gesture also publishes the legacy event above. Only with the ctl group (the
 * same opt-in as the event entity); never blocks. json: at most A2K_MQTT_GESTURE_MAX bytes (longer is dropped). */
#define A2K_MQTT_GESTURE_MAX 144
void a2k_mqtt_button_gesture(const char *gesture, const char *json);
/* Before a2k_mqtt_start: the configured gestures (canonical names, at most A2K_BTN_TRIG_N) for the device triggers; returns how many. Called from the
 * MQTT thread at every announce; call a2k_mqtt_resync after the bindings changed. NULL (the default) = no triggers. */
void a2k_mqtt_set_buttons(int (*list)(char names[][A2K_BTN_NAME]));
/* Called by the PTZ thread after every state change (cheap, thread safe, never blocks on the network): published at once and with every
 * periodic republish, like the day/night state. state: "home" "parked" "moved" "calibrating" "unknown"; pan / tilt only when known. */
void a2k_mqtt_ptz(const char *state, int known, int pan, int tilt);
/* Called by the day/night thread after every state change (and once at its start). Cheap, thread safe, never blocks on the
 * network; the state is published at once (a bounded ring, like the person transitions) and at every periodic republish. */
void a2k_mqtt_night(const struct a2k_mqtt_night *n);
/* Called by the snapshot thread with a JPEG (akam2k_snap.h): the module keeps its own copy of the NEWEST one (an older one
 * not sent yet is replaced) and the MQTT thread publishes it raw, not retained, on BASE/snapshot. Never blocks on the
 * network. Dropped (counted, not an error) when MQTT is off, not connected, or len is 0 or over A2K_MQTT_SNAP_MAX. */
void a2k_mqtt_snapshot(const uint8_t *jpeg, size_t len);
/* Publishes "offline", disconnects, joins the thread (waits at most about 3 s). Safe when never started. */
void a2k_mqtt_stop(void);
/* Called by the detection thread with the state it just wrote to the state file. Cheap, thread safe, never blocks on
 * the network. epoch: wall clock seconds. */
void a2k_mqtt_detection(const a2k_det_t *d, const char *status, int64_t epoch);
/* Called by the detection thread with each person-box list it publishes (akam2k_detpub.h JSON, already rate limited). Cheap, never blocks
 * on the network, a no-op when MQTT is off. The module keeps the NEWEST list not sent yet; the MQTT thread publishes it on BASE/detections
 * (QoS 0, not retained, no discovery entity). Lists made while the broker is away are dropped. */
void a2k_mqtt_detections(const char *json);
/* Lists published, lists dropped (replaced before they were sent, too long, or the broker was away). */
void a2k_mqtt_det_stats(unsigned *sent, unsigned *dropped);
/* "off", "connecting", "connected" or "error:<reason>". */
void a2k_mqtt_conn_state(char *out, size_t cap);
/* Counters: messages published, connections made. */
void a2k_mqtt_stats(unsigned *published, unsigned *connects);
/* Snapshots published, snapshots dropped. */
void a2k_mqtt_snap_stats(unsigned *sent, unsigned *dropped);

/* TEST ONLY: replaces the network (NULL = the real one). Call before a2k_mqtt_start. */
void a2k_mqtt_test_set_net(const a2k_mqtt_net_t *n);

#endif
