#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "audio_jitter.h"

/* Speaker output through /dev/pcmC0D0p (specs/05-spec-audio-out.md, step A).
 *
 * ak_rtsp already owns the playback device for its whole life (the driver
 * refuses a second open) and used to write silence to it. When audio out is
 * enabled this module replaces that silence loop: it runs the device
 * sequence PREPARE -> SOURCES -> RESUME (never RESUME before a successful
 * PREPARE), then a playback thread writes 1024-byte stereo periods that are
 * silence, a synthesized beep, or samples pushed with audio_out_push().
 * When disabled (or when the sequence fails) nothing here touches the device
 * and audio.c keeps its legacy silence loop unchanged.
 *
 * The engine (ao_*) works on an instance with injectable device operations
 * and clock so the host tests can drive it with a fake device. The
 * audio_out_* functions are the process-wide singleton used by audio.c,
 * control.c and (later) the talk server. */

/* Device format: 8 kHz, 16-bit, stereo (L = R), periods of 1024 bytes. */
#define AO_RATE            8000
#define AO_CHANNELS        2
#define AO_FRAMES          256                          /* frames per period (32 ms) */
#define AO_PERIOD_BYTES    (AO_FRAMES * AO_CHANNELS * 2)
#define AO_DRV_PERIODS     8                            /* driver periods (~256 ms) */

#define AO_AMP_HOLD_MS     2000   /* amp stays on this long after the last audible period */
#define AO_PRELOAD_PERIODS 2      /* silence written after the amp turns on (64 ms >= 60 ms) */
#define AO_GUARD_CLEAR_MS  5000   /* crash guard removed after this much clean playback */
#define AO_MAX_FAILS       5      /* consecutive write failures before the thread stops */
#define AO_VOL_RAMP        80     /* volume smoothing: 10 ms at 8 kHz */

#define AO_BEEP_SAMPLES    8000   /* 3 x 200 ms tones, 200 ms gaps: exactly 1 s */

enum ao_state {
    AO_OFF = 0,        /* disabled (config, SET audio.out_enable 0) */
    AO_IDLE,
    AO_TALKING,        /* pushed audio playing */
    AO_TEST,           /* beep playing */
    AO_CHIME,          /* reserved for the start-up chime (later) */
    AO_DISABLED_CRASH, /* crash guard marker found: sequence skipped */
    AO_ERROR           /* device sequence or writes failed */
};

enum ao_amp_mode { AO_AMP_AUTO = 0, AO_AMP_ON, AO_AMP_OFF };

typedef struct ao_ops {
    int     (*open)(void *user, const char *path);                  /* fd or -1 (errno) */
    int     (*ioctl)(void *user, int fd, unsigned long req, void *arg);
    ssize_t (*write)(void *user, int fd, const void *buf, size_t n);
    int     (*close)(void *user, int fd);
    int64_t (*now_us)(void *user);                                  /* monotonic */
    void    (*sleep_us)(void *user, long us);
    void    *user;
} ao_ops_t;

typedef struct ao_cfg {
    const char *dev_path;   /* "/dev/pcmC0D0p" */
    const char *conf_dir;   /* "/etc/config/akam": holds audio_out and audio_out.trying */
} ao_cfg_t;

#define AO_CONF_FILE   "audio_out"
#define AO_GUARD_FILE  "audio_out.trying"

/* Fields are internal; do not touch them from outside audio_out.c. */
typedef struct audio_out {
    ao_ops_t ops;
    char     dev_path[64];
    char     conf_path[128];
    char     guard_path[128];
    char     conf_dir[96];

    /* settings (written by the control thread, read by the playback thread) */
    volatile int enabled;       /* 0/1 */
    volatile int volume;        /* 0..100 */
    volatile int gain;          /* 0..5 */
    volatile int source;        /* 4 or 6 */
    volatile int amp_mode;      /* enum ao_amp_mode */
    volatile int test_req;      /* beep requested */

    /* playback thread state */
    volatile int state;         /* enum ao_state */
    volatile int fd;
    int      amp_on;
    int      applied_gain, applied_source;
    int64_t  last_audible_ms;
    int64_t  open_ms;
    int      guard_active;
    int      had_error;
    int      fails;
    int      beep_pos;          /* -1 = not playing */
    int      cur_gain_q15;      /* smoothed software gain */
    int      guard_blocked;     /* marker found at open */
    unsigned long periods_written;

    /* pushed audio: jitter buffer (120 ms pre-fill, 400 ms cap), one producer
     * (talk server) and this module's playback thread as consumer */
    audio_jitter_t    jit;

    /* thread */
    volatile int thread_running;
    int          thread_started;
    pthread_t    tid;
} audio_out_t;

