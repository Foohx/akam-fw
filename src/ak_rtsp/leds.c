#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>

#include "leds.h"

/* Status LED and IR LED policy, see leds.h and specs/image-day-night-leds.md. */

/* ------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------ */

const char *leds_status_name(int s)
{
    switch (s) {
    case LEDS_STATUS_OFF:  return "off";
    case LEDS_STATUS_AUTO: return "auto";
    default:               return "on";
    }
}

const char *leds_ir_name(int ir) { return ir == LEDS_IR_OFF ? "off" : "auto"; }

const char *leds_state_name(int st)
{
    switch (st) {
    case LEDS_ST_ON:           return "on";
    case LEDS_ST_OFF:          return "off";
    case LEDS_ST_BLINK_WIFI:   return "blink_wifi";
    case LEDS_ST_BLINK_STREAM: return "blink_stream";
    case LEDS_ST_BLIP:         return "blip";
    default:                   return "system";
    }
}

static int parse_status(const char *s)
{
    if (!strcmp(s, "on"))   return LEDS_STATUS_ON;
    if (!strcmp(s, "off"))  return LEDS_STATUS_OFF;
    if (!strcmp(s, "auto")) return LEDS_STATUS_AUTO;
    return -1;
}

static int parse_ir(const char *s)
{
    if (!strcmp(s, "auto")) return LEDS_IR_AUTO;
    if (!strcmp(s, "off"))  return LEDS_IR_OFF;
    return -1;
}

/* ------------------------------------------------------------------------
 * Rules and patterns (pure)
 * ------------------------------------------------------------------------ */

static const char *const g_rule_name[LEDS_RULE_N] = { "stream", "boot", "nowifi", "night", "idle" };
/* the key of a rule in the leds file */
static const char *const g_rule_file[LEDS_RULE_N] = { "led_stream", "led_boot", "led_nowifi", "led_night", "led_idle" };

static long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

const char *leds_rule_name(int rule) { return rule >= 0 && rule < LEDS_RULE_N ? g_rule_name[rule] : "?"; }

int leds_rule_find(const char *name)
{
    for (int r = 0; r < LEDS_RULE_N; r++)
        if (name && !strcmp(name, g_rule_name[r])) return r;
    return -1;
}

/* The pattern a rule has before anyone changes it, for a tick of tick_ms: a blink of one tick on and one tick off (boot, nowifi:
 * the old 1 Hz at the 500 ms tick), two ticks on and two off (stream: the old 0.5 Hz), solid on for the rest. Built directly,
 * not parsed, so that the short ticks of the host tests are representable (the grammar has a 50 ms floor). */
static void rule_default(int rule, int tick_ms, a2k_pat_t *p)
{
    memset(p, 0, sizeof(*p));
    if (rule == LEDS_RULE_STREAM || rule == LEDS_RULE_BOOT || rule == LEDS_RULE_NOWIFI) {
        int half = rule == LEDS_RULE_STREAM ? 2 * tick_ms : tick_ms;
        p->kind = A2K_PAT_BLINK;
        p->n = 1;
        p->on_ms = p->off_ms = half;
    } else {
        p->kind = A2K_PAT_ON;
    }
}

/* The rule that wins (first true): stream (a client is playing), boot (not ready), nowifi, night, idle. */
static int pick_rule(int ready, int net_ok, int clients, int night)
{
    return clients > 0 ? LEDS_RULE_STREAM : !ready ? LEDS_RULE_BOOT : !net_ok ? LEDS_RULE_NOWIFI : night ? LEDS_RULE_NIGHT
                                                                                                      : LEDS_RULE_IDLE;
}

/* The state name a rule reports (led.state): what the LED is doing in terms the user knows. */
static int rule_state(int rule)
{
    return rule == LEDS_RULE_STREAM ? LEDS_ST_BLINK_STREAM : (rule == LEDS_RULE_BOOT || rule == LEDS_RULE_NOWIFI) ? LEDS_ST_BLINK_WIFI
                                                                                                                  : LEDS_ST_ON;
}

