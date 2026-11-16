/* akam2k_sntp_core.c -- see akam2k_sntp_core.h. */
#include "akam2k_sntp_core.h"

#include <string.h>

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static uint64_t get64(const uint8_t *p)
{
    return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

void sntp_build_request(uint8_t pkt[SNTP_PKT_LEN], uint64_t xmt_raw)
{
    memset(pkt, 0, SNTP_PKT_LEN);
    pkt[0] = (uint8_t)((4 << 3) | 3); /* LI 0, VN 4, mode 3 (client) */
    put64(pkt + 40, xmt_raw);
}

enum sntp_err sntp_parse_reply(const uint8_t *pkt, size_t len, uint64_t sent_raw, struct sntp_reply *out)
{
    unsigned li, vn, mode;
    if (len < SNTP_PKT_LEN)
        return SNTP_E_SHORT;
    li = pkt[0] >> 6;
    vn = (pkt[0] >> 3) & 7u;
    mode = pkt[0] & 7u;
    if (vn != 3 && vn != 4)
        return SNTP_E_VERSION;
    if (mode != 4)
        return SNTP_E_MODE;
    if (li == 3)
        return SNTP_E_LEAP;
    if (pkt[1] < 1 || pkt[1] > 15)
        return SNTP_E_STRATUM;
    if (get64(pkt + 24) != sent_raw)
        return SNTP_E_ORIGIN;
    out->rx_raw = get64(pkt + 32);
    out->tx_raw = get64(pkt + 40);
    out->stratum = pkt[1];
    if (out->tx_raw == 0)
        return SNTP_E_XMT_ZERO;
    if (out->rx_raw == 0)
        return SNTP_E_RX_ZERO;
    return SNTP_OK;
}

uint64_t sntp_us_to_ntp(int64_t unix_us)
{
    int64_t s = unix_us / 1000000, us = unix_us % 1000000;
    if (us < 0) {
        us += 1000000;
        s--;
    }
    return ((uint64_t)(uint32_t)(s + SNTP_UNIX_TO_NTP) << 32) | (uint32_t)(((uint64_t)us << 32) / 1000000u);
}

int sntp_ntp_to_us(uint64_t raw, int64_t *unix_us)
{
    int64_t sec = (int64_t)(raw >> 32);
    uint32_t frac = (uint32_t)raw;
    if (sec < 0x80000000LL)
        sec += 0x100000000LL; /* era 1: 2036..2104 */
    sec -= SNTP_UNIX_TO_NTP;
    if (sec < 0)
        return -1;
    *unix_us = sec * 1000000 + (int64_t)(((uint64_t)frac * 1000000u) >> 32);
    return 0;
}

void sntp_offset(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t *offset_us, int64_t *delay_us)
{
    *offset_us = ((t2 - t1) + (t3 - t4)) / 2;
    *delay_us = (t4 - t1) - (t3 - t2);
}

int sntp_target(int64_t now_us, int64_t offset_us, int32_t *sec, int32_t *usec)
{
    int64_t t = now_us + offset_us, s = t / 1000000, us = t % 1000000;
    if (us < 0) {
        us += 1000000;
        s--;
    }
    if (s < SNTP_MIN_UNIX || s > 0x7fffffffLL)
        return -1;
    *sec = (int32_t)s;
    *usec = (int32_t)us;
    return 0;
}
