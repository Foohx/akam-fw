#pragma once
/* Pure logic of the control port (8091) read-only gate: who may write, the DEBUG
 * flag with its auto-off, the client slots. No sockets, no clock of its own (the
 * caller passes a monotonic time in ms), so tests/native/control_test.c runs it
 * on the host. See specs/control-port.md */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define CTL_MAX_CLIENTS      4
#define CTL_MAX_REMOTE       (CTL_MAX_CLIENTS - 1)  /* one slot is kept for loopback peers */
#ifndef CTL_IDLE_LOCAL_S
#define CTL_IDLE_LOCAL_S     900   /* loopback, or any peer while DEBUG is on */
#endif
#ifndef CTL_IDLE_REMOTE_S
#define CTL_IDLE_REMOTE_S    120   /* remote peer, DEBUG off */
#endif
#ifndef CTL_SESSION_REMOTE_S
#define CTL_SESSION_REMOTE_S 3600  /* remote peer, DEBUG off */
#endif
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

/* Seconds without a received byte after which a client is dropped. */
int ctl_idle_limit_s(int is_loopback, int debug_on);

/* Absolute session length in seconds, 0 = none. A remote peer (DEBUG off) is cut after CTL_SESSION_REMOTE_S even if it
 * keeps sending a byte now and then, so it cannot hold one of the CTL_MAX_REMOTE slots for ever. */
int ctl_session_limit_s(int is_loopback, int debug_on);

/* Client slots (the caller serialises access). used[i]: 0 free, 1 loopback peer, 2 remote peer. */
typedef struct {
    unsigned char used[CTL_MAX_CLIENTS];
} ctl_slots_t;

/* Slot index, or -1 when none is available to this peer: a remote peer may hold at most
 * CTL_MAX_REMOTE slots, so one stays free for loopback (the SSH helpers). */
int ctl_slots_acquire(ctl_slots_t *s, int is_loopback);
void ctl_slots_release(ctl_slots_t *s, int idx);
int ctl_slots_count(const ctl_slots_t *s);

/* "ak_rtsp ctl <line...>": send the words as one line to 127.0.0.1:port and print the reply
 * lines to out, without the LOG lines the port pushes by itself. Ends at the "." line of a
 * LIST, or 250 ms after the last data once a reply line arrived (1 s while none did), 3 s at most. Returns 0 when the first reply line is not an ERR, 1 otherwise (also on
 * no connection or no reply). */
int ctl_client_run(unsigned short port, int argc, char **argv, FILE *out);
