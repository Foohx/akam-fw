/* MQTT + Home Assistant side of the Rotatable 2K own app, the PURE part (no socket, no thread, no clock): the strict
 * parser of the card file akam/mqtt, the topics, the Home Assistant discovery documents and the state / attribute
 * payloads of the person-detection entities. Host unit-tested (tests/rot2k_mqtt_unit.c) with golden JSON and fuzzed.
 *
 * Same keys, same format and same topic scheme as the 1080p firmware (specs/mqtt.md), except that this app is READ-ONLY:
 * it publishes, it never subscribes and no command topic exists. Same SD-card layout as the 1080p /etc/config/akam/:
 * `akam/mqtt` (this file) and `akam/mqtt_pass` (the broker password alone, see a2k_mqtt_pass_load). The card is FAT: it
 * has no usable file mode, so the 1080p "mode 0600 or refused" rule cannot apply (the card is the physical secret).
 *
 * akam/mqtt: key=value lines, LF (a trailing CR is tolerated), at most A2K_MQTT_CFG_MAX bytes, at most one line per key,
 * an unknown key makes the whole file invalid, a bad value likewise:
 *   enabled=0|1          required
 *   host=NAME            required: a host name or IPv4 address (A-Z a-z 0-9 . -), 1 to 63 characters
 *   port=N               1 to 65535 (default 1883)
 *   user=NAME            0 to 32 of A-Z a-z 0-9 . _ @ + - (a lone - is refused)
 *   client_id=ID         1 to 32 of A-Z a-z 0-9 _ - (default akam-rot2k: the stock host name "YYOS" is not a usable identity)
 *   base_topic=T         A-Z a-z 0-9 _ - and /, 1 to 64, no leading, trailing or double / (default akam/<hostname>)
 *   discovery=on|off     Home Assistant discovery documents (default on)
 *   discovery_prefix=P   same rules as base_topic, 1 to 32 (default homeassistant)
 *   interval=N           seconds between periodic republishes, 10 to 300 (default 30)
 * akam/mqtt_pass: one line, at most A2K_MQTT_PASS_MAX printable characters (no control character), one trailing LF
 * tolerated, absent = no password, ignored when no user is set. NEVER logged or published.
 * No TLS (the 1080p has none either): the password travels in clear on the LAN. */
#ifndef AKAM2K_MQTTCFG_H
#define AKAM2K_MQTTCFG_H
#include <stddef.h>
#include <stdint.h>

#include "hal_sys.h"
#include "detstate.h"
#include "video.h"

#define A2K_MQTT_CFG_MAX 768
#define A2K_MQTT_PASS_MAX 128
#define A2K_MQTT_PASS_NAME "mqtt_pass"
#define A2K_MQTT_MODEL "lsc-rotatable-2k-ak3918av130"
#ifndef A2K_APP_VERSION
#define A2K_APP_VERSION "0.1.0"
#endif

typedef struct {
    int  enabled;
    char host[64];
    int  port;
    char user[33];
    char pass[A2K_MQTT_PASS_MAX + 1];
    char client_id[33];
    char base[65];
    char prefix[33];
    int  discovery;
    int  interval;
} a2k_mqtt_cfg_t;

/* 0 and *out filled (defaults for absent keys; base and client_id stay "" when absent: they depend on the host name),
 * -1 invalid (*out untouched). Never prints anything. */
int a2k_mqtt_cfg_parse(const char *text, size_t len, a2k_mqtt_cfg_t *out);
/* Password file: 0 and out filled (maybe "" when the file is absent or empty), -1 unreadable / not a regular file / too long /
 * control character (out is ""). Mirrors mqtt_pass_load of the 1080p except the file mode check (FAT card). */
int a2k_mqtt_pass_load(const char *path, char *out, size_t cap);
/* "<directory of conf_path>/mqtt_pass" into out. 0, -1 when it does not fit. */
int a2k_mqtt_pass_path(const char *conf_path, char *out, size_t cap);
/* Reads and parses the file. 0 valid, 1 absent (*out zeroed: MQTT off), -1 unreadable or invalid (*out zeroed). */
int a2k_mqtt_cfg_load(const char *path, a2k_mqtt_cfg_t *out);
/* One-line description for the log: host, port, whether a user and a password are set. NEVER contains the password
 * or the user name. */
