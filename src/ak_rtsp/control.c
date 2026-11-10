#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>

#include "ae.h"
#include "night.h"
#include "isp.h"
#include "audio_out.h"
#include "imgq.h"
#include "talk.h"
#include "leds.h"
#include "control.h"
#include "ctl_gate.h"

#ifndef CONTROL_PORT
#define CONTROL_PORT 8091   /* the host test builds with another port */
#endif

static volatile int    g_control_running;
static pthread_t       g_control_tid;
static pthread_t       g_log_tid;
static int             g_server_fd = -1;

/* Up to CTL_MAX_CLIENTS concurrent clients, one small thread each (a remote
 * metrics reader must not kick the SSH helper that runs a command on the
 * loopback). A client that finds every slot busy gets "ERR busy" and is
 * closed. g_slot_lock protects the table and the fds in it. */
#define CTL_STACK_BYTES 65536
#define CTL_IDLE_S      900   /* no byte received for this long: the client is dropped */
#define CTL_SNDTIMEO_S  2     /* a reply to a stuck client gives up after this */

typedef struct {
    int             fd;        /* -1 = free */
    int             loopback;  /* peer is 127.0.0.0/8 */
    int             need_nl;   /* a LOG line was cut short: start the next one on a fresh line */
    pthread_mutex_t wlock;     /* one writer at a time on this socket (replies vs LOG) */
} cli_t;

static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static ctl_slots_t     g_slots;
static cli_t           g_cli[CTL_MAX_CLIENTS] = {
    { -1, 0, 0, PTHREAD_MUTEX_INITIALIZER }, { -1, 0, 0, PTHREAD_MUTEX_INITIALIZER },
    { -1, 0, 0, PTHREAD_MUTEX_INITIALIZER }, { -1, 0, 0, PTHREAD_MUTEX_INITIALIZER },
};

/* RAM-only debug flag (writes from remote peers), off at every start. */
static pthread_mutex_t g_dbg_lock = PTHREAD_MUTEX_INITIALIZER;
static ctl_debug_t     g_dbg;

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int debug_active(void)
{
    pthread_mutex_lock(&g_dbg_lock);
    int on = ctl_debug_active(&g_dbg, mono_ms());
    pthread_mutex_unlock(&g_dbg_lock);
    return on;
}

static int             g_orig_stdout = -1; /* real stdout fd (serial console) */
static int             g_log_pipe_r  = -1;

/* isp.saturation/contrast/brightness/sharpness — the isp_set_*() functions
 * (isp.c) are one-way (GET-modify-SET against the hardware, no getter for
 * "what did we last ask for"), so the logical -50..50 value the user set is
 * tracked here, same ownership pattern as ae_tuning_t/night_tuning_t but
 * without a matching isp_tuning_t struct. 0 = neutral/unchanged from stock
 * default (matches isp_set_* treating the value as a delta off the
 * hardware/cached baseline captured at first call). */
static int g_isp_saturation = 0;
static int g_isp_contrast   = 0;
static int g_isp_brightness = 0;
static int g_isp_sharpness  = 0;

/* One LOG line to every connected client, never blocking: a slot whose socket
 * is busy or full simply misses the line. */
static void log_forward(const char *line)
{
    char   buf[1100];
    int    n = snprintf(buf, sizeof(buf), "LOG %s\n", line);
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    pthread_mutex_lock(&g_slot_lock);
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
        cli_t *c = &g_cli[i];
        if (c->fd < 0 || pthread_mutex_trylock(&c->wlock) != 0) continue;
        if (c->need_nl && send(c->fd, "\n", 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1) c->need_nl = 0;
        if (!c->need_nl) {
            ssize_t w = send(c->fd, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w > 0 && w < n) c->need_nl = 1;
        }
        pthread_mutex_unlock(&c->wlock);
    }
    pthread_mutex_unlock(&g_slot_lock);
}

/* -------------------------------------------------------------------------
 * stdout tee — captures every printf in the whole binary (not just ae.c/
 * night.c) without touching a single existing call site. control_start()
 * redirects STDOUT_FILENO into a pipe; this thread reads it, writes the
 * exact same bytes back to the real stdout (serial console unaffected),
 * and additionally forwards complete lines to the connected control client
 * prefixed "LOG ".
 * ------------------------------------------------------------------------- */
