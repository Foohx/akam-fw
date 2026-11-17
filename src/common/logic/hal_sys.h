/* hal_sys: portable system metrics from /proc (CPU %, load, memory, uptime, Wi-Fi level). Generic: no model path or
 * pin; the only input is a root directory prefix (tests point it at fixtures). Parsers are pure (text in, numbers
 * out) and use no floating point. HAL_SYS_NA marks a value the board does not have (e.g. temperature). */
#ifndef HAL_SYS_H
#define HAL_SYS_H
#include <stddef.h>

#define HAL_SYS_NA (-1000000)

typedef struct {
    int cpu_pct;        /* 0..100, busy share since the previous sample (NA on the first) */
    int load1_x100;     /* load average 1 min x 100 */
    long mem_total_kb, mem_avail_kb;
    long uptime_s;
    int rssi_dbm;       /* negative dBm */
    int link_quality;   /* the raw link quality column */
    int temp_c10;       /* always NA unless a model supplies one */
} hal_sys_sample_t;

typedef struct { unsigned long long total, busy; int have; } hal_sys_t;

/* Pure parsers: 0 on success, -1 when the text does not parse. */
int hal_sys_parse_stat(const char *txt, unsigned long long *total, unsigned long long *busy);
int hal_sys_cpu_pct(hal_sys_t *st, const char *stat_txt);           /* delta since last call, NA if none yet */
int hal_sys_parse_load1(const char *txt, int *x100);
int hal_sys_parse_mem(const char *txt, long *total_kb, long *avail_kb); /* avail falls back to Free+Buffers+Cached; -1 when it cannot be
                                                                          * computed, with *total_kb still set when MemTotal was found */
int hal_sys_parse_uptime(const char *txt, long *s);
int hal_sys_parse_wireless(const char *txt, const char *ifname, int *dbm, int *quality);

/* /proc/net/route text: 1 when `ifname` has an IPv4 route other than the multicast 224.0.0.0/4 (a connected or default route
 * exists only once the interface has an address), else 0. */
int hal_sys_parse_route(const char *txt, const char *ifname);
/* Reads <root>/proc/net/route: the above, 0 when unreadable. */
int hal_sys_net_ipv4(const char *root, const char *ifname);

/* Reads <root>/proc/... (root NULL = the real one) and fills every field it can, NA for the rest. */
void hal_sys_sample(hal_sys_t *st, const char *root, const char *ifname, hal_sys_sample_t *out);

#endif