/* The LED level (0/1) for tick number `tick`, and the state. It is the policy of the engine with the DEFAULT patterns at the
 * nominal 500 ms tick, as a pure function: the reference of the oracle test and of tests/native/leds_test.c. */
int leds_compute(int status, int ready, int net_ok, int clients, unsigned tick, int *state_out)
{
    int st, level;
    if (status == LEDS_STATUS_OFF) {
        st = LEDS_ST_OFF;
        level = 0;
    } else if (status == LEDS_STATUS_AUTO) {
        a2k_pat_t p;
        int rule = pick_rule(ready, net_ok, clients, 0);
        rule_default(rule, LEDS_TICK_MS, &p);
        st = rule_state(rule);
        level = a2k_pat_level(&p, (long)tick * LEDS_TICK_MS);
    } else {
        st = LEDS_ST_ON;
        level = 1;
    }
    if (state_out) *state_out = st;
    return level;
}

/* ------------------------------------------------------------------------
 * Network check
 * ------------------------------------------------------------------------ */

int leds_net_check(const char *ifname, const char *net_dir)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    if (!ifname || !*ifname || strlen(ifname) >= sizeof(ifr.ifr_name)) return 0;
    strcpy(ifr.ifr_name, ifname);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    int rc = ioctl(fd, SIOCGIFADDR, &ifr);
    close(fd);
    if (rc < 0) return 0;
    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    if (sin.sin_addr.s_addr == 0) return 0;

    /* An address can outlive the link; when the kernel reports the link down
     * (or the file is absent or odd) believe it only if it says so. */
    if (net_dir && *net_dir) {
        char path[160], buf[24];
        snprintf(path, sizeof(path), "%s/%s/operstate", net_dir, ifname);
        int ofd = open(path, O_RDONLY);
        if (ofd >= 0) {
            ssize_t n = read(ofd, buf, sizeof(buf) - 1);
            close(ofd);
            if (n > 0) {
                buf[n] = '\0';
                if (!strncmp(buf, "down", 4) || !strncmp(buf, "dormant", 7) ||
                    !strncmp(buf, "lowerlayerdown", 14) || !strncmp(buf, "notpresent", 10))
                    return 0;
            }
        }
    }
    return 1;
}

static int net_now(leds_t *l)
{
    if (l->ops.net_ok) return l->ops.net_ok(l->ops.user);
    return leds_net_check(l->net_ifname, l->net_dir);
}

/* ------------------------------------------------------------------------
 * sysfs writes
 * ------------------------------------------------------------------------ */

static int write_small(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_TRUNC); /* like a shell redirection */
    if (fd < 0) return -1;
    size_t len = strlen(val);
    ssize_t n;
    do { n = write(fd, val, len); } while (n < 0 && errno == EINTR);
    int e = errno;
    close(fd);
    if (n != (ssize_t)len) { errno = n < 0 ? e : EIO; return -1; }
    return 0;
}

/* The driver of the hal_light descriptor of the 1080p: a kernel LED-class device (hal_light.h LIGHT_LEDCLASS). "trigger" is set
 * to "none" once (a timer or mmc trigger would fight the writes; best effort, the file may be absent on another board),
 * then <led_dir>/brightness. A failure is logged once until a write succeeds again and is retried at the next evaluation.
 * The emitter is the first member of leds_t.emitter, so the driver finds its instance from the pointer it is given. */
static int led_class_set(const hal_light_emitter_t *e, int on)
{
    leds_t *l = ((const struct { hal_light_emitter_t em; leds_t *owner; } *)e)->owner;
    char path[128];
    if (!l->trigger_done) {
        l->trigger_done = 1;
        snprintf(path, sizeof(path), "%s/trigger", l->led_dir);
        (void)write_small(path, "none");
    }
    snprintf(path, sizeof(path), "%s/brightness", l->led_dir);
    if (write_small(path, on ? "1" : "0") < 0) {
        if (!l->err_logged) {
            l->err_logged = 1;
            printf("[leds] cannot write %s: %s (retrying quietly)\n", path, strerror(errno));
        }
        return -1;
    }
    l->err_logged = 0;
    return 0;
}

