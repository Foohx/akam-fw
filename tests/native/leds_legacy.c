/* VERBATIM COPY of platforms/anyka-ak3918/leds.c as it was before the LED engine moved to light_logic (convergence P6), identifiers prefixed
 * legacy_/LEGACY_. It is the ORACLE of tests/native/leds_oracle_test.c: do not edit, do not fix. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <net/if.h>
#include <netinet/in.h>

#include "leds_legacy.h"

/* Status LED and IR LED policy, see leds.h and specs/image-day-night-leds.md. */

/* ------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------ */

const char *legacy_leds_status_name(int s)
{
    switch (s) {
    case LEGACY_LEDS_STATUS_OFF:  return "off";
    case LEGACY_LEDS_STATUS_AUTO: return "auto";
    default:               return "on";
    }
}

const char *legacy_leds_ir_name(int ir) { return ir == LEGACY_LEDS_IR_OFF ? "off" : "auto"; }

const char *legacy_leds_state_name(int st)
{
    switch (st) {
    case LEGACY_LEDS_ST_ON:           return "on";
    case LEGACY_LEDS_ST_OFF:          return "off";
    case LEGACY_LEDS_ST_BLINK_WIFI:   return "blink_wifi";
    case LEGACY_LEDS_ST_BLINK_STREAM: return "blink_stream";
    default:                   return "system";
    }
}

static int parse_status(const char *s)
{
    if (!strcmp(s, "on"))   return LEGACY_LEDS_STATUS_ON;
    if (!strcmp(s, "off"))  return LEGACY_LEDS_STATUS_OFF;
    if (!strcmp(s, "auto")) return LEGACY_LEDS_STATUS_AUTO;
    return -1;
}

static int parse_ir(const char *s)
{
    if (!strcmp(s, "auto")) return LEGACY_LEDS_IR_AUTO;
    if (!strcmp(s, "off"))  return LEGACY_LEDS_IR_OFF;
    return -1;
}

/* ------------------------------------------------------------------------
 * State machine (pure)
 * ------------------------------------------------------------------------ */

int legacy_leds_compute(int status, int ready, int net_ok, int clients, unsigned tick, int *state_out)
{
    int st, level;
    if (status == LEGACY_LEDS_STATUS_OFF) {
        st = LEGACY_LEDS_ST_OFF;
        level = 0;
    } else if (status == LEGACY_LEDS_STATUS_AUTO) {
        if (clients > 0) {            /* privacy indicator: wins over everything */
            st = LEGACY_LEDS_ST_BLINK_STREAM;
            level = ((tick / 2) & 1u) == 0;   /* 1 s on, 1 s off at a 500 ms tick */
        } else if (!ready || !net_ok) {
            st = LEGACY_LEDS_ST_BLINK_WIFI;
            level = (tick & 1u) == 0;         /* 500 ms on, 500 ms off */
        } else {
            st = LEGACY_LEDS_ST_ON;
            level = 1;
        }
    } else {
        st = LEGACY_LEDS_ST_ON;
        level = 1;
    }
    if (state_out) *state_out = st;
    return level;
}

/* ------------------------------------------------------------------------
 * Network check
 * ------------------------------------------------------------------------ */

int legacy_leds_net_check(const char *ifname, const char *net_dir)
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

