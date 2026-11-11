#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "audio_out.h"

#define AO_MIN_PERIOD_US 57600L /* 90% of 64 ms: pacing floor if write() does not block */

static void ao_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void ao_log(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* stdout, not stderr: only stdout is tee'd to the LOG lines of port 8091. */
    printf("[audio_out] %s\n", buf);
}

/* ------------------------------------------------------------------------
 * Pure helpers
 * ------------------------------------------------------------------------ */

/* 1 kHz at 8 kHz repeats every 8 samples: sin(k * 45 deg) at -28 dBFS
 * (32767 * 10^(-28/20) = 1305; 1305 * sqrt(1/2) = 923); tuned by ear at the default volume 100, the same loudness as -16 dBFS at volume 50. */
static const int16_t k_sine8[8] = { 0, 923, 1305, 923, 0, -923, -1305, -923 };

#define BEEP_TONE_SAMPLES 1600 /* 200 ms */
#define BEEP_RAMP         80   /* 10 ms */

int16_t ao_beep_sample(unsigned idx)
{
    if (idx >= AO_BEEP_SAMPLES) return 0;
    unsigned pos = idx % (2 * BEEP_TONE_SAMPLES); /* tone, then 200 ms gap */
    if (pos >= BEEP_TONE_SAMPLES) return 0;
    int up = pos < BEEP_RAMP ? (int)pos : BEEP_RAMP;
    int dn_n = (int)(BEEP_TONE_SAMPLES - 1 - pos);
    int dn = dn_n < BEEP_RAMP ? dn_n : BEEP_RAMP;
    int f = up < dn ? up : dn;
    return (int16_t)((int)k_sine8[pos & 7] * f / BEEP_RAMP);
}

int ao_volume_gain_q15(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume * volume * 32768 / 10000;
}

const char *ao_state_name(int s)
{
    switch (s) {
    case AO_OFF:            return "off";
    case AO_IDLE:           return "idle";
    case AO_TALKING:        return "talking";
    case AO_TEST:           return "test";
    case AO_CHIME:          return "chime";
    case AO_DISABLED_CRASH: return "disabled_crash";
    default:                return "error";
    }
}

static const char *amp_mode_name(int m)
{
    return m == AO_AMP_ON ? "on" : m == AO_AMP_OFF ? "off" : "auto";
}

static const char *duplex_name(int d)
{
    return d == AO_DUPLEX_FULL ? "full" : "half";
}

/* ------------------------------------------------------------------------
 * Instance setup, settings
 * ------------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void ao_init(audio_out_t *ao, const ao_ops_t *ops, const ao_cfg_t *cfg)
{
    memset(ao, 0, sizeof(*ao));
    ao->ops = *ops;
    copy_str(ao->dev_path, sizeof(ao->dev_path), cfg->dev_path);
    copy_str(ao->conf_dir, sizeof(ao->conf_dir), cfg->conf_dir);
    snprintf(ao->conf_path, sizeof(ao->conf_path), "%s/%s", ao->conf_dir, AO_CONF_FILE);
    snprintf(ao->guard_path, sizeof(ao->guard_path), "%s/%s", ao->conf_dir, AO_GUARD_FILE);
    ao->enabled  = 1;
    ao->volume   = 100;
    ao->gain     = 2;   /* stock value */
    ao->source   = 4;   /* stock playback route */
    ao->amp_mode = AO_AMP_AUTO;
    ao->talk_enable = 1;
    ao->talk_duplex = AO_DUPLEX_HALF;
    ao->mic_enable = 1;
    ao->persist  = 1;
    ao->state    = AO_OFF;
    ao->fd       = -1;
    ao->beep_pos = -1;
    ao->cur_gain_q15 = ao_volume_gain_q15(ao->volume);
    audio_jitter_init(&ao->jit);
}

void ao_set_volume(audio_out_t *ao, int v)
{
    ao->volume = v < 0 ? 0 : v > 100 ? 100 : v;
}
void ao_set_gain(audio_out_t *ao, int g)
{
    ao->gain = g < 0 ? 0 : g > 5 ? 5 : g;
}
void ao_set_source(audio_out_t *ao, int s)
{
    if (s == 4 || s == 6) ao->source = s;
}
void ao_set_amp_mode(audio_out_t *ao, int m)
{
    if (m == AO_AMP_AUTO || m == AO_AMP_ON || m == AO_AMP_OFF) ao->amp_mode = m;
}
void ao_play_test(audio_out_t *ao) { ao->test_req = 1; }
int  ao_test_active(const audio_out_t *ao) { return ao->test_req || ao->beep_pos >= 0; }
int  ao_amp_is_on(const audio_out_t *ao) { return ao->amp_on; }
int  ao_device_open(const audio_out_t *ao) { return ao->fd >= 0; }

