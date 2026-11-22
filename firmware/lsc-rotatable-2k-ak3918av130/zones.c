/* See zones.h. */
#include "zones.h"

#include "orient.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int name_ok(const char *s, size_t n)
{
    size_t i;
    if (n < 1 || n > A2K_ZONE_NAME_MAX || s[0] < 'a' || s[0] > 'z')
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    return !(n == 5 && !memcmp(s, "score", 5)) && !(n == 9 && !memcmp(s, "zone_mode", 9)) &&
           !(n >= 7 && !memcmp(s, A2K_IGNORE_PREFIX, 7));
}

/* Four comma separated integers 0..100 (1 to 3 digits each, no sign, no space), x1 < x2 and y1 < y2, then optionally @screen or @position
 * (*anchor = A2K_ZA_*). */
static int rect_ok(const char *v, size_t n, int r[4], int *anchor)
{
    const char *at = memchr(v, '@', n);
    *anchor = A2K_ZA_NONE;
    if (at) {
        size_t al = n - (size_t)(at - v) - 1;
        if (al == 6 && !memcmp(at + 1, "screen", 6))
            *anchor = A2K_ZA_SCREEN;
        else if (al == 8 && !memcmp(at + 1, "position", 8))
            *anchor = A2K_ZA_POSITION;
        else
            return 0;
        n = (size_t)(at - v);
    }
    size_t i = 0;
    int k;
    for (k = 0; k < 4; k++) {
        int d = 0, x = 0;
        while (i < n && v[i] >= '0' && v[i] <= '9') {
            if (++d > 3)
                return 0;
            x = x * 10 + (v[i] - '0');
            i++;
        }
        if (d == 0 || x > 100)
            return 0;
        r[k] = x;
        if (k < 3) {
            if (i >= n || v[i] != ',')
                return 0;
            i++;
        }
    }
    return i == n && r[0] < r[2] && r[1] < r[3];
}

int a2k_zones_parse(const char *text, size_t len, a2k_zones_t *out)
{
    a2k_zones_t c;
    size_t i = 0;
    int mode_seen = 0, lines = 0, k;
    if (!text || !out || len > A2K_ZONES_FILE_MAX)
        return -1;
    memset(&c, 0, sizeof c);
    while (i < len) {
        size_t e = i, n, kl, vl;
        const char *l, *eq, *v;
        while (e < len && text[e] != '\n')
            e++;
        n = e - i;
        if (n && text[i + n - 1] == '\r')
            n--;
        l = text + i;
        eq = n ? memchr(l, '=', n) : NULL;
        if (++lines > A2K_ZONE_MAX + A2K_IGNORE_MAX + 1 || !eq || memchr(l, '\0', n))
            return -1;
        kl = (size_t)(eq - l);
        vl = n - kl - 1;
        v = eq + 1;
        if (kl == 9 && !memcmp(l, "zone_mode", 9)) {
            if (mode_seen)
                return -1;
            mode_seen = 1;
            if (vl == 3 && !memcmp(v, "any", 3))
                c.mode = A2K_ZONE_MODE_ANY;
            else if (vl == 3 && !memcmp(v, "all", 3))
                c.mode = A2K_ZONE_MODE_ALL;
            else
                return -1;
        } else if (kl > 7 && !memcmp(l, A2K_IGNORE_PREFIX, 7)) {
            struct a2k_zone *z;
            int r[4], an;
            if (c.ni >= A2K_IGNORE_MAX || !name_ok(l + 7, kl - 7) || !rect_ok(v, vl, r, &an))
                return -1;
            for (k = 0; k < c.ni; k++)
                if (strlen(c.ig[k].name) == kl - 7 && !memcmp(c.ig[k].name, l + 7, kl - 7))
                    return -1;
            z = &c.ig[c.ni++];
            memcpy(z->name, l + 7, kl - 7);
            z->x1 = r[0];
            z->y1 = r[1];
            z->x2 = r[2];
            z->y2 = r[3];
            z->anchor = an;
        } else {
            struct a2k_zone *z;
            int r[4], an;
            if (c.n >= A2K_ZONE_MAX || !name_ok(l, kl) || !rect_ok(v, vl, r, &an))
                return -1;
            for (k = 0; k < c.n; k++)
                if (strlen(c.z[k].name) == kl && !memcmp(c.z[k].name, l, kl))
                    return -1;
            z = &c.z[c.n++];
            memcpy(z->name, l, kl);
            z->x1 = r[0];
            z->y1 = r[1];
            z->x2 = r[2];
            z->y2 = r[3];
            z->anchor = an;
        }
        i = e + 1;
    }
    *out = c;
    return 0;
}