/* ---- engine (instance) -------------------------------------------------- */

/* Defaults: enabled, volume 50, gain 3, source 6, amp auto. */
void ao_init(audio_out_t *ao, const ao_ops_t *ops, const ao_cfg_t *cfg);
/* Reads <conf_dir>/audio_out (lines key=value: enable, volume, gain, source).
 * Absent file or bad lines keep the defaults. Returns 1 if the file was read. */
int  ao_load_config(audio_out_t *ao);
/* Writes the four persistent keys (atomic tmp + rename, mode 0600, directory
 * 0700), keeping any other line of the existing file. 0 or -1 (errno). */
int  ao_save_config(audio_out_t *ao);

/* Crash guard + device sequence. Returns 0 when the device is running,
 * -1 otherwise (state tells why; the device is closed and the amp off). The
 * guard marker is created before the first device call and removed again
 * after a failed (not crashed) sequence. */
int  ao_open(audio_out_t *ao);
/* Writes one period (preceded by the amp preload when the amp just turned
 * on). Returns 0, or -1 when the thread must stop (device error). */
int  ao_step(audio_out_t *ao);
/* Amp off, close the device, remove the guard marker. Idempotent. */
void ao_close(audio_out_t *ao);

int  ao_thread_start(audio_out_t *ao);   /* 0 or -1 */
void ao_thread_stop(audio_out_t *ao);    /* joins; does not close */

void   ao_set_volume(audio_out_t *ao, int v);   /* clamped 0..100 */
void   ao_set_gain(audio_out_t *ao, int g);     /* clamped 0..5 */
void   ao_set_source(audio_out_t *ao, int s);   /* 4 or 6, else ignored */
void   ao_set_amp_mode(audio_out_t *ao, int m);
void   ao_play_test(audio_out_t *ao);           /* queue the beep */
int    ao_test_active(const audio_out_t *ao);   /* queued or playing */
/* Non-blocking; mono s16 8 kHz. Goes through the jitter buffer: nothing is
 * played until 120 ms are queued, an underrun re-arms the pre-fill, and an
 * overrun drops the oldest audio. Returns n_samples (never short). */
size_t ao_push(audio_out_t *ao, const int16_t *pcm, size_t n_samples);
/* Samples waiting in the jitter buffer. */
size_t ao_queued(audio_out_t *ao);
const char *ao_state_name(int state);
int    ao_amp_is_on(const audio_out_t *ao);
int    ao_device_open(const audio_out_t *ao);

/* Pure helpers (tested on the host). */
int16_t ao_beep_sample(unsigned idx);           /* idx in [0, AO_BEEP_SAMPLES), else 0 */
int     ao_volume_gain_q15(int volume);         /* (v/100)^2 in Q15; 100 -> 32768 */

/* ---- 8091 keys ---------------------------------------------------------- */

/* All three work on any instance; the audio_out_ctl_* ones on the singleton.
 * get: writes the value into out, returns 0, or -1 when key is not an
 *      audio.out_* key.
 * set: returns 0 and writes the "OK ..." line into msg, or 1 with an "ERR ..."
 *      line; -1 when key is not an audio.out_* key (msg untouched).
 * list: calls emit(arg, "audio.out_x=v") for each key. */
int  ao_ctl_get(audio_out_t *ao, const char *key, char *out, size_t cap);
int  ao_ctl_set(audio_out_t *ao, const char *key, const char *val, char *msg, size_t cap);
void ao_ctl_list(audio_out_t *ao, void (*emit)(void *arg, const char *line), void *arg);

/* ---- singleton used by the camera ---------------------------------------- */

/* Called by audio.c after capture runs. Loads the config, honours the crash
 * guard, runs the device sequence and starts the playback thread. Returns 0
 * when audio out owns the playback device; -1 when disabled or failed (the
 * caller then uses its legacy silence loop, nothing has been left open). */
int  audio_out_start(void);
void audio_out_stop(void);
/* Async-signal-safe amp off (SIGTERM path). */
void audio_out_emergency_off(void);
/* Non-blocking push of mono s16 8 kHz samples, for the talk server (T3);
 * same semantics as ao_push(). */
size_t audio_out_push(const int16_t *pcm, size_t n_samples);
int  audio_out_ctl_get(const char *key, char *out, size_t cap);
int  audio_out_ctl_set(const char *key, const char *val, char *msg, size_t cap);
void audio_out_ctl_list(void (*emit)(void *arg, const char *line), void *arg);