static const hal_light_ops_t g_led_class_ops = { led_class_set };

/* Caller holds l->lock. */
static void write_level(leds_t *l, int level, long now_ms)
{
    if (l->last_level < 0) l->light.level[0] = -1; /* "unknown": write even when the engine thinks the LED already has it */
    if (light_set(&l->light, "status", level ? LIGHT_ON : LIGHT_OFF, now_ms) == 0) l->last_level = level;
}

/* ------------------------------------------------------------------------
 * Engine
 * ------------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void leds_init(leds_t *l, const leds_ops_t *ops, const leds_cfg_t *cfg)
{
    memset(l, 0, sizeof(*l));
    if (ops) l->ops = *ops;
    copy_str(l->led_dir, sizeof(l->led_dir), cfg->led_dir);
    copy_str(l->conf_dir, sizeof(l->conf_dir), cfg->conf_dir);
    snprintf(l->conf_path, sizeof(l->conf_path), "%s/%s", l->conf_dir, LEDS_CONF_FILE);
    copy_str(l->net_ifname, sizeof(l->net_ifname), cfg->net_ifname ? cfg->net_ifname : "wlan0");
    copy_str(l->net_dir, sizeof(l->net_dir), cfg->net_dir ? cfg->net_dir : "/sys/class/net");
    l->tick_ms = cfg->tick_ms > 0 ? cfg->tick_ms : LEDS_TICK_MS;
    /* the emitter "status": a LED-class device written through light_logic */
    l->emitter.owner = l;
    l->emitter.em.name = "status";
    l->emitter.em.kind = LIGHT_LEDCLASS;
    l->emitter.em.active_high = 1;
    l->emitter.em.led_dir = l->led_dir;
    l->light_desc.emitters = &l->emitter.em;
    l->light_desc.count = 1;
    (void)light_init(&l->light, &l->light_desc, &g_led_class_ops); /* cannot fail: one emitter, no exclusion */
    for (int r = 0; r < LEDS_RULE_N; r++) rule_default(r, l->tick_ms, &l->pat[r]);
    l->rule_now = -1;
    l->next_ms = -1;
    l->t0_ms = mono_ms();
    l->status = LEDS_STATUS_ON;
    l->ir = LEDS_IR_AUTO;
    l->last_level = -1;
    l->state = LEDS_ST_UNMANAGED;
    l->wake_r = l->wake_w = -1;
    pthread_mutex_init(&l->lock, NULL);
    pthread_mutex_init(&l->cl_lock, NULL);
}

void leds_destroy(leds_t *l)
{
    leds_thread_stop(l);
    if (l->wake_r >= 0) close(l->wake_r);
    if (l->wake_w >= 0) close(l->wake_w);
    l->wake_r = l->wake_w = -1;
    pthread_mutex_destroy(&l->lock);
    pthread_mutex_destroy(&l->cl_lock);
}

int leds_get_status(leds_t *l) { pthread_mutex_lock(&l->lock); int v = l->status; pthread_mutex_unlock(&l->lock); return v; }
int leds_get_ir(leds_t *l)     { pthread_mutex_lock(&l->lock); int v = l->ir;     pthread_mutex_unlock(&l->lock); return v; }
int leds_get_state(leds_t *l)  { pthread_mutex_lock(&l->lock); int v = l->state;  pthread_mutex_unlock(&l->lock); return v; }

void leds_set_ready(leds_t *l, int ready)
{
    pthread_mutex_lock(&l->lock);
    l->ready = ready ? 1 : 0;
    pthread_mutex_unlock(&l->lock);
}

void leds_client_delta(leds_t *l, int delta)
{
    pthread_mutex_lock(&l->cl_lock);
    l->clients += delta;
    if (l->clients < 0) l->clients = 0;
    pthread_mutex_unlock(&l->cl_lock);
}

