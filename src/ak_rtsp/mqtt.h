/* Minimal MQTT 3.1.1 packet codec (specs/07 section 2). Pure: no sockets, no clock, no allocation, no globals, so
 * tests/native/mqtt_codec_test.c runs it on the host with golden bytes and malformed input.
 *
 * What the camera needs and nothing more: it is a client that connects (with a will), publishes at QoS 0 (retained
 * or not), subscribes at QoS 0, answers a QoS 1 delivery with PUBACK, pings and disconnects. Everything else a broker
 * may send is framed and then ignored by the caller.
 *
 * Encoders write a whole packet into out[cap] and return its length, or -1 when it does not fit or an argument is
 * not valid MQTT (a string over 65535 bytes, a topic with a wildcard or a NUL, malformed UTF-8, a password without a
 * user name). They never write past cap. The ones with a variable body (connect, publish, subscribe) build the body
 * after the longest possible fixed header and then move it down, so cap must be at least the packet length plus 4.
 *
 * mqtt_frame() cuts one packet out of a receive buffer: it is the only place that looks at the fixed header, so the
 * "remaining length" rules (1 to 4 bytes, shortest form only, at most 268435455) are enforced in one spot. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    MQTT_CONNECT = 1, MQTT_CONNACK = 2, MQTT_PUBLISH = 3, MQTT_PUBACK = 4, MQTT_SUBSCRIBE = 8, MQTT_SUBACK = 9,
    MQTT_UNSUBSCRIBE = 10, MQTT_UNSUBACK = 11, MQTT_PINGREQ = 12, MQTT_PINGRESP = 13, MQTT_DISCONNECT = 14
};

#define MQTT_VARLEN_MAX 268435455u
#define MQTT_STR_MAX 65535u

/* Remaining length: encode (1 to 4 bytes, returns the count or -1 when v is too large or cap too small) and decode
 * (returns the byte count, 0 when the input ends inside the number, -1 when it is longer than 4 bytes or not in its
 * shortest form, for example 0x80 0x00). */
int mqtt_varlen_enc(uint8_t *out, size_t cap, uint32_t v);
int mqtt_varlen_dec(const uint8_t *in, size_t len, uint32_t *v);

/* 1 when s[0..n) is a valid MQTT string: well-formed UTF-8 (shortest forms, no surrogates, at most U+10FFFF), no
 * U+0000, n <= 65535. */
int mqtt_utf8_ok(const char *s, size_t n);
/* 1 when s may be the topic NAME of a PUBLISH: a valid, non-empty string without '+' and '#'. */
int mqtt_topic_name_ok(const char *s, size_t n);

/* Backoff before the attempt number `attempt` (0 = the first retry): min_ms doubled per attempt, capped at max_ms,
 * then spread by about +-20 % with rnd (any 32-bit value). Never below min_ms * 4 / 5, never above max_ms. */
unsigned mqtt_backoff_ms(unsigned attempt, uint32_t rnd, unsigned min_ms, unsigned max_ms);

typedef struct {
    const char *client_id;       /* required: 1 to 65535 bytes (brokers guarantee 23) */
    const char *user;            /* NULL or "" = none */
    const char *pass;            /* NULL or "" = none; needs a user name */
    uint16_t    keepalive;       /* seconds, 0 = no keepalive */
    int         clean;           /* clean session flag */
    const char *will_topic;      /* NULL = no will */
    const char *will_msg;        /* payload of the will (NUL terminated text) */
    int         will_retain;
} mqtt_conn_t;

int mqtt_enc_connect(uint8_t *out, size_t cap, const mqtt_conn_t *c);
/* QoS 0 PUBLISH (no packet identifier). */
int mqtt_enc_publish(uint8_t *out, size_t cap, const char *topic, const void *payload, size_t plen, int retain);
/* SUBSCRIBE of one topic filter at QoS 0, packet identifier pid (non-zero). */
int mqtt_enc_subscribe(uint8_t *out, size_t cap, uint16_t pid, const char *filter);
int mqtt_enc_puback(uint8_t *out, size_t cap, uint16_t pid);
int mqtt_enc_pingreq(uint8_t *out, size_t cap);
int mqtt_enc_disconnect(uint8_t *out, size_t cap);

typedef struct {
    uint8_t        type;    /* 1..14 (0 and 15 are reserved and refused) */
    uint8_t        flags;   /* low nibble of the first byte */
    const uint8_t *body;    /* after the fixed header, blen bytes */
    size_t         blen;
    size_t         total;   /* the whole packet: fixed header plus body */
} mqtt_pkt_t;

/* Cut one packet from buf[0..len). Returns total (> 0) when complete, 0 when more bytes are needed, -1 when the
 * stream is malformed (reserved type, bad remaining length, wrong fixed-header flags for that type), -2 when the
 * packet is well formed but its body is larger than max_body (p->total is still filled in: the caller skips that
 * many bytes without storing them). */
int mqtt_frame(const uint8_t *buf, size_t len, size_t max_body, mqtt_pkt_t *p);

int mqtt_dec_connack(const mqtt_pkt_t *p, int *session_present, int *rc);
/* *rc is the granted QoS of the one filter, or 0x80 for a refusal. */
int mqtt_dec_suback(const mqtt_pkt_t *p, uint16_t *pid, int *rc);
int mqtt_dec_puback(const mqtt_pkt_t *p, uint16_t *pid);

typedef struct {
    const char    *topic;    /* not NUL terminated */
    size_t         tlen;
    const uint8_t *payload;
    size_t         plen;
    int            qos;      /* 0..2 */
    int            retain;
    int            dup;
    uint16_t       pid;      /* QoS 1 and 2 only */
} mqtt_pub_t;

/* 0 and *out filled, -1 when the body is not a valid PUBLISH (topic length past the body, empty or invalid topic,
 * wildcard in the topic, QoS 3, QoS > 0 without a packet identifier). */
int mqtt_dec_publish(const mqtt_pkt_t *p, mqtt_pub_t *out);
