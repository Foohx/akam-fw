#include "tx_pace.h"
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/sockios.h>

void tx_pace_init(tx_pace_t *p) { memset(p, 0, sizeof(*p)); }

size_t tx_pace_base(int kbps)
{
    size_t half_s = kbps > 0 ? (size_t)kbps * 1000u / 8u / 2u : 0;
    return half_s > TX_PACE_MIN_HIGH ? half_s : TX_PACE_MIN_HIGH;
}

size_t tx_pace_cap(size_t sndbuf_reported) { return sndbuf_reported / 4 * 3; }

int64_t tx_pace_max_skip_ms(int gop_s)
{
    int64_t g = (int64_t)(gop_s > 0 ? gop_s : 0) * 3000;
    return g > 10000 ? g : 10000;
}

size_t tx_pace_low(size_t base) { return base / 4; }

size_t tx_pace_high(const tx_pace_t *p, const tx_pace_cfg_t *c, size_t frame_len)
{
    size_t k = p->key_len > TX_PACE_IKEY_CAP ? TX_PACE_IKEY_CAP : p->key_len;
    size_t hi = c->base + k;
    if (c->cap) {
        size_t lim = c->cap > frame_len ? c->cap - frame_len : 0;
        if (hi > lim) hi = lim;
    }
    if (hi < c->base / 2) hi = c->base / 2;
    return hi;
}

static void enter_skip(tx_pace_t *p, size_t backlog, int64_t now_ms, tx_pace_event_t *e)
{
    p->skipping = 1;
    p->run_skipped = 1;
    p->skip_since_ms = now_ms;
    p->best_backlog = backlog;
    p->best_ms = now_ms;
    e->entered = 1;
    if (!p->have_log || now_ms - p->last_log_ms >= TX_PACE_LOG_MS) {
        p->have_log = 1;
        p->last_log_ms = now_ms;
        p->run_logged = 1;
        e->log_enter = 1;
    } else {
        p->run_logged = 0;
    }
    e->log_it = e->log_enter || e->log_leave;
}

tx_pace_verdict_t tx_pace_no_room(tx_pace_t *p, size_t backlog, int64_t now_ms, tx_pace_event_t *ev)
{
    tx_pace_event_t none;
    memset(&none, 0, sizeof(none));
    tx_pace_event_t *e = ev ? ev : &none; /* *ev is kept: it may hold the "left SKIP" event of this frame's decide */
    if (!p->skipping) enter_skip(p, backlog, now_ms, e);
    else p->run_skipped++;
    return TX_PACE_SKIP;
}

size_t tx_pace_wire_len(size_t frame_len) { return frame_len + frame_len / 64 + 256; }

tx_pace_verdict_t tx_pace_decide(tx_pace_t *p, const tx_pace_cfg_t *c, size_t backlog, size_t frame_len,
                                 size_t room, int is_key, int64_t now_ms, tx_pace_event_t *ev)
{
    tx_pace_event_t none;
    memset(&none, 0, sizeof(none));
    tx_pace_event_t *e = ev ? ev : &none;
    *e = none;
    if (is_key) p->key_len = frame_len;
    size_t low = tx_pace_low(c->base);
    int fits = tx_pace_wire_len(frame_len) <= room;

    if (!p->skipping) {
        size_t high = tx_pace_high(p, c, frame_len);
        size_t limit = is_key && c->base < high ? c->base : high; /* a keyframe needs a shallower backlog */
        int key_ok = is_key && backlog <= low; /* always sendable, even above the buffer size */
        int over = backlog > limit || (c->cap && backlog + frame_len > c->cap && !key_ok) || !fits;
        if (!over) return TX_PACE_SEND;
        enter_skip(p, backlog, now_ms, e);
        e->no_room = !fits;
        return TX_PACE_SKIP;
    }

    e->no_room = !fits;
    if (is_key && backlog <= low && fits) {
        p->skipping = 0;
        e->left = 1;
        e->run_skipped = p->run_skipped;
        e->log_leave = p->run_logged; /* the leave line only closes an enter line that was printed */
        e->log_it = e->log_leave;
        p->run_skipped = 0;
        p->run_logged = 0;
        return TX_PACE_SEND;
    }
    if (backlog < p->best_backlog) {
        p->best_backlog = backlog;
        p->best_ms = now_ms;
    }
    p->run_skipped++;
    if (c->stall_ms > 0 && backlog > low && now_ms - p->best_ms >= c->stall_ms) {
        e->drop_reason = "TCP link stalled while skipping";
        return TX_PACE_DROP_CLIENT;
    }
    if (c->max_skip_ms > 0 && now_ms - p->skip_since_ms >= c->max_skip_ms) {
        e->drop_reason = "video skipped for too long, link cannot carry the stream";
        return TX_PACE_DROP_CLIENT;
    }
    return TX_PACE_SKIP;
}

size_t tx_pace_backlog(int fd)
{
    int n = 0;
    if (ioctl(fd, SIOCOUTQ, &n) != 0 || n < 0) return 0;
    return (size_t)n;
}

size_t tx_pace_sndbuf(int fd)
{
    int v = 0;
    socklen_t l = sizeof(v);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, &l) != 0 || v < 0) return 0;
    return (size_t)v;
}
