/* Dead-man LEASE of a continuous PTZ command (pure: the clock is passed in, no I/O, no global state). A velocity command is only valid for a lease (150..2000 ms,
 * the ONVIF Timeout or the tracker's lease_ms): when no command arrives (an identical repeat counts) within that time the motion must stop by itself. The same value
 * is used as the HOLD of a manual command over tracking: while it is live the tracker's commands are refused. Boundary rule: a lease armed at t with length L is live at
 * every instant up to and INCLUDING t + L, expired from t + L + 1 (strictly more than L ms since the last command). */
#ifndef AKAM_PTZ_LEASE_H
#define AKAM_PTZ_LEASE_H
#include <stdint.h>

typedef struct {
    int armed;
    int64_t armed_ms;
    int lease_ms;
} ptzl_t;

void ptzl_clear(ptzl_t *l);
/* A command (or an identical repeat) arrived at now_ms with a lease of lease_ms: restarts the lease. A non positive lease_ms clears it. */
void ptzl_arm(ptzl_t *l, int64_t now_ms, int lease_ms);
/* 1 while armed and not expired at now_ms. A clock that went backwards (now < armed) counts as live (never a spurious expiry). */
int ptzl_live(const ptzl_t *l, int64_t now_ms);
/* 1 when armed and expired (the dead-man fires); 0 when not armed or still live. */
int ptzl_expired(const ptzl_t *l, int64_t now_ms);
/* Milliseconds left (0 when expired or not armed). */
int ptzl_remaining_ms(const ptzl_t *l, int64_t now_ms);

#endif
