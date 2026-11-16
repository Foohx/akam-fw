/* hal_sys: see hal_sys.h. */
#include "hal_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dig(char c) { return c >= '0' && c <= '9'; }

static const char *next_line(const char *p)
{
    p = strchr(p, '\n');
    return p ? p + 1 : NULL;
}

int hal_sys_parse_stat(const char *txt, unsigned long long *total, unsigned long long *busy)
{
    unsigned long long f[8] = { 0 }, t = 0;
    const char *p = txt;
    char *e;
    int n = 0;
    if (!p || strncmp(p, "cpu ", 4))
        return -1;
    for (p += 4; n < 8; n++) {
        while (*p == ' ')
            p++;
        if (!dig(*p))
            break;
        f[n] = strtoull(p, &e, 10);
        p = e;
    }
    if (n < 4)
        return -1;
    for (int i = 0; i < n; i++)
        t += f[i];
    *total = t;
    *busy = t - f[3] - (n > 4 ? f[4] : 0); /* minus idle and iowait */
    return 0;
}

int hal_sys_cpu_pct(hal_sys_t *st, const char *txt)
{
    unsigned long long t, b;
    int pct = HAL_SYS_NA;
    if (hal_sys_parse_stat(txt, &t, &b) != 0)
        return HAL_SYS_NA;
    if (st->have && t > st->total && b >= st->busy) {
        unsigned long long dt = t - st->total, db = b - st->busy;
        pct = (int)((db * 100 + dt / 2) / dt);
        if (pct > 100)
            pct = 100;
    }
    st->have = 1;
    st->total = t;
    st->busy = b;
    return pct;
}

int hal_sys_parse_load1(const char *txt, int *x100)
{
    int i = 0, v = 0, fd = 0;
    if (!txt || !dig(txt[0]))
        return -1;
    for (; dig(txt[i]); i++)
        if (i < 6)
            v = v * 10 + (txt[i] - '0');
    if (txt[i] == '.')
        for (i++; dig(txt[i]); i++)
            if (fd < 2) {
                v = v * 10 + (txt[i] - '0');
                fd++;
            }
    for (; fd < 2; fd++)
        v *= 10;
    *x100 = v;
    return 0;
}

static long kb_of(const char *txt, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = txt; p; p = next_line(p))
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            p += kl + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return dig(*p) ? strtol(p, NULL, 10) : -1;
        }
    return -1;
}

int hal_sys_parse_mem(const char *txt, long *total_kb, long *avail_kb)
{
    long t = kb_of(txt, "MemTotal"), a = kb_of(txt, "MemAvailable");
    if (t < 0)
        return -1;
    if (a < 0) {
        long f = kb_of(txt, "MemFree"), b = kb_of(txt, "Buffers"), c = kb_of(txt, "Cached");
        a = f >= 0 && b >= 0 && c >= 0 ? f + b + c : -1;
    }
    if (a < 0)
        return -1;
    *total_kb = t;
    *avail_kb = a;
    return 0;
}

int hal_sys_parse_uptime(const char *txt, long *s)
{
    if (!txt || !dig(txt[0]))
        return -1;
    *s = strtol(txt, NULL, 10);
    return 0;
}

/* " wlan0: 0000   48.  -62.  -256 ...": status, link quality, level (dBm when negative, with or without '.'). */
int hal_sys_parse_wireless(const char *txt, const char *ifname, int *dbm, int *quality)
{
    size_t il = strlen(ifname);
    for (const char *p = txt; p; p = next_line(p)) {
        const char *q = p;
        long lv;
        while (*q == ' ')
            q++;
        if (strncmp(q, ifname, il) || q[il] != ':')
            continue;
        q += il + 1;
        for (int field = 0; field < 2; field++) {
            while (*q == ' ')
                q++;
            if (field == 1 && dig(*q))
                *quality = (int)strtol(q, NULL, 10);
            while (dig(*q) || (*q >= 'a' && *q <= 'f') || *q == '.')
                q++;
        }
        while (*q == ' ')
            q++;
        if (*q != '-' || !dig(q[1]))
            return -1;
        lv = strtol(q + 1, NULL, 10);
        if (lv < 1 || lv > 127)
            return -1;
        *dbm = (int)-lv;
        return 0;
    }
    return -1;
}

int hal_sys_parse_route(const char *txt, const char *ifname)
{
    size_t il = strlen(ifname);
    for (const char *q = txt; q && *q; q = strchr(q, '\n') ? strchr(q, '\n') + 1 : NULL) {
        char dest[16];
        if (strncmp(q, ifname, il) || q[il] != '\t' || sscanf(q + il + 1, "%15s", dest) != 1)
            continue;
        if (strcmp(dest, "000000E0") != 0 && strcmp(dest, "000000e0") != 0)
            return 1;
    }
    return 0;
}

static int slurp(const char *root, const char *rel, char *b, size_t cap)
{
    char path[160];
    FILE *f;
    size_t n;
    if (root)
        snprintf(path, sizeof path, "%s%s", root, rel);
    else
        snprintf(path, sizeof path, "%s", rel);
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(b, 1, cap - 1, f);
    fclose(f);
    b[n] = '\0';
    return n ? 0 : -1;
}

int hal_sys_net_ipv4(const char *root, const char *ifname)
{
    char b[4096];
    return slurp(root, "/proc/net/route", b, sizeof b) == 0 && hal_sys_parse_route(b, ifname);
}

void hal_sys_sample(hal_sys_t *st, const char *root, const char *ifname, hal_sys_sample_t *o)
{
    char b[2048];
    long l;
    o->cpu_pct = o->load1_x100 = o->rssi_dbm = o->link_quality = o->temp_c10 = HAL_SYS_NA;
    o->mem_total_kb = o->mem_avail_kb = o->uptime_s = HAL_SYS_NA;
    if (slurp(root, "/proc/stat", b, sizeof b) == 0)
        o->cpu_pct = hal_sys_cpu_pct(st, b);
    if (slurp(root, "/proc/loadavg", b, sizeof b) == 0)
        (void)hal_sys_parse_load1(b, &o->load1_x100);
    if (slurp(root, "/proc/meminfo", b, sizeof b) == 0)
        (void)hal_sys_parse_mem(b, &o->mem_total_kb, &o->mem_avail_kb);
    if (slurp(root, "/proc/uptime", b, sizeof b) == 0 && hal_sys_parse_uptime(b, &l) == 0)
        o->uptime_s = l;
    if (slurp(root, "/proc/net/wireless", b, sizeof b) == 0)
        (void)hal_sys_parse_wireless(b, ifname, &o->rssi_dbm, &o->link_quality);
}