void a2k_mqtt_cfg_describe(const a2k_mqtt_cfg_t *c, char *out, size_t cap);
/* Zeroes the secret (volatile, not optimised away). */
void a2k_mqtt_cfg_wipe(a2k_mqtt_cfg_t *c);
/* Reads a MAC address file ("aa:bb:cc:dd:ee:ff\n", lower case) into mac[18]. 0 or -1 (absent, malformed). */
int a2k_mqtt_mac_load(const char *path, char mac[18]);

/* ---- identifiers and topics ---- */
/* Controls of person tracking, the zones and the push button (only with ids->ctl), after the PTZ entities. Index k of the ctl group (the
 * callbacks of mqtt_bind.h use it): 0 `track` switch (ON | OFF), 1 `zones` text and 2 `ignore_zones` text (zones.h: "name=x1,y1,x2,y2;...",
 * zone_mode=all|any in the first; "none" or empty clears), 3 `track_area_new` text (a new tracking area name: command only), 4 `track_areas`
 * sensor (read only), 5 `pan_only` switch (ON = the tracker never moves the tilt), 6 `image_flip`, 7 `image_mirror`, 8 `ceiling_mount` switches (the picture orientation, orient.h), then per tracking-area SLOT s (0..A2K_CTL_SLOTS-1, exists while s < ntarea) four entities: `tarea<s>` switch (enabled), buttons
 * `tarea<s>_a` / `tarea<s>_b` (set corner A / B to the current position) and `tarea<s>_del`. Command topic BASE/<id>/set (button payload exactly
 * PRESS, switch ON / OFF, text: [0-9a-z_=,;:.@-], at most A2K_CTL_VAL - 1 bytes), state BASE/<id>/state. The `button` EVENT entity (event_type
 * press) publishes {"event_type":"press"} on BASE/button/event (not retained). */
#define A2K_CTL_TRACK 0
#define A2K_CTL_ZONES 1
#define A2K_CTL_IGNORE 2
#define A2K_CTL_NEWAREA 3
#define A2K_CTL_AREAS 4
#define A2K_CTL_PANONLY 5 /* switch: ON = pan only tracking (track.tilt 0: the tilt never moves), OFF = pan and tilt */
#define A2K_CTL_FLIP 6    /* switch: ON = the picture is flipped vertically (settings row image.flip) */
#define A2K_CTL_MIRROR 7  /* switch: ON = the picture is mirrored horizontally (image.mirror) */
#define A2K_CTL_CEILING 8 /* switch: ON = flip and mirror both on (ceiling mount, image.ceiling); reads ON only while both are */
#define A2K_CTL_SLOT0 9
#define A2K_CTL_SLOTS 4
#define A2K_CTL_PER_SLOT 4
#define A2K_CTL_N (A2K_CTL_SLOT0 + A2K_CTL_SLOTS * A2K_CTL_PER_SLOT)
#define A2K_CTL_VAL 256
enum { A2K_CK_SWITCH, A2K_CK_TEXT, A2K_CK_SENSOR, A2K_CK_BUTTON };
/* The programmable push button (button_rt.h): one Home Assistant DEVICE TRIGGER per configured gesture (at most A2K_BTN_TRIG_N = BC_MAX_BIND of
 * button_cfg.h, names of at most A2K_BTN_NAME - 1 = BG_NAME_MAX - 1 characters), only with ids->ctl. Every gesture is published on BASE/button as
 * {"gesture":"..","actions":[..],"result":".."} (not retained); the trigger of slot k matches on value_template {{ value_json.gesture }}. */
#define A2K_BTN_TRIG_N 16
#define A2K_BTN_NAME 16

