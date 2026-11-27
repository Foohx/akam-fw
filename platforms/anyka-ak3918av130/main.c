/* akamd: PoC-3 of the Rotatable 2K own-app study. Both VI channels are captured and encoded (main 2304x1296
 * H.264 Main, sub 640x360) and served over RTSP (TCP interleaved):
 *
 *   akamd [-p PORT] [-m MAIN_KBPS] [-s SUB_KBPS] [-g GOP] [-a AUTH_FILE] [-n FRAMES] [-t SECONDS]
 *               [-D 0|1] [-H HZ] [-T THRESHOLD] [--detect-on-count K] [--detect-off-s SECONDS] [--detect-debug] [--mqtt-conf FILE]
 *               [--zones-conf FILE] [--snapshot 0|1] [--snapshot-interval SECONDS] [--night auto|day|night] [--ir-led auto|off]
 *               [--image-brightness N] [--image-contrast N] [--image-sharpness N] [--mains-hz 50|60] [--wdr 0|1] [--audio 0|1]
 *               [--led auto|on|off] [--led-boot P] [--led-nowifi P] [--led-person P] [--led-stream P] [--led-night P] [--led-idle P]
 *               [--ptz-cal auto|off] [--ptz-park PAN:TILT] [--ptz-privacy 0|1]
 *               [--track 0|1] [--ptz-travel-budget-k 10..99999] [--track-return home|stay] [--track-return-delay-s N] [--track-grace-s N] [--track-band-pct 2..40] [--track-tilt 0|1] [--track-lead-pct 0..100] [--track-head-pct 0..50] [--track-gain-pan 200..4000] [--track-gain-tilt 200..4000] [--track-tilt-band-pct 2..40] [--track-debug 0..2] [--track-lag-ms -150..300] [--detect-age-window 0|1] [--track-tilt-return-s 0..3600] [--track-area NAME=PMIN:PMAX:TMIN:TMAX]... [--button-long-ms 1500..10000] [--track-invert-pan 0|1] [--track-invert-tilt 0|1]
 *               [--ptz-scene-check 0|1] [--ptz-scene-min 0.80..0.99] [--ptz-drift-ref 0|1] [--ptz-drift-correct off|adjust|full] [--ptz-boot-calibrate auto|always] [--ptz-tilt-rehome off|idle] [--ptz-tilt-rehome-gap-min 1..1440] [--api-port 0|1024..65535]
 *   akamd --ptz-stop   (one motor stop request on the motor node, used by the supervisor after the app died)
 *   akamd --api-key new --scope read|write|admin --name LABEL | list | revoke ID|NAME [--force]   (key tool, no hardware touched)
 *   long forms: --port --main-kbps --sub-kbps --gop --auth --frames --seconds --detect --detect-hz --detect-threshold
 *
 * Defaults: port 554 (A2K_RTSP_PORT: rtsp://<ip>:554/stream0 main, /stream1 sub), main 2000 kbit/s, sub 800 kbit/s, GOP 30 (2 s at 15 fps), credentials file
 * /mnt/sdcard/akam/rtsp.conf (absent = no authentication; see rtsp_auth.h for its one-line format).
 * Person detection (detect.c) runs alongside the streams: -D (default 1), -H 1..10 inferences/s (default 5),
 * -T classify threshold 500..950 (default 700), --detect-on-count 1..10 (default 2), --detect-off-s 1..60 (default 3), --detect-debug (per-inference frame age/count/score, 1 line/s, default off), --detect-log 0|1 (the same, conf detect_debug), --detect-head-thr 300..950 (default 600: the score a head box needs, conf detect_head_threshold; below -T the model runs at it and the lower-scored persons are dropped), --detect-model-thr 300..950 (experiment E1: the model threshold, lower than -T; the lower-scored persons are only logged);
 * state in /tmp/akam2k_detect.state and one log line per transition. A detection failure never stops the streams.
 * MQTT / Home Assistant (mqtt_bind.c): off unless the card file /mnt/sdcard/akam/mqtt (--mqtt-conf) exists and has
 * enabled=1; the broker password is read by the app itself from akam/mqtt_pass next to it, never from the command line.
 * Detection zones (zones.h): the optional card file /mnt/sdcard/akam/zones (--zones-conf) restricts the detection to up
 * to 4 named rectangles (percent of the image); a person counts by its feet point; one extra MQTT binary_sensor per zone.
 * No file, or an invalid one (logged once), means the whole image.
 * JPEG snapshot (snap.h): --snapshot 0|1 (default 1), --snapshot-interval 5..300 s (default 10): when the person goes
 * ON, one 640x360 JPEG of the sub stream is stored in /tmp and published on MQTT (needs the card file akam/mqtt).
 * Day/night (night_ctl.h): --night auto (default: the vendor day/night decision behind the shared 1080p policy) | day | night,
 * --ir-led auto (default: the IR LED lights in night mode) | off. Image (image.h): --image-brightness / --image-contrast /
 * --image-sharpness 0..100 (default: not applied, the ISP conf values stay), --mains-hz 50 (default) | 60, --wdr 1 (default: WDR re-applied after every ISP mode switch, like the stock) | 0. Microphone (audio.h): --audio 0 (default: the capture device is never opened, no audio track in the SDP) | 1 (an L16/8000 track on both streams, RTP over TCP, streamid=1). All RAM state: the
 * filter is left in the DAY position, the coil released and the IR LED off at stop. A failure never stops the streams.
 * PTZ (ptz.h, hal_ptz): the motor boots calibrated (a saved position from a clean stop, else a sweep to the stops then home) unless
 * --ptz-cal off (never touched, motion refused); --ptz-park PAN:TILT (default pan home, tilt soft min = looking down); --ptz-privacy 1 sends
 * no video while parked (RTSP stays up). MQTT buttons ptz_left/right/up/down/home/park/recal (Recalibrate PTZ) and sensors ptz_state/ptz_pan/ptz_tilt.
 * Test mode: --frames N (stop after N main-stream frames) and/or --seconds N (stop after N seconds); with neither the
 * program runs until SIGTERM/SIGINT. Both end with a clean teardown of venc/vi/sdk and print a summary.
 *
 * Files: main.c (this: options, signals, supervision), cap.c (all vendor calls: the PoC-2 sequence),
 * stream.c (frame ring between capture and clients), rtsp_session.c (the RTSP listener) and mqtt_bind.c (the only file that connect()s), plus the
 * shared RTSP/RTP/SDP/H.264 sources of src/net/rtsp and third_party/smolrtsp compiled unmodified (see the Makefile). No
 * vendor header is used. This file opens no device node, flash or nvram path; the vendor libraries open theirs. */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include "api_bind.h"
#include "api_events.h"
#include "audio.h"
#include "audio_out.h"
#include "speaker.h"
#include "api_httpd.h"
#include "settings.h"
#include "cap.h"
#include "isp.h"
#include "button.h"
#include "button_rt.h"
#include "leds.h"
#include "image.h"
#include "orient.h"
#include "log.h"
#include "mqtt_bind.h"
#include "night_ctl.h"
#include "onvif_services.h"
#include "pin.h"
#include "ramlog.h"
#include "patrol.h"
#include "ptz.h"
#include "scene.h"
#include "scene_check.h"
#include "rtsp_session.h"
#include "store.h"
#include "track_run.h"
#include "motion.h"
#include "motion_grid.h"
#include "video.h"
#include "zlive.h"
#include "zones.h"
#include "button_logic.h"
#include "hal_sys.h"
#include "rtp_clock.h"

