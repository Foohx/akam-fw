/* MQTT service of ak_rtsp: the optional, outbound-only client that publishes the camera to a broker and takes the
 * allow-listed commands of mqtt_ha.h (specs/mqtt.md). Same on/off wiring as ONVIF (onvif_cfg.h).
 *
 * Config file /etc/config/akam/mqtt, written by akam_mqtt.sh: key=value lines, LF (a trailing CR is tolerated), at
 * most MQTT_CFG_MAX bytes, any order, no repeated key, unknown key = invalid:
 *   enabled=0|1            required
 *   host=NAME              required: a host name or IPv4 address (A-Z a-z 0-9 . -), 1 to 63 characters
 *   port=N                 1 to 65535 (default 1883)
 *   user=NAME              0 to 32 of A-Z a-z 0-9 . _ @ + - (default none; a lone - is refused)
 *   client_id=ID           1 to 32 of A-Z a-z 0-9 _ - (default: the hostname)
 *   base_topic=T           A-Z a-z 0-9 _ - and /, 1 to 64, no leading, trailing or double / (default akam/<hostname>)
 *   discovery=on|off       Home Assistant discovery documents (default on)
 *   discovery_prefix=P     same character rules as base_topic, 1 to 32 (default homeassistant)
 *   interval=N             seconds between sensor publishes, 10 to 300 (default 30)
 * The broker password is NOT in this file: it is the single line of /etc/config/akam/mqtt_pass (mode 0600, at most
 * 128 printable characters), read only when the connection is made, never logged, never published. Absent file = no
 * password. A password file that other users can read is refused (the module stays off with a log line).
 *
 * Absent or invalid file, or enabled=0: OFF. Off creates NO thread, NO socket, NO timer and no pipe. On: one thread
 * (the only one), one outbound TCP connection to host:port (plain TCP, no TLS: step 2b), nothing else.
 *
 * States (GET mqtt.state): off | connecting | connected | error:<reason>. error:* is the reason of the last failed
 * attempt while the thread backs off (5 s doubling to 60 s, spread by 20 %) and retries; reasons: dns, connect,
 * timeout, closed, protocol, refused_protocol, refused_id, refused_unavailable, refused_credentials,
 * refused_not_authorized, refused. The thread never touches the video path; a dead broker costs one failed connect
 * per backoff period. */
#pragma once
#include <stddef.h>

#include "mqtt_ha.h"

#ifndef MQTT_CFG_PATH /* tests point mqtt_cfg_path elsewhere */
#define MQTT_CFG_PATH "/etc/config/akam/mqtt"
#endif
#ifndef MQTT_PASS_PATH
#define MQTT_PASS_PATH "/etc/config/akam/mqtt_pass"
#endif
#define MQTT_CFG_MAX 512
#define MQTT_PASS_MAX 128
#define MQTT_STATE_MAX 40

typedef struct {
    int  enabled;
    char host[64];
    int  port;
    char user[33];
    char client_id[33];
    char base[65];
    char prefix[33];
    int  discovery;
    int  interval;
} mqtt_cfg_t;

extern const char *mqtt_cfg_path;
extern const char *mqtt_pass_path;

/* TEST-ONLY timings: defaults 5000, 60000, 60, 2000, 10000, 10000, 60000 (backoff minimum and cap, MQTT keepalive
 * in seconds, the period of the change scan, how long a PINGREQ or the CONNECT may stay unanswered, how long a
 * session must last before the backoff starts again from its minimum). */
typedef struct { unsigned backoff_min_ms, backoff_max_ms, keepalive_s, tick_ms, ping_reply_ms, connack_ms, stable_ms; } mqtt_svc_tune_t;
extern mqtt_svc_tune_t mqtt_svc_tune;

#ifndef MQTT_NO_DNS
#include <netinet/in.h>
/* TEST-ONLY: replaces the system name resolver (runs in a detached helper thread). NULL in production. */
extern int (*mqtt_svc_resolver)(const char *host, struct in_addr *out);
#endif

/* The camera access (production: mqtt_ops.c). Must be set before the first reload; with none the module stays off. */
void mqtt_svc_set_ops(const mqtt_ops_t *ops);

/* Strict parse of the file content. 0 = valid (*out filled with the defaults for absent keys; base and client_id
 * stay "" when absent, they depend on the hostname), -1 = invalid. */
int mqtt_cfg_parse(const char *text, size_t len, mqtt_cfg_t *out);
/* 0 valid, 1 absent (*out = off), -1 invalid or unreadable (*out = off, one log line). */
int mqtt_cfg_load(mqtt_cfg_t *out);
/* The password file: 0 and *out (maybe "") or -1 (unreadable, too long, control characters, not private). */
int mqtt_pass_load(char *out, size_t cap);

/* Boot and `MQTT reload`: re-read both files and bring the thread to the state they ask for. Fills state
 * (>= MQTT_STATE_MAX). Returns 0 for off, connecting, connected; -1 when the start failed. Thread safe. */
int  mqtt_svc_reload(char *state, size_t cap);
void mqtt_svc_state(char *state, size_t cap);
/* "published=N received=N applied=N rejected=N connects=N" (counters since the thread started). */
void mqtt_svc_stats(char *out, size_t cap);
/* Stop the thread: publishes "offline", sends DISCONNECT, joins. Safe when off. */
void mqtt_svc_stop(void);

/* One-shot connection test with the saved files (works while the service is off, never starts the service): a
 * short-lived thread connects with the client id "<id>-test", waits for CONNACK, disconnects. mqtt_svc_test_start
 * returns 0 (started or already running) or -1 (no valid file, no ops). mqtt_svc_test_result: "idle" (never
 * run), "running", "ok", or "error:<reason>" (reasons as above, plus config). */
int  mqtt_svc_test_start(void);
void mqtt_svc_test_result(char *out, size_t cap);
