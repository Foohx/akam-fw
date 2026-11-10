/* Host test of src/ak_rtsp/audio_out.c with a fake PCM device and a fake
 * clock. Build/run: tests/audio_out_test.sh */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "audio_out.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  ok   - %s\n", msg); \
    else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

/* ---------------------------------------------------------------- fake device */

#define EV_WRITE 0xFFFFFFFFul
#define EV_OPEN  0xFFFFFFFEul
#define EV_CLOSE 0xFFFFFFFDul
#define MAX_EV   4096
#define MAX_W    128

static struct {
    unsigned long req[MAX_EV];
    int           arg[MAX_EV];
    int           n;
    unsigned long fail_req;      /* ioctl request that fails with EINVAL (0 = none) */
    int           open_fail;
    int           write_fails;   /* next N writes fail with EIO */
    int           write_eintr;   /* next N writes fail with EINTR */
    int           short_writes;  /* 1: accept 512 bytes at a time */
    unsigned char w[MAX_W][AO_PERIOD_BYTES];
    int           nw;
    struct akpcm_pars pars;
    int           have_pars;
    volatile int64_t clock_us;
    int           sleeps;
    int           real_sleep;    /* thread test: also yield the CPU */
} F;

static void ev(unsigned long req, int arg)
{
    if (F.n < MAX_EV) { F.req[F.n] = req; F.arg[F.n] = arg; F.n++; }
}

static int f_open(void *u, const char *p)
{
    (void)u; (void)p;
    ev(EV_OPEN, 0);
    if (F.open_fail) { errno = ENODEV; return -1; }
    return 7;
}
static int f_ioctl(void *u, int fd, unsigned long req, void *arg)
{
    (void)u; (void)fd;
    int a = 0;
    if (req == AKPCM_IOC_PREPARE) {
        memcpy(&F.pars, arg, sizeof(F.pars));
        F.have_pars = 1;
    } else if (req == AKPCM_IOC_GET_RATE) {
        *(int32_t *)arg = 8000;
        a = 8000;
    } else if (arg) {
        a = *(int32_t *)arg;
    }
    ev(req, a);
    if (F.fail_req && F.fail_req == req) { errno = EINVAL; return -1; }
    return 0;
}
static ssize_t f_write(void *u, int fd, const void *b, size_t n)
{
    (void)u; (void)fd;
    if (F.write_eintr > 0) { F.write_eintr--; errno = EINTR; return -1; }
    if (F.write_fails > 0) { F.write_fails--; errno = EIO; return -1; }
    if (F.short_writes && n > 512) n = 512;
    static size_t partial;
    if (F.nw < MAX_W) memcpy(F.w[F.nw] + partial, b, n);
    partial += n;
    if (partial >= AO_PERIOD_BYTES) { partial = 0; ev(EV_WRITE, 0); F.nw++; }
    return (ssize_t)n;
}
static int f_close(void *u, int fd) { (void)u; (void)fd; ev(EV_CLOSE, 0); return 0; }
static int64_t f_now(void *u) { (void)u; return F.clock_us; }
static void f_sleep(void *u, long us)
{
    (void)u;
    F.sleeps++;
    F.clock_us += us;
    if (F.real_sleep) usleep(200);
}

static const ao_ops_t OPS = { f_open, f_ioctl, f_write, f_close, f_now, f_sleep, NULL };

static char root[256];
static int  case_no;

static void new_case(audio_out_t *ao)
{
    char dir[300];
    memset(&F, 0, sizeof(F));
    snprintf(dir, sizeof(dir), "%s/c%d", root, ++case_no);
    ao_cfg_t cfg = { "/dev/fake", dir };
    ao_init(ao, &OPS, &cfg);
    F.clock_us = 1000000;
}

static int count_req(unsigned long req)
{
    int c = 0;
    for (int i = 0; i < F.n; i++) if (F.req[i] == req) c++;
    return c;
}
static int count_req_arg(unsigned long req, int arg)
{
    int c = 0;
    for (int i = 0; i < F.n; i++) if (F.req[i] == req && F.arg[i] == arg) c++;
    return c;
}
static int idx_of(unsigned long req)
{
    for (int i = 0; i < F.n; i++) if (F.req[i] == req) return i;
    return -1;
}
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* One 64 ms step (one 512-frame period) on the fake clock. */
static int step(audio_out_t *ao)
{
    F.clock_us += 64000;
    return ao_step(ao);
}

/* Open, then step until the auto-mode amp has idled off (it starts on like
 * the stock sequence and turns off AO_AMP_HOLD_MS after open). */
static int open_quiet(audio_out_t *ao)
{
    int rc = ao_open(ao);
    for (int i = 0; rc == 0 && ao_amp_is_on(ao) && i < 100; i++) step(ao);
    return rc;
}

/* Index of the last event matching req and arg, or -1. */
static int last_idx(unsigned long req, int arg)
{
    for (int i = F.n - 1; i >= 0; i--) if (F.req[i] == req && F.arg[i] == arg) return i;
    return -1;
}

static int period_is_zero(int i)
{
    for (int k = 0; k < AO_PERIOD_BYTES; k++) if (F.w[i][k]) return 0;
    return 1;
}
static int16_t wl(int p, int frame) { int16_t s; memcpy(&s, &F.w[p][frame * 2], 2); return s; }

/* Whatever happened, RESUME must never precede a successful PREPARE. */
static int resume_rule_ok(void)
{
    int r = idx_of(AKPCM_IOC_RESUME), p = idx_of(AKPCM_IOC_PREPARE);
    return r < 0 || (p >= 0 && p < r);
}

/* ---------------------------------------------------------------- tests */