static int net_now(legacy_leds_t *l)
{
    if (l->ops.net_ok) return l->ops.net_ok(l->ops.user);
    return legacy_leds_net_check(l->net_ifname, l->net_dir);
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

/* Caller holds l->lock. A failure is logged once until a write succeeds again. */
static void write_level(legacy_leds_t *l, int level)
{
    char path[128];
    if (!l->trigger_done) {
        /* A timer or mmc trigger would fight our writes. Best effort: the
         * file may be absent on another board. */
        l->trigger_done = 1;
        snprintf(path, sizeof(path), "%s/trigger", l->led_dir);
        (void)write_small(path, "none");
    }
    snprintf(path, sizeof(path), "%s/brightness", l->led_dir);
    if (write_small(path, level ? "1" : "0") < 0) {
        if (!l->err_logged) {
            l->err_logged = 1;
            printf("[leds] cannot write %s: %s (retrying quietly)\n", path, strerror(errno));
        }
        return;
    }
    l->err_logged = 0;
    l->last_level = level;
}

/* ------------------------------------------------------------------------
 * Engine
 * ------------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

void legacy_leds_init(legacy_leds_t *l, const legacy_leds_ops_t *ops, const legacy_leds_cfg_t *cfg)
{
    memset(l, 0, sizeof(*l));
    if (ops) l->ops = *ops;
    copy_str(l->led_dir, sizeof(l->led_dir), cfg->led_dir);
    copy_str(l->conf_dir, sizeof(l->conf_dir), cfg->conf_dir);
    snprintf(l->conf_path, sizeof(l->conf_path), "%s/%s", l->conf_dir, LEGACY_LEDS_CONF_FILE);
    copy_str(l->net_ifname, sizeof(l->net_ifname), cfg->net_ifname ? cfg->net_ifname : "wlan0");
    copy_str(l->net_dir, sizeof(l->net_dir), cfg->net_dir ? cfg->net_dir : "/sys/class/net");
    l->tick_ms = cfg->tick_ms > 0 ? cfg->tick_ms : LEGACY_LEDS_TICK_MS;
    l->status = LEGACY_LEDS_STATUS_ON;
    l->ir = LEGACY_LEDS_IR_AUTO;
    l->last_level = -1;
    l->state = LEGACY_LEDS_ST_UNMANAGED;
    l->wake_r = l->wake_w = -1;
    pthread_mutex_init(&l->lock, NULL);
    pthread_mutex_init(&l->cl_lock, NULL);
}

void legacy_leds_destroy(legacy_leds_t *l)
{
    legacy_leds_thread_stop(l);
    if (l->wake_r >= 0) close(l->wake_r);
    if (l->wake_w >= 0) close(l->wake_w);
    l->wake_r = l->wake_w = -1;
    pthread_mutex_destroy(&l->lock);
    pthread_mutex_destroy(&l->cl_lock);
}

int legacy_leds_get_status(legacy_leds_t *l) { pthread_mutex_lock(&l->lock); int v = l->status; pthread_mutex_unlock(&l->lock); return v; }
int legacy_leds_get_ir(legacy_leds_t *l)     { pthread_mutex_lock(&l->lock); int v = l->ir;     pthread_mutex_unlock(&l->lock); return v; }
int legacy_leds_get_state(legacy_leds_t *l)  { pthread_mutex_lock(&l->lock); int v = l->state;  pthread_mutex_unlock(&l->lock); return v; }

void legacy_leds_set_ready(legacy_leds_t *l, int ready)
{
    pthread_mutex_lock(&l->lock);
    l->ready = ready ? 1 : 0;
    pthread_mutex_unlock(&l->lock);
}

void legacy_leds_client_delta(legacy_leds_t *l, int delta)
{
    pthread_mutex_lock(&l->cl_lock);
    l->clients += delta;
    if (l->clients < 0) l->clients = 0;
    pthread_mutex_unlock(&l->cl_lock);
}

int legacy_leds_clients(legacy_leds_t *l)
{
    pthread_mutex_lock(&l->cl_lock);
    int n = l->clients;
    pthread_mutex_unlock(&l->cl_lock);
    return n;
}

/* Evaluates the state machine and writes the LED when its level changed.
 * advance: step the blink phase (the thread's tick); an immediate re-evaluation
 * after a SET keeps the phase. */
static void evaluate(legacy_leds_t *l, int advance)
{
    pthread_mutex_lock(&l->lock);
    int auto_mode = l->status == LEGACY_LEDS_STATUS_AUTO;
    pthread_mutex_unlock(&l->lock);

    /* The network and the client count are read outside the main lock. */
    int net = auto_mode ? net_now(l) : 1;
    int clients = legacy_leds_clients(l);

    pthread_mutex_lock(&l->lock);
    int st;
    int level = legacy_leds_compute(l->status, l->ready, net, clients, l->tick, &st);
    if (!l->take_over) {
        l->state = LEGACY_LEDS_ST_UNMANAGED;
    } else {
        l->state = st;
        if (level != l->last_level) write_level(l, level);
    }
    if (advance) l->tick++;
    pthread_mutex_unlock(&l->lock);
}

void legacy_leds_tick(legacy_leds_t *l) { evaluate(l, 1); }

void legacy_leds_apply(legacy_leds_t *l)
{
    pthread_mutex_lock(&l->lock);
    int allowed = l->ir == LEGACY_LEDS_IR_AUTO;
    if (l->status != LEGACY_LEDS_STATUS_ON) l->take_over = 1;
    pthread_mutex_unlock(&l->lock);
    if (l->ops.ir_allowed) l->ops.ir_allowed(l->ops.user, allowed);
    evaluate(l, 0);
}

static void wake_thread(legacy_leds_t *l)
{
    if (l->wake_w >= 0) {
        char c = 1;
        ssize_t n = write(l->wake_w, &c, 1);
        (void)n; /* full pipe = already awake */
    }
}

static void *legacy_leds_thread_main(void *arg)
{
    legacy_leds_t *l = arg;
    while (l->thread_running) {
        legacy_leds_tick(l);
        int ms = legacy_leds_get_status(l) == LEGACY_LEDS_STATUS_AUTO ? l->tick_ms : l->tick_ms * 4;
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

int legacy_leds_thread_start(legacy_leds_t *l)
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
    if (pthread_create(&l->tid, NULL, legacy_leds_thread_main, l) != 0) {
        l->thread_running = 0;
        close(fds[0]); close(fds[1]);
        l->wake_r = l->wake_w = -1;
        return -1;
    }
    l->thread_started = 1;
    return 0;
}

void legacy_leds_thread_stop(legacy_leds_t *l)
{
    if (!l->thread_started) return;
    l->thread_running = 0;
    wake_thread(l);
    pthread_join(l->tid, NULL);
    l->thread_started = 0;
}

/* ------------------------------------------------------------------------
 * Saved settings
 * ------------------------------------------------------------------------ */

int legacy_leds_load_config(legacy_leds_t *l)
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
        if (!strcmp(k, "status") && (x = parse_status(v)) >= 0) l->status = x;
        else if (!strcmp(k, "ir_led") && (x = parse_ir(v)) >= 0) l->ir = x;
    }
    pthread_mutex_unlock(&l->lock);
    return 1;
}

