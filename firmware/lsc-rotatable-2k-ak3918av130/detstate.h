/* Person-presence state of the Rotatable 2K own app: a hysteresis state machine fed with one observation per NPU
 * inference, and the writer of the small key=value state file a later MQTT bridge reads. Pure logic, no vendor call, no
 * socket: host unit-tested (tests/rot2k_detect_unit.c). The only file access is a2k_det_state_write(): it writes
 * "<path>.tmp" and renames it over <path> (an atomic update for a reader). */
#ifndef AKAM2K_DETSTATE_H
#define AKAM2K_DETSTATE_H
#include <stdint.h>

#define A2K_ZONE_MAX 8          /* detection zones (zones.h) */
#define A2K_ZONE_NAME_MAX 16    /* characters of a zone name */
#define A2K_DET_CLASS_PERSON 0
#define A2K_DET_CLASS_HEAD 1
/* Fixed state file (tmpfs). A reader sees either the previous or the new complete file. */
#define A2K_DET_STATE_FILE "/tmp/akam2k_detect.state"

struct a2k_det_box { int cls, score, x1, y1, x2, y2; };

/* What one inference saw: the best person (class 0) box, if any, plus the class 1 (head) box count and best score as
 * extra information only (they never drive the state). */
struct a2k_det_obs {
    int person;            /* 1 when a valid class 0 result was present */
    struct a2k_det_box best;
    int heads;             /* valid class 1 results */
    int head_score;        /* best class 1 score, 0 when none */
    /* zones (a2k_det_obs_add_z): bit i = a person counts inside zone i; zbest[i] = its best box. With zones in use
     * `person`/`best` are the best box that counts (zone_mode any) or, for zone_mode all, only when every zone has one. */
    unsigned zmask;
    struct a2k_det_box zbest[A2K_ZONE_MAX];
};

struct a2k_det_cfg {
    int on_count;          /* consecutive positive inferences before ON (default 2) */
    int off_ms;            /* ms without a positive inference before OFF (default 3000) */
};

enum { A2K_DET_EV_NONE = 0, A2K_DET_EV_ON = 1, A2K_DET_EV_OFF = 2 };

struct a2k_det_event {
    int type;              /* A2K_DET_EV_* */
    int64_t t_ms;          /* monotonic ms of the event */
    struct a2k_det_box box;/* ON: the best box of the confirming streak; OFF: the last box seen */
    int64_t present_ms;    /* OFF: ms from the ON event to the OFF event */
    int64_t absent_ms;     /* OFF: ms since the last positive inference */
};

typedef struct {
    struct a2k_det_cfg cfg;
    int present;           /* the confirmed state */
    int streak;            /* consecutive positive inferences so far */
    struct a2k_det_box streak_best;
    struct a2k_det_box last_box;   /* most recent positive box */
    int64_t last_pos_ms;
    int64_t on_ms;
    int heads, head_score; /* of the latest inference */
    int last_event;        /* A2K_DET_EV_* of the latest event, NONE before any */
    int64_t last_event_ms;
    int64_t last_event_epoch; /* wall clock seconds, 0 when unknown (set by the caller) */
    unsigned zone_mask;    /* zones with a confirmed person (copied by the caller from a2k_zdet_t), 0 without zones */
    unsigned long inferences;
} a2k_det_t;

void a2k_det_init(a2k_det_t *d, const struct a2k_det_cfg *cfg);
/* Feeds one inference taken at monotonic time now_ms. Returns 1 and fills *ev when the state changed. */
int a2k_det_update(a2k_det_t *d, int64_t now_ms, const struct a2k_det_obs *obs, struct a2k_det_event *ev);

/* Per-zone presence: one more hysteresis machine per zone (same K and M as the global one), fed from the same inference. */
typedef struct {
    int n;                         /* zones in use, 0..A2K_ZONE_MAX */
    a2k_det_t z[A2K_ZONE_MAX];
} a2k_zdet_t;
void a2k_zdet_init(a2k_zdet_t *zd, int n, const struct a2k_det_cfg *cfg);
/* Feeds the inference to every zone machine. Returns the bit mask of the zones whose state changed; ev[i] is filled for
 * those. */