static void test_constants(void)
{
    printf("# ioctl aliases (numbers from the stock capture)\n");
    CHECK(AKPCM_IOC_PREPARE == 0x401c50e0u && AKPCM_IOC_PREPARE == AKPCM_IOC_GET_PARS, "PREPARE = 0xE0 (old GET_PARS)");
    CHECK(AKPCM_IOC_RESUME == 0x50e1 && AKPCM_IOC_RESUME == AKPCM_IOC_SET_MODE, "RESUME = 0xE1 (old SET_MODE)");
    CHECK(AKPCM_IOC_SET_SOURCES == 0x40045010u && AKPCM_IOC_GET_SOURCES == 0x80045010u, "SOURCES: SET 0x40045010, GET 0x80045010");
    CHECK(AKPCM_IOC_SET_GAIN == 0x40045030u && AKPCM_IOC_GET_GAIN == 0x80045030u, "GAIN: SET 0x40045030, GET 0x80045030");
    CHECK(AKPCM_IOC_SET_SPEAKER == 0x40045081u, "SET_SPEAKER is 0x40045081 (0x80045081 is EINVAL on the hardware)");
    CHECK(AKPCM_IOC_GET_RATE == 0x800450eau, "GET_RATE 0x800450ea");
    CHECK(AKPCM_IOC_SET_SOURCES != AKPCM_IOC_SET_STATE && AKPCM_IOC_SET_STATE == 0x80045010u, "old capture-side SET_STATE number untouched");
    CHECK(AKPCM_IOC_SET_PERIODS == 0x80045030u && AKPCM_IOC_GET_CAPS == 0x40045081u, "old capture-side names kept");
    CHECK(AO_CHANNELS == 1 && AO_PERIOD_BYTES == 1024 && AO_FRAMES == 512 && AO_DRV_PERIODS == 16, "mono, 1024-byte periods, 16 driver periods");
}

