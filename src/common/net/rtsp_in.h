#pragma once
/* Input demultiplexer of an RTSP/TCP connection (RFC 2326 section 10.12).
 *
 * The client-to-server byte stream interleaves two kinds of items:
 *   - RTSP requests: text, CRLF CRLF terminated header block plus an optional
 *     Content-Length body;
 *   - interleaved binary frames: '$', channel (1 byte), length (2 bytes,
 *     big endian), payload. While playing over TCP, players send their RTCP
 *     receiver reports (and BYE) this way, every few seconds.
 * Neither can be handed raw to the RTSP request parser: it never consumes a
 * trailing '$' frame on its own, so such frames used to pile up in the read
 * buffer until it was full and the connection was dropped as "disconnected".
 *
 * This module only frames the stream: it hands out one complete item at a
 * time, keeps a bounded buffer, and never needs more space than it has.
 * No SmolRTSP and no hardware: tests/rtsp_in_test.sh. */
#include <stddef.h>
#include <stdint.h>

#define RTSP_IN_CAP 4096 /* largest RTSP request (header block + body) */

typedef struct {
    char buf[RTSP_IN_CAP];
    size_t len;     /* buffered bytes */
    size_t pending; /* bytes of the item last handed out, dropped next call */
    size_t skip;    /* bytes of an oversized interleaved frame still to discard */
    unsigned long skipped_frames; /* oversized frames discarded so far */
} rtsp_in_t;

typedef enum {
    RTSP_IN_MORE,        /* need more bytes */
    RTSP_IN_MESSAGE,     /* data/len: one complete RTSP request */
    RTSP_IN_INTERLEAVED, /* channel, data/len: payload of one '$' frame */
    RTSP_IN_OVERFLOW,    /* a request does not fit in RTSP_IN_CAP */
    RTSP_IN_GARBAGE      /* bytes that are neither '$' frame nor RTSP text */
} rtsp_in_kind_t;

typedef struct {
    rtsp_in_kind_t kind;
    const char *data; /* valid until the next rtsp_in_* call */
    size_t len;
    uint8_t channel;
} rtsp_in_item_t;

void rtsp_in_init(rtsp_in_t *in);

/* Where to recv() next. *cap is always > 0 unless OVERFLOW / GARBAGE was
 * returned (the connection is dropped then): never call recv with length 0. */
char *rtsp_in_space(rtsp_in_t *in, size_t *cap);
/* n bytes were written at the pointer rtsp_in_space() returned. */
void rtsp_in_commit(rtsp_in_t *in, size_t n);

/* Next complete item, or MORE. Call until it returns MORE after each commit. */
rtsp_in_kind_t rtsp_in_next(rtsp_in_t *in, rtsp_in_item_t *item);

/* Buffered bytes, for diagnostics after OVERFLOW / GARBAGE. */
const char *rtsp_in_data(const rtsp_in_t *in, size_t *len);

/* Unsigned integer value of a header ("CSeq", "Content-Length"; name matched
 * case-insensitively) in a message. Returns 1 and sets *out, 0 when the
 * header is absent, -1 when its value is not a number. */
int rtsp_in_header_uint(const char *msg, size_t len, const char *name,
                        unsigned long *out);

/* Callbacks of rtsp_in_pump(); a non-zero return stops the pump. */
typedef struct {
    void *user;
    int (*on_message)(void *user, const char *data, size_t len);
    int (*on_interleaved)(void *user, uint8_t channel, const uint8_t *data,
                          size_t len);
} rtsp_in_ops_t;

typedef enum {
    RTSP_IN_PUMP_OK,       /* everything buffered was delivered, need more */
    RTSP_IN_PUMP_STOPPED,  /* a callback asked to stop */
    RTSP_IN_PUMP_OVERFLOW, /* see RTSP_IN_OVERFLOW */
    RTSP_IN_PUMP_GARBAGE   /* see RTSP_IN_GARBAGE */
} rtsp_in_pump_t;

/* Delivers every complete item currently buffered, in order. */
rtsp_in_pump_t rtsp_in_pump(rtsp_in_t *in, const rtsp_in_ops_t *ops);

/* RTCP (RFC 3550) compound packet scan: 1 when it contains a BYE (PT 203),
 * 0 otherwise, including when it is malformed. *rr_count, when not NULL,
 * receives the number of receiver reports (PT 201) seen. */
int rtsp_rtcp_scan(const uint8_t *data, size_t len, unsigned *rr_count);