unsigned a2k_zdet_update(a2k_zdet_t *zd, int64_t now_ms, const struct a2k_det_obs *obs, struct a2k_det_event ev[A2K_ZONE_MAX]);
/* Bit mask of the zones with a confirmed person. */
unsigned a2k_zdet_mask(const a2k_zdet_t *zd);

/* Collects validated results (see detect.c) into one observation: best class 0, head extras. */
void a2k_det_obs_reset(struct a2k_det_obs *o);
void a2k_det_obs_add(struct a2k_det_obs *o, const struct a2k_det_box *b);
/* The person score a box needs: `thr`, or the lower `close_thr` for a CLOSE person (box at least A2K_DET_CLOSE_PCT of the frame height tall, in a frame `fh` tall).
 * Close persons are clipped / partial and score lower (user test 2026-11-19: 61 / 301 and 189 / 300 inferences positive at close range); a large box is not a far
 * false positive (those are small). close_thr 0 or >= thr: off. 1 = the box passes. */
#define A2K_DET_CLOSE_PCT 50
int a2k_det_person_score_ok(int score, int thr, int close_thr, int y1, int y2, int fh);
/* A class 0 box that lies in the zones of `hit` (never 0: the caller skips a box outside every zone). Class 1 goes to
 * a2k_det_obs_add as before. Call a2k_det_obs_finish once per inference when zones are in use. */
void a2k_det_obs_add_z(struct a2k_det_obs *o, const struct a2k_det_box *b, unsigned hit);
/* nz zones in use; all = zone_mode all (every zone needs a person at the same time). */
void a2k_det_obs_finish(struct a2k_det_obs *o, int nz, int all);
/* Same with the zones in force given as a bit mask (some may be suspended): zone_mode all needs a person in each of them. */
void a2k_det_obs_finish_mask(struct a2k_det_obs *o, unsigned full, int all);

/* The state file content. status: "run", "stopped" or "failed" (the detection thread's own state). */
struct a2k_det_snapshot {
    const char *status;
    int enabled, hz, threshold;
    int64_t seq;           /* incremented by the caller at every write */
    int64_t now_epoch;     /* wall clock seconds (0 when unknown) */
    int64_t uptime_ms;     /* monotonic ms */
    const a2k_det_t *det;
    /* zones, optional: zn 0 = the lines are not written */
    int zn, zall;
    const char *zname[A2K_ZONE_MAX];
    unsigned zmask;
};
/* Formats the snapshot as key=value lines into buf; returns the length, or -1 when it does not fit. */
int a2k_det_state_format(char *buf, unsigned cap, const struct a2k_det_snapshot *s);
/* Formats, writes <path>.tmp, renames it over <path>. 0 or -1 (the temp file is removed on failure). */
int a2k_det_state_write(const char *path, const struct a2k_det_snapshot *s);

/* The frame age reference of the detection thread: the raw age (now - frame stamp) minus the SMALLEST raw age seen. The all-time minimum (window 0, the default) keeps a one-off
 * small value for ever, so a drifting clock reads as a growing age; with the window on (akam2k.conf detect_age_window=1, experiment E3) the minimum is over the last 60 s (one bucket
 * per second). a2k_agewin_age() returns the age and folds the sample in. */
#define A2K_AGEWIN_S 60
struct a2k_agewin {
    long long min_raw[A2K_AGEWIN_S];
    long long sec[A2K_AGEWIN_S]; /* the second a bucket holds, -1 = empty */
    long long all_min;
    int have, window;
};
void a2k_agewin_init(struct a2k_agewin *w, int window);
long long a2k_agewin_age(struct a2k_agewin *w, long long now_ms, long long raw);

/* The model must sit under /appfs/resource/model/ and contain no "..": 1 when acceptable. */
int a2k_model_path_ok(const char *path);

#endif