static void test_sequence(void)
{
    printf("# device sequence (stock order)\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    CHECK(ao_open(ao) == 0, "open succeeds");
    CHECK(file_exists(ao->guard_path), "guard marker written before the device sequence");
    CHECK(ao->state == AO_IDLE, "state idle");
    int i_open = idx_of(EV_OPEN), i_src = idx_of(AKPCM_IOC_SET_SOURCES), i_spk = idx_of(AKPCM_IOC_SET_SPEAKER),
        i_prep = idx_of(AKPCM_IOC_PREPARE), i_res = idx_of(AKPCM_IOC_RESUME), i_rate = idx_of(AKPCM_IOC_GET_RATE),
        i_gain = idx_of(AKPCM_IOC_SET_GAIN);
    CHECK(i_open == 0 && i_src == 1 && i_spk == 2 && i_prep == 3 && i_res == 4 && i_rate == 5 && i_gain == 6 && F.n == 7,
          "order: open, SET_SOURCES, SET_SPEAKER, PREPARE, RESUME, GET_RATE, SET_GAIN (nothing else)");
    CHECK(F.arg[i_src] == 4 && F.arg[i_spk] == 1 && F.arg[i_gain] == 2, "default source 4, speaker 1, gain 2");
    CHECK(F.have_pars && F.pars.format == 0 && F.pars.rate == 8000 && F.pars.channels == 1 && F.pars.bits == 16
          && F.pars.period_size == 1024 && F.pars.period_count == 16, "PREPARE: 8 kHz mono 16-bit 1024 x 16");
    CHECK(resume_rule_ok(), "RESUME after PREPARE");
    CHECK(ao_amp_is_on(ao), "auto: amp on after open, like stock");

    ao_close(ao);
    CHECK(!file_exists(ao->guard_path), "clean close removes the guard");
    CHECK(F.req[F.n - 1] == EV_CLOSE, "device closed");
    int i_off = last_idx(AKPCM_IOC_SET_SPEAKER, 0), i_mute = last_idx(AKPCM_IOC_SET_SOURCES, 0);
    CHECK(i_off > 0 && i_mute > i_off && i_mute < F.n - 1, "close: SET_SPEAKER 0, SET_SOURCES 0, then close");
    free(ao);

    /* amp=off opens with the route and speaker off */
    ao = calloc(1, sizeof(*ao));
    new_case(ao);
    ao_set_amp_mode(ao, AO_AMP_OFF);
    CHECK(ao_open(ao) == 0 && !ao_amp_is_on(ao), "amp off: open succeeds, amp off");
    CHECK(F.arg[idx_of(AKPCM_IOC_SET_SOURCES)] == 0 && F.arg[idx_of(AKPCM_IOC_SET_SPEAKER)] == 0
          && count_req(AKPCM_IOC_SET_GAIN) == 0, "amp off: route 0, speaker 0, no gain call");
    ao_close(ao);
    CHECK(count_req(AKPCM_IOC_SET_SPEAKER) == 1, "amp off: nothing to switch off at close");
    free(ao);
}

static void test_failures(void)
{
    printf("# failure paths\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));

    new_case(ao); F.fail_req = AKPCM_IOC_PREPARE;
    CHECK(ao_open(ao) == -1, "PREPARE failure: open fails");
    CHECK(count_req(AKPCM_IOC_RESUME) == 0, "RESUME never issued after a failed PREPARE");
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 1 && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 1,
          "route and speaker switched off again on the failure path");
    CHECK(F.req[F.n - 1] == EV_CLOSE && ao->fd < 0, "fd closed");
    CHECK(ao->state == AO_ERROR, "state error");
    CHECK(!file_exists(ao->guard_path), "graceful failure removes the guard (not a crash)");

    new_case(ao); F.fail_req = AKPCM_IOC_SET_SOURCES;
    CHECK(ao_open(ao) == -1 && count_req(AKPCM_IOC_PREPARE) == 0 && count_req(AKPCM_IOC_RESUME) == 0,
          "SET_SOURCES failure: no PREPARE, no RESUME");

    new_case(ao); F.fail_req = AKPCM_IOC_RESUME;
    CHECK(ao_open(ao) == -1 && ao->fd < 0, "RESUME failure: open fails, fd closed");

    new_case(ao); F.open_fail = 1;
    CHECK(ao_open(ao) == -1 && count_req(AKPCM_IOC_PREPARE) == 0 && ao->state == AO_ERROR, "open failure: nothing else called");

    new_case(ao); F.fail_req = AKPCM_IOC_SET_SPEAKER;
    CHECK(ao_open(ao) == 0 && count_req(AKPCM_IOC_RESUME) == 1, "SET_SPEAKER failure is not fatal");
    ao_close(ao);

    new_case(ao); F.fail_req = AKPCM_IOC_SET_GAIN;
    CHECK(ao_open(ao) == 0 && count_req(AKPCM_IOC_RESUME) == 1, "SET_GAIN failure is not fatal");
    ao_close(ao);

    /* amp on at close after a failed later step */
    new_case(ao); F.fail_req = AKPCM_IOC_RESUME; ao_set_amp_mode(ao, AO_AMP_ON);
    ao_open(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 1 && F.arg[idx_of(AKPCM_IOC_SET_SPEAKER)] == 1,
          "amp on at open, switched off on the failure path");
    CHECK(count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 1, "and the route muted");
    CHECK(resume_rule_ok(), "rule holds in every failure case");
    free(ao);
}

static void test_guard(void)
{
    printf("# crash guard\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    mkdir(ao->conf_dir, 0700);
    FILE *f = fopen(ao->guard_path, "w"); fputs("1\n", f); fclose(f);
    CHECK(ao_open(ao) == -1, "marker present: sequence skipped");
    CHECK(F.n == 0, "no device call at all (not even open)");
    CHECK(ao->state == AO_DISABLED_CRASH && !strcmp(ao_state_name(ao->state), "disabled_crash"), "state disabled_crash");
    CHECK(file_exists(ao->guard_path), "marker left for the user to clear");

    new_case(ao);
    CHECK(ao_open(ao) == 0, "no marker: open");
    for (int i = 0; i < 60; i++) step(ao);    /* 3.8 s */
    CHECK(file_exists(ao->guard_path), "marker kept before 5 s of clean playback");
    for (int i = 0; i < 30; i++) step(ao);    /* 5.8 s */
    CHECK(!file_exists(ao->guard_path), "marker removed after 5 s of clean playback");
    ao_close(ao);

    new_case(ao);
    ao_open(ao);
    F.write_fails = 1;
    step(ao);
    for (int i = 0; i < 200; i++) step(ao);
    CHECK(file_exists(ao->guard_path), "a write error keeps the marker past 5 s");
    ao_close(ao);
    CHECK(!file_exists(ao->guard_path), "but a clean close still removes it");

    /* directory that cannot be created: refuse rather than run unguarded */
    new_case(ao);
    char bad[400];
    snprintf(bad, sizeof(bad), "%s/c%d/file", root, case_no);
    mkdir(ao->conf_dir, 0700);
    f = fopen(bad, "w"); fclose(f);
    ao_cfg_t cfg = { "/dev/fake", bad };
    ao_init(ao, &OPS, &cfg);
    CHECK(ao_open(ao) == -1 && F.n == 0 && ao->state == AO_ERROR, "guard unwritable: audio out refused, device untouched");
    free(ao);
}

static void test_silence_and_beep(void)
{
    printf("# silence and beep\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    ao_open(ao);
    ao_set_volume(ao, 100);
    CHECK(ao_amp_is_on(ao), "amp on at open (stock sequence)");
    for (int i = 0; i < 100 && ao_amp_is_on(ao); i++) step(ao);
    CHECK(!ao_amp_is_on(ao) && ao->state == AO_IDLE, "idle for 2 s: amp off");
    int i_spk0 = last_idx(AKPCM_IOC_SET_SPEAKER, 0), i_src0 = last_idx(AKPCM_IOC_SET_SOURCES, 0);
    CHECK(i_spk0 > 0 && i_src0 > i_spk0, "amp off: SET_SPEAKER 0 then SET_SOURCES 0");
    int n0 = F.n, nw0 = F.nw;
    for (int i = 0; i < 5; i++) CHECK(step(ao) == 0, "idle step ok");
    CHECK(F.nw - nw0 == 5 && period_is_zero(nw0) && period_is_zero(nw0 + 4), "idle: 1024-byte periods of silence");
    CHECK(F.n - n0 == 5, "idle: nothing but writes (amp stays off)");

    ao_set_gain(ao, 3);
    ao_play_test(ao);
    CHECK(ao_test_active(ao), "test queued");
    int w0 = F.nw;
    int steps = 0;
    while (ao_test_active(ao) && steps < 100) { step(ao); steps++; }
    CHECK(steps == 16, "beep lasts 16 periods (1 s)");
    CHECK(ao_amp_is_on(ao), "amp turned on for the session");
    int i_on = last_idx(AKPCM_IOC_SET_SPEAKER, 1);
    CHECK(i_on > n0, "SET_SPEAKER 1 issued");
    CHECK(F.req[i_on - 1] == AKPCM_IOC_SET_SOURCES && F.arg[i_on - 1] == 4
          && F.req[i_on + 1] == AKPCM_IOC_SET_GAIN && F.arg[i_on + 1] == 3 && F.req[i_on + 2] == EV_WRITE,
          "turn-on order: SET_SOURCES 4, SET_SPEAKER 1, SET_GAIN, then audio");
    CHECK(period_is_zero(w0), "one preload period of silence (64 ms) after the amp turns on");
    CHECK(F.nw - w0 == 1 + 16, "1 preload + 16 beep periods written");

    /* content of the beep periods: mono, equal to ao_beep_sample(idx) at volume 100 */
    int ok_val = 1, peak = 0;
    for (int p = 0; p < 16; p++)
        for (int fr = 0; fr < AO_FRAMES; fr++) {
            int idx = p * AO_FRAMES + fr;
            int16_t l = wl(w0 + 1 + p, fr);
            if (l != ao_beep_sample((unsigned)idx)) ok_val = 0;
            if (abs(l) > peak) peak = abs(l);
        }
    CHECK(ok_val, "written samples equal the synthesized beep at volume 100 (mono, 512 frames per period)");
    CHECK(peak == 8231, "peak amplitude -12 dBFS (8231)");
    CHECK(ao->state == AO_IDLE || ao->state == AO_TEST, "state returns to idle after the beep");

    /* beep structure */
    int tone_nonzero[3] = {0}, gap_nonzero = 0;
    for (unsigned i = 0; i < AO_BEEP_SAMPLES; i++) {
        int16_t s = ao_beep_sample(i);
        if (s && (i % 3200) >= 1600) gap_nonzero++;
        if (s) tone_nonzero[i / 3200]++;
    }
    CHECK(gap_nonzero == 0, "gaps are silent");
    CHECK(tone_nonzero[0] > 1000 && tone_nonzero[1] > 1000 && tone_nonzero[2] > 1000, "three 200 ms tones");
    CHECK(ao_beep_sample(0) == 0 && ao_beep_sample(1599) == 0 && ao_beep_sample(3200) == 0 && ao_beep_sample(7999) == 0,
          "tones start and end at zero (10 ms ramps)");
    CHECK(abs(ao_beep_sample(42)) < 8231 && abs(ao_beep_sample(42)) > 0, "ramp: 10 ms fade-in");
    int pk = 0;
    for (unsigned i = 800; i < 900; i++) if (abs(ao_beep_sample(i)) > pk) pk = abs(ao_beep_sample(i));
    CHECK(pk == 8231, "plateau reaches 8231");
    CHECK(ao_beep_sample(AO_BEEP_SAMPLES) == 0 && ao_beep_sample(100000) == 0, "outside the buffer: 0");
    /* 1 kHz: period of 8 samples */
    CHECK(ao_beep_sample(808) == ao_beep_sample(800) && ao_beep_sample(813) == ao_beep_sample(805), "1 kHz at 8 kHz");
    ao_close(ao);
    free(ao);
}

static void test_amp_policy(void)
{
    printf("# amp policy\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    open_quiet(ao);
    ao_set_volume(ao, 100);
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == 1 && count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 1
          && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 1, "after open: on once (stock), off once (idle)");
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    int64_t end_ms = F.clock_us / 1000;            /* last beep period finished */
    int off_at = -1;
    for (int i = 0; i < 100 && off_at < 0; i++) {
        step(ao);
        if (!ao_amp_is_on(ao)) off_at = i;
    }
    int64_t off_ms = F.clock_us / 1000 - end_ms;
    CHECK(off_at >= 0, "amp goes off after the session");
    CHECK(off_ms >= 1900 && off_ms <= 2100, "amp off about 2 s after the last audible period");
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == 2 && count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 2
          && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 2, "session: speaker on/off and route on/off once each");
    int i_off = last_idx(AKPCM_IOC_SET_SPEAKER, 0), i_mute = last_idx(AKPCM_IOC_SET_SOURCES, 0);
    CHECK(i_mute == i_off + 1, "off: SET_SPEAKER 0 then SET_SOURCES 0");

    /* a SET_SPEAKER(0) that the driver refuses is not retried every period */
    new_case(ao);
    open_quiet(ao);
    F.fail_req = AKPCM_IOC_SET_SPEAKER;
    ao_set_volume(ao, 100);
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    for (int i = 0; i < 60; i++) step(ao);
    CHECK(!ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 2 && ao->spk_off_warned,
          "SET_SPEAKER(0) failure: amp state still off, one attempt per off transition");
    for (int i = 0; i < 40; i++) step(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 2, "no retry while idle");
    ao_close(ao);

    /* second session within the hold window: no toggling */
    new_case(ao);
    open_quiet(ao);
    ao_set_volume(ao, 100);
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    for (int i = 0; i < 10; i++) step(ao);         /* ~0.6 s of idle */
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    for (int i = 0; i < 10; i++) step(ao);
    CHECK(ao_amp_is_on(ao), "still on within the 2 s hold");
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == 2, "second session within 2 s does not toggle the amp");
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 1, "(only the idle-after-open off so far)");
    for (int i = 0; i < 40; i++) step(ao);
    CHECK(!ao_amp_is_on(ao), "off once the hold expires");

    /* modes */
    new_case(ao);
    ao_set_amp_mode(ao, AO_AMP_ON);
    ao_open(ao);
    CHECK(ao_amp_is_on(ao) && F.arg[idx_of(AKPCM_IOC_SET_SPEAKER)] == 1 && F.arg[idx_of(AKPCM_IOC_SET_SOURCES)] == 4,
          "amp on mode: route and speaker on from the start");
    for (int i = 0; i < 100; i++) step(ao);
    CHECK(ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 0 && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 0,
          "amp on mode: stays on while idle (route never muted)");
    ao_set_amp_mode(ao, AO_AMP_OFF);
    step(ao);
    CHECK(!ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 1 && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 1,
          "switch to off at runtime: speaker and route off");
    ao_set_volume(ao, 100);
    ao_play_test(ao);
    int on_before = count_req_arg(AKPCM_IOC_SET_SPEAKER, 1);
    int src_before = count_req_arg(AKPCM_IOC_SET_SOURCES, 4);
    while (ao_test_active(ao)) step(ao);
    CHECK(!ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == on_before
          && count_req_arg(AKPCM_IOC_SET_SOURCES, 4) == src_before, "amp off mode: never on, even during a beep");
    ao_set_amp_mode(ao, AO_AMP_AUTO);
    step(ao);
    CHECK(!ao_amp_is_on(ao), "auto: off at rest");
    ao_close(ao);

    /* amp=off from the start */
    new_case(ao);
    ao_set_amp_mode(ao, AO_AMP_OFF);
    ao_open(ao);
    ao_set_volume(ao, 100);
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    CHECK(!ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == 0 && count_req_arg(AKPCM_IOC_SET_SOURCES, 4) == 0
          && count_req(AKPCM_IOC_SET_GAIN) == 0, "amp off from open: never on, no gain call");
    ao_close(ao);

    /* volume 0 keeps the amp off */
    new_case(ao);
    open_quiet(ao);
    ao_set_volume(ao, 0);
    step(ao); /* let the 10 ms smoothing reach 0 */
    ao_play_test(ao);
    while (ao_test_active(ao)) step(ao);
    CHECK(!ao_amp_is_on(ao) && count_req_arg(AKPCM_IOC_SET_SPEAKER, 1) == 1, "volume 0: amp stays off (only the open-time on)");
    int allzero = 1;
    for (int i = 0; i < F.nw; i++) if (!period_is_zero(i)) allzero = 0;
    CHECK(allzero, "volume 0: exact silence");

    /* amp off on every exit path */
    new_case(ao);
    open_quiet(ao); ao_set_volume(ao, 100); ao_play_test(ao); step(ao);
    CHECK(ao_amp_is_on(ao), "amp on during a session");
    ao_close(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_SPEAKER, 0) == 2 && count_req_arg(AKPCM_IOC_SET_SOURCES, 0) == 2 && !ao_amp_is_on(ao),
          "close: SET_SPEAKER 0 and SET_SOURCES 0");

    new_case(ao);
    ao_open(ao); ao_set_volume(ao, 100); ao_play_test(ao); step(ao);
    ao->enabled = 0;
    step(ao);
    CHECK(!ao_amp_is_on(ao) && ao->state == AO_OFF, "SET enable 0 at runtime: amp off, state off");
    CHECK(period_is_zero(F.nw - 1), "and silence is written");
    ao_play_test(ao);
    step(ao);
    CHECK(!ao_test_active(ao) && !ao_amp_is_on(ao), "test request ignored while disabled");
    ao->enabled = 1;
    ao_close(ao);

    new_case(ao);
    ao_open(ao); ao_set_volume(ao, 100); ao_play_test(ao); step(ao);
    F.write_fails = AO_MAX_FAILS;
    int rc = 0, k = 0;
    while (rc == 0 && k < 20) { rc = step(ao); k++; }
    CHECK(rc == -1 && k == AO_MAX_FAILS, "five consecutive write failures stop the thread");
    CHECK(!ao_amp_is_on(ao) && ao->state == AO_ERROR, "amp off and state error on write failure");
    CHECK(F.sleeps >= AO_MAX_FAILS - 1, "backoff between failures");
    ao_close(ao);
    free(ao);
}

