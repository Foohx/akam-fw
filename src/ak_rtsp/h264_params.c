#include <string.h>
#include <stdio.h>
#include "h264_params.h"

/* Finds the next Annex-B start code (00 00 01) at or after from, looking only
 * at d[0..len). Returns its index, or len when there is none. */
static size_t next_start_code(const uint8_t *d, size_t len, size_t from)
{
    for (size_t i = from; i + 2 < len; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return i;
    return len;
}

int h264_params_scan(h264_params_t *p, const uint8_t *data, size_t data_len,
                     size_t scan_limit)
{
    int found = 0;
    if (scan_limit > data_len) scan_limit = data_len;

    size_t i = next_start_code(data, scan_limit, 0);
    while (i < scan_limit) {
        size_t start = i + 3;
        if (start >= data_len) break;
        /* Bound the search for the end of this NAL so a large VCL NAL does
         * not make us read the whole (uncached) frame. */
        size_t bound = start + H264_PS_MAX + 4;
        if (bound > data_len) bound = data_len;
        size_t next = next_start_code(data, bound, start);
        bool   have_next = next < bound;
        uint8_t type = data[start] & 0x1F;

        if (type == 7 || type == 8) {
            if (have_next || bound == data_len) {
                size_t end = next;
                while (end > start && data[end - 1] == 0) end--; /* trailing zeros / 4-byte start code */
                if (end > start + 1 && end - start <= H264_PS_MAX) {
                    size_t n = end - start;
                    if (type == 7) {
                        memcpy(p->sps, data + start, n);
                        p->sps_len = n;
                        found |= H264_FOUND_SPS;
                    } else {
                        memcpy(p->pps, data + start, n);
                        p->pps_len = n;
                        found |= H264_FOUND_PPS;
                    }
                }
            }
        } else if (type >= 1 && type <= 5) {
            break; /* first slice: parameter sets always precede it */
        }
        if (!have_next || next >= scan_limit) break;
        i = next;
    }
    return found;
}

bool h264_params_ready(const h264_params_t *p)
{
    return p && p->sps_len >= 4 && p->pps_len >= 1;
}

size_t h264_base64(const uint8_t *in, size_t n, char *out, size_t cap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((n + 2) / 3) + 1;
    if (cap < need) return 0;

    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

size_t h264_fmtp_build(const h264_params_t *p, char *out, size_t cap)
{
    int n;
    if (!h264_params_ready(p)) {
        n = snprintf(out, cap, "packetization-mode=1");
    } else {
        char sps64[4 * ((H264_PS_MAX + 2) / 3) + 1];
        char pps64[4 * ((H264_PS_MAX + 2) / 3) + 1];
        if (!h264_base64(p->sps, p->sps_len, sps64, sizeof(sps64)) ||
            !h264_base64(p->pps, p->pps_len, pps64, sizeof(pps64)))
            return 0;
        n = snprintf(out, cap,
                     "packetization-mode=1;profile-level-id=%02X%02X%02X;"
                     "sprop-parameter-sets=%s,%s",
                     p->sps[1], p->sps[2], p->sps[3], sps64, pps64);
    }
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}