typedef struct {
    char base[72], prefix[40], host[33], node[40], mac[18], version[40];
    int  nzones;                                  /* detection zones (zones.h): one binary_sensor person_<name> each */
    char zone[A2K_ZONE_MAX][A2K_ZONE_NAME_MAX + 1];
    int  snapshot;                                /* 1: the JPEG snapshot `image` entity exists (snap.h) */
    int  night;                                   /* 1: the day/night binary_sensor `night` exists (night_ctl.h) */
    int  sys;                                     /* 1: the diagnostic sensors of hal_sys exist (A2K_E_SYS0..) */
    int  leds;                                    /* 1: the status LED / IR LED selects and the rule texts exist (A2K_E_LED0..), commanded */
    int  ctl;                                     /* 1: the button event, tracking switch, zone texts and tracking-area controls exist (A2K_E_CTL0..) */
    int  ntarea;                                  /* tracking areas (a2k_track): slots 0..ntarea-1 have their four entities */
    char tarea[A2K_CTL_SLOTS][A2K_ZONE_NAME_MAX + 1];
    int  ptz;                                     /* 1: the PTZ buttons and sensors exist (A2K_E_PTZ0..), commanded */
    int  vid;                                     /* 1: the video settings numbers and selects exist (A2K_E_VID0..), commanded */
    int  nbtn;                                    /* configured button gestures (with ids->ctl): slots 0..nbtn-1 have their device trigger */
    char btn[A2K_BTN_TRIG_N][A2K_BTN_NAME];
} a2k_mqtt_ids_t;

/* host: the camera host name; mac "aa:bb:cc:dd:ee:ff" (or "": -2, the node id must be the stable hardware one);
 * cfg->base / cfg->prefix may be "" (defaults akam/<hostname> and homeassistant). 0, -1 (invalid topic), -2 (no MAC). */
int a2k_mqtt_ids_init(a2k_mqtt_ids_t *ids, const a2k_mqtt_cfg_t *cfg, const char *host, const char *mac,
                      const char *version);
/* 1 when s is a usable topic prefix (same rule as mqtt_ha_topic_ok of the 1080p, which this calls). */
int a2k_mqtt_topic_ok(const char *s, size_t maxlen);

enum { A2K_E_PERSON, A2K_E_SCORE, A2K_E_LAST, A2K_E_VIEWERS, A2K_E_VERSION, A2K_E_COUNT };
/* Entities beyond the fixed ones: the snapshot image (only with ids->snapshot) and zone i = A2K_E_ZONE0 + i (i < ids->nzones). */
#define A2K_E_IMAGE A2K_E_COUNT
#define A2K_E_ZONE0 (A2K_E_COUNT + 1)
/* The day/night binary_sensor (only with ids->night), after the last possible zone: ON = night mode (filter out, ISP night). */
#define A2K_E_NIGHT (A2K_E_ZONE0 + A2K_ZONE_MAX)

/* hal_sys diagnostic sensors (only with ids->sys), after the night sensor: cpu %, memory available kB, Wi-Fi dBm
 * (attribute link_quality), uptime s, load 1 min. A value that is HAL_SYS_NA is not published. */
#define A2K_E_SYS0 (A2K_E_NIGHT + 1)
#define A2K_SYS_N 5

/* Status LED / IR LED controls (only with ids->leds), after the sys sensors: entity 0 `status_led` select (on | off | auto),
 * 1 `ir_led` select (auto | off), 2.. one `text` per rule (led_boot led_nowifi led_person led_stream led_night led_idle, a
 * pattern of leds.h). Command topic BASE/<id>/set (exact option / printable pattern text, anything else refused), state
 * BASE/<id>/state (not retained, like every state here; the discovery is). */
#define A2K_E_LED0 (A2K_E_SYS0 + A2K_SYS_N)
#define A2K_LED_N 8
#define A2K_LED_VAL 32

/* PTZ (only with ids->ptz), after the LED controls: seven `button`s ptz_left ptz_right ptz_up ptz_down ptz_home ptz_park ptz_recal (Recalibrate PTZ) (command topic
 * BASE/<id>/set, the payload is exactly PRESS; the index is the A2K_PTZ_* command of ptz.h), the `ptz_state` sensor (home |
 * parked | moved | calibrating | unknown) and the diagnostic sensors ptz_pan / ptz_tilt (steps, only while the position is known). */