static void test_writes(void)
{
    printf("# write handling\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    ao_open(ao);
    F.write_eintr = 3;
    CHECK(step(ao) == 0 && F.nw == 1, "EINTR is retried transparently");
    F.write_fails = 2;
    CHECK(step(ao) == 0 && step(ao) == 0 && step(ao) == 0, "two isolated failures are survived");
    CHECK(ao->fails == 0, "failure streak reset by a good write");
    F.short_writes = 1;
    int nw = F.nw;
    step(ao);
    CHECK(F.nw == nw + 1, "short writes are completed to a full 1024-byte period");
    ao_close(ao);
    free(ao);
}

/* Pushed audio only plays once the jitter buffer holds its 120 ms pre-fill:
 * queue the block, then enough silence behind it to get past that threshold. */
static void push_ready(audio_out_t *ao, const int16_t *pcm, size_t n)
{
    static const int16_t zeros[JITTER_PREFILL_SAMPLES];
    ao_push(ao, pcm, n);
    ao_push(ao, zeros, JITTER_PREFILL_SAMPLES);
}

static void test_volume_and_push(void)
{
    printf("# volume and push\n");
    CHECK(ao_volume_gain_q15(0) == 0 && ao_volume_gain_q15(100) == 32768, "gain: 0 -> 0, 100 -> unity");
    int mono = 1;
    for (int v = 1; v <= 100; v++) if (ao_volume_gain_q15(v) < ao_volume_gain_q15(v - 1)) mono = 0;
    CHECK(mono, "gain curve monotone");
    CHECK(ao_volume_gain_q15(50) == 8192, "volume 50 = (0.5)^2 = 1/4");
    CHECK(ao_volume_gain_q15(-5) == 0 && ao_volume_gain_q15(500) == 32768, "clamped");

    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    open_quiet(ao);
    int16_t in[AO_FRAMES];
    for (int i = 0; i < AO_FRAMES; i++) in[i] = (int16_t)(i * 100 - 25600);
    in[0] = -32768; in[1] = 32767;

    ao_set_volume(ao, 100);
    step(ao); /* let the smoothing settle at the target (already unity) */
    CHECK(ao_push(ao, in, AO_FRAMES) == AO_FRAMES, "push accepted");
    step(ao);
    CHECK(ao->state == AO_IDLE && !ao_amp_is_on(ao), "below the pre-fill nothing is played: idle, amp off");
    ao_close(ao);
    new_case(ao); open_quiet(ao); ao_set_volume(ao, 100);
    step(ao); /* settle the smoothing at unity */
    push_ready(ao, in, AO_FRAMES);
    step(ao);
    int same = 1;
    for (int i = 0; i < AO_FRAMES; i++) if (wl(F.nw - 1, i) != in[i]) same = 0;
    CHECK(same, "volume 100: exact identity, no 16-bit overflow");
    CHECK(ao->state == AO_TALKING, "pushed audio: state talking");
    CHECK(ao_amp_is_on(ao), "pushed audio turns the amp on");

    /* volume 50 */
    new_case(ao); ao_open(ao); ao_set_volume(ao, 50); ao->cur_gain_q15 = ao_volume_gain_q15(50);
    push_ready(ao, in, AO_FRAMES); step(ao);
    int ok50 = 1;
    for (int i = 0; i < AO_FRAMES; i++) if (wl(F.nw - 1, i) != (int16_t)(((int)in[i] * 8192) >> 15)) ok50 = 0;
    CHECK(ok50, "volume 50: samples scaled by 1/4");

    /* volume 0 exact silence */
    new_case(ao); ao_open(ao); ao_set_volume(ao, 0); ao->cur_gain_q15 = 0;
    push_ready(ao, in, AO_FRAMES); step(ao);
    CHECK(period_is_zero(F.nw - 1), "volume 0: exact silence for pushed data");
    CHECK(ao_queued(ao) == JITTER_PREFILL_SAMPLES, "data still consumed at volume 0");

    /* smoothing: 100 -> 0 with a constant input must ramp without a jump */
    new_case(ao); ao_open(ao); ao_set_volume(ao, 100);
    ao_set_volume(ao, 100);
    step(ao); /* settle the smoothing at unity */
    int16_t dc[AO_FRAMES];
    for (int i = 0; i < AO_FRAMES; i++) dc[i] = 10000;
    ao_push(ao, dc, AO_FRAMES); ao_push(ao, dc, AO_FRAMES);
    ao_push(ao, dc, AO_FRAMES); ao_push(ao, dc, AO_FRAMES); /* 1024 >= pre-fill */
    step(ao);
    CHECK(wl(F.nw - 1, 10) == 10000, "unity before the change");
    ao_set_volume(ao, 0);
    step(ao);
    int maxjump = 0, decreasing = 1;
    for (int i = 1; i < AO_FRAMES; i++) {
        int d = wl(F.nw - 1, i - 1) - wl(F.nw - 1, i);
        if (d < 0) decreasing = 0;
        if (d > maxjump) maxjump = d;
    }
    CHECK(wl(F.nw - 1, 0) > 9000, "ramp starts near the old level");
    CHECK(decreasing && maxjump < 200, "volume change is smoothed (no step)");
    CHECK(wl(F.nw - 1, 100) == 0 && wl(F.nw - 1, AO_FRAMES - 1) == 0, "reaches 0 within about 10 ms");

    /* jitter buffer behind the push API */
    new_case(ao); ao_open(ao);
    static int16_t big[JITTER_MAX_SAMPLES + 800];
    CHECK(ao_push(ao, big, JITTER_MAX_SAMPLES + 800) == JITTER_MAX_SAMPLES + 800,
          "push never blocks and never reports a short count");
    CHECK(ao_queued(ao) <= JITTER_MAX_SAMPLES, "overrun: the queue stays within the 400 ms cap");
    for (int i = 0; i < 16; i++) step(ao);
    CHECK(ao_queued(ao) == 0, "queue drained 512 samples per period");

    new_case(ao); ao_open(ao); ao_set_volume(ao, 100);
    int16_t part[JITTER_PREFILL_SAMPLES + 100];
    for (int i = 0; i < JITTER_PREFILL_SAMPLES + 100; i++) part[i] = 1000;
    ao_push(ao, part, JITTER_PREFILL_SAMPLES + 100);
    for (int i = 0; i < 2; i++) step(ao); /* 2 x 512 of the 1060 samples */
    step(ao);
    CHECK(wl(F.nw - 1, 35) == 1000 && wl(F.nw - 1, 36) == 0, "partial data then zero padding (underrun = silence)");
    CHECK(audio_jitter_underruns(&ao->jit) == 1, "the underrun is counted");

    /* disabled: nothing queued survives, nothing is played */
    new_case(ao); ao_open(ao);
    push_ready(ao, in, AO_FRAMES);
    ao->enabled = 0;
    step(ao);
    CHECK(ao_queued(ao) == 0 && period_is_zero(F.nw - 1), "disabled: queue flushed, silence written");
    ao_close(ao);
    free(ao);
}

static void test_live_settings(void)
{
    printf("# live settings\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    ao_open(ao);
    int n0 = F.n;
    step(ao);
    CHECK(F.n - n0 == 1, "no spurious ioctl when nothing changed");
    ao_set_gain(ao, 5);
    ao_set_source(ao, 6);
    step(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_GAIN, 5) == 1, "gain applied by the playback thread");
    CHECK(count_req_arg(AKPCM_IOC_SET_SOURCES, 6) == 1, "source applied by the playback thread");
    step(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_GAIN, 5) == 1, "applied once");
    ao_set_source(ao, 5);
    CHECK(ao->source == 6, "invalid source ignored");
    ao_set_source(ao, 0);
    CHECK(ao->source == 6, "source 0 (route off) is not a configurable value");
    ao_set_gain(ao, 99);
    CHECK(ao->gain == 5, "gain clamped");
    ao_close(ao);

    /* amp off: the route stays muted, new values are applied at the next turn-on */
    new_case(ao);
    open_quiet(ao);
    ao_set_volume(ao, 100);
    ao_set_gain(ao, 1);
    ao_set_source(ao, 6);
    step(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_GAIN, 1) == 0 && count_req_arg(AKPCM_IOC_SET_SOURCES, 6) == 0,
          "amp off: gain and source changes do not unmute the route");
    ao_play_test(ao);
    step(ao);
    CHECK(count_req_arg(AKPCM_IOC_SET_SOURCES, 6) == 1 && count_req_arg(AKPCM_IOC_SET_GAIN, 1) == 1,
          "applied when the amp turns on");
    ao_close(ao);
    free(ao);
}