static void *log_pump(void *arg)
{
    (void)arg;
    char   raw[512];
    char   line[1024];
    size_t line_len = 0;

    for (;;) {
        ssize_t n = read(g_log_pipe_r, raw, sizeof(raw));
        if (n <= 0) break;  /* pipe write end closed = process exiting */

        write(g_orig_stdout, raw, n);  /* unchanged passthrough to serial console */

        for (ssize_t i = 0; i < n; i++) {
            char c = raw[i];
            if (c == '\n') {
                line[line_len] = '\0';
                log_forward(line);
                line_len = 0;
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Command protocol
 * ------------------------------------------------------------------------- */
/* Per-client write lock: LOG lines (log_forward) use the same lock, so a line
 * is never cut in the middle by a reply. The send timeout bounds a stuck client. */
static void reply(cli_t *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len < sizeof(buf) - 1) { buf[len] = '\n'; len++; }
    pthread_mutex_lock(&c->wlock);
    if (c->need_nl) { (void)send(c->fd, "\n", 1, MSG_NOSIGNAL); c->need_nl = 0; }
    (void)send(c->fd, buf, len, MSG_NOSIGNAL);
    pthread_mutex_unlock(&c->wlock);
}

static const char *night_mode_name(night_override_t m)
{
    switch (m) {
    case NIGHT_MODE_FORCE_DAY:   return "day";
    case NIGHT_MODE_FORCE_NIGHT: return "night";
    default:                     return "auto";
    }
}

static int night_mode_parse(const char *s, night_override_t *out)
{
    if (!strcmp(s, "auto"))  { *out = NIGHT_MODE_AUTO;        return 0; }
    if (!strcmp(s, "day"))   { *out = NIGHT_MODE_FORCE_DAY;   return 0; }
    if (!strcmp(s, "night")) { *out = NIGHT_MODE_FORCE_NIGHT; return 0; }
    return -1;
}

static void audio_list_emit(void *arg, const char *line)
{
    reply((cli_t *)arg, "%s", line);
}

static void do_list(cli_t *c)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    reply(c, "ae.stable_range=%d",       a.stable_range);
    reply(c, "ae.hold_range=%d",         a.hold_range);
    reply(c, "ae.speed=%d",              a.speed);
    reply(c, "ae.exp_max=%d",            a.exp_max);
    reply(c, "ae.enabled=%d",            a.enabled);
    reply(c, "night.mode=%s",            night_mode_name(n.override));
    reply(c, "night.state=%s",           night_is_night() ? "night" : "day");
    reply(c, "night.trigger_hw_exp=%u",  n.trigger_hw_exp);
    reply(c, "night.day_hw_exp=%u",      n.day_hw_exp);
    reply(c, "night.confirm_samples=%d", n.confirm_samples);
    reply(c, "night.lock_ms=%d",         n.lock_ms);
    reply(c, "isp.saturation=%d",        g_isp_saturation);
    reply(c, "isp.contrast=%d",          g_isp_contrast);
    reply(c, "isp.brightness=%d",        g_isp_brightness);
    reply(c, "isp.sharpness=%d",         g_isp_sharpness);
    /* VENC QP is stock-matched ground truth but
     * not live-settable yet — no confirmed ioctl to update rc params on an
     * already-created encoder channel without a DEACTIVATE/CREATE_ENC
     * cycle. Reported for visibility only. */
    reply(c, "venc.minqp=28 (read-only)");
    reply(c, "venc.maxqp=43 (read-only)");
    night_ctl_list(audio_list_emit, c);
    ledsys_ctl_list(audio_list_emit, c);
    audio_out_ctl_list(audio_list_emit, c);
    imgq_ctl_list(audio_list_emit, c);
    talk_ctl_list(audio_list_emit, c);
    reply(c, ".");
}

static void do_get(cli_t *c, const char *param)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if      (!strcmp(param, "ae.stable_range"))       reply(c, "ae.stable_range=%d", a.stable_range);
    else if (!strcmp(param, "ae.hold_range"))         reply(c, "ae.hold_range=%d", a.hold_range);
    else if (!strcmp(param, "ae.speed"))              reply(c, "ae.speed=%d", a.speed);
    else if (!strcmp(param, "ae.exp_max"))            reply(c, "ae.exp_max=%d", a.exp_max);
    else if (!strcmp(param, "ae.enabled"))            reply(c, "ae.enabled=%d", a.enabled);
    else if (!strcmp(param, "night.mode"))            reply(c, "night.mode=%s", night_mode_name(n.override));
    else if (!strcmp(param, "night.state"))           reply(c, "night.state=%s", night_is_night() ? "night" : "day");
    else if (!strcmp(param, "night.trigger_hw_exp"))  reply(c, "night.trigger_hw_exp=%u", n.trigger_hw_exp);
    else if (!strcmp(param, "night.day_hw_exp"))      reply(c, "night.day_hw_exp=%u", n.day_hw_exp);
    else if (!strcmp(param, "night.confirm_samples")) reply(c, "night.confirm_samples=%d", n.confirm_samples);
    else if (!strcmp(param, "night.lock_ms"))         reply(c, "night.lock_ms=%d", n.lock_ms);
    else if (!strcmp(param, "isp.saturation"))        reply(c, "isp.saturation=%d", g_isp_saturation);
    else if (!strcmp(param, "isp.contrast"))          reply(c, "isp.contrast=%d", g_isp_contrast);
    else if (!strcmp(param, "isp.brightness"))        reply(c, "isp.brightness=%d", g_isp_brightness);
    else if (!strcmp(param, "isp.sharpness"))         reply(c, "isp.sharpness=%d", g_isp_sharpness);
    else if (!strcmp(param, "venc.minqp"))            reply(c, "venc.minqp=28 (read-only)");
    else if (!strcmp(param, "venc.maxqp"))            reply(c, "venc.maxqp=43 (read-only)");
    else if (!strcmp(param, "ctl.debug"))             reply(c, "ctl.debug=%d", debug_active());
    else if (!strcmp(param, "ctl.debug_remaining")) {
        pthread_mutex_lock(&g_dbg_lock);
        int r = ctl_debug_remaining_s(&g_dbg, mono_ms());
        pthread_mutex_unlock(&g_dbg_lock);
        reply(c, "ctl.debug_remaining=%d", r);
    } else {
        char line[320];
        if (night_ctl_get(param, line, sizeof(line)) == 0)          reply(c, "%s", line);
        else if (ledsys_ctl_get(param, line, sizeof(line)) == 0)    reply(c, "%s", line);
        else if (audio_out_ctl_get(param, line, sizeof(line)) == 0) reply(c, "%s", line);
        else if (imgq_ctl_get(param, line, sizeof(line)) == 0)      reply(c, "%s", line);
        else if (talk_ctl_get(param, line, sizeof(line)) == 0)      reply(c, "%s", line);
        else                                                   reply(c, "ERR unknown param %s", param);
    }
}

static void do_set(cli_t *c, const char *param, const char *value)
{
    ae_tuning_t    a; ae_get_tuning(&a);
    night_tuning_t n; night_get_tuning(&n);

    if (!strcmp(param, "ae.stable_range")) {
        a.stable_range = atoi(value);
        ae_set_tuning(&a);
        reply(c, "OK ae.stable_range=%d", a.stable_range);
    } else if (!strcmp(param, "ae.hold_range")) {
        a.hold_range = atoi(value);
        ae_set_tuning(&a);
        reply(c, "OK ae.hold_range=%d", a.hold_range);
    } else if (!strcmp(param, "ae.speed")) {
        a.speed = atoi(value);
        ae_set_tuning(&a);
        reply(c, "OK ae.speed=%d", a.speed);
    } else if (!strcmp(param, "ae.exp_max")) {
        a.exp_max = atoi(value);
        ae_set_tuning(&a);
        reply(c, "OK ae.exp_max=%d", a.exp_max);
    } else if (!strcmp(param, "ae.enabled")) {
        a.enabled = atoi(value) ? 1 : 0;
        ae_set_tuning(&a);
        reply(c, "OK ae.enabled=%d", a.enabled);
    } else if (!strcmp(param, "night.mode")) {
        night_override_t m;
        if (night_mode_parse(value, &m) != 0) {
            reply(c, "ERR invalid night.mode value %s (want auto|day|night)", value);
            return;
        }
        n.override = m;
        night_set_tuning(&n);
        reply(c, "OK night.mode=%s", night_mode_name(m));
    } else if (!strcmp(param, "night.trigger_hw_exp")) {
        n.trigger_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(c, "OK night.trigger_hw_exp=%u", n.trigger_hw_exp);
    } else if (!strcmp(param, "night.day_hw_exp")) {
        n.day_hw_exp = (uint32_t)strtoul(value, NULL, 10);
        night_set_tuning(&n);
        reply(c, "OK night.day_hw_exp=%u", n.day_hw_exp);
    } else if (!strcmp(param, "night.confirm_samples")) {
        n.confirm_samples = atoi(value);
        night_set_tuning(&n);
        reply(c, "OK night.confirm_samples=%d", n.confirm_samples);
    } else if (!strcmp(param, "night.lock_ms")) {
        n.lock_ms = atoi(value);
        night_set_tuning(&n);
        reply(c, "OK night.lock_ms=%d", n.lock_ms);
    } else if (!strcmp(param, "isp.saturation")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(c, "ERR isp.saturation range [-50,50]"); return; }
        if (isp_set_saturation(v) != 0) { reply(c, "ERR isp_set_saturation failed"); return; }
        g_isp_saturation = v;
        reply(c, "OK isp.saturation=%d", v);
    } else if (!strcmp(param, "isp.contrast")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(c, "ERR isp.contrast range [-50,50]"); return; }
        if (isp_set_contrast(v) != 0) { reply(c, "ERR isp_set_contrast failed"); return; }
        g_isp_contrast = v;
        reply(c, "OK isp.contrast=%d", v);
    } else if (!strcmp(param, "isp.brightness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(c, "ERR isp.brightness range [-50,50]"); return; }
        if (isp_set_brightness(v) != 0) { reply(c, "ERR isp_set_brightness failed"); return; }
        g_isp_brightness = v;
        reply(c, "OK isp.brightness=%d", v);
    } else if (!strcmp(param, "isp.sharpness")) {
        int v = atoi(value);
        if (v < -50 || v > 50) { reply(c, "ERR isp.sharpness range [-50,50]"); return; }
        if (isp_set_sharpness(v) != 0) { reply(c, "ERR isp_set_sharpness failed"); return; }
        g_isp_sharpness = v;
        reply(c, "OK isp.sharpness=%d", v);
    } else if (!strcmp(param, "venc.minqp") || !strcmp(param, "venc.maxqp")) {
        reply(c, "ERR %s is read-only in this version "
                  "(requires an encoder restart, not yet implemented)", param);
    } else {
        char msg[160];
        if (night_ctl_set(param, value, msg, sizeof(msg)) >= 0)          reply(c, "%s", msg);
        else if (ledsys_ctl_set(param, value, msg, sizeof(msg)) >= 0)    reply(c, "%s", msg);
        else if (audio_out_ctl_set(param, value, msg, sizeof(msg)) >= 0) reply(c, "%s", msg);
        else if (imgq_ctl_set(param, value, msg, sizeof(msg)) >= 0)      reply(c, "%s", msg);
        else if (talk_ctl_set(param, value, msg, sizeof(msg)) >= 0)      reply(c, "%s", msg);
        else                                                        reply(c, "ERR unknown param %s", param);
    }
}