int leds_clients(leds_t *l)
{
    pthread_mutex_lock(&l->cl_lock);
    int n = l->clients;
    pthread_mutex_unlock(&l->cl_lock);
    return n;
}

/* The time the patterns are evaluated at, ms (caller holds l->lock). With the thread running it is the real time since leds_init
 * (so a pattern of any period keeps its phase); without it, tick * tick_ms: one step per leds_tick(), which is what the host
 * tests and the oracle drive. With the defaults and the nominal 500 ms tick the two give the same levels as the old tick counter. */
static long pat_now(const leds_t *l)
{
    return l->use_real ? mono_ms() - l->t0_ms : (long)l->tick * l->tick_ms;
}

/* Evaluates the rules and writes the LED when its level changed.
 * advance: step the blink phase (a leds_tick() of the host tests); an immediate re-evaluation after a SET keeps the phase. */
static void evaluate(leds_t *l, int advance)
{
    pthread_mutex_lock(&l->lock);
    int auto_mode = l->status == LEDS_STATUS_AUTO;
    pthread_mutex_unlock(&l->lock);

    /* The network, the night state and the client count are read outside the main lock (and only in auto mode: a solid LED does
     * not even look at the network). */
    int net = auto_mode ? net_now(l) : 1;
    int night = auto_mode && l->night_fn ? l->night_fn(l->night_user) : 0;
    int clients = leds_clients(l);

    pthread_mutex_lock(&l->lock);
    long now = pat_now(l);
    int st, level, rule = -1;
    long next = -1;
    if (l->status == LEDS_STATUS_OFF) {
        st = LEDS_ST_OFF;
        level = 0;
    } else if (l->status == LEDS_STATUS_AUTO) {
        a2k_pat_t *p;
        rule = pick_rule(l->ready, net, clients, night);
        p = &l->pat[rule];
        st = rule_state(rule);
        level = a2k_pat_level(p, now);
        next = a2k_pat_next_ms(p, now);
    } else {
        st = LEDS_ST_ON;
        level = 1;
    }
    if (l->blip_on) { /* an explicit blip overrides every mode, off included, for its duration */
        long m = mono_ms();
        if (m >= l->blip_until_ms) {
            l->blip_on = 0;
        } else {
            st = LEDS_ST_BLIP;
            level = a2k_pat_level(&l->blip_pat, m - l->blip_start_ms);
            next = a2k_pat_next_ms(&l->blip_pat, m - l->blip_start_ms);
            if (next < 0 || next > l->blip_until_ms - m) next = l->blip_until_ms - m;
        }
    }
    l->rule_now = rule;
    l->next_ms = next;
    if (!l->take_over) {
        l->state = LEDS_ST_UNMANAGED;
    } else {
        l->state = st;
        if (level != l->last_level) write_level(l, level, now);
    }
    if (advance) l->tick++;
    pthread_mutex_unlock(&l->lock);
}

void leds_tick(leds_t *l) { evaluate(l, 1); }

void leds_apply(leds_t *l)
{
    pthread_mutex_lock(&l->lock);
    int allowed = l->ir == LEDS_IR_AUTO;
    if (l->status != LEDS_STATUS_ON) l->take_over = 1;
    pthread_mutex_unlock(&l->lock);
    if (l->ops.ir_allowed) l->ops.ir_allowed(l->ops.user, allowed);
    evaluate(l, 0);
}

static void wake_thread(leds_t *l)
{
    if (l->wake_w >= 0) {
        char c = 1;
        ssize_t n = write(l->wake_w, &c, 1);
        (void)n; /* full pipe = already awake */
    }
}