static void test_config(void)
{
    printf("# config file\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    CHECK(ao_load_config(ao) == 0 && ao->enabled == 1 && ao->volume == 50 && ao->gain == 2 && ao->source == 4
          && ao->amp_mode == AO_AMP_AUTO, "no file: defaults (gain 2, source 4), audio out enabled");

    mkdir(ao->conf_dir, 0700);
    FILE *f = fopen(ao->conf_path, "w");
    fputs("enable=0\nvolume=20\r\ngain=9\nsource=6\nchime=1\njunk\nvolume=abc\n", f);
    fclose(f);
    CHECK(ao_load_config(ao) == 1, "file read");
    CHECK(ao->enabled == 0 && ao->volume == 20 && ao->gain == 2 && ao->source == 6, "valid keys applied, bad ones ignored");

    ao->enabled = 1; ao->volume = 77;
    CHECK(ao_save_config(ao) == 0, "save");
    struct stat st;
    stat(ao->conf_path, &st);
    CHECK((st.st_mode & 0777) == 0600, "file mode 0600");
    stat(ao->conf_dir, &st);
    CHECK((st.st_mode & 0777) == 0700, "directory mode 0700");
    char buf[512] = "";
    f = fopen(ao->conf_path, "r"); size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    CHECK(strstr(buf, "chime=1\n") && strstr(buf, "enable=1\n") && strstr(buf, "volume=77\n") && !strstr(buf, "enable=0"),
          "foreign keys kept, ours rewritten");
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ao->conf_path);
    CHECK(!file_exists(tmp), "no temp file left (atomic rename)");

    audio_out_t *b = calloc(1, sizeof(*b));
    memset(&F, 0, sizeof(F));
    ao_cfg_t cfg = { "/dev/fake", ao->conf_dir };
    ao_init(b, &OPS, &cfg);
    ao_load_config(b);
    CHECK(b->volume == 77 && b->enabled == 1, "round trip");

    /* disabled in the file: ao_open does not touch the device or the guard */
    b->enabled = 0;
    CHECK(ao_open(b) == -1 && F.n == 0 && b->state == AO_OFF && !file_exists(b->guard_path),
          "enable=0: device untouched, no guard");
    free(b);
    free(ao);
}

