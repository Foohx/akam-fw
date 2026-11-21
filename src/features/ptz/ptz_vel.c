/* Generic pan/tilt velocity mapping, see ptz_vel.h. */
#include "ptz_vel.h"

int ptzv_parse_milli(const char *s, int *milli)
{
    int neg = 0, digits = 0, frac = 0;
    long whole = 0, f = 0, den = 1, v;
    if (!s)
        return -1;
    if (*s == '-')
        neg = 1, s++;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (whole > 1000)
            return -1; /* way out of range: do not overflow */
        whole = whole * 10 + (*s - '0');
        digits++;
    }
    if (*s == '.') {
        s++;
        for (; *s >= '0' && *s <= '9'; s++, frac++) {
            if (frac >= 4)
                return -1;
            f = f * 10 + (*s - '0');
            den *= 10;
        }
        if (!frac)
            return -1; /* "1." */
    }
    if (*s || !digits)
        return -1;
    v = whole * 1000 + (f * 1000 + den / 2) / den; /* thousandths, rounded to nearest */
    if (v > 1000)
        return -2;
    *milli = (int)(neg ? -v : v);
    return 0;
}

void ptzv_axis(int milli, int *sign, int *divisor)
{
    int a = milli < 0 ? -milli : milli;
    long r, d;
    *sign = 0;
    *divisor = 0;
    if (a < PTZV_DEADZONE_MILLI || a > 1000)
        return;
    *sign = milli < 0 ? -1 : 1;
    /* rate * 950 = 47500 + 450 * (a - 50); divisor = round(1000 / rate) = round(950000 / (rate * 950)) */
    r = 47500 + 450L * (a - PTZV_DEADZONE_MILLI);
    d = (950000L + r / 2) / r;
    if (d < PTZV_DIV_MIN)
        d = PTZV_DIV_MIN;
    if (d > PTZV_DIV_MAX)
        d = PTZV_DIV_MAX;
    *divisor = (int)d;
}

int ptzv_map(int pan_milli, int tilt_milli, ptzv_cmd_t *out)
{
    if (pan_milli > 1000) pan_milli = 1000;
    if (pan_milli < -1000) pan_milli = -1000;
    if (tilt_milli > 1000) tilt_milli = 1000;
    if (tilt_milli < -1000) tilt_milli = -1000;
    ptzv_axis(pan_milli, &out->sx, &out->dx);
    ptzv_axis(tilt_milli, &out->sy, &out->dy);
    if (out->sx && out->sy) { /* diagonal: the verified limit */
        if (out->dx < PTZV_DIV_MIN_DIAG)
            out->dx = PTZV_DIV_MIN_DIAG;
        if (out->dy < PTZV_DIV_MIN_DIAG)
            out->dy = PTZV_DIV_MIN_DIAG;
    }
    return out->sx || out->sy;
}

int ptzv_lease_clamp(long ms)
{
    if (ms <= 0)
        return PTZV_LEASE_DEF_MS;
    if (ms < PTZV_LEASE_MIN_MS)
        return PTZV_LEASE_MIN_MS;
    if (ms > PTZV_LEASE_MAX_MS)
        return PTZV_LEASE_MAX_MS;
    return (int)ms;
}

/* xsd:duration, time part only: PT[nH][nM][n[.f]S] with at least one component; only the seconds may be fractional (3 digits used). */
int ptzv_parse_duration_ms(const char *s, long *ms)
{
    long total = 0;
    int any = 0;
    if (!s || s[0] != 'P' || s[1] != 'T')
        return -1; /* days / months / years are meaningless for a move timeout */
    s += 2;
    while (*s) {
        long whole = 0, frac_ms = 0;
        int nd = 0, fd = 0;
        for (; *s >= '0' && *s <= '9'; s++, nd++) {
            if (whole > 100000)
                return -1;
            whole = whole * 10 + (*s - '0');
        }
        if (!nd)
            return -1;
        if (*s == '.') {
            long mul = 100;
            s++;
            for (; *s >= '0' && *s <= '9'; s++, fd++) {
                if (fd < 3) {
                    frac_ms += (*s - '0') * mul;
                    mul /= 10;
                }
            }
            if (!fd)
                return -1;
        }
        if (*s == 'S')
            total += whole * 1000 + frac_ms;
        else if (*s == 'M' && !fd)
            total += whole * 60000;
        else if (*s == 'H' && !fd)
            total += whole * 3600000;
        else
            return -1;
        s++;
        any = 1;
        if (total > 100000000L)
            return -1;
    }
    if (!any)
        return -1;
    *ms = total;
    return 0;
}

int ptzv_parse_args(const char *pan, const char *tilt, const char *lease, ptzv_cmd_t *out, int *lease_ms, const char **why)
{
    int pm = 0, tm = 0;
    long l = 0;
    const char *e = "";
    if (!pan && !tilt) {
        *why = "pan and/or tilt is required";
        return -1;
    }
    if ((pan && ptzv_parse_milli(pan, &pm) != 0) || (tilt && ptzv_parse_milli(tilt, &tm) != 0)) {
        *why = "pan and tilt are numbers in -1..1 (at most 4 decimals; quote fractions in JSON)";
        return -1;
    }
    *lease_ms = PTZV_LEASE_DEF_MS;
    if (lease) {
        for (e = lease; *e >= '0' && *e <= '9' && e - lease < 6; e++)
            l = l * 10 + (*e - '0');
        if (e == lease || *e || l < PTZV_LEASE_MIN_MS || l > PTZV_LEASE_MAX_MS) {
            *why = "lease_ms is a whole number of milliseconds in 150..2000";
            return -1;
        }
        *lease_ms = (int)l;
    }
    (void)ptzv_map(pm, tm, out);
    return 0;
}
