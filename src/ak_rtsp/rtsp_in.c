#include "rtsp_in.h"

#include <string.h>

void rtsp_in_init(rtsp_in_t *in)
{
    memset(in, 0, sizeof(*in));
}

static void drop_front(rtsp_in_t *in, size_t n)
{
    if (n >= in->len) {
        in->len = 0;
        return;
    }
    memmove(in->buf, in->buf + n, in->len - n);
    in->len -= n;
}

static void release_pending(rtsp_in_t *in)
{
    if (in->pending) {
        drop_front(in, in->pending);
        in->pending = 0;
    }
}

char *rtsp_in_space(rtsp_in_t *in, size_t *cap)
{
    release_pending(in);
    *cap = RTSP_IN_CAP - in->len;
    return in->buf + in->len;
}

void rtsp_in_commit(rtsp_in_t *in, size_t n)
{
    in->len += n;
    if (in->len > RTSP_IN_CAP) in->len = RTSP_IN_CAP; /* caller bug */
    if (in->skip) {
        size_t d = in->skip < in->len ? in->skip : in->len;
        drop_front(in, d);
        in->skip -= d;
    }
}

const char *rtsp_in_data(const rtsp_in_t *in, size_t *len)
{
    *len = in->len;
    return in->buf;
}

static int is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int rtsp_in_header_uint(const char *msg, size_t len, const char *name,
                        unsigned long *out)
{
    size_t nl = strlen(name);
    size_t i = 0;
    /* Skip the request line, then walk the header lines. */
    while (i < len && msg[i] != '\n') i++;
    i++;
    while (i < len) {
        size_t e = i;
        while (e < len && msg[e] != '\n') e++;
        if (e - i == 0 || (e - i == 1 && msg[i] == '\r')) break; /* blank */
        if (e - i > nl && msg[i + nl] == ':') {
            size_t k = 0;
            while (k < nl && lower(msg[i + k]) == lower(name[k])) k++;
            if (k == nl) {
                size_t p = i + nl + 1;
                while (p < e && (msg[p] == ' ' || msg[p] == '\t')) p++;
                unsigned long v = 0;
                int digits = 0;
                while (p < e && msg[p] >= '0' && msg[p] <= '9') {
                    if (v > 100000000UL) return -1;
                    v = v * 10 + (unsigned long)(msg[p] - '0');
                    digits++;
                    p++;
                }
                while (p < e && (msg[p] == ' ' || msg[p] == '\t' || msg[p] == '\r')) p++;
                if (!digits || p != e) return -1;
                *out = v;
                return 1;
            }
        }
        i = e + 1;
    }
    return 0;
}

/* Offset of the "\r\n\r\n" ending the header block, or (size_t)-1. */
static size_t find_header_end(const char *b, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return i;
    }
    return (size_t)-1;
}

rtsp_in_kind_t rtsp_in_next(rtsp_in_t *in, rtsp_in_item_t *item)
{
    release_pending(in);

    /* Clients may put a CRLF between items (RFC 2326 section 10.12 note). */
    size_t z = 0;
    while (z < in->len && (in->buf[z] == '\r' || in->buf[z] == '\n')) z++;
    if (z) drop_front(in, z);
    if (in->len == 0) return RTSP_IN_MORE;

    if (in->buf[0] == '$') {
        if (in->len < 4) return RTSP_IN_MORE;
        size_t plen = ((size_t)(uint8_t)in->buf[2] << 8) | (uint8_t)in->buf[3];
        size_t total = 4 + plen;
        if (total > RTSP_IN_CAP) {
            /* Too big to buffer (never legitimate from a client): discard it
             * as it streams in instead of growing or stalling. */
            in->skip = total - in->len;
            in->len = 0;
            in->skipped_frames++;
            return RTSP_IN_MORE;
        }
        if (in->len < total) return RTSP_IN_MORE;
        item->kind = RTSP_IN_INTERLEAVED;
        item->channel = (uint8_t)in->buf[1];
        item->data = in->buf + 4;
        item->len = plen;
        in->pending = total;
        return RTSP_IN_INTERLEAVED;
    }

    if (!is_alpha(in->buf[0])) return RTSP_IN_GARBAGE;

    size_t he = find_header_end(in->buf, in->len);
    if (he == (size_t)-1)
        return in->len >= RTSP_IN_CAP ? RTSP_IN_OVERFLOW : RTSP_IN_MORE;

    size_t hdr = he + 4;
    unsigned long cl = 0;
    if (rtsp_in_header_uint(in->buf, hdr, "Content-Length", &cl) < 0)
        return RTSP_IN_GARBAGE;
    if (cl > RTSP_IN_CAP - hdr) return RTSP_IN_OVERFLOW;
    size_t total = hdr + (size_t)cl;
    if (in->len < total) return RTSP_IN_MORE;

    item->kind = RTSP_IN_MESSAGE;
    item->data = in->buf;
    item->len = total;
    item->channel = 0;
    in->pending = total;
    return RTSP_IN_MESSAGE;
}

rtsp_in_pump_t rtsp_in_pump(rtsp_in_t *in, const rtsp_in_ops_t *ops)
{
    for (;;) {
        rtsp_in_item_t it;
        switch (rtsp_in_next(in, &it)) {
        case RTSP_IN_MORE:
            return RTSP_IN_PUMP_OK;
        case RTSP_IN_OVERFLOW:
            return RTSP_IN_PUMP_OVERFLOW;
        case RTSP_IN_GARBAGE:
            return RTSP_IN_PUMP_GARBAGE;
        case RTSP_IN_MESSAGE:
            if (ops->on_message && ops->on_message(ops->user, it.data, it.len))
                return RTSP_IN_PUMP_STOPPED;
            break;
        case RTSP_IN_INTERLEAVED:
            if (ops->on_interleaved &&
                ops->on_interleaved(ops->user, it.channel,
                                    (const uint8_t *)it.data, it.len))
                return RTSP_IN_PUMP_STOPPED;
            break;
        }
    }
}

int rtsp_rtcp_scan(const uint8_t *data, size_t len, unsigned *rr_count)
{
    int bye = 0;
    unsigned rr = 0;
    size_t off = 0;
    while (off + 4 <= len) {
        if ((data[off] >> 6) != 2) break; /* RTCP version 2 */
        size_t plen = 4 * ((((size_t)data[off + 2] << 8) | data[off + 3]) + 1);
        if (off + plen > len) break;
        if (data[off + 1] == 203) bye = 1;
        else if (data[off + 1] == 201) rr++;
        off += plen;
    }
    if (rr_count) *rr_count = rr;
    return bye;
}