/* Blocking byte-at-a-time line reader — traffic on this protocol is tiny
 * (a human moving sliders), so simplicity wins over buffered-recv efficiency.
 * The socket has SO_RCVTIMEO = CTL_IDLE_S: an idle client ends the read. */
static int read_line(int fd, char *buf, size_t cap)
{
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;  /* EOF, error or idle timeout */
        if (c == '\r') continue;
        if (c == '\n') { buf[i] = '\0'; return (int)i; }
        if (i < cap - 1) buf[i++] = c;
    }
}

static void handle_control_client(cli_t *c)
{
    char line[256];
    for (;;) {
        int n = read_line(c->fd, line, sizeof(line));
        if (n < 0) return;
        if (n == 0) continue;

        char *cmd = strtok(line, " ");
        if (!cmd) continue;

        /* Read-only gate: writes only from loopback or while DEBUG is on. */
        if (ctl_is_write_cmd(cmd) && !ctl_write_allowed(c->loopback, debug_active())) {
            reply(c, "%s", CTL_READONLY_MSG);
            continue;
        }

        if (!strcasecmp(cmd, "LIST")) {
            do_list(c);
        } else if (!strcasecmp(cmd, "GET")) {
            char *param = strtok(NULL, " ");
            if (!param) { reply(c, "ERR GET requires a param name"); continue; }
            do_get(c, param);
        } else if (!strcasecmp(cmd, "SET")) {
            char *param = strtok(NULL, " ");
            char *value = strtok(NULL, " ");
            if (!param || !value) { reply(c, "ERR SET requires param and value"); continue; }
            do_set(c, param, value);
        } else if (!strcasecmp(cmd, "DEBUG")) {
            char *a1 = strtok(NULL, " ");
            char *a2 = strtok(NULL, " ");
            char  out[96];
            pthread_mutex_lock(&g_dbg_lock);
            ctl_debug_command(&g_dbg, mono_ms(), c->loopback, a1, a2, out, sizeof(out));
            pthread_mutex_unlock(&g_dbg_lock);
            reply(c, "%s", out);
        } else {
            reply(c, "ERR unknown command %s", cmd);
        }
    }
}