int ao_talk_available(const audio_out_t *ao)
{
    return ao->enabled && ao->talk_enable && ao->fd >= 0 &&
           ao->state != AO_ERROR && ao->state != AO_DISABLED_CRASH;
}

int ao_talk_half_duplex(const audio_out_t *ao) { return ao->talk_duplex != AO_DUPLEX_FULL; }

/* Single core, and the claim comes from the RTSP thread only: a plain flag is
 * enough (the release may come from another thread at shutdown). */
int ao_feeder_claim(audio_out_t *ao)
{
    if (ao->feeder) return -1;
    ao->feeder = 1;
    return 0;
}

void ao_feeder_release(audio_out_t *ao) { ao->feeder = 0; }

/* ------------------------------------------------------------------------
 * Persistent settings file (key=value lines)
 * ------------------------------------------------------------------------ */

static int parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    if (!s || !*s) return -1;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

int ao_load_config(audio_out_t *ao)
{
    char buf[512];
    int fd = open(ao->conf_path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        size_t ll = strlen(eq + 1);
        if (ll && eq[ll] == '\r') eq[ll] = '\0'; /* tolerate CRLF */
        const char *k = line, *v = eq + 1;
        long x;
        if      (!strcmp(k, "enable") && !parse_int(v, 0, 1, &x))    ao->enabled = (int)x;
        else if (!strcmp(k, "volume") && !parse_int(v, 0, 100, &x))  ao->volume = (int)x;
        else if (!strcmp(k, "gain")   && !parse_int(v, 0, 5, &x))    ao->gain = (int)x;
        else if (!strcmp(k, "source") && !parse_int(v, 4, 6, &x))    ao_set_source(ao, (int)x);
        else if (!strcmp(k, "talk_enable") && !parse_int(v, 0, 1, &x)) ao->talk_enable = (int)x;
        else if (!strcmp(k, "mic_enable") && !parse_int(v, 0, 1, &x))  ao->mic_enable = (int)x;
        else if (!strcmp(k, "talk_duplex")) {
            if (!strcmp(v, "half")) ao->talk_duplex = AO_DUPLEX_HALF;
            else if (!strcmp(v, "full")) ao->talk_duplex = AO_DUPLEX_FULL;
        }
    }
    ao->cur_gain_q15 = ao_volume_gain_q15(ao->volume);
    return 1;
}

