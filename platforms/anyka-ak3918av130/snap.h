/* JPEG snapshot on detection, Rotatable 2K own app. When the global person goes ON, ONE JPEG of the sub channel (640x360)
 * is taken, kept in a small ring of files in /tmp and handed to the MQTT module (the Home Assistant `image` entity).
 *
 * Split so that nothing here can hold up capture or RTSP:
 *  - the JPEG is produced by the vendor's JPEG encoder (a third ak_venc_open_ex handle, profile Jpeg) from a frame the sub
 *    capture thread already holds: cap.c polls a2k_snap_pending() once per frame (a volatile read), encodes only
 *    when a snapshot was asked for and hands the bytes to a2k_snap_submit(), which copies them (at most
 *    A2K_SNAP_MAX_JPEG) and returns. No extra frame is pulled, so the sub stream never loses one to a second reader;
 *  - the detection thread only calls a2k_snap_request() (a mutex and two integers);
 *  - one thread of its own (64 KiB stack) writes the ring files and calls a2k_mqtt_snapshot(). A slow /tmp or broker costs
 *    that thread only.
 * Bounded: one request at a time, one JPEG in flight (malloc of its own size, at most 200 KB, freed after use), at most
 * A2K_SNAP_RING files of /tmp/akam2k_snap_<slot>.jpg (a fixed set of names: a restart overwrites them, nothing piles up)
 * and at most A2K_SNAP_TOTAL_MAX bytes in them. Any failure (encoder cannot be opened,
 * three encodes in a row fail, MemAvailable low) is logged once and only ends the snapshots. No vendor call lives here:
 * the unit test (tests/rot2k_snap_unit.c) runs everything with a stub encoder. */
#ifndef AKAM2K_SNAP_H
#define AKAM2K_SNAP_H
#include <stddef.h>
#include <stdint.h>

#define A2K_SNAP_MAX_JPEG (200u << 10)    /* a bigger JPEG is not kept nor published */
#define A2K_SNAP_RING 3                   /* files kept */
#define A2K_SNAP_TOTAL_MAX (600u << 10)   /* bytes in them */
#define A2K_SNAP_DIR "/tmp"
#define A2K_SNAP_W 640
#define A2K_SNAP_H 360

struct a2k_snap_cfg {
    int enabled;       /* 0/1 (default 1) */
    int interval_s;    /* minimum seconds between two snapshots, 5..300 (default 10) */
};
void a2k_snap_defaults(struct a2k_snap_cfg *c);

/* ---- ring bookkeeping (pure) ---- */
typedef struct {
    int n;                                    /* entries, oldest first */
    struct { int slot; uint32_t len; } e[A2K_SNAP_RING];
    unsigned next;                            /* slot of the next entry = next % A2K_SNAP_RING */
    size_t total;
    size_t cap;                               /* byte cap */
} a2k_snapring_t;
void a2k_snapring_init(a2k_snapring_t *r, size_t cap);
/* Registers a new file of len bytes. Returns its slot; *drop = bit mask of OTHER slots whose file must be deleted (evicted
 * to respect the byte cap). The slot itself is overwritten by the caller. */
int a2k_snapring_add(a2k_snapring_t *r, uint32_t len, unsigned *drop);

/* ---- life cycle ---- */
/* Starts the thread. Only call once the JPEG encoder is open. 0, or -1 (logged, snapshots off). */
int a2k_snap_start(const struct a2k_snap_cfg *c);
/* Joins the thread (the caller has set a2k_stop or not: it wakes itself) and frees a JPEG still in flight. Safe when never started. */
void a2k_snap_stop(void);
/* 1 while snapshots are on (configured and not disabled since). */
int a2k_snap_enabled(void);
/* Switches the snapshots off for the rest of the run; logs `why` once. */
void a2k_snap_disable(const char *why);

/* ---- producers ---- */
/* Detection thread, person ON: asks for one snapshot. 1 accepted, 0 refused (off, one already asked for, min interval not
 * elapsed, MemAvailable low). now_ms: monotonic. */
int a2k_snap_request(int64_t now_ms);
/* Capture thread: 1 when a snapshot was asked for and not delivered yet. Cheap. */
int a2k_snap_pending(void);
/* Capture thread: the JPEG of the pending request (copied). A JPEG of 0 bytes or over A2K_SNAP_MAX_JPEG is dropped. */
void a2k_snap_submit(const uint8_t *jpeg, size_t len);
/* Capture thread: the encode of the pending request failed. Three in a row disable the snapshots. */
void a2k_snap_failed(void);

/* Counters for the log: requests accepted, stored, dropped (oversized or empty), failed. */
void a2k_snap_stats(unsigned *requested, unsigned *stored, unsigned *dropped, unsigned *failed);

/* TEST ONLY */
void a2k_snap_test_set_dir(const char *dir);

#endif
