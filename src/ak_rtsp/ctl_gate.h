#pragma once
/* Pure logic of the control port (8091) read-only gate: who may write, the DEBUG
 * flag with its auto-off, the client slots. No sockets, no clock of its own (the
 * caller passes a monotonic time in ms), so tests/native/control_test.c runs it
 * on the host. See specs/07-spec-standard-protocols.md section 4. */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define CTL_MAX_CLIENTS      4
#define CTL_DEBUG_DEF_MIN    60
#define CTL_DEBUG_MAX_MIN    1440
#define CTL_READONLY_MSG     "ERR read-only mode, see akam_debug.sh"

/* 1 when the peer is 127.0.0.0/8 (or ::1, or an IPv4-mapped 127/8), else 0. */
int ctl_peer_is_loopback(const struct sockaddr *sa);

/* Writes (SET, SAVE, RESET) are allowed from loopback, or from anyone while debug is on. */
int ctl_write_allowed(int is_loopback, int debug_on);

/* 1 for the commands that change state (case-insensitive). */
int ctl_is_write_cmd(const char *cmd);

/* RAM-only debug flag, off at start. Time is a monotonic millisecond count. */
typedef struct {
    int     on;
    int64_t until_ms;
} ctl_debug_t;

/* 1 while the flag is on; turns it off by itself once now_ms reaches the deadline. */
int ctl_debug_active(ctl_debug_t *d, int64_t now_ms);

/* Seconds left (0 when off). */
int ctl_debug_remaining_s(ctl_debug_t *d, int64_t now_ms);

/* DEBUG command: arg is "on", "off", "status" (case-insensitive), arg2 the optional
 * minutes. Writes the one-line reply in out. A non-loopback caller gets an ERR. */
void ctl_debug_command(ctl_debug_t *d, int64_t now_ms, int is_loopback,
                       const char *arg, const char *arg2, char *out, size_t cap);

/* Client slots (the caller serialises access). */
typedef struct {
    unsigned char used[CTL_MAX_CLIENTS];
} ctl_slots_t;

int ctl_slots_acquire(ctl_slots_t *s);               /* slot index, or -1 when all are busy */
void ctl_slots_release(ctl_slots_t *s, int idx);
int ctl_slots_count(const ctl_slots_t *s);

/* "ak_rtsp ctl <line...>": send the words as one line to 127.0.0.1:port and print the reply
 * lines to out, without the LOG lines the port pushes by itself. Ends 1 s after the last
 * data, 3 s at most. Returns 0 when the first reply line is not an ERR, 1 otherwise (also on
 * no connection or no reply). */
int ctl_client_run(unsigned short port, int argc, char **argv, FILE *out);