static int key_is_ours(const char *line)
{
    return !strncmp(line, "status=", 7) || !strncmp(line, "ir_led=", 7);
}

int legacy_leds_save_config(legacy_leds_t *l)
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
    pthread_mutex_unlock(&l->lock);
    int w = snprintf(out + outn, sizeof(out) - outn, "status=%s\nir_led=%s\n",
                     legacy_leds_status_name(status), legacy_leds_ir_name(ir));
    if (w < 0 || (size_t)w >= sizeof(out) - outn) { errno = ENOSPC; return -1; }
    outn += (size_t)w;

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
 * 8091 keys
 * ------------------------------------------------------------------------ */

static int is_ours(const char *key)
{
    return !strcmp(key, "led.status") || !strcmp(key, "led.state") || !strcmp(key, "night.ir_led");
}

int legacy_leds_ctl_get(legacy_leds_t *l, const char *key, char *out, size_t cap)
{
    if (!is_ours(key)) return -1;
    if (!strcmp(key, "led.status"))     snprintf(out, cap, "led.status=%s", legacy_leds_status_name(legacy_leds_get_status(l)));
    else if (!strcmp(key, "led.state")) snprintf(out, cap, "led.state=%s", legacy_leds_state_name(legacy_leds_get_state(l)));
    else                                snprintf(out, cap, "night.ir_led=%s", legacy_leds_ir_name(legacy_leds_get_ir(l)));
    return 0;
}

void legacy_leds_ctl_list(legacy_leds_t *l, void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = { "led.status", "led.state", "night.ir_led" };
    char line[64];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        legacy_leds_ctl_get(l, keys[i], line, sizeof(line));
        emit(arg, line);
    }
}

/* Fills note with " (not saved: ...)" on failure, else "". */
static void persist_note(legacy_leds_t *l, char *note, size_t cap)
{
    note[0] = '\0';
    if (legacy_leds_save_config(l) < 0) snprintf(note, cap, " (not saved: %s)", strerror(errno));
}

int legacy_leds_ctl_set(legacy_leds_t *l, const char *key, const char *val, char *msg, size_t cap)
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
        snprintf(msg, cap, "OK led.status=%s%s", legacy_leds_status_name(v), note);
    } else if (!strcmp(key, "night.ir_led")) {
        int v = parse_ir(val);
        if (v < 0) { snprintf(msg, cap, "ERR invalid night.ir_led value %s (want auto|off)", val); return 1; }
        pthread_mutex_lock(&l->lock);
        int changed = l->ir != v;
        l->ir = v;
        pthread_mutex_unlock(&l->lock);
        if (changed) {
            if (l->ops.ir_allowed) l->ops.ir_allowed(l->ops.user, v == LEGACY_LEDS_IR_AUTO);
            persist_note(l, note, sizeof(note));
        }
        snprintf(msg, cap, "OK night.ir_led=%s%s", legacy_leds_ir_name(v), note);
    } else {
        snprintf(msg, cap, "ERR led.state is read-only");
        return 1;
    }
    return 0;
}
