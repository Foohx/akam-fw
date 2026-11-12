#pragma once
/* Kernel log filter ("ak_rtsp klog"): replaces the stock `klogd -n`.
 *
 * Why: the vendor LED driver prints a line on every brightness write (about 60 lines a minute while the
 * status LED blinks) and the encoder driver prints bursts of identical errors while the network is bad, so
 * the 3 x 200 KB syslog files rotate every few minutes and the useful history is gone. This filter reads
 * the kernel log and forwards it to syslogd with the same facility (kern) and priority, except:
 *
 *   - lines that match the drop list (klog_is_noise) never reach syslog;
 *   - a message that was already forwarded in the last `window` seconds is counted instead of forwarded,
 *     and one summary line per distinct message and per window follows ("message repeated N times ...").
 *     Messages are compared after the kernel/driver counter prefix ("[1234]" or "[ 12.345]") is removed
 *     and every run of digits is replaced by "#", so "sh (101): drop_caches: 3" and "... (102) ..." are
 *     the same message. The first occurrence is forwarded verbatim, immediately. A first occurrence the sink
 *     could not deliver is not remembered as forwarded (the next one is tried again) and is counted; the
 *     count is reported as "N kernel lines lost (syslogd busy)" with the next line that does get through.
 *   - crit, alert and emerg lines (priority <= 2: oops, panic, BUG) are never counted or collapsed: every
 *     one is forwarded. Err (3) is still collapsed: the encoder driver's bursts of identical errors are
 *     err, and they are the reason for the filter.
 *
 * The core below is pure (time and output are injected) so the host test can drive it with a fake clock. */

#include <stddef.h>

#define KLOG_MAX_KEYS  32
#define KLOG_KEY_MAX   120
#define KLOG_TEXT_MAX  240
#define KLOG_WINDOW_S  60

/* Returns 1 when the line was handed to syslogd, 0 when it was lost (socket full or no syslogd). */
typedef int (*klog_emit_fn)(void *user, int prio, const char *text);

typedef struct {
    int      used;
    int      prio;                /* priority of the last occurrence (0..7) */
    unsigned suppressed;          /* occurrences counted since last_emit */
    long     last_emit;           /* seconds, injected clock */
    char     key[KLOG_KEY_MAX];
    char     text[KLOG_TEXT_MAX]; /* last occurrence, without the "<N>" prefix */
} klog_ent_t;

typedef struct {
    klog_emit_fn emit;
    void        *user;
    long         window;
    unsigned long dropped;        /* lines removed by the drop list */
    unsigned long lost;           /* lines the sink could not deliver, not yet reported */
    klog_ent_t   e[KLOG_MAX_KEYS];
} klog_t;

void klog_init(klog_t *k, klog_emit_fn emit, void *user, long window_s);
/* One kernel log line without its newline, optionally starting with "<N>" (as read from /proc/kmsg). */
void klog_feed(klog_t *k, const char *line, long now);
/* Emits the summaries that are due and frees idle entries. */
void klog_tick(klog_t *k, long now);
/* Seconds until the next summary is due (0 = now), or -1 when nothing is pending. */
long klog_next_due(const klog_t *k, long now);
/* Emits every pending summary (end of input). */
void klog_flush(klog_t *k);
/* 1 for lines that are never forwarded (the LED driver's per-write trace). msg has no "<N>" prefix. */
int  klog_is_noise(const char *msg);

/* The reader loop (klog_run.c). kmsg_path: /proc/kmsg; sock_path: /dev/log. Returns 1 when the kernel log
 * cannot be read (the caller falls back to the stock klogd) and 0 at end of input (never on /proc/kmsg).
 * Pending summaries are written out at end of input. */
/* Lines are sent without blocking; when the socket is full (a unix datagram queue holds about 11 messages on
 * this kernel) the sender waits up to 300 ms for room, then drops the line (see klog.c lost). */
int  klog_run(const char *kmsg_path, const char *sock_path, long window_s);
/* "ak_rtsp klog [--kmsg PATH] [--sock PATH] [--window SECONDS]" */
int  klog_main(int argc, char **argv);