static void *control_client_thread(void *arg)
{
    int    idx = (int)(intptr_t)arg;
    cli_t *c   = &g_cli[idx];
    printf("[control] tuning client connected%s\n", c->loopback ? " (local)" : "");
    handle_control_client(c);

    pthread_mutex_lock(&g_slot_lock);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    ctl_slots_release(&g_slots, idx);
    pthread_mutex_unlock(&g_slot_lock);
    printf("[control] tuning client disconnected\n");
    return NULL;
}

static void *control_accept_loop(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("[control] socket"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(CONTROL_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[control] bind"); close(server_fd); return NULL;
    }
    if (listen(server_fd, CTL_MAX_CLIENTS) < 0) {
        perror("[control] listen"); close(server_fd); return NULL;
    }

    g_server_fd = server_fd;
    printf("[control] tuning server listening on port %d\n", CONTROL_PORT);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CTL_STACK_BYTES);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    while (g_control_running) {
        struct sockaddr_storage client_addr;
        socklen_t alen = sizeof(client_addr);
        int fd = accept(server_fd, (struct sockaddr *)&client_addr, &alen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  /* server_fd closed by control_stop() */
        }

        pthread_mutex_lock(&g_slot_lock);
        int idx = ctl_slots_acquire(&g_slots);
        if (idx >= 0) {
            cli_t *c = &g_cli[idx];
            struct timeval rt = { CTL_IDLE_S, 0 }, st = { CTL_SNDTIMEO_S, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
            c->fd       = fd;
            c->loopback = ctl_peer_is_loopback((struct sockaddr *)&client_addr);
            c->need_nl  = 0;
            pthread_t t;
            if (pthread_create(&t, &attr, control_client_thread, (void *)(intptr_t)idx) != 0) {
                c->fd = -1;
                ctl_slots_release(&g_slots, idx);
                idx = -2;
            }
        }
        pthread_mutex_unlock(&g_slot_lock);
        if (idx < 0) {
            if (idx == -1) (void)send(fd, "ERR busy\n", 9, MSG_DONTWAIT | MSG_NOSIGNAL);
            close(fd);
        }
    }
    pthread_attr_destroy(&attr);
    return NULL;
}

int control_start(void)
{
    /* Tee stdout BEFORE any other init runs, so the full boot log (ISP,
     * VI, VENC) reaches the tuning tab's debug view, not just ae/night's
     * own logging. */
    g_orig_stdout = dup(STDOUT_FILENO);
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("[control] pipe"); return -1; }
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("[control] dup2"); return -1; }
    close(pipefd[1]);
    g_log_pipe_r = pipefd[0];
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffered so printf flushes promptly through the pipe */

    if (pthread_create(&g_log_tid, NULL, log_pump, NULL) != 0) {
        perror("[control] log_pump pthread_create");
        return -1;
    }

    g_control_running = 1;
    if (pthread_create(&g_control_tid, NULL, control_accept_loop, NULL) != 0) {
        perror("[control] pthread_create");
        g_control_running = 0;
        return -1;
    }
    return 0;
}

void control_stop(void)
{
    g_control_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }
    pthread_mutex_lock(&g_slot_lock);
    for (int i = 0; i < CTL_MAX_CLIENTS; i++) {
        /* shutdown() makes a client thread blocked in recv() return; the
         * thread closes its own fd. */
        if (g_cli[i].fd >= 0) shutdown(g_cli[i].fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_slot_lock);
    pthread_join(g_control_tid, NULL);
    /* log_pump isn't joined — it blocks on read() of the tee'd stdout pipe
     * and only returns when that pipe closes at process exit, which is
     * always imminent by the time control_stop() runs. */
}