#define A2K_E_PTZ0 (A2K_E_LED0 + A2K_LED_N)
#define A2K_PTZ_BTN 7
#define A2K_E_PTZ_STATE (A2K_E_PTZ0 + A2K_PTZ_BTN)
#define A2K_E_PTZ_PAN (A2K_E_PTZ_STATE + 1)
#define A2K_E_PTZ_TILT (A2K_E_PTZ_STATE + 2)
#define A2K_PTZ_N (A2K_PTZ_BTN + 3)

/* The ctl group entity indexes in the A2K_E_* space: after the PTZ entities, then the button event. */
#define A2K_E_CTL0 (A2K_E_PTZ0 + A2K_PTZ_N)
#define A2K_E_BUTTON (A2K_E_CTL0 + A2K_CTL_N)

/* Video settings (only with ids->vid), after the button event: entity k = stream * A2K_VP_N + parameter (video.h): per stream a `number` (config
 * category, box mode, min / max from a2k_vid_range) video<N>_kbps, video<N>_max_kbps (0 = automatic, 1.25 x), video<N>_fps, video<N>_min_qp,
 * video<N>_max_qp, video<N>_gop and a `select` video<N>_rc (vbr | cbr | avbr). Command topic BASE/<id>/set (digits only, or exactly one option),
 * state BASE/<id>/state. A change re-opens that stream's encoder (about 1 to 2 s without video on it). */
#define A2K_E_VID0 (A2K_E_BUTTON + 1)
#define A2K_VID_N (A2K_VID_STREAMS * A2K_VP_N)
/* The button gesture device triggers (only with ids->ctl), after the video entities: slot k = A2K_E_BTN0 + k. */
#define A2K_E_BTN0 (A2K_E_VID0 + A2K_VID_N)

struct a2k_mqtt_ptz {
    int  valid;                  /* 0 until the first report */
    char state[12];
    int  known, pan, tilt;
};

/* What the day/night engine reported last (night_ctl.h distilled). */
struct a2k_mqtt_night {
    int      valid;              /* 0 until the first report */
    int      night;
    int      mode;               /* 0 auto, 1 forced day, 2 forced night */
    int      ir_on;
    int      auto_off;
    unsigned switches;
    int64_t  last_switch_epoch;  /* wall clock seconds, 0 unknown */
    char     why[96];
};

/* What the detection state module reported last (a2k_det_t distilled; box and score are zero while nobody is there,
 * like the state file). */
struct a2k_mqtt_det {
    int     valid;               /* 0 until the first report */
    int     present;
    int     score, cls, x1, y1, x2, y2;
    int     heads, head_score;
    unsigned zmask;              /* bit i: a person is confirmed in zone i */
    int64_t last_on_epoch;       /* wall clock seconds of the last OFF->ON transition, 0 unknown */
    char    status[10];          /* "run", "stopped", "failed" */
    struct a2k_mqtt_night dn;    /* the day/night state, kept across detection reports */
    int      sys_valid;          /* 0 until the first hal_sys sample; kept across detection reports */
    hal_sys_sample_t sys;
    struct a2k_mqtt_ptz ptz;
    int      led_valid;          /* the values below were read from the app */
    char     led[A2K_LED_N][A2K_LED_VAL];
};

/* "BASE/+/set": the one subscription of the controls. */
int a2k_mqtt_led_filter(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
/* 1 when (topic, payload) is a valid command of control *ent (0..A2K_LED_N-1) with the text in val; 0 for anything else (not
 * ours, wrong option, character outside [0-9a-z:], empty or over A2K_LED_VAL - 1 bytes). The app still validates a pattern. */
int a2k_mqtt_led_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *ent,
                         char *val, size_t vcap);

/* 1 when (topic, payload) is the command of a PTZ button: the topic BASE/<id>/set of an existing button AND the payload exactly "PRESS"
 * (anything else is 0; a retained replay is the caller's to ignore): *cmd = 0..A2K_PTZ_BTN-1. */
int a2k_mqtt_ptz_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *cmd);

