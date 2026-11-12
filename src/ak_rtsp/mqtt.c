/* See mqtt.h. */
#include "mqtt.h"

#include <string.h>

/* ------------------------------------------------------------------------ remaining length */

int mqtt_varlen_enc(uint8_t *out, size_t cap, uint32_t v)
{
    int n = 0;
    if (v > MQTT_VARLEN_MAX) return -1;
    do {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        if ((size_t)n >= cap) return -1;
        out[n++] = b;
    } while (v);
    return n;
}

int mqtt_varlen_dec(const uint8_t *in, size_t len, uint32_t *v)
{
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        if ((size_t)i >= len) return 0;
        val |= (uint32_t)(in[i] & 0x7f) << (7 * i);
        if (!(in[i] & 0x80)) {
            if (i > 0 && in[i] == 0) return -1;     /* not the shortest form (0x80 0x00 for 0, ...) */
            *v = val;
            return i + 1;
        }
    }
    return -1;                                      /* a fifth byte would be needed */
}

/* ------------------------------------------------------------------------ strings */

int mqtt_utf8_ok(const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    if (n > MQTT_STR_MAX) return 0;
    while (i < n) {
        uint8_t c = p[i];
        uint32_t cp;
        size_t extra;
        if (c == 0) return 0;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; cp = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; cp = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; cp = c & 0x07; }
        else return 0;
        if (i + extra >= n) return 0;                  /* truncated sequence */
        for (size_t k = 1; k <= extra; k++) {
            if ((p[i + k] & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if (extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) return 0;
        if (extra == 3 && (cp < 0x10000 || cp > 0x10ffff)) return 0;
        i += extra + 1;
    }
    return 1;
}

int mqtt_topic_name_ok(const char *s, size_t n)
{
    if (n == 0 || !mqtt_utf8_ok(s, n)) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '+' || s[i] == '#') return 0;
    return 1;
}

unsigned mqtt_backoff_ms(unsigned attempt, uint32_t rnd, unsigned min_ms, unsigned max_ms)
{
    uint64_t d = min_ms;
    for (unsigned i = 0; i < attempt && d < max_ms; i++) d *= 2;
    if (d > max_ms) d = max_ms;
    /* spread: d * (80 .. 120) / 100, then clamp so the cap holds */
    d = d * (80 + (rnd % 41)) / 100;
    if (d > max_ms) d = max_ms;
    if (d < (uint64_t)min_ms * 4 / 5) d = (uint64_t)min_ms * 4 / 5;
    return (unsigned)d;
}

/* ------------------------------------------------------------------------ encoders */

typedef struct { uint8_t *b; size_t cap, n; int bad; } wr_t;

static void w_u8(wr_t *w, unsigned v)
{
    if (w->n >= w->cap) { w->bad = 1; return; }
    w->b[w->n++] = (uint8_t)v;
}

static void w_u16(wr_t *w, unsigned v) { w_u8(w, v >> 8); w_u8(w, v & 0xff); }

static void w_raw(wr_t *w, const void *p, size_t n)
{
    if (w->bad || n > w->cap - w->n) { w->bad = 1; return; }
    if (n) memcpy(w->b + w->n, p, n);
    w->n += n;
}

static void w_str(wr_t *w, const char *s)
{
    size_t n = strlen(s);
    if (n > MQTT_STR_MAX) { w->bad = 1; return; }
    w_u16(w, (unsigned)n);
    w_raw(w, s, n);
}

/* Assemble header byte + remaining length + body (already built in body[0..blen)). */
static int finish(uint8_t *out, size_t cap, unsigned first, const uint8_t *body, size_t blen)
{
    uint8_t vl[4];
    int vn = mqtt_varlen_enc(vl, sizeof(vl), (uint32_t)blen);
    if (vn < 0 || 1 + (size_t)vn + blen > cap) return -1;
    out[0] = (uint8_t)first;
    memcpy(out + 1, vl, (size_t)vn);
    if (blen) memmove(out + 1 + vn, body, blen);
    return 1 + vn + (int)blen;
}

/* Bodies are built at out + 5 (room for the longest fixed header), then moved down by finish(). */
#define BODY_OFF 5

int mqtt_enc_connect(uint8_t *out, size_t cap, const mqtt_conn_t *c)
{
    int has_user = c->user && c->user[0], has_pass = c->pass && c->pass[0];
    if (cap <= BODY_OFF || !c->client_id || !c->client_id[0]) return -1;
    if (has_pass && !has_user) return -1;
    if (c->will_topic && !mqtt_topic_name_ok(c->will_topic, strlen(c->will_topic))) return -1;
    if (!mqtt_utf8_ok(c->client_id, strlen(c->client_id))) return -1;
    if (has_user && !mqtt_utf8_ok(c->user, strlen(c->user))) return -1;
    wr_t w = { out + BODY_OFF, cap - BODY_OFF, 0, 0 };
    unsigned fl = (c->clean ? 0x02 : 0) | (c->will_topic ? 0x04 | (c->will_retain ? 0x20 : 0) : 0) |
                  (has_user ? 0x80 : 0) | (has_pass ? 0x40 : 0);
    w_str(&w, "MQTT");
    w_u8(&w, 4);
    w_u8(&w, fl);
    w_u16(&w, c->keepalive);
    w_str(&w, c->client_id);
    if (c->will_topic) {
        const char *m = c->will_msg ? c->will_msg : "";
        w_str(&w, c->will_topic);
        w_u16(&w, (unsigned)strlen(m));             /* will message: binary data with a 2-byte length */
        w_raw(&w, m, strlen(m));
    }
    if (has_user) w_str(&w, c->user);
    if (has_pass) {                                 /* password: binary data, any bytes */
        size_t pl = strlen(c->pass);
        if (pl > MQTT_STR_MAX) return -1;
        w_u16(&w, (unsigned)pl);
        w_raw(&w, c->pass, pl);
    }
    if (w.bad) return -1;
    return finish(out, cap, MQTT_CONNECT << 4, out + BODY_OFF, w.n);
}

int mqtt_enc_publish(uint8_t *out, size_t cap, const char *topic, const void *payload, size_t plen, int retain)
{
    size_t tl = topic ? strlen(topic) : 0;
    if (cap <= BODY_OFF || !mqtt_topic_name_ok(topic ? topic : "", tl)) return -1;
    wr_t w = { out + BODY_OFF, cap - BODY_OFF, 0, 0 };
    w_u16(&w, (unsigned)tl);
    w_raw(&w, topic, tl);
    w_raw(&w, payload, plen);
    if (w.bad) return -1;
    return finish(out, cap, (MQTT_PUBLISH << 4) | (retain ? 1 : 0), out + BODY_OFF, w.n);
}

int mqtt_enc_subscribe(uint8_t *out, size_t cap, uint16_t pid, const char *filter)
{
    size_t fl = filter ? strlen(filter) : 0;
    if (cap <= BODY_OFF || pid == 0 || fl == 0 || !mqtt_utf8_ok(filter, fl)) return -1;
    wr_t w = { out + BODY_OFF, cap - BODY_OFF, 0, 0 };
    w_u16(&w, pid);
    w_u16(&w, (unsigned)fl);
    w_raw(&w, filter, fl);
    w_u8(&w, 0);                                    /* requested QoS 0 */
    if (w.bad) return -1;
    return finish(out, cap, (MQTT_SUBSCRIBE << 4) | 2, out + BODY_OFF, w.n);
}

int mqtt_enc_puback(uint8_t *out, size_t cap, uint16_t pid)
{
    if (cap < 4) return -1;
    out[0] = MQTT_PUBACK << 4;
    out[1] = 2;
    out[2] = (uint8_t)(pid >> 8);
    out[3] = (uint8_t)(pid & 0xff);
    return 4;
}

int mqtt_enc_pingreq(uint8_t *out, size_t cap)
{
    if (cap < 2) return -1;
    out[0] = MQTT_PINGREQ << 4;
    out[1] = 0;
    return 2;
}

int mqtt_enc_disconnect(uint8_t *out, size_t cap)
{
    if (cap < 2) return -1;
    out[0] = MQTT_DISCONNECT << 4;
    out[1] = 0;
    return 2;
}

/* ------------------------------------------------------------------------ decoder */

static int flags_ok(unsigned type, unsigned flags)
{
    switch (type) {
    case MQTT_PUBLISH: return 1;
    case 6: case MQTT_SUBSCRIBE: case MQTT_UNSUBSCRIBE: return flags == 2;
    default: return flags == 0;
    }
}

int mqtt_frame(const uint8_t *buf, size_t len, size_t max_body, mqtt_pkt_t *p)
{
    uint32_t rl;
    unsigned type;
    int vn;
    if (len < 1) return 0;
    type = buf[0] >> 4;
    if (type == 0 || type == 15 || !flags_ok(type, buf[0] & 0x0f)) return -1;
    vn = mqtt_varlen_dec(buf + 1, len - 1, &rl);
    if (vn < 0) return -1;
    if (vn == 0) return 0;
    p->type = (uint8_t)type;
    p->flags = buf[0] & 0x0f;
    p->body = buf + 1 + vn;
    p->blen = rl;
    p->total = 1 + (size_t)vn + (size_t)rl;
    if (rl > max_body) return -2;
    if (len < p->total) return 0;
    return (int)p->total;
}

int mqtt_dec_connack(const mqtt_pkt_t *p, int *session_present, int *rc)
{
    if (p->type != MQTT_CONNACK || p->blen != 2 || (p->body[0] & 0xfe)) return -1;
    *session_present = p->body[0] & 1;
    *rc = p->body[1];
    return 0;
}

int mqtt_dec_suback(const mqtt_pkt_t *p, uint16_t *pid, int *rc)
{
    if (p->type != MQTT_SUBACK || p->blen != 3) return -1;
    *pid = (uint16_t)((p->body[0] << 8) | p->body[1]);
    *rc = p->body[2];
    if (*rc != 0 && *rc != 1 && *rc != 2 && *rc != 0x80) return -1;
    return 0;
}

int mqtt_dec_puback(const mqtt_pkt_t *p, uint16_t *pid)
{
    if (p->type != MQTT_PUBACK || p->blen != 2) return -1;
    *pid = (uint16_t)((p->body[0] << 8) | p->body[1]);
    return 0;
}

int mqtt_dec_publish(const mqtt_pkt_t *p, mqtt_pub_t *out)
{
    size_t tl, off;
    if (p->type != MQTT_PUBLISH || p->blen < 2) return -1;
    out->qos = (p->flags >> 1) & 3;
    out->retain = p->flags & 1;
    out->dup = (p->flags >> 3) & 1;
    if (out->qos == 3) return -1;
    tl = (size_t)((p->body[0] << 8) | p->body[1]);
    off = 2 + tl;
    if (off > p->blen) return -1;
    out->topic = (const char *)p->body + 2;
    out->tlen = tl;
    if (!mqtt_topic_name_ok(out->topic, tl)) return -1;
    out->pid = 0;
    if (out->qos > 0) {
        if (off + 2 > p->blen) return -1;
        out->pid = (uint16_t)((p->body[off] << 8) | p->body[off + 1]);
        if (out->pid == 0) return -1;
        off += 2;
    }
    out->payload = p->body + off;
    out->plen = p->blen - off;
    return 0;
}
