/* Dead-man lease of a continuous PTZ command, see ptz_lease.h. */
#include "ptz_lease.h"

void ptzl_clear(ptzl_t *l)
{
    l->armed = 0;
    l->armed_ms = 0;
    l->lease_ms = 0;
}

void ptzl_arm(ptzl_t *l, int64_t now_ms, int lease_ms)
{
    if (lease_ms <= 0) {
        ptzl_clear(l);
        return;
    }
    l->armed = 1;
    l->armed_ms = now_ms;
    l->lease_ms = lease_ms;
}

int ptzl_expired(const ptzl_t *l, int64_t now_ms)
{
    return l->armed && now_ms - l->armed_ms > l->lease_ms;
}

int ptzl_live(const ptzl_t *l, int64_t now_ms)
{
    return l->armed && !ptzl_expired(l, now_ms);
}

int ptzl_remaining_ms(const ptzl_t *l, int64_t now_ms)
{
    int64_t left;
    if (!ptzl_live(l, now_ms))
        return 0;
    left = l->armed_ms + l->lease_ms - now_ms;
    return left < 0 ? 0 : left > l->lease_ms ? l->lease_ms : (int)left;
}