static int key_is_ours(const char *line)
{
    static const char *const keys[] = { "enable=", "volume=", "gain=", "source=",
                                        "talk_enable=", "talk_duplex=", "mic_enable=" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (!strncmp(line, keys[i], strlen(keys[i]))) return 1;
    return 0;
}

int ao_save_config(audio_out_t *ao)
{
    char old[1024], out[1280];
    size_t olen = 0, outn = 0;

    int fd = open(ao->conf_path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, old, sizeof(old) - 1);
        close(fd);
        if (n > 0) olen = (size_t)n;
    }
    old[olen] = '\0';

    /* keep the lines we do not own (mute_mic, chime, ...) */
    char *save = NULL;
    for (char *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line || key_is_ours(line)) continue;
        int w = snprintf(out + outn, sizeof(out) - outn, "%s\n", line);
        if (w < 0 || (size_t)w >= sizeof(out) - outn) break;
        outn += (size_t)w;
    }
    int w = snprintf(out + outn, sizeof(out) - outn,
                     "enable=%d\nvolume=%d\ngain=%d\nsource=%d\n"
                     "talk_enable=%d\ntalk_duplex=%s\nmic_enable=%d\n",
                     ao->enabled, ao->volume, ao->gain, ao->source,
                     ao->talk_enable, duplex_name(ao->talk_duplex), ao->mic_enable);
    if (w < 0 || (size_t)w >= sizeof(out) - outn) { errno = ENOSPC; return -1; }
    outn += (size_t)w;

    if (mkdir(ao->conf_dir, 0700) < 0 && errno != EEXIST) return -1;
    char tmp[sizeof(ao->conf_path) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ao->conf_path);
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
    if (rename(tmp, ao->conf_path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    return 0;
}

/* ------------------------------------------------------------------------
 * Crash guard
 * ------------------------------------------------------------------------ */

static int guard_exists(const audio_out_t *ao)
{
    struct stat st;
    return stat(ao->guard_path, &st) == 0;
}

/* The marker must be on flash before the first risky ioctl: a kernel panic
 * does not flush anything. */
static int guard_create(const audio_out_t *ao)
{
    if (mkdir(ao->conf_dir, 0700) < 0 && errno != EEXIST) return -1;
    int fd = open(ao->guard_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    if (write(fd, "1\n", 2) != 2) { int e = errno; close(fd); unlink(ao->guard_path); errno = e; return -1; }
    fsync(fd);
    close(fd);
    return 0;
}

static void guard_remove(audio_out_t *ao)
{
    if (ao->guard_active) {
        unlink(ao->guard_path);
        ao->guard_active = 0;
    }
}

/* ------------------------------------------------------------------------
 * Device sequence
 * ------------------------------------------------------------------------ */

/* Raw int32 ioctl: 0 or -1 with errno from the driver, nothing logged. */
static int dev_ioctl_quiet(audio_out_t *ao, unsigned long req, int value)
{
    int32_t v = value;
    return ao->ops.ioctl(ao->ops.user, ao->fd, req, &v);
}

static int dev_ioctl_i32(audio_out_t *ao, const char *name, unsigned long req, int value)
{
    if (dev_ioctl_quiet(ao, req, value) < 0) {
        ao_log("%s(%d) failed: %s (errno=%d)", name, value, strerror(errno), errno);
        return -1;
    }
    return 0;
}

/* SET_SPEAKER failures are logged and never retried every period; never fall
 * back to sysfs GPIO 69, the driver owns that line (spec 05, 3.2). Turning
 * the speaker off is not in the stock sequence (stock only mutes the route),
 * so a failure there is logged once only. */
static void speaker_set(audio_out_t *ao, int on)
{
    if (dev_ioctl_quiet(ao, AKPCM_IOC_SET_SPEAKER, on) < 0) {
        if (on || !ao->spk_off_warned) {
            ao_log("SET_SPEAKER(%d) failed: %s (errno=%d)%s", on, strerror(errno), errno,
                   on ? "" : " (not logged again)");
            if (!on) ao->spk_off_warned = 1;
        }
    }
}

/* Amplifier on: the stock per-prompt sequence SET_SOURCES(route),
 * SET_SPEAKER(1), SET_GAIN. Off: speaker off, then the route muted
 * (SET_SOURCES 0). Neither is fatal. */
static void amp_set(audio_out_t *ao, int on)
{
    if (on) {
        /* One snapshot of the settings (the 8091 thread may change them at any
         * time), so that what is applied is what is recorded as applied. */
        int src = ao->source, gain = ao->gain;
        ao->applied_source = src;
        ao->applied_gain = gain;
        dev_ioctl_i32(ao, "SET_SOURCES", AKPCM_IOC_SET_SOURCES, src);
        speaker_set(ao, 1);
        dev_ioctl_i32(ao, "SET_GAIN", AKPCM_IOC_SET_GAIN, gain);
    } else {
        speaker_set(ao, 0);
        dev_ioctl_i32(ao, "SET_SOURCES", AKPCM_IOC_SET_SOURCES, 0);
    }
    ao->amp_on = on;
}

static void fail_cleanup(audio_out_t *ao)
{
    if (ao->fd >= 0) {
        if (ao->amp_on) amp_set(ao, 0);
        ao->ops.close(ao->ops.user, ao->fd);
        ao->fd = -1;
    }
    ao->amp_on = 0;
}

int ao_open(audio_out_t *ao)
{
    ao->guard_blocked = 0;
    ao->had_error = 0;
    ao->fails = 0;
    ao->beep_pos = -1;
    ao->test_req = 0;
    ao->amp_on = 0;
    ao->periods_written = 0;
    audio_jitter_reset(&ao->jit);
    ao->last_audible_ms = -(int64_t)AO_AMP_HOLD_MS - 1;
    ao->cur_gain_q15 = ao_volume_gain_q15(ao->volume);

    if (!ao->enabled) { ao->state = AO_OFF; return -1; }

    if (guard_exists(ao)) {
        ao->guard_blocked = 1;
        ao->state = AO_DISABLED_CRASH;
        ao_log("skipped: crash guard %s exists (ak_rtsp died during audio init on "
               "the previous start); remove it to retry", ao->guard_path);
        return -1;
    }
    if (guard_create(ao) < 0) {
        ao->state = AO_ERROR;
        ao_log("skipped: cannot write crash guard %s: %s", ao->guard_path, strerror(errno));
        return -1;
    }
    ao->guard_active = 1;

    ao->fd = ao->ops.open(ao->ops.user, ao->dev_path);
    if (ao->fd < 0) {
        ao_log("open %s failed: %s", ao->dev_path, strerror(errno));
        goto fail;
    }

    /* Same order as the stock application: route, speaker, PREPARE, RESUME,
     * read the rate back. amp=off opens with the route and speaker off. In
     * auto the amp starts on like stock and the idle timer turns it off. */
    int amp_initial = (ao->amp_mode != AO_AMP_OFF);
    int init_src = amp_initial ? ao->source : 0;
    if (dev_ioctl_i32(ao, "SET_SOURCES", AKPCM_IOC_SET_SOURCES, init_src) < 0) goto fail;
    speaker_set(ao, amp_initial); /* not fatal: a bring-up can still learn what the driver accepts */
    ao->amp_on = amp_initial;     /* so that a failure below switches it off again */
    ao->applied_source = ao->source;
    ao->applied_gain = ao->gain;

    struct akpcm_pars pars;
    memset(&pars, 0, sizeof(pars));
    pars.format       = 0;
    pars.rate         = AO_RATE;
    pars.channels     = AO_CHANNELS;
    pars.bits         = 16;
    pars.period_size  = AO_PERIOD_BYTES;
    pars.period_count = AO_DRV_PERIODS;
    if (ao->ops.ioctl(ao->ops.user, ao->fd, AKPCM_IOC_PREPARE, &pars) < 0) {
        ao_log("PREPARE failed: %s (errno=%d)", strerror(errno), errno);
        goto fail; /* RESUME is never issued without a successful PREPARE */
    }
    if (ao->ops.ioctl(ao->ops.user, ao->fd, AKPCM_IOC_RESUME, NULL) < 0) {
        ao_log("RESUME failed: %s (errno=%d)", strerror(errno), errno);
        goto fail;
    }
    int32_t rate = 0;
    if (ao->ops.ioctl(ao->ops.user, ao->fd, AKPCM_IOC_GET_RATE, &rate) < 0)
        ao_log("GET_RATE failed: %s (errno=%d)", strerror(errno), errno);
    else if (rate != AO_RATE)
        ao_log("driver reports rate %d, expected %d", (int)rate, AO_RATE);
    if (amp_initial) dev_ioctl_i32(ao, "SET_GAIN", AKPCM_IOC_SET_GAIN, ao->gain);

    ao->open_ms = ao->ops.now_us(ao->ops.user) / 1000;
    ao->last_audible_ms = ao->open_ms; /* auto: the amp idles off AO_AMP_HOLD_MS after open */
    ao->state = AO_IDLE;
    printf("[ak_rtsp] audio_out: %s ready (%d Hz mono, %d x %d bytes, source %d, gain %d, amp %s)\n",
           ao->dev_path, AO_RATE, AO_DRV_PERIODS, AO_PERIOD_BYTES, ao->source, ao->gain,
           amp_mode_name(ao->amp_mode));
    return 0;

fail:
    fail_cleanup(ao);
    guard_remove(ao); /* failed cleanly: not a crash */
    ao->state = AO_ERROR;
    return -1;
}

void ao_close(audio_out_t *ao)
{
    fail_cleanup(ao);
    guard_remove(ao);
}

/* ------------------------------------------------------------------------
 * Playback
 * ------------------------------------------------------------------------ */

/* Software volume with a 10 ms ramp toward the target (no pop), applied to
 * the mono block in place. */
static void apply_volume(audio_out_t *ao, int16_t *mono, size_t n)
{
    int target = ao_volume_gain_q15(ao->volume);
    const int step = 32768 / AO_VOL_RAMP;
    int cur = ao->cur_gain_q15;
    for (size_t i = 0; i < n; i++) {
        if (cur < target) { cur += step; if (cur > target) cur = target; }
        else if (cur > target) { cur -= step; if (cur < target) cur = target; }
        int32_t v = ((int32_t)mono[i] * cur) >> 15;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        mono[i] = (int16_t)v;
    }
    ao->cur_gain_q15 = cur;
}

/* 0 = written, -1 = failed (errno set). EINTR is retried. */
static int write_period(audio_out_t *ao, const void *buf)
{
    size_t off = 0;
    while (off < AO_PERIOD_BYTES) {
        ssize_t n = ao->ops.write(ao->ops.user, ao->fd, (const char *)buf + off,
                                  AO_PERIOD_BYTES - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = EIO; return -1; }
        off += (size_t)n;
    }
    ao->periods_written++;
    return 0;
}

static int step_failed(audio_out_t *ao)
{
    ao->had_error = 1;
    ao_log("playback write failed: %s (errno=%d)", strerror(errno), errno);
    if (++ao->fails >= AO_MAX_FAILS) {
        ao_log("playback write failed %d times in a row, stopping playback", ao->fails);
        if (ao->amp_on) amp_set(ao, 0);
        ao->state = AO_ERROR;
        return -1;
    }
    ao->ops.sleep_us(ao->ops.user, 200000);
    return 0;
}

int ao_step(audio_out_t *ao)
{
    if (ao->fd < 0) return -1;
    int64_t now = ao->ops.now_us(ao->ops.user) / 1000;

    /* Live settings: only this thread talks to the device. While the amp is
     * off the route stays muted and the new values are applied when it turns
     * on (amp_set). */
    if (ao->amp_on) {
        int gain = ao->gain, src = ao->source;
        if (gain != ao->applied_gain) {
            ao->applied_gain = gain;
            dev_ioctl_i32(ao, "SET_GAIN", AKPCM_IOC_SET_GAIN, gain);
        }
        if (src != ao->applied_source) {
            ao->applied_source = src;
            dev_ioctl_i32(ao, "SET_SOURCES", AKPCM_IOC_SET_SOURCES, src);
        }
    }

    int16_t mono[AO_FRAMES];
    int en = ao->enabled, audible = 0, talking = 0, test = 0;
    memset(mono, 0, sizeof(mono));
    if (en) {
        if (ao->test_req && ao->beep_pos < 0) { ao->beep_pos = 0; ao->test_req = 0; }
        if (ao->beep_pos >= 0) {
            for (int i = 0; i < AO_FRAMES; i++) mono[i] = ao_beep_sample((unsigned)(ao->beep_pos + i));
            ao->beep_pos += AO_FRAMES;
            if (ao->beep_pos >= AO_BEEP_SAMPLES) ao->beep_pos = -1;
            test = audible = 1;
        } else {
            /* pads with silence; the result is the real sample count (0 while pre-filling).
             * Real samples are "talking"; only a loud enough period is "audible" (keeps
             * the amp on): a stream of G.711 silence must not light the amplifier. */
            size_t real = audio_jitter_pull(&ao->jit, mono, AO_FRAMES);
            if (real > 0) {
                talking = 1;
                for (size_t i = 0; i < real; i++) {
                    int a = mono[i] < 0 ? -(int)mono[i] : mono[i];
                    if (a >= AO_AUDIBLE_PEAK) { audible = 1; break; }
                }
            }
        }
    } else {
        ao->beep_pos = -1;
        ao->test_req = 0;
        audio_jitter_reset(&ao->jit); /* nothing queued survives a disable */
    }
    if (ao->volume == 0) audible = 0; /* volume 0 = mute: amp stays off */

    /* Amplifier policy (spec 05, 3.2): auto = on while audible, off after AO_AMP_HOLD_MS idle. */
    if (audible) ao->last_audible_ms = now;
    int want;
    switch (ao->amp_mode) {
    case AO_AMP_ON:  want = en; break;
    case AO_AMP_OFF: want = 0; break;
    default:
        want = en && (audible || (ao->amp_on && now - ao->last_audible_ms < AO_AMP_HOLD_MS));
        break;
    }
    if (want && !ao->amp_on) {
        amp_set(ao, 1);
        if (audible) {
            int16_t zero[AO_FRAMES];
            memset(zero, 0, sizeof(zero));
            for (int i = 0; i < AO_PRELOAD_PERIODS; i++)
                if (write_period(ao, zero) < 0) return step_failed(ao);
        }
    } else if (!want && ao->amp_on) {
        amp_set(ao, 0);
    }

    apply_volume(ao, mono, AO_FRAMES);
    if (write_period(ao, mono) < 0) return step_failed(ao);
    ao->fails = 0;

    ao->state = !en ? AO_OFF : test ? AO_TEST : talking ? AO_TALKING : AO_IDLE;

    if (ao->guard_active && !ao->had_error && now - ao->open_ms >= AO_GUARD_CLEAR_MS) {
        guard_remove(ao);
        ao_log("playback healthy, crash guard cleared");
    }
    return 0;
}

size_t ao_push(audio_out_t *ao, const int16_t *pcm, size_t n)
{
    return audio_jitter_push(&ao->jit, pcm, n);
}

size_t ao_queued(audio_out_t *ao) { return audio_jitter_level(&ao->jit); }

/* ------------------------------------------------------------------------
 * Thread
 * ------------------------------------------------------------------------ */

static void *ao_thread_main(void *arg)
{
    audio_out_t *ao = arg;
    while (ao->thread_running) {
        int64_t t0 = ao->ops.now_us(ao->ops.user);
        if (ao_step(ao) < 0) break;
        long elapsed = (long)(ao->ops.now_us(ao->ops.user) - t0);
        /* A healthy write blocks for about one period once the driver buffer
         * is full. If it does not, pace the loop instead of spinning. */
        if (elapsed < 0) elapsed = 0;
        if (elapsed < AO_MIN_PERIOD_US) ao->ops.sleep_us(ao->ops.user, AO_MIN_PERIOD_US - elapsed);
    }
    return NULL;
}

int ao_thread_start(audio_out_t *ao)
{
    if (ao->thread_started) return 0;
    ao->thread_running = 1;
    if (pthread_create(&ao->tid, NULL, ao_thread_main, ao) != 0) {
        ao->thread_running = 0;
        return -1;
    }
    ao->thread_started = 1;
    return 0;
}

void ao_thread_stop(audio_out_t *ao)
{
    ao->thread_running = 0;
    if (ao->thread_started) {
        pthread_join(ao->tid, NULL);
        ao->thread_started = 0;
    }
}

/* ------------------------------------------------------------------------
 * 8091 keys
 * ------------------------------------------------------------------------ */

static int is_mic_key(const char *key) { return !strcmp(key, "audio.mic_enable"); }

static int is_audio_key(const char *key) { return !strncmp(key, "audio.out_", 10); }

static int is_talk_key(const char *key) { return !strncmp(key, "audio.talk_", 11); }

int ao_ctl_get(audio_out_t *ao, const char *key, char *out, size_t cap)
{
    if (is_mic_key(key)) { snprintf(out, cap, "audio.mic_enable=%d", ao->mic_enable); return 0; }
    if (is_talk_key(key)) {
        const char *t = key + 11;
        if      (!strcmp(t, "enable")) snprintf(out, cap, "audio.talk_enable=%d", ao->talk_enable);
        else if (!strcmp(t, "duplex")) snprintf(out, cap, "audio.talk_duplex=%s", duplex_name(ao->talk_duplex));
        else return -1; /* the read-only talk counters belong to talk.c */
        return 0;
    }
    if (!is_audio_key(key)) return -1;
    const char *k = key + 10;
    if      (!strcmp(k, "enable")) snprintf(out, cap, "audio.out_enable=%d", ao->enabled);
    else if (!strcmp(k, "volume")) snprintf(out, cap, "audio.out_volume=%d", ao->volume);
    else if (!strcmp(k, "gain"))   snprintf(out, cap, "audio.out_gain=%d", ao->gain);
    else if (!strcmp(k, "source")) snprintf(out, cap, "audio.out_source=%d", ao->source);
    else if (!strcmp(k, "amp"))    snprintf(out, cap, "audio.out_amp=%s", amp_mode_name(ao->amp_mode));
    else if (!strcmp(k, "test"))   snprintf(out, cap, "audio.out_test=%d", ao_test_active(ao) ? 1 : 0);
    else if (!strcmp(k, "state"))  snprintf(out, cap, "audio.out_state=%s", ao_state_name(ao->state));
    else if (!strcmp(k, "underruns")) snprintf(out, cap, "audio.out_underruns=%lu", (unsigned long)audio_jitter_underruns(&ao->jit));
    else if (!strcmp(k, "drops"))  snprintf(out, cap, "audio.out_drops=%lu", (unsigned long)audio_jitter_drops(&ao->jit));
    else if (!strcmp(k, "buf_ms")) snprintf(out, cap, "audio.out_buf_ms=%lu", (unsigned long)audio_jitter_level_ms(&ao->jit));
    else snprintf(out, cap, "ERR unknown param %s", key);
    return 0;
}

void ao_ctl_list(audio_out_t *ao, void (*emit)(void *arg, const char *line), void *arg)
{
    static const char *const keys[] = {
        "audio.out_enable", "audio.out_volume", "audio.out_gain", "audio.out_source",
        "audio.out_amp", "audio.out_test", "audio.out_state",
        "audio.out_underruns", "audio.out_drops", "audio.out_buf_ms",
        "audio.talk_enable", "audio.talk_duplex", "audio.mic_enable"
    };
    char line[80];
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        ao_ctl_get(ao, keys[i], line, sizeof(line));
        emit(arg, line);
    }
}

/* Persist the four saved keys after a change (8091 commands only, never from
 * the playback thread; a same-value SET does not touch the flash). Fills note
 * with " (not saved: ...)" on failure, else "". */
static void persist_note(audio_out_t *ao, char *note, size_t cap)
{
    note[0] = '\0';
    if (!ao->persist) return; /* a remote peer (DEBUG): RAM only */
    if (ao_save_config(ao) < 0) snprintf(note, cap, " (not saved: %s)", strerror(errno));
}

int ao_ctl_set(audio_out_t *ao, const char *key, const char *val, char *msg, size_t cap)
{
    long x;
    if (is_mic_key(key)) {
        char note[64] = "";
        if (parse_int(val, 0, 1, &x) < 0) { snprintf(msg, cap, "ERR audio.mic_enable range [0,1]"); return 1; }
        if (ao->mic_enable != (int)x) {
            ao->mic_enable = (int)x;
            persist_note(ao, note, sizeof(note));
            if (!note[0]) snprintf(note, sizeof(note), " (restart to apply)");
        }
        snprintf(msg, cap, "OK audio.mic_enable=%ld%s", x, note);
        return 0;
    }
    if (is_talk_key(key)) {
        const char *t = key + 11;
        char note[64] = "";
        if (!strcmp(t, "enable")) {
            if (parse_int(val, 0, 1, &x) < 0) { snprintf(msg, cap, "ERR audio.talk_enable range [0,1]"); return 1; }
            if (ao->talk_enable != (int)x) { ao->talk_enable = (int)x; persist_note(ao, note, sizeof(note)); }
            snprintf(msg, cap, "OK audio.talk_enable=%ld%s", x, note);
        } else if (!strcmp(t, "duplex")) {
            int d;
            if      (!strcmp(val, "half")) d = AO_DUPLEX_HALF;
            else if (!strcmp(val, "full")) d = AO_DUPLEX_FULL;
            else { snprintf(msg, cap, "ERR invalid audio.talk_duplex value %s (want half|full)", val); return 1; }
            if (ao->talk_duplex != d) { ao->talk_duplex = d; persist_note(ao, note, sizeof(note)); }
            snprintf(msg, cap, "OK audio.talk_duplex=%s%s", duplex_name(d), note);
        } else {
            return -1; /* talk.c: read-only state and counters */
        }
        return 0;
    }
    if (!is_audio_key(key)) return -1;
    const char *k = key + 10;

    if (!strcmp(k, "enable")) {
        if (parse_int(val, 0, 1, &x) < 0) { snprintf(msg, cap, "ERR audio.out_enable range [0,1]"); return 1; }
        ao->enabled = (int)x;
        char note[64] = "";
        persist_note(ao, note, sizeof(note));
        if (!note[0] && x == 1 && !ao_device_open(ao))
            snprintf(note, sizeof(note), " (restart to apply)");
        snprintf(msg, cap, "OK audio.out_enable=%ld%s", x, note);
    } else if (!strcmp(k, "volume")) {
        if (parse_int(val, 0, 100, &x) < 0) { snprintf(msg, cap, "ERR audio.out_volume range [0,100]"); return 1; }
        char note[64] = "";
        if (ao->volume != (int)x) { ao->volume = (int)x; persist_note(ao, note, sizeof(note)); }
        snprintf(msg, cap, "OK audio.out_volume=%ld%s", x, note);
    } else if (!strcmp(k, "gain")) {
        if (parse_int(val, 0, 5, &x) < 0) { snprintf(msg, cap, "ERR audio.out_gain range [0,5]"); return 1; }
        char note[64] = "";
        if (ao->gain != (int)x) { ao->gain = (int)x; persist_note(ao, note, sizeof(note)); }
        snprintf(msg, cap, "OK audio.out_gain=%ld%s", x, note);
    } else if (!strcmp(k, "source")) {
        if (parse_int(val, 4, 6, &x) < 0 || x == 5) { snprintf(msg, cap, "ERR audio.out_source want 4 or 6"); return 1; }
        char note[64] = "";
        if (ao->source != (int)x) { ao->source = (int)x; persist_note(ao, note, sizeof(note)); }
        snprintf(msg, cap, "OK audio.out_source=%ld%s", x, note);
    } else if (!strcmp(k, "amp")) {
        int m;
        if      (!strcmp(val, "auto")) m = AO_AMP_AUTO;
        else if (!strcmp(val, "on"))   m = AO_AMP_ON;
        else if (!strcmp(val, "off"))  m = AO_AMP_OFF;
        else { snprintf(msg, cap, "ERR invalid audio.out_amp value %s (want auto|on|off)", val); return 1; }
        ao->amp_mode = m;
        snprintf(msg, cap, "OK audio.out_amp=%s", amp_mode_name(m));
    } else if (!strcmp(k, "test")) {
        if (parse_int(val, 0, 1, &x) < 0) { snprintf(msg, cap, "ERR audio.out_test want 1 (play the beep)"); return 1; }
        if (x == 1) {
            if (!ao_device_open(ao) || !ao->enabled || ao->state == AO_ERROR) {
                snprintf(msg, cap, "ERR audio out is not running (audio.out_state=%s)", ao_state_name(ao->state));
                return 1;
            }
            ao_play_test(ao);
        }
        snprintf(msg, cap, "OK audio.out_test=%ld", x);
    } else if (!strcmp(k, "state") || !strcmp(k, "underruns") || !strcmp(k, "drops") || !strcmp(k, "buf_ms")) {
        snprintf(msg, cap, "ERR audio.out_%s is read-only", k);
    } else {
        snprintf(msg, cap, "ERR unknown param %s", key);
    }
    return msg[0] == 'E' ? 1 : 0;
}

/* ------------------------------------------------------------------------
 * Process-wide instance on the real device
 * ------------------------------------------------------------------------ */

static int real_open(void *u, const char *path) { (void)u; return open(path, O_WRONLY); }
static int real_ioctl(void *u, int fd, unsigned long req, void *arg)
{
    (void)u;
    return ioctl(fd, req, arg);
}
static ssize_t real_write(void *u, int fd, const void *b, size_t n) { (void)u; return write(fd, b, n); }
static int real_close(void *u, int fd) { (void)u; return close(fd); }
static int64_t real_now_us(void *u)
{
    (void)u;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void real_sleep_us(void *u, long us) { (void)u; usleep((useconds_t)us); }

static audio_out_t   g_ao;
static pthread_once_t g_ao_once = PTHREAD_ONCE_INIT;
static int            g_ao_started;

static void single_init(void)
{
    static const ao_ops_t ops = { real_open, real_ioctl, real_write, real_close,
                                  real_now_us, real_sleep_us, NULL };
    static const ao_cfg_t cfg = { "/dev/pcmC0D0p", "/etc/config/akam" };
    ao_init(&g_ao, &ops, &cfg);
    ao_load_config(&g_ao);
}

static audio_out_t *single(void)
{
    pthread_once(&g_ao_once, single_init);
    return &g_ao;
}

int audio_out_start(void)
{
    audio_out_t *ao = single();
    if (ao_open(ao) < 0) {
        printf("[ak_rtsp] audio_out: not active (%s), legacy silence playback\n",
               ao_state_name(ao->state));
        return -1;
    }
    if (ao_thread_start(ao) < 0) {
        perror("[audio_out] pthread_create");
        ao_close(ao);
        ao->state = AO_ERROR;
        return -1;
    }
    g_ao_started = 1;
    return 0;
}

void audio_out_stop(void)
{
    if (!g_ao_started) return;
    ao_thread_stop(&g_ao);
    ao_close(&g_ao);
    g_ao_started = 0;
}

void audio_out_emergency_off(void)
{
    /* Called from a signal handler: only async-signal-safe calls. */
    int fd = g_ao.fd;
    if (g_ao_started && fd >= 0) {
        int32_t v = 0;
        ioctl(fd, AKPCM_IOC_SET_SPEAKER, &v);
        v = 0;
        ioctl(fd, AKPCM_IOC_SET_SOURCES, &v); /* route off, as stock does when idle */
    }
    /* A stop request is not a kernel crash: if it lands inside the first
     * seconds of playback (reboot, restart by hand) the marker must not
     * disable audio out on the next start. A real crash never gets here. */
    if (g_ao.guard_active)
        unlink(g_ao.guard_path);
}

size_t audio_out_push(const int16_t *pcm, size_t n) { return ao_push(single(), pcm, n); }
size_t audio_out_queued(void) { return ao_queued(single()); }
int  audio_out_talk_available(void) { return ao_talk_available(single()); }
int  audio_out_talk_half_duplex(void) { return ao_talk_half_duplex(single()); }
int  audio_out_feeder_claim(void) { return ao_feeder_claim(single()); }
void audio_out_feeder_release(void) { ao_feeder_release(single()); }

int audio_out_mic_enable(void) { return single()->mic_enable; }
void audio_out_set_persist(int persist) { single()->persist = persist ? 1 : 0; }

int audio_out_ctl_get(const char *key, char *out, size_t cap) { return ao_ctl_get(single(), key, out, cap); }
int audio_out_ctl_set(const char *key, const char *val, char *msg, size_t cap)
{
    return ao_ctl_set(single(), key, val, msg, cap);
}
void audio_out_ctl_list(void (*emit)(void *arg, const char *line), void *arg)
{
    ao_ctl_list(single(), emit, arg);
}