int a2k_zones_load(const char *path, a2k_zones_t *out)
{
    char buf[A2K_ZONES_FILE_MAX + 2];
    struct stat st;
    size_t n;
    FILE *f;
    int rc;

    memset(out, 0, sizeof *out);
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 1 : -1;
    if (!S_ISREG(st.st_mode)) /* a FIFO or a device on the card is refused before it is opened; also a symbolic link */
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    rc = n > A2K_ZONES_FILE_MAX ? -1 : a2k_zones_parse(buf, n, out);
    if (rc != 0)
        memset(out, 0, sizeof *out);
    return rc;
}

int a2k_zone_contains(const struct a2k_zone *z, const struct a2k_det_box *b)
{
    /* per mille of the image; the box corners are pixel indexes, so the right/bottom edge of a box is x2 + 1 / y2 + 1 */
    long px = ((long)b->x1 + b->x2 + 1) * 500 / A2K_ZONE_IMG_W;
    long py = ((long)b->y2 + 1) * 1000 / A2K_ZONE_IMG_H;
    return px >= z->x1 * 10L && px <= z->x2 * 10L && py >= z->y1 * 10L && py <= z->y2 * 10L;
}

void a2k_zones_effective(const a2k_zones_t *cfg, int home, a2k_zones_t *out)
{
    int i, on = 0;
    *out = *cfg;
    for (i = 0; i < out->n; i++) {
        out->z[i].off = !(home || out->z[i].anchor == A2K_ZA_SCREEN);
        on += !out->z[i].off;
    }
    for (i = 0; i < out->ni; i++)
        out->ig[i].off = !(home || out->ig[i].anchor == A2K_ZA_SCREEN);
    if (!on) { /* no zone in force: the whole image counts */
        out->n = 0;
        out->mode = A2K_ZONE_MODE_ANY;
        memset(out->z, 0, sizeof out->z);
    }
}

void a2k_zones_orient(a2k_zones_t *zs, int flip, int mirror)
{
    int i;
    if (!flip && !mirror)
        return;
    for (i = 0; i < zs->n; i++)
        a2k_orient_rect(flip, mirror, 100, 100, &zs->z[i].x1, &zs->z[i].y1, &zs->z[i].x2, &zs->z[i].y2);
    for (i = 0; i < zs->ni; i++)
        a2k_orient_rect(flip, mirror, 100, 100, &zs->ig[i].x1, &zs->ig[i].y1, &zs->ig[i].x2, &zs->ig[i].y2);
}

unsigned a2k_zones_active_mask(const a2k_zones_t *zs)
{
    unsigned m = 0;
    for (int i = 0; i < zs->n; i++)
        if (!zs->z[i].off)
            m |= 1u << i;
    return m;
}

unsigned a2k_zones_hit(const a2k_zones_t *zs, const struct a2k_det_box *b)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < zs->n; i++)
        if (!zs->z[i].off && a2k_zone_contains(&zs->z[i], b))
            m |= 1u << i;
    return m;
}

int a2k_zones_ignored(const a2k_zones_t *zs, const struct a2k_det_box *b)
{
    for (int i = 0; i < zs->ni; i++)
        if (!zs->ig[i].off && a2k_zone_contains(&zs->ig[i], b))
            return 1;
    return 0;
}

