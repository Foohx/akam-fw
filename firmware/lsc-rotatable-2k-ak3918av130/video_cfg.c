/* See video.h: the pure rules of the video settings (no I/O, no thread, no log). */
#include "video.h"

#include <stdio.h>
#include <string.h>

#include "store.h"

/* lo, hi per stream. QP 10..51 is a conservative H.264 window (the vendor lib refuses an init QP below 10, seen on hardware). */
static const struct { int lo, hi; } g_rng[A2K_VID_STREAMS][A2K_VP_N] = {
    { { 64, 8000 }, { 64, 12000 }, { 1, A2K_VID_FPS_MAX }, { 10, 51 }, { 10, 51 }, { 1, 300 }, { 0, 4 } },
    { { 32, 1000 }, { 32, 1500 }, { 1, A2K_VID_FPS_MAX }, { 10, 51 }, { 10, 51 }, { 1, 300 }, { 0, 4 } },
};
static const char *const g_pn[A2K_VP_N] = { "kbps", "max_kbps", "fps", "min_qp", "max_qp", "gop", "rc" };

int a2k_vid_range(int chn, int p, int *lo, int *hi)
{
    if (chn < 0 || chn >= A2K_VID_STREAMS || p < 0 || p >= A2K_VP_N)
        return -1;
    *lo = g_rng[chn][p].lo;
    *hi = g_rng[chn][p].hi;
    return 0;
}

int a2k_vid_value_ok(int chn, int p, int val)
{
    int lo, hi;
    if (a2k_vid_range(chn, p, &lo, &hi) != 0)
        return 0;
    if (p == A2K_VP_RC)
        return val == A2K_RC_CBR || val == A2K_RC_VBR || val == A2K_RC_AVBR;
    if (p == A2K_VP_MAXKBPS && val == 0)
        return 1;
    return val >= lo && val <= hi;
}

int a2k_vid_eff_max_kbps(const a2k_vid_t *t)
{
    long m = t->v[A2K_VP_MAXKBPS] ? t->v[A2K_VP_MAXKBPS] : t->v[A2K_VP_KBPS] + t->v[A2K_VP_KBPS] / 4;
    return m > 65535 ? 65535 : (int)m;
}

int a2k_vid_valid(int chn, const a2k_vid_t *t)
{
    for (int p = 0; p < A2K_VP_N; p++)
        if (!a2k_vid_value_ok(chn, p, t->v[p]))
            return 0;
    return t->v[A2K_VP_MINQP] <= t->v[A2K_VP_MAXQP] && a2k_vid_eff_max_kbps(t) >= t->v[A2K_VP_KBPS];
}

void a2k_vid_defaults(int chn, int kbps, int gop, a2k_vid_t *out)
{
    memset(out, 0, sizeof *out);
    out->v[A2K_VP_KBPS] = kbps;
    out->v[A2K_VP_MAXKBPS] = 0;
    out->v[A2K_VP_FPS] = 15;
    out->v[A2K_VP_MINQP] = chn == 0 ? 32 : 26; /* sub: 800 kbit/s Main at 640x360 wants a lower QP window than the old 512 Baseline */
    out->v[A2K_VP_MAXQP] = chn == 0 ? 51 : 44;
    out->v[A2K_VP_GOP] = gop;
    out->v[A2K_VP_RC] = A2K_RC_VBR;
}

const char *a2k_vid_rc_name(int rc)
{
    return rc == A2K_RC_VBR ? "vbr" : rc == A2K_RC_CBR ? "cbr" : rc == A2K_RC_AVBR ? "avbr" : "?";
}

int a2k_vid_parse(int p, const char *text, int *out)
{
    int v = 0, d = 0;
    if (!text || p < 0 || p >= A2K_VP_N)
        return -1;
    if (p == A2K_VP_RC) {
        if (!strcmp(text, "vbr"))
            *out = A2K_RC_VBR;
        else if (!strcmp(text, "cbr"))
            *out = A2K_RC_CBR;
        else if (!strcmp(text, "avbr"))
            *out = A2K_RC_AVBR;
        else
            return -1;
        return 0;
    }
    for (const char *s = text; *s; s++) {
        if (*s < '0' || *s > '9' || ++d > 5)
            return -1;
        v = v * 10 + (*s - '0');
    }
    if (!d)
        return -1;
    *out = v;
    return 0;
}

void a2k_vid_key(int chn, int p, char *out, size_t cap) { snprintf(out, cap, "video%d_%s", chn, g_pn[p]); }

int a2k_vid_text(int p, int v, char *out, size_t cap)
{
    int n = p == A2K_VP_RC ? snprintf(out, cap, "%s", a2k_vid_rc_name(v)) : snprintf(out, cap, "%d", v);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

int a2k_vid_parse_file(const char *text, size_t len, const a2k_vid_t base[A2K_VID_STREAMS], a2k_vid_t out[A2K_VID_STREAMS])
{
    a2k_vid_t t[A2K_VID_STREAMS];
    unsigned seen[A2K_VID_STREAMS] = { 0, 0 };
    size_t i = 0;
    int lines = 0;
    if (!text || len > A2K_STORE_MAX)
        return -1;
    memcpy(t, base, sizeof t);
    while (i < len) {
        size_t e = i, n, kl;
        char key[32], val[16];
        int v = 0, found = 0;
        const char *eq;
        while (e < len && text[e] != '\n')
            e++;
        n = e - i;
        eq = n ? memchr(text + i, '=', n) : NULL;
        if (++lines > A2K_VID_STREAMS * A2K_VP_N || !eq || memchr(text + i, '\0', n))
            return -1;
        kl = (size_t)(eq - (text + i));
        if (kl >= sizeof key || n - kl - 1 >= sizeof val)
            return -1;
        memcpy(key, text + i, kl);
        key[kl] = '\0';
        memcpy(val, eq + 1, n - kl - 1);
        val[n - kl - 1] = '\0';
        for (int c = 0; c < A2K_VID_STREAMS && !found; c++)
            for (int p = 0; p < A2K_VP_N && !found; p++) {
                char k[32];
                a2k_vid_key(c, p, k, sizeof k);
                if (strcmp(k, key) != 0)
                    continue;
                if ((seen[c] & (1u << p)) || a2k_vid_parse(p, val, &v) != 0 || !a2k_vid_value_ok(c, p, v))
                    return -1;
                seen[c] |= 1u << p;
                t[c].v[p] = v;
                found = 1;
            }
        if (!found)
            return -1;
        i = e + 1;
    }
    for (int c = 0; c < A2K_VID_STREAMS; c++)
        if (!a2k_vid_valid(c, &t[c]))
            return -1;
    memcpy(out, t, sizeof t);
    return 0;
}

int a2k_vid_format_file(const a2k_vid_t base[A2K_VID_STREAMS], const a2k_vid_t cur[A2K_VID_STREAMS], char *out, size_t cap)
{
    size_t n = 0;
    if (!cap)
        return -1;
    out[0] = '\0';
    for (int c = 0; c < A2K_VID_STREAMS; c++)
        for (int p = 0; p < A2K_VP_N; p++) {
            char k[32], v[16];
            int w;
            if (cur[c].v[p] == base[c].v[p])
                continue;
            a2k_vid_key(c, p, k, sizeof k);
            if (a2k_vid_text(p, cur[c].v[p], v, sizeof v) != 0)
                return -1;
            w = snprintf(out + n, cap - n, "%s=%s\n", k, v);
            if (w < 0 || (size_t)w >= cap - n)
                return -1;
            n += (size_t)w;
        }
    return (int)n;
}