#define DEFAULT_AUTH "/mnt/sdcard/akam/rtsp.conf"
#define STATS_EVERY_S 10
#define SYS_EVERY_S 30 /* hal_sys sample: one read of five /proc files */

volatile sig_atomic_t a2k_stop;
static volatile const char *volatile g_step = "start";
static struct timeval t0;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static long ms_since_start(void)
{
    struct timeval n;
    gettimeofday(&n, NULL);
    return (n.tv_sec - t0.tv_sec) * 1000L + (n.tv_usec - t0.tv_usec) / 1000L;
}

/* RAM copy of every log line (the stdout log goes to the card file, which is absent card-less): two tmpfs segments /tmp/akam2k_main.log and /tmp/akam2k_main.log.1, written in turn, each capped
 * at A2K_RAMLOG_MAX; the segment headers carry a sequence number so the order survives an app restart (ramlog.h, tested by tests/rot2k_ramlog_unit.c). tmpfs only, never flash. Read both: every line starts with its ms clock. */
static struct a2k_ramlog g_ram = A2K_RAMLOG_INIT;

void a2k_log_ram_set(const char *path, long max) /* tests: another file (path and path.1) / cap; closes the current one */
{
    pthread_mutex_lock(&g_log_mu);
    a2k_ramlog_set(&g_ram, path, max);
    pthread_mutex_unlock(&g_log_mu);
}

static void ramlog_put(const char *line, size_t n) { a2k_ramlog_put(&g_ram, line, n); }

void a2k_log(const char *fmt, ...)
{
    va_list ap;
    char body[512], line[560];
    int n;
    pthread_mutex_lock(&g_log_mu);
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    n = snprintf(line, sizeof line, "[%7ldms] %s\n", ms_since_start(), body);
    if (n > (int)sizeof line - 1)
        n = (int)sizeof line - 1;
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    if (n > 0)
        ramlog_put(line, (size_t)n);
    pthread_mutex_unlock(&g_log_mu);
}

void a2k_set_step(const char *step) { g_step = step; }