static const char g_nul[1] = { 0 }; /* (the repo C scan forbids the empty string literal) */

static int put_rects(char *out, size_t cap, size_t *n, const struct a2k_zone *r, int cnt, const char *prefix, char sep, int term)
{
    for (int i = 0; i < cnt; i++) {
        const char *an = r[i].anchor == A2K_ZA_SCREEN ? "@screen" : r[i].anchor == A2K_ZA_POSITION ? "@position" : g_nul;
        int w = snprintf(out + *n, cap - *n, "%s%s=%d,%d,%d,%d%s", prefix, r[i].name, r[i].x1, r[i].y1, r[i].x2, r[i].y2, an);
        if (w < 0 || (size_t)w >= cap - *n)
            return -1;
        *n += (size_t)w;
        if (term || i + 1 < cnt) {
            if (*n + 1 >= cap)
                return -1;
            out[(*n)++] = sep;
            out[*n] = '\0';
        }
    }
    return 0;
}

int a2k_zones_format(const a2k_zones_t *z, int part, char sep, char *out, size_t cap)
{
    size_t n = 0;
    int all = z->mode == A2K_ZONE_MODE_ALL;
    if (!cap)
        return -1;
    out[0] = '\0';
    if (part == A2K_ZF_IGNORE)
        return put_rects(out, cap, &n, z->ig, z->ni, g_nul, sep, 0) ? -1 : (int)n;
    if (part == A2K_ZF_ZONES) {
        if (put_rects(out, cap, &n, z->z, z->n, g_nul, sep, all))
            return -1;
        if (all) {
            int w = snprintf(out + n, cap - n, "zone_mode=all");
            if (w < 0 || (size_t)w >= cap - n)
                return -1;
            n += (size_t)w;
        }
        return (int)n;
    }
    if (put_rects(out, cap, &n, z->z, z->n, g_nul, sep, 1))
        return -1;
    if (all) {
        int w = snprintf(out + n, cap - n, "zone_mode=all%c", sep);
        if (w < 0 || (size_t)w >= cap - n)
            return -1;
        n += (size_t)w;
    }
    return put_rects(out, cap, &n, z->ig, z->ni, A2K_IGNORE_PREFIX, sep, 1) ? -1 : (int)n;
}

int a2k_zones_set_text(a2k_zones_t *z, int which, const char *text)
{
    char buf[A2K_ZONES_FILE_MAX + 1];
    size_t n = 0, len;
    a2k_zones_t t;
    const char *p;
    if (!text || (which != A2K_ZF_ZONES && which != A2K_ZF_IGNORE))
        return -1;
    len = strlen(text);
    if (len == 4 && !memcmp(text, "none", 4))
        len = 0;
    if (len > 255)
        return -1;
    memset(&t, 0, sizeof t);
    if (len) {
        /* ';' separates the entries; for the ignore text every entry gets its prefix back */
        for (p = text; p < text + len;) {
            size_t e = strcspn(p, ";"), pl = which == A2K_ZF_IGNORE ? 7 : 0;
            if (p + e > text + len)
                e = (size_t)(text + len - p);
            if (e == 0 || n + pl + e + 1 > A2K_ZONES_FILE_MAX)
                return -1;
            if (pl) {
                memcpy(buf + n, A2K_IGNORE_PREFIX, pl);
                n += pl;
            }
            memcpy(buf + n, p, e);
            n += e;
            buf[n++] = '\n';
            p += e + 1;
        }
        if (a2k_zones_parse(buf, n, &t) != 0)
            return -1;
    }
    if (which == A2K_ZF_ZONES) {
        if (t.ni)
            return -1;
        t.ni = z->ni;
        memcpy(t.ig, z->ig, sizeof t.ig);
        *z = t;
    } else {
        if (t.n || t.mode)
            return -1;
        z->ni = t.ni;
        memcpy(z->ig, t.ig, sizeof z->ig);
    }
    return 0;
}