static void *leds_thread_main(void *arg)
{
    leds_t *l = arg;
    while (l->thread_running) {
        evaluate(l, 0);
        /* Sleep until the next change of the pattern, but at most one tick in auto mode (the network is looked at each tick) and
         * four ticks otherwise. With the default patterns the edge is one tick away: the cadence of the old engine. */
        pthread_mutex_lock(&l->lock);
        long next = l->next_ms;
        int ms = l->status == LEDS_STATUS_AUTO ? l->tick_ms : l->tick_ms * 4;
        pthread_mutex_unlock(&l->lock);
        if (next >= 0 && next < ms) ms = next < 5 ? 5 : (int)next;
        struct pollfd p = { l->wake_r, POLLIN, 0 };
        int r = poll(&p, 1, ms);
        if (r > 0 && (p.revents & POLLIN)) {
            char buf[16];
            ssize_t n = read(l->wake_r, buf, sizeof(buf));
            (void)n;
        }
    }
    return NULL;
}

int leds_thread_start(leds_t *l)
{
    if (l->thread_started) return 0;
    int fds[2];
    if (pipe(fds) < 0) return -1;
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(fds[1], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[1], F_SETFL, fl | O_NONBLOCK);
    l->wake_r = fds[0];
    l->wake_w = fds[1];
    l->thread_running = 1;
    pthread_mutex_lock(&l->lock);
    l->use_real = 1;
    pthread_mutex_unlock(&l->lock);
    if (pthread_create(&l->tid, NULL, leds_thread_main, l) != 0) {
        l->thread_running = 0;
        pthread_mutex_lock(&l->lock);
        l->use_real = 0;
        pthread_mutex_unlock(&l->lock);
        close(fds[0]); close(fds[1]);
        l->wake_r = l->wake_w = -1;
        return -1;
    }
    l->thread_started = 1;
    return 0;
}

void leds_thread_stop(leds_t *l)
{
    if (!l->thread_started) return;
    l->thread_running = 0;
    wake_thread(l);
    pthread_join(l->tid, NULL);
    l->thread_started = 0;
    pthread_mutex_lock(&l->lock);
    l->use_real = 0;
    pthread_mutex_unlock(&l->lock);
}

/* ------------------------------------------------------------------------
 * Saved settings
 * ------------------------------------------------------------------------ */

int leds_load_config(leds_t *l)
{
    char buf[512];
    int fd = open(l->conf_path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *save = NULL;
    pthread_mutex_lock(&l->lock);
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        size_t vl = strlen(eq + 1);
        if (vl && eq[vl] == '\r') eq[vl] = '\0'; /* tolerate CRLF */
        const char *k = line, *v = eq + 1;
        int x;
        a2k_pat_t p;
        if (!strcmp(k, "status") && (x = parse_status(v)) >= 0) l->status = x;
        else if (!strcmp(k, "ir_led") && (x = parse_ir(v)) >= 0) l->ir = x;
        else if (!strncmp(k, "led_", 4) && a2k_pat_parse(v, &p) == 0) { /* a bad pattern keeps the default */
            for (int r = 0; r < LEDS_RULE_N; r++)
                if (!strcmp(k, g_rule_file[r])) l->pat[r] = p;
        }
    }
    pthread_mutex_unlock(&l->lock);
    return 1;
}

static int key_is_ours(const char *line)
{
    if (!strncmp(line, "status=", 7) || !strncmp(line, "ir_led=", 7)) return 1;
    for (int r = 0; r < LEDS_RULE_N; r++) {
        size_t kl = strlen(g_rule_file[r]);
        if (!strncmp(line, g_rule_file[r], kl) && line[kl] == '=') return 1;
    }
    return 0;
}

/* One writer at a time: the 8091 thread (led.status) and the API thread (a pattern) both save, and share the temp file name. */
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;
static int save_config_locked(leds_t *l);

int leds_save_config(leds_t *l)
{
    pthread_mutex_lock(&g_save_mu);
    int rc = save_config_locked(l);
    int e = errno;
    pthread_mutex_unlock(&g_save_mu);
    errno = e;
    return rc;
}

