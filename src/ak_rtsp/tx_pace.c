#include "tx_pace.h"
#include <string.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>

void tx_pace_init(tx_pace_t *p) { memset(p, 0, sizeof(*p)); }

size_t tx_pace_base(int kbps)
{
    size_t half_s = kbps > 0 ? (size_t)kbps * 1000u / 8u / 2u : 0;
    return half_s > TX_PACE_MIN_HIGH ? half_s : TX_PACE_MIN_HIGH;
}

size_t tx_pace_high(const tx_pace_t *p, size_t base)
{
    size_t k = p->key_len > TX_PACE_IKEY_CAP ? TX_PACE_IKEY_CAP : p->key_len;
    return base + k;
}

size_t tx_pace_low(size_t base) { return base / 4; }

tx_pace_verdict_t tx_pace_decide(tx_pace_t *p, size_t backlog, size_t frame_len, int is_key, size_t base,
                                 int64_t now_ms, tx_pace_event_t *ev)
{
    tx_pace_event_t none = { 0, 0, 0, 0 };
    tx_pace_event_t *e = ev ? ev : &none;
    *e = none;
    if (is_key) p->key_len = frame_len;

    if (!p->skipping) {
        if (backlog <= tx_pace_high(p, base)) return TX_PACE_SEND;
        p->skipping = 1;
        p->run_skipped = 1;
        e->entered = 1;
        if (!p->have_log || now_ms - p->last_log_ms >= TX_PACE_LOG_MS) {
            p->have_log = 1;
            p->last_log_ms = now_ms;
            p->run_logged = 1;
            e->log_it = 1;
        } else {
            p->run_logged = 0;
        }
        return TX_PACE_SKIP;
    }

    if (is_key && backlog <= tx_pace_low(base)) {
        p->skipping = 0;
        e->left = 1;
        e->run_skipped = p->run_skipped;
        e->log_it = p->run_logged; /* the leave line only closes an enter line that was printed */
        p->run_skipped = 0;
        p->run_logged = 0;
        return TX_PACE_SEND;
    }
    p->run_skipped++;
    return TX_PACE_SKIP;
}

size_t tx_pace_backlog(int fd)
{
    int n = 0;
    if (ioctl(fd, SIOCOUTQ, &n) != 0 || n < 0) return 0;
    return (size_t)n;
}
