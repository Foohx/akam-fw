/* Home Assistant side of the MQTT client (specs/mqtt.md): the entity table, the MQTT discovery
 * documents, the state values and the command allow-list. Pure logic: it reads and writes the camera only through
 * the mqtt_ops_t table, so tests/native/mqtt_ha_test.c and mqtt_svc_test.c run it on the host with a fake camera.
 *
 * Topics (BASE = the configured base topic, default akam/<hostname>; NODE = akam_<mac without colons>; with no MAC
 * known mqtt_ha_init() fails with -2 and the caller retries later):
 *   BASE/status                  availability: "online" (retained) / "offline" (retained, also the will)
 *   BASE/<entity>/state          state of the entity (controls retained, sensors not)
 *   BASE/<entity>/set            command topic (select, number, switch, button only)
 *   PREFIX/<component>/NODE/<entity>/config    retained discovery document
 *
 * Command allow-list (decision U9): status LED, IR LEDs, day/night mode, speaker volume, talk enable, beep, and
 * the four picture sliders. Nothing that reboots, changes the network, the hostname or a password exists here: an
 * entity that is not in the table cannot be commanded, whatever topic a publisher uses. Every payload is validated
 * against the entity's own rule (exact option, integer inside its bounds, ON/OFF, PRESS) before the setter runs. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MQTT_HA_MAX_ENT 24
#define MQTT_HA_VAL_MAX 24          /* longest published state text, with the NUL */
#define MQTT_HA_PAYLOAD_MAX 1024    /* longest discovery document the builder may produce */
#define MQTT_HA_TOPIC_MAX 160

/* How the module reaches the camera. get: value text of a key (no "key=" prefix), 0 or -1 when unknown. set: apply
 * a validated value like a loopback 8091 SET; persist 0 keeps it in RAM only. 0 when the answer is OK, else -1
 * with a one-line reason in msg. */
typedef struct {
    int         (*get)(const char *key, char *out, size_t cap);
    int         (*set)(const char *key, const char *value, int persist, char *msg, size_t cap);
    void        (*hostname)(char *out);       /* >= 33 bytes */
    const char *(*version)(void);
    int64_t     (*now_ms)(void);              /* monotonic milliseconds; NULL = no rate limit on saved changes */
} mqtt_ops_t;

/* Command rate limits (all token buckets, evaluated with ops->now_ms; NULL now_ms = no limits):
 *  - every command: burst MQTT_HA_CMD_BURST, refilled MQTT_HA_CMD_PER_S per second. An excess command is counted in
 *    `invalid`, NOT applied and NOT answered with a state publish (so a flood cannot make the camera publish back).
 *  - saved (flash) changes: burst MQTT_HA_PERSIST_BURST, one token back every MQTT_HA_PERSIST_REFILL_MS. A Home
 *    Assistant scene that sets LED + IR + night mode + talk fits in the burst; sustained use is one change per 30 s
 *    (about 2900 flash writes a day at the very most, and a flood is cut to that). A command whose value already is the
 *    current one is applied without touching the setter and costs no token. A refused saved command is counted in
 *    `invalid` and the entity's state is published again so Home Assistant shows the real value.
 *  - beep: at most one per MQTT_HA_BEEP_GAP_MS.
 * The state lives in an mqtt_ha_limits_t (a zeroed one starts full) that survives mqtt_ha_init(), so a reconnect (a new session) does not refill
 * the buckets: the owner passes its own static one with mqtt_ha_use_limits() after every init. */
#define MQTT_HA_CMD_BURST 10
#define MQTT_HA_CMD_PER_S 5
#define MQTT_HA_PERSIST_BURST 4
#define MQTT_HA_PERSIST_REFILL_MS 30000
#define MQTT_HA_BEEP_GAP_MS 2000

typedef struct {
    int64_t cmd_milli, cmd_ms;               /* tokens (x1000) and the time of the last refill */
    int64_t persist_milli, persist_ms;
    int64_t beep_ms;
    int     have_beep;
} mqtt_ha_limits_t;

typedef struct {
    char     base[72];
    char     prefix[40];
    char     host[33];
    char     node[40];
    char     mac[18];                        /* "aa:bb:cc:dd:ee:ff" or "" */
    char     version[40];
    int      discovery;
    const mqtt_ops_t *ops;
    uint8_t  present[MQTT_HA_MAX_ENT];       /* optional entities the camera really has */
    uint8_t  have[MQTT_HA_MAX_ENT];          /* a state was published */
    char     last[MQTT_HA_MAX_ENT][MQTT_HA_VAL_MAX];
    mqtt_ha_limits_t  own_lim;               /* used until mqtt_ha_use_limits() */
    mqtt_ha_limits_t *lim;
    int      reject_idx;                     /* entity of the last rejected command (-1 none): the caller republishes its state */
    unsigned invalid;                        /* rejected commands, for the stats */
    unsigned applied;
} mqtt_ha_t;

/* base and prefix may be "" (defaults akam/<hostname> and homeassistant). Returns 0, -1 when a topic would not
 * be valid or would be too long, -2 when the MAC address is not known yet (retry later). */
int  mqtt_ha_init(mqtt_ha_t *h, const mqtt_ops_t *ops, const char *base, const char *prefix, int discovery);

/* Keep the rate-limit state in *lim (static in the caller) across mqtt_ha_init() calls. Call after every init. */
void mqtt_ha_use_limits(mqtt_ha_t *h, mqtt_ha_limits_t *lim);

/* 1 when s is a usable base topic or prefix: 1 to maxlen of A-Z a-z 0-9 _ - and /, no leading, trailing or double /. */
int mqtt_ha_topic_ok(const char *s, size_t maxlen);

size_t mqtt_ha_count(void);
/* Number of entities that are present on this camera. */
size_t mqtt_ha_present(const mqtt_ha_t *h);
int    mqtt_ha_entity_present(const mqtt_ha_t *h, size_t i);
const char *mqtt_ha_entity_id(size_t i);

/* "BASE/status". Returns the length or -1 when it does not fit. */
int mqtt_ha_status_topic(const mqtt_ha_t *h, char *out, size_t cap);
/* "BASE/+/set": the one subscription that covers every command topic. */
int mqtt_ha_cmd_filter(const mqtt_ha_t *h, char *out, size_t cap);
/* Discovery topic and document of entity i. Returns the payload length, -1 when it does not fit, 0 when the entity
 * is not present: topic is filled and the caller publishes an EMPTY retained payload there to remove a document an
 * earlier run left in the broker (e.g. a Wi-Fi signal entity after the camera went wired). */
int mqtt_ha_discovery(const mqtt_ha_t *h, size_t i, char *topic, size_t tcap, char *payload, size_t pcap);

/* State of entity i: reads the camera and fills topic, text and retain. Returns 1 when there is something to
 * publish (a new value, or force), 0 when unchanged, unknown, or the entity has no state (a button). */
int mqtt_ha_state(mqtt_ha_t *h, size_t i, int force, char *topic, size_t tcap, char *val, size_t vcap, int *retain);
/* 1 when entity i is a sensor-like entity that is only republished every interval (not scanned for changes). */
int mqtt_ha_is_periodic(size_t i);

/* A message received on a topic. Returns the entity index (>= 0) when a command was applied, -1 when it was
 * rejected or is not ours (invalid is counted for anything that matched a command topic), -2 when the topic is not
 * a command topic at all. retained = the broker's retain flag: such a message is never obeyed (it would replay at
 * every reconnect). */
int mqtt_ha_command(mqtt_ha_t *h, const char *topic, size_t tlen, const uint8_t *payload, size_t plen, int retained);