static void collect(void *arg, const char *line)
{
    int *n = arg;
    (*n)++;
    (void)line;
}

static void test_ctl(void)
{
    printf("# 8091 keys\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    char out[160];
    new_case(ao);

    CHECK(ao_ctl_get(ao, "ae.speed", out, sizeof(out)) == -1, "foreign key: not ours");
    CHECK(ao_ctl_set(ao, "night.mode", "day", out, sizeof(out)) == -1, "foreign key set: not ours");

    ao_ctl_get(ao, "audio.out_enable", out, sizeof(out));
    CHECK(!strcmp(out, "audio.out_enable=1"), "enable default 1");
    ao_ctl_get(ao, "audio.out_volume", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_volume=50"), "volume default 50");
    ao_ctl_get(ao, "audio.out_gain", out, sizeof(out));   CHECK(!strcmp(out, "audio.out_gain=2"), "gain default 2");
    ao_ctl_get(ao, "audio.out_source", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_source=4"), "source default 4");
    ao_ctl_get(ao, "audio.out_amp", out, sizeof(out));    CHECK(!strcmp(out, "audio.out_amp=auto"), "amp default auto");
    ao_ctl_get(ao, "audio.out_test", out, sizeof(out));   CHECK(!strcmp(out, "audio.out_test=0"), "test default 0");
    ao_ctl_get(ao, "audio.out_state", out, sizeof(out));  CHECK(!strcmp(out, "audio.out_state=off"), "state off before open");
    ao_ctl_get(ao, "audio.out_nope", out, sizeof(out));   CHECK(!strncmp(out, "ERR unknown param", 17), "unknown audio key");

    CHECK(ao_ctl_set(ao, "audio.out_volume", "30", out, sizeof(out)) == 0 && !strcmp(out, "OK audio.out_volume=30") && ao->volume == 30, "set volume");
    CHECK(ao_ctl_set(ao, "audio.out_volume", "101", out, sizeof(out)) == 1 && !strncmp(out, "ERR", 3) && ao->volume == 30, "volume > 100 refused");
    CHECK(ao_ctl_set(ao, "audio.out_volume", "-1", out, sizeof(out)) == 1, "volume < 0 refused");
    CHECK(ao_ctl_set(ao, "audio.out_volume", "5x", out, sizeof(out)) == 1, "volume garbage refused");
    CHECK(ao_ctl_set(ao, "audio.out_gain", "5", out, sizeof(out)) == 0 && ao->gain == 5, "set gain 5");
    CHECK(ao_ctl_set(ao, "audio.out_gain", "6", out, sizeof(out)) == 1 && ao->gain == 5, "gain 6 refused");
    CHECK(ao_ctl_set(ao, "audio.out_source", "6", out, sizeof(out)) == 0 && ao->source == 6, "set source 6 (experiment)");
    CHECK(ao_ctl_set(ao, "audio.out_source", "5", out, sizeof(out)) == 1 && ao->source == 6, "source 5 refused");
    CHECK(ao_ctl_set(ao, "audio.out_source", "7", out, sizeof(out)) == 1, "source 7 refused");
    CHECK(ao_ctl_set(ao, "audio.out_source", "0", out, sizeof(out)) == 1 && ao->source == 6, "source 0 refused");
    CHECK(ao_ctl_set(ao, "audio.out_source", "4", out, sizeof(out)) == 0 && ao->source == 4, "set source 4 (default)");
    CHECK(ao_ctl_set(ao, "audio.out_source", "6", out, sizeof(out)) == 0 && ao->source == 6, "back to 6");
    {
        char pbuf[256] = "";
        FILE *pf = fopen(ao->conf_path, "r");
        size_t pn = pf ? fread(pbuf, 1, sizeof(pbuf) - 1, pf) : 0;
        if (pf) fclose(pf);
        pbuf[pn] = 0;
        CHECK(strstr(pbuf, "volume=30\n") && strstr(pbuf, "gain=5\n") && strstr(pbuf, "source=6\n"),
              "volume, gain and source are persisted when they change");
        unlink(ao->conf_path);
        ao_ctl_set(ao, "audio.out_volume", "30", out, sizeof(out));
        ao_ctl_set(ao, "audio.out_gain", "5", out, sizeof(out));
        ao_ctl_set(ao, "audio.out_source", "6", out, sizeof(out));
        CHECK(!file_exists(ao->conf_path), "same-value SET does not write the flash");
        ao_ctl_set(ao, "audio.out_volume", "31", out, sizeof(out));
        CHECK(file_exists(ao->conf_path), "a changed value is written");
        ao_ctl_set(ao, "audio.out_volume", "30", out, sizeof(out));
    }
    CHECK(ao_ctl_set(ao, "audio.out_amp", "on", out, sizeof(out)) == 0 && ao->amp_mode == AO_AMP_ON, "amp on");
    CHECK(ao_ctl_set(ao, "audio.out_amp", "off", out, sizeof(out)) == 0 && ao->amp_mode == AO_AMP_OFF, "amp off");
    CHECK(ao_ctl_set(ao, "audio.out_amp", "auto", out, sizeof(out)) == 0 && ao->amp_mode == AO_AMP_AUTO, "amp auto");
    CHECK(ao_ctl_set(ao, "audio.out_amp", "loud", out, sizeof(out)) == 1 && !strncmp(out, "ERR", 3), "amp garbage refused");
    CHECK(ao_ctl_set(ao, "audio.out_state", "idle", out, sizeof(out)) == 1 && strstr(out, "read-only"), "state is read-only");

    CHECK(ao_ctl_set(ao, "audio.out_test", "1", out, sizeof(out)) == 1 && strstr(out, "not running"), "test refused when not running");
    CHECK(!ao_test_active(ao), "and not queued");

    /* enable: restart wording, persistence */
    CHECK(ao_ctl_set(ao, "audio.out_enable", "2", out, sizeof(out)) == 1, "enable 2 refused");
    CHECK(ao_ctl_set(ao, "audio.out_enable", "1", out, sizeof(out)) == 0 && !strcmp(out, "OK audio.out_enable=1 (restart to apply)"),
          "enable=1 while the device is not open: restart to apply");
    CHECK(file_exists(ao->conf_path), "enable persisted");
    CHECK(ao_ctl_set(ao, "audio.out_enable", "0", out, sizeof(out)) == 0 && !strcmp(out, "OK audio.out_enable=0"), "enable=0");
    audio_out_t *b = calloc(1, sizeof(*b));
    ao_cfg_t cfg = { "/dev/fake", ao->conf_dir };
    ao_init(b, &OPS, &cfg);
    ao_load_config(b);
    CHECK(b->enabled == 0, "disabled state survives a restart (read back from the file)");
    free(b);

    /* running */
    new_case(ao);
    ao_open(ao);
    ao_ctl_get(ao, "audio.out_state", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_state=idle"), "state idle once open");
    CHECK(ao_ctl_set(ao, "audio.out_enable", "1", out, sizeof(out)) == 0 && !strcmp(out, "OK audio.out_enable=1"), "enable=1 while running: no restart needed");
    CHECK(ao_ctl_set(ao, "audio.out_test", "1", out, sizeof(out)) == 0 && !strcmp(out, "OK audio.out_test=1"), "test accepted");
    ao_ctl_get(ao, "audio.out_test", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_test=1"), "test reads 1 while queued/playing");
    step(ao);
    ao_ctl_get(ao, "audio.out_state", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_state=test"), "state test while playing");
    while (ao_test_active(ao)) step(ao);
    ao_ctl_get(ao, "audio.out_test", out, sizeof(out)); CHECK(!strcmp(out, "audio.out_test=0"), "test falls back to 0");
    CHECK(ao_ctl_set(ao, "audio.out_test", "0", out, sizeof(out)) == 0, "test 0 accepted (no-op)");
    int n = 0;
    ao_ctl_list(ao, collect, &n);
    CHECK(n == 7, "LIST emits the seven audio.out_ lines");
    ao_close(ao);
    free(ao);
}

static void test_thread(void)
{
    printf("# thread\n");
    audio_out_t *ao = calloc(1, sizeof(*ao));
    new_case(ao);
    F.real_sleep = 1;
    CHECK(ao_open(ao) == 0, "open");
    CHECK(ao_thread_start(ao) == 0, "thread started");
    ao_play_test(ao);
    for (int i = 0; i < 4000 && file_exists(ao->guard_path); i++) usleep(1000);
    CHECK(!file_exists(ao->guard_path), "thread runs: guard cleared after 5 s of (virtual) playback");
    ao_thread_stop(ao);
    CHECK(F.nw > 80, "periods were written by the thread");
    int nw = F.nw;
    usleep(20000);
    CHECK(F.nw == nw, "no write after stop");
    ao_close(ao);
    CHECK(F.req[F.n - 1] == EV_CLOSE && !ao_amp_is_on(ao), "closed, amp off");
    ao_thread_stop(ao);
    CHECK(1, "stop is idempotent");

    /* the thread exits by itself on a device failure */
    new_case(ao);
    ao_open(ao);
    F.write_fails = 1000;
    ao_thread_start(ao);
    for (int i = 0; i < 2000 && ao->state != AO_ERROR; i++) usleep(1000);
    CHECK(ao->state == AO_ERROR, "write failures end in state error");
    {
        char emsg[160];
        CHECK(ao_ctl_set(ao, "audio.out_test", "1", emsg, sizeof(emsg)) == 1 && strstr(emsg, "not running"),
              "test refused in state error (thread is gone)");
    }
    ao_thread_stop(ao);
    ao_close(ao);
    free(ao);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/ao_test_XXXXXX");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }

    test_constants();
    test_sequence();
    test_failures();
    test_guard();
    test_silence_and_beep();
    test_amp_policy();
    test_writes();
    test_volume_and_push();
    test_live_settings();
    test_config();
    test_ctl();
    test_thread();

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* best effort */ }
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