static int save_config_locked(leds_t *l)
{
    char old[1024], out[1280];
    size_t olen = 0, outn = 0;

    int fd = open(l->conf_path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, old, sizeof(old) - 1);
        close(fd);
        if (n > 0) olen = (size_t)n;
    }
    old[olen] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line || key_is_ours(line)) continue;
        int w = snprintf(out + outn, sizeof(out) - outn, "%s\n", line);
        if (w < 0 || (size_t)w >= sizeof(out) - outn) break;
        outn += (size_t)w;
    }
    pthread_mutex_lock(&l->lock);
    int status = l->status, ir = l->ir;
    a2k_pat_t pats[LEDS_RULE_N];
    memcpy(pats, l->pat, sizeof(pats));
    pthread_mutex_unlock(&l->lock);
    int w = snprintf(out + outn, sizeof(out) - outn, "status=%s\nir_led=%s\n",
                     leds_status_name(status), leds_ir_name(ir));
    if (w < 0 || (size_t)w >= sizeof(out) - outn) { errno = ENOSPC; return -1; }
    outn += (size_t)w;
    for (int r = 0; r < LEDS_RULE_N; r++) { /* only the patterns that differ from the default: the default file stays two lines */
        a2k_pat_t d;
        char txt[A2K_PAT_MAX];
        rule_default(r, l->tick_ms, &d);
        if (!memcmp(&d, &pats[r], sizeof(d))) continue;
        a2k_pat_format(&pats[r], txt, sizeof(txt));
        w = snprintf(out + outn, sizeof(out) - outn, "%s=%s\n", g_rule_file[r], txt);
        if (w < 0 || (size_t)w >= sizeof(out) - outn) { errno = ENOSPC; return -1; }
        outn += (size_t)w;
    }

    if (mkdir(l->conf_dir, 0700) < 0 && errno != EEXIST) return -1;
    char tmp[sizeof(l->conf_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", l->conf_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < outn) {
        ssize_t n = write(fd, out + off, outn - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int e = errno; close(fd); unlink(tmp); errno = e; return -1; }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, l->conf_path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}

/* ------------------------------------------------------------------------
 * Patterns and the transient blip
 * ------------------------------------------------------------------------ */

void leds_set_night_hook(leds_t *l, int (*night)(void *user), void *user)
{
    pthread_mutex_lock(&l->lock);
    l->night_fn = night;
    l->night_user = user;
    pthread_mutex_unlock(&l->lock);
}

int leds_pat_get(leds_t *l, int rule, char *out, size_t cap)
{
    if (rule < 0 || rule >= LEDS_RULE_N) return -1;
    pthread_mutex_lock(&l->lock);
    a2k_pat_t p = l->pat[rule];
    pthread_mutex_unlock(&l->lock);
    char txt[A2K_PAT_MAX];
    a2k_pat_format(&p, txt, sizeof(txt));
    snprintf(out, cap, "%s", txt);
    return 0;
}

void leds_pat_default(const leds_t *l, int rule, char *out, size_t cap)
{
    a2k_pat_t d;
    rule_default(rule, l->tick_ms, &d);
    a2k_pat_format(&d, out, cap);
}

int leds_pat_set(leds_t *l, int rule, const char *text)
{
    a2k_pat_t p;
    if (rule < 0 || rule >= LEDS_RULE_N || a2k_pat_parse(text, &p) != 0) return -1;
    pthread_mutex_lock(&l->lock);
    int changed = memcmp(&l->pat[rule], &p, sizeof(p)) != 0;
    l->pat[rule] = p;
    pthread_mutex_unlock(&l->lock);
    if (!changed) return 0; /* same pattern: no flash write */
    evaluate(l, 0);
    wake_thread(l);
    return leds_save_config(l) < 0 ? -2 : 0;
}

int leds_blip(leds_t *l, const char *pattern, long dur_ms)
{
    a2k_pat_t p;
    if (dur_ms < 1 || dur_ms > 60000 || a2k_pat_parse(pattern, &p) != 0) return -1;
    pthread_mutex_lock(&l->lock);
    l->blip_pat = p;
    l->blip_start_ms = mono_ms();
    l->blip_until_ms = l->blip_start_ms + dur_ms;
    l->blip_on = 1;
    l->take_over = 1; /* a blip shows even when status=on: the engine drives the LED from now on (and gives the rules back after it) */
    pthread_mutex_unlock(&l->lock);
    evaluate(l, 0);
    wake_thread(l);
    /* the first level of the blip must really have reached the LED, else the caller must not be told it works */
    pthread_mutex_lock(&l->lock);
    int shown = l->state == LEDS_ST_BLIP && l->last_level == a2k_pat_level(&l->blip_pat, mono_ms() - l->blip_start_ms);
    if (!shown) l->blip_on = 0;
    pthread_mutex_unlock(&l->lock);
    if (!shown) {
        evaluate(l, 0);
        return -2;
    }
    return 0;
}

void leds_blip_clear(leds_t *l)
{
    pthread_mutex_lock(&l->lock);
    l->blip_on = 0;
    pthread_mutex_unlock(&l->lock);
    evaluate(l, 0);
    wake_thread(l);
}

int leds_rule_now(leds_t *l)
{
    pthread_mutex_lock(&l->lock);
    int r = l->rule_now;
    pthread_mutex_unlock(&l->lock);
    return r;
}

/* ------------------------------------------------------------------------
 * 8091 keys
 * ------------------------------------------------------------------------ */

static int is_ours(const char *key)
{
    return !strcmp(key, "led.status") || !strcmp(key, "led.state") || !strcmp(key, "night.ir_led");
}

int leds_ctl_get(leds_t *l, const char *key, char *out, size_t cap)
{
    if (!is_ours(key)) return -1;
    if (!strcmp(key, "led.status"))     snprintf(out, cap, "led.status=%s", leds_status_name(leds_get_status(l)));
    else if (!strcmp(key, "led.state")) snprintf(out, cap, "led.state=%s", leds_state_name(leds_get_state(l)));
    else                                snprintf(out, cap, "night.ir_led=%s", leds_ir_name(leds_get_ir(l)));
    return 0;
}

void leds_ctl_list(leds_t *l, void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = { "led.status", "led.state", "night.ir_led" };
    char line[64];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        leds_ctl_get(l, keys[i], line, sizeof(line));
        emit(arg, line);
    }
}

