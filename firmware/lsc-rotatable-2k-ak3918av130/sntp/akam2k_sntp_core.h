/* akam2k_sntp_core.h -- the pure part of akam2k_sntp (packet build/parse, NTP era handling, offset math).
 * No socket, no clock, no libc call beyond string.h: the host unit test (tests/rot2k_sntp_unit.c) compiles it natively.
 * All times are int64 microseconds since the Unix epoch unless stated; NTP timestamps are carried as raw
 * 64-bit values (seconds since 1900 << 32 | fraction). */
#ifndef AKAM2K_SNTP_CORE_H
#define AKAM2K_SNTP_CORE_H
#include <stddef.h>
#include <stdint.h>

#define SNTP_PKT_LEN 48
#define SNTP_UNIX_TO_NTP 2208988800LL      /* seconds between 1900-01-01 and 1970-01-01 */
#define SNTP_MIN_UNIX 1577836800LL         /* 2020-01-01: a server earlier than this is lying or broken */

enum sntp_err {
    SNTP_OK = 0,
    SNTP_E_SHORT,      /* fewer than 48 bytes */
    SNTP_E_VERSION,    /* version not 3 or 4 */
    SNTP_E_MODE,       /* not mode 4 (server) */
    SNTP_E_LEAP,       /* leap indicator 3: the server clock is not synchronised */
    SNTP_E_STRATUM,    /* 0 (kiss-o'-death) or above 15 */
    SNTP_E_ORIGIN,     /* originate timestamp is not what we sent (stale, spoofed or off-path reply) */
    SNTP_E_XMT_ZERO,   /* transmit timestamp is zero */
    SNTP_E_RX_ZERO     /* receive timestamp is zero (would be read as year 2036 by the era rule) */
};

struct sntp_reply {
    uint64_t rx_raw;   /* server receive timestamp (t2), raw NTP */
    uint64_t tx_raw;   /* server transmit timestamp (t3), raw NTP */
    unsigned stratum;
};

/* Build the 48 byte client request: LI 0, VN 4, mode 3, everything else zero except the transmit timestamp. */
void sntp_build_request(uint8_t pkt[SNTP_PKT_LEN], uint64_t xmt_raw);

/* Validate a reply against the transmit timestamp we sent. */
enum sntp_err sntp_parse_reply(const uint8_t *pkt, size_t len, uint64_t sent_raw, struct sntp_reply *out);

/* Unix microseconds <-> raw NTP. ntp_to_us picks the era by the top bit of the seconds: set = era 0 (1968..2036,
 * must be 1970 or later), clear = era 1 (2036..2104). Returns 0 on success, -1 for a time before 1970. */
uint64_t sntp_us_to_ntp(int64_t unix_us);
int sntp_ntp_to_us(uint64_t raw, int64_t *unix_us);

/* t1 client send, t2 server receive, t3 server transmit, t4 client receive. */
void sntp_offset(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t *offset_us, int64_t *delay_us);

/* Target clock = now + offset, split for settimeofday with a 32-bit time_t. Returns 0, or -1 when it does not fit
 * in a signed 32-bit number of seconds (after 2038-01-19) or is before 2020. */
int sntp_target(int64_t now_us, int64_t offset_us, int32_t *sec, int32_t *usec);
#endif