/* Reads one "Key:   value kB" line of a /proc text file; -1 when absent. */
static long proc_kb(const char *file, const char *key)
{
    char line[160];
    long v = -1;
    size_t kl = strlen(key);
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') {
            v = strtol(line + kl + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

void a2k_mem_report(const char *tag)
{
    a2k_log("mem %s: VmRSS %ld kB, VmHWM %ld kB, MemFree %ld kB, MemAvailable %ld kB", tag,
            proc_kb("/proc/self/status", "VmRSS"), proc_kb("/proc/self/status", "VmHWM"),
            proc_kb("/proc/meminfo", "MemFree"), proc_kb("/proc/meminfo", "MemAvailable"));
}

static double cpu_seconds(void)
{
    struct rusage u;
    if (getrusage(RUSAGE_SELF, &u) < 0)
        return 0;
    return (double)u.ru_utime.tv_sec + (double)u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}

long a2k_mem_avail_kb(void) { return proc_kb("/proc/meminfo", "MemAvailable"); }

/* Async-signal-safe: write() and _exit() only. */
static void say(const char *a, const char *b)
{
    if (write(1, a, strlen(a)) < 0) { }
    if (b && write(1, b, strlen(b)) < 0) { }
    if (write(1, "\n", 1) < 0) { }
}

static void on_alarm(int sig)
{
    (void)sig;
    a2k_pin_emergency_low(); /* never die with the IR-cut coil energised */
    audio_out_emergency_off(); /* amplifier off, crash guard removed (a stop is not a kernel crash) */
    say("akamd: TIMEOUT, guard fired in step: ", (const char *)g_step);
    _exit(124);
}

static void on_crash(int sig)
{
    (void)sig;
    a2k_ptz_emergency_stop(); /* a dead process must not leave the head walking toward a mechanical stop */
    a2k_pin_emergency_low();
    audio_out_emergency_off();
    say("akamd: CRASH signal in step: ", (const char *)g_step);
    _exit(2);
}

static void on_term(int sig)
{
    (void)sig;
    a2k_stop = 1;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_crash;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sa.sa_handler = on_term;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static int usage(void)
{
    fprintf(stderr, "usage: akamd [-p port] [-m main_kbps] [-s sub_kbps] [-g gop] [-a auth_file] [-n frames] "
                    "[-t seconds] [-D 0|1] [-H hz] [-T threshold] [--detect-debug] [--mqtt-conf file] [--zones-conf file] "
                    "[--snapshot 0|1] [--snapshot-interval seconds] [--night auto|day|night] [--ir-led auto|off] "
                    "[--image-brightness 0..100] [--image-contrast 0..100] [--image-sharpness 0..100] [--mains-hz 50|60] [--wdr 0|1] [--audio 0|1] [--video<0|1>-kbps|max-kbps|fps|min-qp|max-qp|gop N] [--video<0|1>-rc vbr|cbr|avbr] [--video-live 0|1] [--venc-test 0|1] [--isp-test 0|1] [--api-port 0|1024..65535] [--onvif 0|1] [--onvif-port 1024..65535]\n");
    return 3;
}

/* Strict decimal parse: whole string, range-checked; -1 on any error. */
static int parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

static void stream_stats(const char *tag, a2k_stream_t *s, unsigned long long *last_bytes, unsigned long *last_frames,
                         double secs)
{
    unsigned long frames, keys, over;
    unsigned long long bytes;
    pthread_mutex_lock(&s->mu);
    frames = s->frames;
    keys = s->keys;
    over = s->oversized;
    bytes = s->bytes;
    pthread_mutex_unlock(&s->mu);
    if (secs > 0)
        a2k_log("%s %s: %.1f fps, %.0f kbit/s over %.1f s (total %lu frames, %lu keyframes, %llu bytes, %lu dropped)",
                tag, s->name, (double)(frames - *last_frames) / secs,
                (double)(bytes - *last_bytes) * 8.0 / 1000.0 / secs, secs, frames, keys, bytes, over);
    *last_bytes = bytes;
    *last_frames = frames;
}

/* The day/night engine reports to the MQTT module (a small copy under its lock, never the network). */
static void night_to_mqtt(const a2k_night_info_t *i)
{
    struct a2k_mqtt_night n;
    char d[A2K_EVT_DATA];
    memset(&n, 0, sizeof n);
    n.valid = i->valid;
    n.night = i->night;
    n.mode = i->mode;
    n.ir_on = i->ir_on;
    n.auto_off = i->auto_off;
    n.switches = i->switches;
    n.last_switch_epoch = i->last_switch_epoch;
    snprintf(n.why, sizeof n.why, "%s", i->why);
    a2k_mqtt_night(&n);
    snprintf(d, sizeof d, "{\"night\":%s,\"ir_on\":%s}", i->night ? "true" : "false", i->ir_on ? "true" : "false");
    a2k_api_event("night", d);
}

/* The Home Assistant controls (mqtt_cfg.h A2K_E_LED0..): 0 status LED, 1 IR LED, 2.. the rule patterns. Runtime only. */
static int led_get(int ent, char *out, size_t cap)
{
    if (ent == 0)
        snprintf(out, cap, "%s", a2k_leds_mode_name(a2k_leds_run_mode()));
    else if (ent == 1)
        snprintf(out, cap, "%s", a2k_night_ir_auto() ? "auto" : "off");
    else if (ent >= 2 && ent < 2 + A2K_RULE_N)
        a2k_leds_run_pattern(ent - 2, out, cap);
    else
        return -1;
    return 0;
}

static int led_set(int ent, const char *val)
{
    if (ent == 0)
        return a2k_leds_run_set_mode(a2k_leds_parse_mode(val));
    if (ent == 1)
        return !strcmp(val, "auto") ? a2k_night_set_ir(1) : !strcmp(val, "off") ? a2k_night_set_ir(0) : -1;
    if (ent >= 2 && ent < 2 + A2K_RULE_N)
        return a2k_leds_run_set_pattern(ent - 2, val);
    return -1;
}

static const a2k_mqtt_leds_t led_ctl = { led_get, led_set };

/* The PTZ thread reports its state to the MQTT module (a small copy under its lock, never the network); the buttons queue a command. */
static void ptz_to_mqtt(const a2k_ptz_info_t *i)
{
    char d[A2K_EVT_DATA];
    a2k_mqtt_ptz(a2k_ptz_state_name(i->state), i->known, (int)i->pan, (int)i->tilt);
    snprintf(d, sizeof d, "{\"state\":\"%s\",\"known\":%s,\"pan\":%d,\"tilt\":%d}", a2k_ptz_state_name(i->state),
             i->known ? "true" : "false", i->known ? (int)i->pan : -1, i->known ? (int)i->tilt : -1);
    a2k_api_event("ptz", d);
}

/* Motion state changes (motion.h): an event on /events and, with MQTT, the binary sensor / direction sensor. Called from the detection thread, never blocks. */
static void motion_to_events(const struct a2k_motion_info *i)
{
    char d[A2K_EVT_DATA];
    snprintf(d, sizeof d, "{\"active\":%s,\"dir\":\"%s\",\"x\":%d,\"y\":%d}", i->active ? "true" : "false", mg_dir_name(i->dir), i->x, i->y);
    a2k_api_event("motion", d);
    a2k_mqtt_motion(i->active, mg_dir_name(i->dir));
}

static const a2k_mqtt_ptz_t ptz_ctl = { a2k_ptz_command, a2k_ptz_vel_manual, a2k_ptz_manual_stop, };

/* The tracking / zones / tracking-areas controls (mqtt_cfg.h, the ctl group): index k of A2K_CTL_*. */
static int ctl_get(int k, char *out, size_t cap)
{
    a2k_track_t *t = a2k_track_run_get();
    if (k == A2K_CTL_ZONES || k == A2K_CTL_IGNORE) {
        a2k_zlive_get_text(k == A2K_CTL_ZONES ? A2K_ZF_ZONES : A2K_ZF_IGNORE, out, cap);
        return 0;
    }
    if (k >= A2K_CTL_FLIP && k <= A2K_CTL_CEILING) { /* the picture orientation: not a tracker control, it exists without tracking */
        int f, m;
        a2k_orient_get(&f, &m);
        snprintf(out, cap, "%s", (k == A2K_CTL_FLIP ? f : k == A2K_CTL_MIRROR ? m : f && m) ? "ON" : "OFF");
        return 0;
    }
    if (!t)
        return -1;
    if (k == A2K_CTL_TRACK) {
        snprintf(out, cap, "%s", a2k_track_enabled(t) ? "ON" : "OFF");
        return 0;
    }
    if (k == A2K_CTL_PANONLY) {
        snprintf(out, cap, "%s", a2k_track_get_tilt(t) ? "OFF" : "ON");
        return 0;
    }
    if (k == A2K_CTL_AREAS) {
        a2k_track_areas_text(t, out, cap);
        return 0;
    }
    if (k >= A2K_CTL_SLOT0 && (k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT == 0) {
        a2k_tareas_t a;
        int s = (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT;
        a2k_track_areas(t, &a);
        if (s >= a.n)
            return -1;
        snprintf(out, cap, "%s", a.a[s].enabled ? "ON" : "OFF");
        return 0;
    }
    return -1;
}

static int ctl_set(int k, const char *val)
{
    a2k_track_t *t = a2k_track_run_get();
    if (k == A2K_CTL_ZONES || k == A2K_CTL_IGNORE)
        return a2k_zlive_set_text(k == A2K_CTL_ZONES ? A2K_ZF_ZONES : A2K_ZF_IGNORE, val);
    if (k >= A2K_CTL_FLIP && k <= A2K_CTL_CEILING) { /* through the settings registry: the same setter as PATCH /settings, saved like any user change */
        static const char *const key[3] = { "image.flip", "image.mirror", "image.ceiling" };
        const char *v = !strcmp(val, "ON") ? "1" : "0";
        char detail[64];
        a2k_log("image: %s %s (Home Assistant)", key[k - A2K_CTL_FLIP], v);
        return a2k_settings_apply(1, &key[k - A2K_CTL_FLIP], &v, detail, sizeof detail);
    }
    if (!t)
        return -1;
    if (k == A2K_CTL_TRACK) {
        a2k_track_set_enabled(t, !strcmp(val, "ON"));
        a2k_log("track: person tracking %s (Home Assistant)", !strcmp(val, "ON") ? "on" : "off");
        return 0;
    }
    if (k == A2K_CTL_PANONLY) {
        a2k_log("track: tilt %s (Home Assistant)", !strcmp(val, "ON") ? "off (pan only)" : "follows (pan and tilt)");
        return a2k_track_set_tilt(t, !strcmp(val, "ON") ? 0 : 1);
    }
    if (k == A2K_CTL_NEWAREA)
        return a2k_track_area_new(t, val);
    if (k >= A2K_CTL_SLOT0) {
        int s = (k - A2K_CTL_SLOT0) / A2K_CTL_PER_SLOT;
        switch ((k - A2K_CTL_SLOT0) % A2K_CTL_PER_SLOT) {
        case 0: return a2k_track_area_enable(t, s, !strcmp(val, "ON"));
        case 1: return a2k_track_area_corner(t, s, 0);
        case 2: return a2k_track_area_corner(t, s, 1);
        default: return a2k_track_area_delete(t, s);
        }
    }
    return -1;
}

static int ctl_areas(char names[][A2K_ZONE_NAME_MAX + 1])
{
    a2k_track_t *t = a2k_track_run_get();
    a2k_tareas_t a;
    if (!t)
        return 0;
    a2k_track_areas(t, &a);
    for (int i = 0; i < a.n; i++)
        snprintf(names[i], A2K_ZONE_NAME_MAX + 1, "%s", a.a[i].name);
    return a.n;
}

static const a2k_mqtt_ctl_t ctl_ops = { ctl_get, ctl_set, ctl_areas };

/* The video settings entities (mqtt_cfg.h, the vid group): k = stream * A2K_VP_N + parameter. */
static int vid_get(int k, char *out, size_t cap) { return a2k_vid_get_text(k / A2K_VP_N, k % A2K_VP_N, out, cap); }
static int vid_set_cb(int k, const char *val) { return a2k_vid_set(k / A2K_VP_N, k % A2K_VP_N, val); }
static const a2k_mqtt_vid_t vid_ctl = { vid_get, vid_set_cb };

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "port", required_argument, NULL, 'p' },     { "main-kbps", required_argument, NULL, 'm' },
        { "sub-kbps", required_argument, NULL, 's' }, { "gop", required_argument, NULL, 'g' },
        { "auth", required_argument, NULL, 'a' },     { "frames", required_argument, NULL, 'n' },
        { "seconds", required_argument, NULL, 't' },  { "detect", required_argument, NULL, 'D' },
        { "detect-hz", required_argument, NULL, 'H' }, { "detect-threshold", required_argument, NULL, 'T' },
        { "detect-on-count", required_argument, NULL, 1000 }, { "detect-off-s", required_argument, NULL, 1001 },
        { "detect-debug", no_argument, NULL, 1002 },
        { "detect-log", required_argument, NULL, 1060 },
        { "detect-model-thr", required_argument, NULL, 1061 },
        { "mqtt-conf", required_argument, NULL, 1003 },
        { "zones-conf", required_argument, NULL, 1004 },
        { "snapshot", required_argument, NULL, 1005 },
        { "snapshot-interval", required_argument, NULL, 1006 },
        { "night", required_argument, NULL, 1007 },
        { "ir-led", required_argument, NULL, 1008 },
        { "image-brightness", required_argument, NULL, 1009 },
        { "image-contrast", required_argument, NULL, 1010 },
        { "image-sharpness", required_argument, NULL, 1011 },
        { "mains-hz", required_argument, NULL, 1012 },
        { "wdr", required_argument, NULL, 1013 },
        { "audio", required_argument, NULL, 1014 },
        { "led", required_argument, NULL, 1015 },
        { "led-boot", required_argument, NULL, 1016 },
        { "led-nowifi", required_argument, NULL, 1017 },
        { "led-person", required_argument, NULL, 1018 },
        { "led-stream", required_argument, NULL, 1019 },
        { "led-night", required_argument, NULL, 1020 },
        { "led-idle", required_argument, NULL, 1021 },
        { "ptz-cal", required_argument, NULL, 1022 },
        { "ptz-park", required_argument, NULL, 1023 },
        { "ptz-privacy", required_argument, NULL, 1024 },
        { "track", required_argument, NULL, 1025 },
        { "track-area", required_argument, NULL, 1026 },
        { "button-long-ms", required_argument, NULL, 1027 },
        { "track-invert-pan", required_argument, NULL, 1028 },
        { "track-invert-tilt", required_argument, NULL, 1029 },
        { "ptz-scene-check", required_argument, NULL, 1030 },
        { "ptz-scene-min", required_argument, NULL, 1031 },
        { "ptz-drift-ref", required_argument, NULL, 1180 },
        { "ptz-drift-correct", required_argument, NULL, 1181 },
        { "ptz-boot-calibrate", required_argument, NULL, 1182 },
        { "ptz-tilt-rehome", required_argument, NULL, 1183 },
        { "ptz-tilt-rehome-gap-min", required_argument, NULL, 1184 },
        { "ptz-travel-budget-k", required_argument, NULL, 1038 },
        { "track-return", required_argument, NULL, 1032 },
        { "track-return-delay-s", required_argument, NULL, 1033 },
        { "track-grace-s", required_argument, NULL, 1034 },
        { "track-band-pct", required_argument, NULL, 1035 },
        { "track-tilt", required_argument, NULL, 1036 },
        { "track-lead-pct", required_argument, NULL, 1037 },
        { "track-head-pct", required_argument, NULL, 1039 },
        { "track-gain-pan", required_argument, NULL, 1042 },
        { "track-gain-tilt", required_argument, NULL, 1043 },
        { "track-tilt-band-pct", required_argument, NULL, 1044 },
        { "track-debug", required_argument, NULL, 1040 },
        { "track-tilt-return-s", required_argument, NULL, 1041 },
        { "track-motion", required_argument, NULL, 1045 },
        { "track-zone-pct", required_argument, NULL, 1046 },
        { "track-zone-y-pct", required_argument, NULL, 1047 },
        { "track-zone-head-pct", required_argument, NULL, 1048 },
        { "track-lease-ms", required_argument, NULL, 1049 },
        { "track-lost-policy", required_argument, NULL, 1050 },
        { "track-tilt-back-s", required_argument, NULL, 1051 },
        { "track-close-scan", required_argument, NULL, 1090 },
        { "track-close-scan-up", required_argument, NULL, 1091 },
        { "track-close-scan-down", required_argument, NULL, 1092 },
        { "track-close-scan-cooldown-s", required_argument, NULL, 1093 },
        { "track-search", required_argument, NULL, 1094 },
        { "track-pass-s", required_argument, NULL, 1095 },
        { "track-pass-home-s", required_argument, NULL, 1096 },
        { "track-search-cooldown-s", required_argument, NULL, 1097 },
        { "track-reacq-s", required_argument, NULL, 1098 },
        { "motion", required_argument, NULL, 1099 },
        { "motion-sensitivity", required_argument, NULL, 1107 },
        { "motion-source", required_argument, NULL, 1108 },
        { "motion-md-frame", required_argument, NULL, 1109 },
        { "track-lag-ms", required_argument, NULL, 1062 },
        { "detect-age-window", required_argument, NULL, 1063 },
        { "detect-head-thr", required_argument, NULL, 1064 },
        { "video0-kbps", required_argument, NULL, 1100 },
        { "video0-max-kbps", required_argument, NULL, 1101 },
        { "video0-fps", required_argument, NULL, 1102 },
        { "video0-min-qp", required_argument, NULL, 1103 },
        { "video0-max-qp", required_argument, NULL, 1104 },
        { "video0-gop", required_argument, NULL, 1105 },
        { "video0-rc", required_argument, NULL, 1106 },
        { "video1-kbps", required_argument, NULL, 1110 },
        { "video1-max-kbps", required_argument, NULL, 1111 },
        { "video1-fps", required_argument, NULL, 1112 },
        { "video1-min-qp", required_argument, NULL, 1113 },
        { "video1-max-qp", required_argument, NULL, 1114 },
        { "video1-gop", required_argument, NULL, 1115 },
        { "video1-rc", required_argument, NULL, 1116 },
        { "video-live", required_argument, NULL, 1120 },
        { "venc-test", required_argument, NULL, 1121 },
        { "isp-test", required_argument, NULL, 1122 },
        { "api-port", required_argument, NULL, 1130 },
        { "onvif", required_argument, NULL, 1140 },
        { "onvif-port", required_argument, NULL, 1141 },
        { NULL, 0, NULL, 0 },
    };
    static a2k_stream_t streams[2];
    a2k_stream_t *sp[2] = { &streams[0], &streams[1] };
    struct a2k_cap_cfg cfg = { 2000, 800, 30, { 0 }, { 0 }, 1, 0, 0 };
    const char *auth = DEFAULT_AUTH, *mqtt_conf = A2K_MQTT_CONF_DEFAULT, *zones_conf = A2K_ZONES_DEFAULT;
    a2k_night_cfg_t ncfg;
    a2k_image_cfg_t icfg;
    int port = A2K_RTSP_PORT, api_port = A2K_API_PORT, frames = 0, seconds = 0, ch, rc = 0, led_mode = A2K_LEDS_ON, audio_on = 0;
    int onvif_on = 0, onvif_port = 0;
    const char *led_pat[A2K_RULE_N] = { 0 };
    a2k_ptz_cfg_t pcfg;
    a2k_vid_t vid_opt[A2K_VID_STREAMS], vid_base[A2K_VID_STREAMS];
    unsigned vid_set[A2K_VID_STREAMS] = { 0, 0 };
    int track_inv_pan = 1, track_inv_tilt = 0, scene_min = SC_MIN_DEFAULT;
    int motion_on = 1, motion_sens = 5, motion_src = A2K_MOTION_DIFF, motion_md_pic = 1;
    int ptz_on = 0, track_on = 0, button_long = BTN_LONG_DEFAULT_MS;
    struct tl_cfg tune;
    int track_debug = 0;
    char tarea_conf[A2K_TAREA_MAX * 48 + 1] = { 0 };
    int tarea_n = 0;
    long t_start;
    double cpu0, cpu_last;
    long t_last, t_sys = -SYS_EVERY_S * 1000L;
    hal_sys_t sys_st = { 0, 0, 0 };
    unsigned long long lb[2] = { 0, 0 }, tb[2];
    unsigned long lf[2] = { 0, 0 }, tf[2];

    if (argc == 2 && strcmp(argv[1], "--ptz-stop") == 0) /* the supervisor, after the app died: one motor stop request, nothing else */
        return a2k_ptz_oneshot_stop() == 0 ? 0 : 1;
    if (argc >= 2 && strcmp(argv[1], "--api-key") == 0) { /* the on-camera key tool: no hardware, no thread, exits here */
        a2k_api_env_t kenv;
        a2k_api_env_2k(&kenv);
        return a2k_api_key_cli(&kenv, argc - 2, argv + 2);
    }
    a2k_detect_defaults(&cfg.det);
    a2k_snap_defaults(&cfg.snap);
    a2k_night_defaults(&ncfg);
    a2k_image_defaults(&icfg);
    a2k_ptz_defaults(&pcfg);
    memset(vid_opt, 0, sizeof vid_opt);
    tl_cfg_defaults(&tune);
    tl_cfg_geometry_2k(&tune);
    tune.search_mode = TL_SEARCH_MOTION; /* the 2K default (2026-11-26): the motion-aware search; track_search legacy / API track.search legacy bring the old one back */
    while ((ch = getopt_long(argc, argv, "p:m:s:g:a:n:t:D:H:T:", longopts, NULL)) != -1) {
        switch (ch) {
        case 'p': if (parse_int(optarg, 1, 65535, &port)) return usage(); break;
        case 'm': if (parse_int(optarg, 64, 8000, &cfg.main_kbps)) return usage(); break;
        case 's': if (parse_int(optarg, 32, 2000, &cfg.sub_kbps)) return usage(); break;
        case 'g': if (parse_int(optarg, 1, 300, &cfg.gop)) return usage(); break;
        case 'a': auth = optarg; break;
        case 'n': if (parse_int(optarg, 1, 1000000, &frames)) return usage(); break;
        case 't': if (parse_int(optarg, 1, 86400, &seconds)) return usage(); break;
        case 'D': if (parse_int(optarg, 0, 1, &cfg.det.enabled)) return usage(); break;
        case 'H': if (parse_int(optarg, 1, 10, &cfg.det.hz)) return usage(); break;
        case 'T': if (parse_int(optarg, 500, 950, &cfg.det.threshold)) return usage(); break;
        case 1000: if (parse_int(optarg, 1, 10, &cfg.det.on_count)) return usage(); break;
        case 1001: {
            int sec;
            if (parse_int(optarg, 1, 60, &sec)) return usage();
            cfg.det.off_ms = sec * 1000;
            break;
        }
        case 1002: cfg.det.debug = 1; break;
        case 1060: if (parse_int(optarg, 0, 1, &cfg.det.debug)) return usage(); break;
        case 1061: if (parse_int(optarg, 300, 950, &cfg.det.model_thr)) return usage(); break;
        case 1003: mqtt_conf = optarg; break; /* a path only: the password is read from that file by the app itself */
        case 1004: zones_conf = optarg; break;
        case 1005: if (parse_int(optarg, 0, 1, &cfg.snap.enabled)) return usage(); break;
        case 1006: if (parse_int(optarg, 5, 300, &cfg.snap.interval_s)) return usage(); break;
        case 1007: if ((ncfg.mode = a2k_night_parse_mode(optarg)) < 0) return usage(); break;
        case 1008: if ((ncfg.ir_led_auto = a2k_night_parse_ir(optarg)) < 0) return usage(); break;
        case 1009: if (parse_int(optarg, 0, 100, &icfg.brightness)) return usage(); break;
        case 1010: if (parse_int(optarg, 0, 100, &icfg.contrast)) return usage(); break;
        case 1011: if (parse_int(optarg, 0, 100, &icfg.sharpness)) return usage(); break;
        case 1012: if (parse_int(optarg, 50, 60, &icfg.mains_hz) || (icfg.mains_hz != 50 && icfg.mains_hz != 60)) return usage(); break;
        case 1013: if (parse_int(optarg, 0, 1, &ncfg.wdr)) return usage(); break;
        case 1014: if (parse_int(optarg, 0, 1, &audio_on)) return usage(); break;
        case 1015: if ((led_mode = a2k_leds_parse_mode(optarg)) < 0) return usage(); break;
        case 1016: case 1017: case 1018: case 1019: case 1020: case 1021: led_pat[ch - 1016] = optarg; break; /* validated by the engine: a bad one is logged and the default stays */
        case 1022: if ((pcfg.cal = a2k_ptz_parse_cal(optarg)) < 0) return usage(); break;
        case 1023: if (a2k_ptz_parse_park(optarg, &pcfg.park_pan, &pcfg.park_tilt)) return usage(); break;
        case 1024: if (parse_int(optarg, 0, 1, &pcfg.privacy)) return usage(); break;
        case 1025: if (parse_int(optarg, 0, 1, &track_on)) return usage(); break;
        case 1026: { /* NAME=PMIN:PMAX:TMIN:TMAX, repeatable (up to 4); checked as the persisted line it becomes */
            char line[96];
            a2k_tareas_t tmp;
            size_t used = strlen(tarea_conf);
            int w = snprintf(line, sizeof line, "track_area_%s\n", optarg);
            if (w < 0 || w >= (int)sizeof line || tarea_n >= A2K_TAREA_MAX || a2k_tareas_parse(line, (size_t)w, &tmp) != 0 ||
                used + (size_t)w >= sizeof tarea_conf)
                return usage();
            memcpy(tarea_conf + used, line, (size_t)w + 1);
            tarea_n++;
            break;
        }
        case 1028: if (parse_int(optarg, 0, 1, &track_inv_pan)) return usage(); break;
        case 1029: if (parse_int(optarg, 0, 1, &track_inv_tilt)) return usage(); break;
        case 1030: if (parse_int(optarg, 0, 1, &pcfg.scene_check)) return usage(); break;
        case 1031: if ((scene_min = sc_parse_min(optarg)) < 0) return usage(); break;
        case 1180: if (parse_int(optarg, 0, 1, &pcfg.drift_ref)) return usage(); break;
        case 1181: if ((pcfg.drift_correct = a2k_ptz_parse_drift_correct(optarg)) < 0) return usage(); break;
        case 1182: if ((pcfg.boot_always = a2k_ptz_parse_boot_calibrate(optarg)) < 0) return usage(); break;
        case 1183: if ((pcfg.tilt_rehome = a2k_ptz_parse_tilt_rehome(optarg)) < 0) return usage(); break;
        case 1184: { int g; if (parse_int(optarg, 1, 1440, &g)) return usage(); pcfg.rehome_gap_min = (unsigned)g; break; }
        case 1038: { int k; if (parse_int(optarg, 10, 99999, &k)) return usage(); pcfg.travel_budget = (unsigned long)k * 1000UL; break; }
        case 1032: if (!strcmp(optarg, "home")) tune.return_home = 1; else if (!strcmp(optarg, "stay")) tune.return_home = 0; else return usage(); break;
        case 1033: if (parse_int(optarg, 0, 86400, &tune.return_delay_s)) return usage(); break;
        case 1034: if (parse_int(optarg, 0, 600, &tune.lost_grace_s)) return usage(); break;
        case 1035: if (parse_int(optarg, 2, 40, &tune.deadband_pct)) return usage(); break;
        case 1036: if (parse_int(optarg, 0, 1, &tune.tilt_enable)) return usage(); break;
        case 1037: if (parse_int(optarg, 0, 100, &tune.lead_pct)) return usage(); break;
        case 1041: if (parse_int(optarg, 0, 3600, &tune.tilt_return_s)) return usage(); break;
        case 1040: if (parse_int(optarg, 0, 2, &track_debug)) return usage(); break;
        case 1042: if (parse_int(optarg, 200, 4000, &tune.gain_pan)) return usage(); break;
        case 1043: if (parse_int(optarg, 200, 4000, &tune.gain_tilt)) return usage(); break;
        case 1044: if (parse_int(optarg, 2, 40, &tune.tband_pct)) return usage(); break;
        case 1039: if (parse_int(optarg, 0, 50, &tune.head_pct)) return usage(); break;
        case 1045: if (!strcmp(optarg, "continuous")) tune.motion = TL_MOTION_CONT; else if (!strcmp(optarg, "steps")) tune.motion = TL_MOTION_STEPS; else return usage(); break;
        case 1046: if (parse_int(optarg, 0, 90, &tune.zone_x_pct)) return usage(); break;
        case 1047: if (parse_int(optarg, 0, 90, &tune.zone_y_pct)) return usage(); break;
        case 1048: if (parse_int(optarg, 0, 90, &tune.zone_head_pct)) return usage(); break;
        case 1049: if (parse_int(optarg, 150, 2000, &tune.lease_ms)) return usage(); break;
        case 1050: if (tl_lost_parse(optarg, &tune) != 0) return usage(); break;
        case 1090: if (parse_int(optarg, 0, 1, &tune.close_scan)) return usage(); break;
        case 1091: if (parse_int(optarg, 0, 600, &tune.close_scan_up)) return usage(); break;
        case 1092: if (parse_int(optarg, 0, 600, &tune.close_scan_down)) return usage(); break;
        case 1093: if (parse_int(optarg, 0, 600, &tune.close_scan_cooldown_s)) return usage(); break;
        case 1094: if (!strcmp(optarg, "motion")) tune.search_mode = TL_SEARCH_MOTION; else if (!strcmp(optarg, "legacy")) tune.search_mode = TL_SEARCH_LEGACY; else return usage(); break;
        case 1095: if (parse_int(optarg, 0, 60, &tune.pass_s)) return usage(); break;
        case 1096: if (parse_int(optarg, 0, 600, &tune.pass_home_s)) return usage(); break;
        case 1097: if (parse_int(optarg, 0, 3600, &tune.search_cooldown_s)) return usage(); break;
        case 1098: if (parse_int(optarg, 0, 60, &tune.reacq_s)) return usage(); break;
        case 1099: if (parse_int(optarg, 0, 1, &motion_on)) return usage(); break;
        case 1107: if (parse_int(optarg, 1, 10, &motion_sens)) return usage(); break;
        case 1108: if (!strcmp(optarg, "md")) motion_src = A2K_MOTION_MD; else if (!strcmp(optarg, "diff")) motion_src = A2K_MOTION_DIFF; else return usage(); break;
        case 1109: if (!strcmp(optarg, "picture")) motion_md_pic = 1; else if (!strcmp(optarg, "sensor")) motion_md_pic = 0; else return usage(); break;
        case 1051: if (parse_int(optarg, 0, 600, &tune.tilt_back_s)) return usage(); break;
        case 1062: if (parse_int(optarg, -150, 300, &tune.lag_ms)) return usage(); break;
        case 1063: if (parse_int(optarg, 0, 1, &cfg.det.age_window)) return usage(); break;
        case 1064: if (parse_int(optarg, 300, 950, &cfg.det.head_thr)) return usage(); break;
        case 1027: if (parse_int(optarg, BTN_LONG_MIN_MS, BTN_LONG_MAX_MS, &button_long)) return usage(); break;
        case 1120: if (parse_int(optarg, 0, 1, &cfg.video_live)) return usage(); break;
        case 1121: if (parse_int(optarg, 0, 1, &cfg.venc_test)) return usage(); break;
        case 1122: if (parse_int(optarg, 0, 1, &cfg.isp_test)) return usage(); break;
        case 1130: if (parse_int(optarg, 0, 65535, &api_port) || (api_port && api_port < 1024)) return usage(); break; /* 0 = no API */
        case 1140: if (parse_int(optarg, 0, 1, &onvif_on)) return usage(); break; /* ONVIF: opt-in (onvif_services.h) */
        case 1141: if (parse_int(optarg, 1024, 65535, &onvif_port)) return usage(); break;
        case 1100: case 1101: case 1102: case 1103: case 1104: case 1105: case 1106:
        case 1110: case 1111: case 1112: case 1113: case 1114: case 1115: case 1116: { /* --video<N>-<param>: range checked here, the whole set below */
            int vc = (ch - 1100) / 10, vp = (ch - 1100) % 10, vv;
            if (a2k_vid_parse(vp, optarg, &vv) != 0 || !a2k_vid_value_ok(vc, vp, vv))
                return usage();
            vid_opt[vc].v[vp] = vv;
            vid_set[vc] |= 1u << vp;
            break;
        }
        default: return usage();
        }
    }
    if (optind != argc)
        return usage();

    gettimeofday(&t0, NULL);
    /* Redirected to the card log, stdout would be block buffered: line buffered, stderr unbuffered (a2k_log also flushes). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    install_signals();
    a2k_speaker_init(); /* before anything reaches the speaker engine: its conf dir and the opt-in default */
    a2k_log("akamd: port %d, main %d kbit/s, sub %d kbit/s, GOP %d, auth file %s, test limits: %d frames, %d s",
            port, cfg.main_kbps, cfg.sub_kbps, cfg.gop, auth, frames, seconds);
    a2k_log("akamd: detection %s, %d Hz, classify threshold %d, ON after %d, OFF after %d ms",
            cfg.det.enabled ? "on" : "off", cfg.det.hz, cfg.det.threshold, cfg.det.on_count, cfg.det.off_ms);
    if (!cfg.det.enabled)
        cfg.snap.enabled = 0; /* a snapshot is taken when the person goes ON: no detection, no snapshot, no third encoder */
    if (cfg.det.enabled) {
        /* Optional zones: absent = the whole image; unreadable or invalid = logged once, the whole image (never silent) */
        int zr = a2k_zones_load(zones_conf, &cfg.det.zones);
        if (zr == 0 && cfg.det.zones.n > 0)
            a2k_log("zones: %d zone(s) from %s, zone_mode %s", cfg.det.zones.n, zones_conf,
                    cfg.det.zones.mode == A2K_ZONE_MODE_ALL ? "all" : "any");
        else if (zr == 0)
            a2k_log("zones: %s is empty, the whole image counts", zones_conf);
        else if (zr == 1)
            a2k_log("zones: no %s, the whole image counts", zones_conf);
        else
            a2k_log("zones: %s is unreadable or invalid (nothing from it is used), the whole image counts", zones_conf);
    }
    if (cfg.det.enabled) {
        /* runtime zones (/config/akam/zones, edited from Home Assistant) win over the card file; both may be absent */
        (void)a2k_zlive_init(&cfg.det.zones, A2K_STORE_ZONES);
        a2k_zlive_set_notify(a2k_mqtt_zones_changed);
    }
    /* video settings: built-in defaults, then the video<N>_* options of akam.conf (an inconsistent stream is logged and keeps its defaults), then the
     * runtime file written by the Home Assistant entities. Done before the capture threads read them. */
    for (int vc = 0; vc < A2K_VID_STREAMS; vc++) {
        a2k_vid_defaults(vc, vc ? cfg.sub_kbps : cfg.main_kbps, cfg.gop, &vid_base[vc]);
        if (vid_set[vc]) {
            a2k_vid_t t = vid_base[vc];
            for (int vp = 0; vp < A2K_VP_N; vp++)
                if (vid_set[vc] & (1u << vp))
                    t.v[vp] = vid_opt[vc].v[vp];
            if (a2k_vid_valid(vc, &t))
                vid_base[vc] = t;
            else
                a2k_log("video: stream %d options are inconsistent (min QP above max QP, or peak below target), the defaults stay", vc);
        }
        if (!a2k_vid_valid(vc, &vid_base[vc])) /* main_kbps / sub_kbps / gop from the command line are range checked, this never fires on a camera */
            a2k_vid_defaults(vc, vc ? 800 : 2000, 30, &vid_base[vc]);
    }
    (void)a2k_vid_init(vid_base, A2K_STORE_VIDEO);
    a2k_vid_set_notify(a2k_mqtt_resync);
    a2k_mem_report("before init");

    alarm(60); /* bounds a hang in a vendor call during bring-up */
    if (a2k_cap_start(&cfg, sp) < 0)
        return 1;
    alarm(0);
    a2k_mem_report("after capture start");
    if (ncfg.wdr == 1)
        (void)a2k_isp_wdr(1); /* the stock video_init applies WDR first (before sharpness/contrast/brightness); logged, never fatal */
    (void)a2k_image_apply(&icfg); /* a failing effect is logged there and never stops the streams */
    /* Microphone: opt-in (privacy). Off, the device is never opened and nothing below runs. A failure is logged there and
     * never stops the video: the SDP then simply has no audio track. */
    if (audio_on) {
        a2k_log("audio: enabled (--audio 1), microphone track on both streams");
        if (a2k_audio_start() != 0)
            a2k_log("audio: not running");
    } else {
        a2k_log("audio: off (the microphone is never opened; audio=1 in akam.conf enables it)");
    }
    /* status LED: the app owns the front blue LED (default on = left as the system set it); never fatal */
    (void)a2k_leds_start(led_mode, led_pat);
    /* PTZ: its own thread (calibration, motion); a bad ptz.conf or a missing motor is logged there and never stops the streams */
    a2k_ptz_set_notify(ptz_to_mqtt);
    pcfg.scene_check = pcfg.scene_check && cfg.det.enabled; /* the detection channel is the only frame source */
    a2k_scene_set_min(scene_min);
    a2k_scene_set_drift_ref(pcfg.drift_ref && pcfg.scene_check); /* the drift reference needs the detection frames like the scene check */
    /* motion detection (motion.h): the frame callback of the detection channel runs it, then the scene check */
    a2k_motion_init(motion_on, motion_sens, motion_src);
    a2k_motion_set_md_frame(motion_md_pic);
    a2k_motion_set_md_reader(a2k_motion_md_read);
    a2k_motion_set_sink(a2k_track_run_set_motion);
    a2k_motion_set_notify(motion_to_events);
    a2k_motion_set_chain(pcfg.scene_check ? a2k_scene_feed : NULL);
    a2k_detect_set_frame_cb(a2k_motion_frame);
    a2k_ptz_set_scene(a2k_scene_hooks());
    ptz_on = a2k_ptz_start(&pcfg) == 0;
    /* person tracking: off unless track=1; the detection thread feeds it (a no-op while it is off or the camera cannot be moved) */
    a2k_track_run_start(track_on, tarea_conf, a2k_mqtt_resync, track_inv_pan, track_inv_tilt, &tune);
    (void)a2k_patrol_run_start(); /* PTZ patrol: off until a route is started or ptz.patrol.idle_start_s is set (specs/ptz-patrol.md); never fatal */
    if (track_debug && a2k_track_run_get()) {
        a2k_track_set_debug(a2k_track_run_get(), track_debug);
        if (track_debug >= 2)
            a2k_ptz_set_ring_hook(a2k_track_rec_ring);
    }
    if (a2k_rtsp_start(port, auth, sp) < 0) {
        a2k_stop = 1;
        alarm(15);
        a2k_audio_stop();
        a2k_cap_stop();
        alarm(0);
        return 1;
    }
    /* ONVIF (device, media, PTZ, WS-Discovery): opt-in, default off; it needs the stream password (the RTSP credentials), see onvif_services.h. Never fatal. */
    a2k_settings_set_onvif(a2k_onvif_state, a2k_onvif_wanted);
    if (onvif_on && ptz_on) /* without a motor there is no PTZ to offer */
        a2k_onvif_set_ptz_ops(a2k_onvif_real_ptz_ops());
    {
        a2k_onvif_cfg_t oc;
        memset(&oc, 0, sizeof oc);
        oc.enabled = onvif_on;
        oc.port = onvif_port;
        oc.rtsp_port = port;
        oc.api_port = api_port;
        oc.auth = a2k_rtsp_auth_db();
        if (onvif_on)
            (void)a2k_onvif_start(&oc);
    }
    /* MQTT / Home Assistant: off without the card file; a bad file or a failed start never stops the streams */
    if (cfg.det.enabled) {
        a2k_zones_t live;
        a2k_zlive_get(&live, NULL);
        a2k_mqtt_set_zones(&live);
    } else {
        a2k_mqtt_set_zones(NULL);
    }
    a2k_button_prepare(button_long); /* the button bindings (the MQTT device triggers are made from them) */
    a2k_mqtt_set_buttons(a2k_btn_gestures);
    a2k_mqtt_set_ctl(&ctl_ops);
    a2k_mqtt_set_vid(&vid_ctl);
    a2k_mqtt_set_snapshot(a2k_snap_enabled()); /* false when the JPEG encoder could not be opened */
    a2k_mqtt_set_night(1);
    a2k_mqtt_set_motion(1); /* the entities always exist; with motion off they stay OFF / none */
    a2k_mqtt_set_sys(1);
    a2k_mqtt_set_leds(&led_ctl);
    a2k_mqtt_set_ptz(ptz_on ? &ptz_ctl : NULL);
    a2k_night_set_notify(night_to_mqtt);
    if (a2k_mqtt_follow_orientation() != 0) /* an orientation change from any source (API, button mode, the saved settings loaded below) is published to Home Assistant */
        a2k_log("mqtt: no orientation listener slot free (the switches resync on the next reconnect)");
    (void)a2k_mqtt_start(mqtt_conf, A2K_MQTT_MAC_PATH, port, a2k_rtsp_clients);
    (void)a2k_button_start(button_long); /* the front button: programmable gestures (default: short = HA event, long = park / home); never fatal */
    /* day/night: a failure is logged there and never stops the streams (the filter and the IR LED are then left alone) */
    if (a2k_night_start(&ncfg) != 0)
        a2k_log("day/night: not running");
    /* local control API (specs/control-api.md): the settings registry over the modules above, then the HTTP thread. Both are optional:
     * a failure is logged and the streams go on; with no API key installed the port stays closed. */
    {
        a2k_settings_cfg_t sc;
        memset(&sc, 0, sizeof sc);
        sc.image = &icfg;
        if (a2k_settings_start(&sc) == 0) {
            a2k_settings_load();
            (void)a2k_speaker_boot(); /* audio.out_enable as loaded (the setter already started it when it changed); a failure is logged */
            a2k_mqtt_resync(); /* the saved settings are in: the first state publish must not show the defaults (the MQTT thread starts before the load) */
            a2k_api_env_t aenv;
            a2k_api_env_2k(&aenv);
            if (api_port && a2k_httpd_start(api_port, 0, &aenv) != 0)
                a2k_log("api: not running");
        }
    }

    t_start = t_last = ms_since_start();
    cpu0 = cpu_last = cpu_seconds();
    while (!a2k_stop && !a2k_cap_failed()) {
        long now;
        unsigned long main_frames;
        usleep(100000);
        now = ms_since_start();
        pthread_mutex_lock(&streams[0].mu);
        main_frames = streams[0].frames;
        pthread_mutex_unlock(&streams[0].mu);
        if (frames && main_frames >= (unsigned long)frames) {
            a2k_log("test mode: %d main frames reached", frames);
            break;
        }
        if (seconds && now - t_start >= seconds * 1000L) {
            a2k_log("test mode: %d s elapsed", seconds);
            break;
        }
        if (now - t_last >= STATS_EVERY_S * 1000L) {
            double secs = (double)(now - t_last) / 1000.0, cpu = cpu_seconds();
            stream_stats("stats", &streams[0], &lb[0], &lf[0], secs);
            stream_stats("stats", &streams[1], &lb[1], &lf[1], secs);
            a2k_log("stats: %d client(s), CPU %.0f %%, VmRSS %ld kB, VmHWM %ld kB", a2k_rtsp_clients(),
                    (cpu - cpu_last) * 100.0 / secs, proc_kb("/proc/self/status", "VmRSS"),
                    proc_kb("/proc/self/status", "VmHWM"));
            if (a2k_audio_enabled())
                a2k_log("stats: audio %s, %lu periods, %u reopens", a2k_audio_up() ? "up" : "DOWN",
                        (unsigned long)a2k_aring_head(a2k_audio_ring()),
                        a2k_audio_restarts());
            if (a2k_snap_enabled()) {
                unsigned rq, st, dr, fl;
                a2k_snap_stats(&rq, &st, &dr, &fl);
                if (rq)
                    a2k_log("stats: snapshots %u requested, %u stored, %u dropped, %u failed", rq, st, dr, fl);
            }
            t_last = now;
            cpu_last = cpu;
        }
        if (now - t_sys >= SYS_EVERY_S * 1000L) {
            hal_sys_sample_t m;
            t_sys = now;
            hal_sys_sample(&sys_st, NULL, "wlan0", &m);
            a2k_mqtt_sys(&m);
            a2k_settings_note_sys(&m);
            a2k_log("stats: sys cpu %d %%, load %d.%02d, mem %ld/%ld kB, up %ld s, wifi %d dBm (lq %d)", m.cpu_pct,
                    m.load1_x100 / 100, m.load1_x100 % 100, m.mem_avail_kb, m.mem_total_kb, m.uptime_s, m.rssi_dbm,
                    m.link_quality);
        }
    }
    if (a2k_cap_failed())
        rc = 1;

    {
        long run_ms = ms_since_start() - t_start;
        double secs = run_ms > 0 ? (double)run_ms / 1000.0 : 1.0;
        tb[0] = tb[1] = 0;
        tf[0] = tf[1] = 0;
        stream_stats("summary", &streams[0], &tb[0], &tf[0], secs);
        stream_stats("summary", &streams[1], &tb[1], &tf[1], secs);
        a2k_log("summary: %.1f s, CPU %.1f %% average, %d client(s) at the end", secs,
                (cpu_seconds() - cpu0) * 100.0 / secs, a2k_rtsp_clients());
        if (streams[0].frames == 0 || streams[1].frames == 0)
            rc = 1; /* a stream that never produced a frame is a failed run */
    }
    a2k_mem_report("at the end");

    a2k_stop = 1;
    alarm(20); /* teardown guard: a wedged vendor call ends the process instead of hanging the camera boot */
    a2k_onvif_stop(); /* first of all: no network client may command the motor while the modules stop */
    a2k_httpd_stop(); /* then the API: no remote command may reach a setter while the modules stop */
    a2k_settings_stop();
    a2k_mqtt_stop(); /* then MQTT, same reason */
    a2k_track_run_stop(); /* the detection thread outlives the motor: no more nudges from here on */
    a2k_patrol_run_stop(); /* a running patrol is cancelled before the motor stops */
    a2k_button_stop(); /* before the PTZ and LED stops it drives, and before the pin files close */
    a2k_ptz_stop(); /* stops the motor (aborts a calibration), writes the position */
    a2k_leds_stop(); /* before the day/night stop, which closes the pin files */
    a2k_night_stop(); /* then: the ISP and the streams must still be up for the day mode and its keyframe */
    a2k_rtsp_stop();
    a2k_speaker_stop(); /* after the clients (the talk session writes to it) */
    a2k_audio_stop(); /* after the clients (they read its ring, which is static and stays valid) */
    a2k_cap_stop();
    alarm(0);
    /* A client thread that outlived the bounded wait of a2k_rtsp_stop() may still hold a frame or the stream mutex:
     * freeing the rings under it would be a use-after-free. The process is about to exit, the OS reclaims them. */
    if (a2k_rtsp_clients() == 0) {
        a2k_stream_destroy(&streams[0]);
        a2k_stream_destroy(&streams[1]);
    } else {
        a2k_log("client threads still running: stream buffers left to the OS");
    }
    a2k_mem_report("after teardown");
    a2k_log("exit %d", rc);
    return rc;
}