int a2k_mqtt_ctl_kind(int k);                           /* A2K_CK_* of ctl entity k */
int a2k_mqtt_ctl_exists(const a2k_mqtt_ids_t *ids, int k); /* 1 for k < A2K_CTL_SLOT0, or a slot below ids->ntarea */
/* Discovery topic of ctl entity k even when it does not exist (to clear a retained config): 0 ok, -1. */
int a2k_mqtt_ctl_config_topic(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap);
/* State topic + value of ctl entity k (switch, text, sensor with a state): 1 filled, 0 none (button, command-only text, missing slot). */
int a2k_mqtt_ctl_state(const a2k_mqtt_ids_t *ids, int k, const char *val, char *topic, size_t tcap);
/* 1 when (topic, payload) is a valid command of ctl entity *k (exists, payload of its kind); val gets the payload text. */
int a2k_mqtt_ctl_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *k, char *val,
                         size_t vcap);
/* State topic of video entity k (0..A2K_VID_N-1), 1 filled, 0 none (no such entity or no value). */
int a2k_mqtt_vid_state(const a2k_mqtt_ids_t *ids, int k, const char *val, char *topic, size_t tcap);
/* Discovery topic of video entity k (to clear it too): 0, -1. */
int a2k_mqtt_vid_config_topic(const a2k_mqtt_ids_t *ids, int k, char *topic, size_t tcap);
/* 1 when (topic, payload) is a valid command of a video entity: *k, val filled. A number: digits only, 1 to 5, within the range of the parameter (0 only for the automatic
 * peak rate); the select: exactly vbr, cbr or avbr. The app still checks the whole set (min QP <= max QP, peak >= target). */
int a2k_mqtt_vid_command(const a2k_mqtt_ids_t *ids, const char *topic, size_t tlen, const uint8_t *pl, size_t plen, int *k, char *val, size_t vcap);
/* The gesture publish topic BASE/button and the config topic of trigger slot k (to announce it, or to clear it with an empty retained payload). */
int a2k_mqtt_gesture_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
int a2k_mqtt_btn_config_topic(const a2k_mqtt_ids_t *ids, int k, char *out, size_t cap);
/* The button event: topic BASE/button/event and the JSON payload. */
int a2k_mqtt_button_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
extern const char a2k_mqtt_button_payload[];
/* The config topic of the per-zone sensor `name` (to clear it when the zone goes away). */
int a2k_mqtt_zone_config_topic(const a2k_mqtt_ids_t *ids, const char *name, char *out, size_t cap);

/* "BASE/snapshot": the raw JPEG of the last snapshot (not retained). */
int a2k_mqtt_snapshot_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
/* "BASE/detections": the person boxes (detpub.h JSON), QoS 0, not retained, no Home Assistant discovery. */
int a2k_mqtt_detections_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
/* "BASE/status" */
int a2k_mqtt_status_topic(const a2k_mqtt_ids_t *ids, char *out, size_t cap);
/* Discovery topic and document of entity e (fixed, A2K_E_IMAGE, A2K_E_ZONE0 + i): returns the payload length, -1 when it does not fit. */
int a2k_mqtt_discovery(const a2k_mqtt_ids_t *ids, int e, char *topic, size_t tcap, char *payload, size_t pcap);
/* State of entity e: 1 and topic/value filled, 0 when there is nothing to publish (no report yet, clock unknown).
 * viewers < 0 = unknown. States are never retained. */
int a2k_mqtt_state(const a2k_mqtt_ids_t *ids, int e, const struct a2k_mqtt_det *d, int viewers, char *topic,
                   size_t tcap, char *val, size_t vcap);
/* Attributes (json_attributes_topic) of entity e: person (score, class, box, heads, head_score, detection status,
 * last detection time), viewers (the two stream URLs, never with credentials). 1 or 0 (none for this entity). ip is
 * the local IPv4 address in dotted form ("" = unknown: no viewers attributes). */
int a2k_mqtt_attrs(const a2k_mqtt_ids_t *ids, int e, const struct a2k_mqtt_det *d, const char *ip, int rtsp_port,
                   char *topic, size_t tcap, char *payload, size_t pcap);

#endif
