/* Home Assistant side of the MQTT client (specs/07 section 2.2 and 2.3): the entity table, the MQTT discovery
 * documents, the state values and the command allow-list. Pure logic: it reads and writes the camera only through
 * the mqtt_ops_t table, so tests/native/mqtt_ha_test.c and mqtt_svc_test.c run it on the host with a fake camera.
 *
 * Topics (BASE = the configured base topic, default akam/<hostname>; NODE = akam_<mac without colons>, or the
 * hostname when the MAC is unknown):
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
} mqtt_ops_t;

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
    unsigned invalid;                        /* rejected commands, for the stats */
    unsigned applied;
} mqtt_ha_t;

/* base and prefix may be "" (defaults akam/<hostname> and homeassistant). Returns 0, or -1 when a topic would not
 * be valid or would be too long. */
int  mqtt_ha_init(mqtt_ha_t *h, const mqtt_ops_t *ops, const char *base, const char *prefix, int discovery);

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
 * is not present (nothing to publish). */
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