/* Fills note with " (not saved: ...)" on failure, else "". */
static void persist_note(leds_t *l, char *note, size_t cap)
{
    note[0] = '\0';
    if (leds_save_config(l) < 0) snprintf(note, cap, " (not saved: %s)", strerror(errno));
}

int leds_ctl_set(leds_t *l, const char *key, const char *val, char *msg, size_t cap)
{
    if (!is_ours(key)) return -1;
    char note[64] = "";

    if (!strcmp(key, "led.status")) {
        int v = parse_status(val);
        if (v < 0) { snprintf(msg, cap, "ERR invalid led.status value %s (want on|off|auto)", val); return 1; }
        pthread_mutex_lock(&l->lock);
        int changed = l->status != v;
        l->status = v;
        if (changed) l->take_over = 1;
        pthread_mutex_unlock(&l->lock);
        if (changed) {
            evaluate(l, 0);
            persist_note(l, note, sizeof(note));
            wake_thread(l);
        }
        snprintf(msg, cap, "OK led.status=%s%s", leds_status_name(v), note);
    } else if (!strcmp(key, "night.ir_led")) {
        int v = parse_ir(val);
        if (v < 0) { snprintf(msg, cap, "ERR invalid night.ir_led value %s (want auto|off)", val); return 1; }
        pthread_mutex_lock(&l->lock);
        int changed = l->ir != v;
        l->ir = v;
        pthread_mutex_unlock(&l->lock);
        if (changed) {
            if (l->ops.ir_allowed) l->ops.ir_allowed(l->ops.user, v == LEDS_IR_AUTO);
            persist_note(l, note, sizeof(note));
        }
        snprintf(msg, cap, "OK night.ir_led=%s%s", leds_ir_name(v), note);
    } else {
        snprintf(msg, cap, "ERR led.state is read-only");
        return 1;
    }
    return 0;
}
