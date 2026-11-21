/* Person-tracking policy of the Rotatable 2K own app: PURE logic (no I/O, no clock, no vendor call), host-tested
 * (tests/rot2k_track_unit.c). Fed once per detection tick (~5 Hz) with the person boxes, it returns at most one pan/tilt
 * nudge in motor steps, a "return home" request, or nothing. Model-agnostic: boxes are plain pixel corners like
 * struct a2k_det_box (x1,y1,x2,y2 in the detector frame, 320x192 on the 2K), the frame size is passed in.
 * Sign convention: dpan > 0 = target is right of centre, dtilt > 0 = target is below centre (image axes); the caller maps
 * them to the motor direction (cfg invert_pan/invert_tilt flip them here). */
#ifndef TRACK_LOGIC_H
#define TRACK_LOGIC_H
#include <stddef.h>
#include <stdint.h>

#define TL_MAX_BOXES 8       /* person boxes the policy looks at */
#define TL_MAX_HEADS 4       /* head boxes accepted next to them (the continuous policy uses the one associated with the target: rule 6) */
#define TL_MAX_ALL (TL_MAX_BOXES + TL_MAX_HEADS)
#define TL_MAX_STEPS 8
/* The lost-target chain (track_lost_policy / track.lost_policy): an ordered list of recovery steps run when the person is lost; a detection at any step ends it. */
enum { TLS_HOLD = 1, TLS_BACKSWEEP, TLS_EXIT, TLS_LOCAL, TLS_ROUTE, TLS_HOME, TLS_STAY };
struct tl_step {
    int kind;                /* TLS_* */
    int a, b, c;             /* hold: a = seconds; local: a = width (steps), b = speed divisor, c = seconds; route: b = seconds */
    char name[17];           /* route: the stored patrol route */
};

/* cls: detector class, 0 = person (also the value of a box built without it), 1 = head (docs/porting/lsc-rotatable-2k/ai-own-app.md section 9); score 0..1000, 0 = unknown. */
enum { TL_CLS_PERSON = 0, TL_CLS_HEAD = 1 };
struct tl_box { int x1, y1, x2, y2; int cls, score; };

struct tl_cfg {
    int deadband_pct;        /* pan: no move while the box centre is within +-this % of the frame width from the centre (the "central band", 8) */
    int tband_pct;           /* tilt: same, % of the frame height (10); the head band is half of it (+-5 %) */
    int gain_pan;            /* steps per full frame width of offset (1000: ~4000 steps/350 deg, ~90 deg HFOV; see tracking.md) */
    int gain_tilt;           /* steps per full frame height of offset (700; nominal geometry 600 ~ 54 deg VFOV, raised after the hardware test of 2026-11-17) */
    int gain_pct;            /* damping: % of the measured offset corrected by one nudge (85: < 100 so a stale measure cannot overshoot) */
    int max_step;            /* clamp of one nudge, per axis (700) */
    int min_interval_ms;     /* rate limit between two nudges (300) */
    int confirm_ticks;       /* consecutive ticks with a target before the first move of a NEW target (2); a followed target needs 1 */
    int settle_ms;           /* >0: frames captured less than this after the motion ended (or before the last nudge was sent) are dropped (220);
                              * 0 = legacy: only the ticks while the motor runs are dropped */
    int fast_pct;            /* offset beyond this % of the frame uses speed_fast (25) */
    int speed_fast;          /* speed divisor of a large correction (2 = ~500 steps/s), 0 = the default speed */
    int lead_pct;            /* velocity lead: % of the predicted target travel added to a nudge, 0 = off (70) */
    int lead_max;            /* cap of the lead, steps (500) */
    int tilt_enable;         /* 1 = tilt tracking; default 0 (opt-in: track_tilt=1) until the tilt direction is verified on the camera */
    int lost_grace_s;        /* no person for this long = lost; nothing moves before (3) */
    int return_home;         /* 1 = go home after the loss (track_return=home), 0 = stay where the person was last seen (default 1; a position set by hand after the last tracking nudge cancels the pending return) */
    int return_delay_s;      /* home mode: seconds after the end of the grace before going home, 0 = right after the grace (30) */
    int head_pct;            /* modern mode, tilt: the vertical reference is the TOP of the box (the head), kept this % of the frame height below the frame top (15);
                              * a box whose top touches the frame edge = head out = tilt up; a short box (far person) is centred on its upper third; 0 = centre of the box */
    int tilt_return_s;       /* stay mode: this many seconds after the lost-person grace the TILT goes back to the home tilt (pan stays); 0 = never (default 0: off, the home return covers both axes) */
    int sticky_gate_pct;     /* the previous target is kept if a box lies within this % of the frame width of it (world position) (35) */
    int invert_pan, invert_tilt;
    /* --- targeting policy and continuous motion (all 0 / TL_MOTION_STEPS in tl_cfg_dev = the policy before them) --- */
    int zone_x_pct;          /* comfort zone, % of the frame WIDTH (total, 40 = +-20 %): no motion while the reference point stays inside and is not heading out; 0 = the dev policy (central band, upper-third rule) */
    int zone_y_pct;          /* body rule (whole person visible): vertical comfort band, % of the frame height (30 = +-15 %) around the frame centre */
    int zone_head_pct;       /* head rule (close / cut person): band around the head row, % of the height (20 = +-10 %) */
    int zone_stop_pct;       /* continuous: the motion stops when the target is within this % of the zone half width (35) */
    int meas_px;             /* Kalman measurement noise of the box reference, px (std, 4: detector jitter; inflated for cut / partial / wide boxes) */
    int accel_mms2;          /* Kalman process noise: std of the person's lateral / depth acceleration, mm/s^2 (4000: walking, feints, reversals) */
    int steps_rad;           /* motor steps per radian (654 = 4000 steps / 350 deg, measured): angle <-> steps */
    int person_h_mm;         /* assumed height of a whole person for the distance estimate (1700, +-20 % error) */
    int shoulder_mm;         /* assumed width of a person seen from the front (450), the fallback distance cue when the box is cut */
    int full_pct;            /* a box that is not cut by the frame and shorter than this % of the height is a whole body: its centre is the reference (85) */
    int predict_ms;          /* motion starts when the predicted reference (velocity) leaves the zone within this long (500) */
    int motion;              /* TL_MOTION_STEPS | TL_MOTION_CONT (tl_update dispatches) */
    int lease_ms;            /* continuous: the lease the tracker asks the motor for, refreshed by every tick (450) */
    int rate_k;              /* continuous: motor steps/s = rate_k / divisor (1100, measured 2026-11-17 at divisor 3: ~370) */
    int gap_predict_ms;      /* continuous: constant-velocity prediction through a detection gap, ms (1500) */
    int lost_n;              /* the lost-target chain: -1 = the legacy return policy (track_return / grace / delay), 0 = none (do nothing), else the number of steps of lost[] (continuous mode) */
    struct tl_step lost[TL_MAX_STEPS];
    int tilt_back_s;         /* continuous: this long after the person was lost with the tilt well above home, the tilt goes back to the home tilt, whatever the return policy does for the pan (5; 0 = off) */
    int tilt_back_min;       /* ... when it is more than this many steps above home (250) */
    int close_scan;          /* continuous: a close person lost with no direction evidence is SEARCHED for, up and down around the loss pose (VSCAN), then the lost policy takes over (1; 0 = the old WAIT then WIDEN back to home) */
    int close_scan_up;       /* ... the scan goes up to this many steps above the loss tilt (250; 0..600; clipped to the soft range) */
    int close_scan_down;     /* ... and down to this many steps below it (300; 0..600) */
    int close_bridge_ms;       /* a frame-filling person missed by the detector for less than this is no loss: nothing moves, no lost step (2500; 0..10000; 0 = off; not a config key) */
    int close_scan_cooldown_s; /* ... at most one VSCAN per this many seconds, and one per loss episode (30; 0..600; 0 = only the episode rule) */
    int head_row_pct;        /* rule 6 (head): the row the head centre is kept at, % of the frame height (28) */
    int head_band_pct;       /* rule 6: vertical band around that row, +-% of the height (12) */
    int head_meas_px;        /* rule 6: measurement noise of the head centre, px (3) */
    int head_mm;             /* rule 6: assumed head height for the distance cue, mm (240, +-20 %: the constant to calibrate in experiment E4) */
    int rev_guard_ms;        /* continuous, per axis: the opposite direction is refused this long after the last command in the old one (700; 0 = off; not a config key) */
    int rev_n;               /* ... after this many reversals inside rev_window_ms (4; 1..TL_REV_MAX) ... */
    int rev_window_ms;       /* ... (4000; 1000..60000) ... */
    int rev_hold_ms;         /* ... the next one is refused and the axis is held still this long (1500; 0..60000) */
    int lag_ms;              /* continuous: video latency subtracted from the capture time when the pose at capture is looked up (50; -150..300, a negative value is allowed: the calibration of tools/track_lagfit may find the stamp later than the exposure) */
};

enum { TL_MOTION_STEPS = 0, TL_MOTION_CONT = 1 };

struct tl_ptz {
    int moving;              /* 1 while the motor runs: never nudge */
    int pan, tilt;           /* current position, motor steps */
    int pan_min, pan_max, tilt_min, tilt_max; /* soft range, inclusive */
    int64_t cap_ms;          /* capture time of the frame (same clock as now_ms), 0 = unknown (= now) */
    int64_t motion_end_ms;   /* when the last motion ended (same clock), 0 = unknown */
    int tilt_home;           /* the home tilt, motor steps, 0 = unknown (no tilt return) */
    int has_cap;             /* continuous: cap_pan / cap_tilt = the pose at the capture time of the frame (driver counters), so a frame taken while moving is usable */
    int cap_pan, cap_tilt;
    int cap_rate_ok;         /* cap_rate_*: the camera speed at the capture instant (steps/s, from the driver samples around it) is known */
    int cap_rate_pan, cap_rate_tilt;
    int over_budget;         /* the travel budget is used up: no local sweep in the lost chain */
    int route_busy;          /* a patrol route started by the lost chain is running (the tracker reports it) */
    int cont;                /* continuous: a motor session is running (the commanded velocity is in tl_t) */
};

enum tl_kind { TL_NONE = 0, TL_NUDGE = 1, TL_HOME = 2, TL_VEL = 3, TL_STOP = 4 };

struct tl_out {
    enum tl_kind kind;
    int dpan, dtilt;         /* TL_NUDGE only, already clipped to the soft range, never both 0 */
    int speed;               /* TL_NUDGE: speed divisor (2..20), 0 = the default speed */
    /* TL_VEL: one ongoing directional move, refreshed every tick (the lease). Direction -1 / 0 / +1 per axis and a speed divisor 2..20 per axis:
     * pan in MOTOR sign (+ = right), tilt in IMAGE sign (+ = below, like dtilt). Both 0 = nothing to move (the caller stops). TL_STOP: stop now. */
    int pan_dir, pan_div, tilt_dir, tilt_div;
    /* the lost chain: 1 = start the patrol route route_name for route_secs, 2 = stop the route the chain started (alongside any kind above) */
    int route_op;
    char route_name[17];
    int route_secs;
};

/* What the last tl_update saw and decided, for the opt-in debug log (track_debug=1): pure data, no I/O. */
struct tl_dbg {
    int wide;                /* the box was too wide for a pan decision */
    int has_box;             /* a box was picked */
    struct tl_box box;
    int rule;                /* 0 = centre of the box (head_pct 0 / legacy), 1 = head clipped at the top, 2 = near (head anchored), 3 = far (upper third), 4 = whole body (centre of the box), 5 = ambiguous (cut at top AND bottom and wider than 80 % of the frame: tilt held),
                              * 6 = head (a head box associated with the target while the person box is cut / wide / absent: the head centre is the reference),
                              * 7 = low crop (top uncut, bottom cut, shorter than 40 %: the top is the reference, no near doubling, no two-frame start; never rule 4) */
    int ty;                  /* rule 1/2: the row the box top is aimed at; rule 3: the row the upper third is aimed at (px) */
    int d2y;                 /* vertical offset in half pixels (target - reference, image sign: > 0 = below) */
    int dp, dt;              /* wanted pan / tilt nudge before the range clip (image sign for tilt) */
    int exit;                /* the exit direction used by the search (TL_EXIT_*) */
    const char *why;         /* "nudge" or the reason nothing was sent (static string) */
    /* continuous mode (the decision log) */
    int ex, ey;              /* predicted reference minus the camera position, motor steps (pan, tilt) */
    int hx, hy;              /* comfort-zone half width / height, steps */
    int mvx, mvy;            /* the axes are being moved (start / stop hysteresis) */
    int vdx, vdy;            /* desired velocity, steps/s, motor sign */
    int kf;                  /* the 3D filter: 0 updated, 1 measurement held by the innovation gate, 2 accepted after two rejections (re-initialised) */
    int hold;                /* the frame was held as an outlier (a sudden jump of the box) */
    const char *dcue;        /* the distance cue of this fresh box: h (height), w (width), head (head height), carried (the last estimate), none; 0 = no fresh box */
    int innx, inny;    /* normalised innovation of the world position before the update, in 1/100 sigma (x: pan filter, y: tilt filter; 0 for a new target / rule change) */
    int rsx, rsy;            /* the measurement noise budget used (1 sigma, motor steps) */
    int head;                /* a head box was associated with the target this tick (1 = used as the reference, 2 = associated but the whole-body rule kept, 3 = none now but the head was the reference within 1.5 s: the cut box top is not used) */
    int upl;                 /* rule 1 was chosen from the ambiguous box because the head had been moving up (the real head-out of an approaching person) */
};

enum { TL_EXIT_NONE = 0, TL_EXIT_DOWN, TL_EXIT_UP, TL_EXIT_LEFT, TL_EXIT_RIGHT };
#define TL_NS 3              /* position samples kept for the velocity */
/* One-dimensional constant-velocity Kalman filter in an angular axis: a = angle (motor steps), v = LATERAL velocity of the person (mm/s); the angular
 * rate is v * steps_rad / distance, so a person walking at a given speed turns the camera consistently near and far. Single precision, + - * / only. */
struct tl_kf1 { float a, v, paa, pav, pvv; };
struct tl_kfd { float d, v, pdd, pdv, pvv; }; /* distance (mm) and its rate */

#define TL_REV_MAX 4
struct tl_rev {              /* the reversal throttle of one axis */
    int dir, nrev;           /* the direction of the last command (motor sign), the reversals inside the window */
    int64_t t_cmd, hold_until, rev_t[TL_REV_MAX];
};

typedef struct {
    struct tl_cfg cfg;
    int has_target;          /* a target was seen in the previous tick */
    struct tl_box target;
    long target_w;           /* its world position along pan (motor steps) */
    int streak;              /* consecutive ticks with a target */
    int64_t last_nudge_ms;
    int last_sign;           /* sign of the last pan nudge */
    int nudged;              /* a nudge was sent since the last home / init */
    int64_t last_seen_ms;
    int home_pending;        /* home request not yet emitted for the current loss */
    int home_retry_n;        /* a refused home (tl_home_refused) was asked again this many times */
    int64_t home_retry_at;   /* ... not before this time */
    int ns;                  /* velocity samples: capture time + world pan position */
    int last_tsign;          /* sign of the last tilt nudge (image sign) */
    int64_t last_tnudge_ms;
    int64_t thold_until;     /* the tilt reversal cap holds the tilt still until then */
    int64_t trev_ms[2];
    int ntrev;
    int64_t hold_until;      /* the reversal cap holds the tracker still until then */
    int64_t rev_ms[2];       /* times of the last direction reversals */
    int nrev;
    int64_t st[TL_NS];
    long sw[TL_NS];
    int64_t prev_area, prev_ms; /* box area of the previous tick (growth = approaching) */
    struct tl_box hist[4];   /* the last boxes seen (stale frames too): the exit direction of a person who leaves */
    int hn;
    int exit_set, exit_dir;  /* the exit direction of the current loss (TL_EXIT_*), computed once */
    int search_n;            /* exit-search nudges sent in this loss (at most 2) */
    int tilt_ref_set, tilt_ref; /* the tilt (motor steps) at the start of the tracking episode: the cumulative tilt UP is capped relative to it */
    int clip_n;              /* consecutive fresh frames with a confirmed head clip (a tall box cut by the top edge) */
    int tilt_pending;        /* the tilt was moved by tracking and has not been brought home yet */
    int settle_wait;         /* a tracking nudge was sent and the pose it leads to is not recorded yet */
    int has_settled, settled_pan, settled_tilt; /* the pose after the last tracking nudge: another pose later = a manual move */
    struct tl_dbg dbg;       /* the last tick */
    /* targeting policy / continuous motion */
    int sm_valid, sm_rule;   /* the filtered reference is valid (world, motor steps), and the rule it belongs to */
    long sm_wx, sm_wy;       /* filtered world position of the reference: pan axis, tilt axis (motor sign) */
    struct tl_kf1 kx, ky;    /* the filters of the pan / tilt axes */
    struct tl_kfd kd;        /* the distance filter */
    int kd_valid, kf_rej;    /* a distance estimate exists; consecutive frames rejected by the innovation gate */
    int64_t kf_t;            /* capture time of the last filter update */
    int kox, koy;            /* Kalman angular rates (steps/s, before the robust gate) */
    struct tl_out lastv;     /* the last velocity command (refreshed on stale frames) */
    int dist_mm;             /* the filtered distance, mm (0 = unknown), for the debug log */
    int kf_note;             /* last kf_measure result (tl_dbg.kf) */
    int64_t kd_t;            /* when the distance estimate was last fed by a box */
    int tback_on, tback_done; /* the tilt is on its way back to home / it was done for this loss */
    int64_t tback_t0, tback_int_ms; /* ... the start; the last time a box interrupted it (no restart for 10 s) */
    int tback_sgn;           /* the side of home (+1 above, -1 below) the tilt-back started from: the direction is latched */
    int cam_rx, cam_ry, cmv, cmvy, tstart_n; /* the camera rate at the capture of the last fresh frame (steps/s); bit history (3 frames) of 'the camera moved at capture' (any axis / the tilt); tilt start confirmations when close */
    int lost_i, ls_init, ls_dir, lost_route_on, ls_route_seen, lost_done; /* the lost chain: current step (-1 = not lost), initialised, oscillation direction, the route it started runs, it was seen busy, the chain ended */
    int64_t lost_t0;
    long ls_center;
    const char *lstep;       /* the current step name ("" = not lost), static string */
    char lroute[17];         /* the route of a route step */
    int64_t tnow, up_ms;     /* the tick time; when the head was last seen rising (uncut tops, a real approach: see head_rule) */
    int amb_latch;           /* an ambiguous climb is on: a cut top keeps being read as a head-out (no clip confirmation) until an uncut-top box is seen */
    int amb_up;              /* the last ambiguous box was read as a real head-out (rule 1 from rule 5): the sky-climb cap applies, no clip confirmation */
    int dwc[4];              /* per dw sample: the box top was cut by the frame */
    int vn;                  /* raw world samples for the robust velocity (fresh detections of the followed target) */
    int64_t vt[6];
    long vwx[6], vwy[6];
    int pace_n, pace_dir;    /* pacing detector: started, direction of the current leg */
    long pace_ext;           /* ... the extreme of the current leg, and its time */
    int64_t pace_et, pace_last;
    long pace_w[3];          /* ... the last 3 turning points: positions, */
    int64_t pace_t[3];       /* ... and times */
    int sw_st, sw_dir, sw_p0, sw_stop; /* the full-range search (sweep_step): state, direction, pan at its start, pan of the last stop */
    int64_t sw_until;
    int vx, vy;              /* robust velocity, motor steps/s (0 = not established) */
    int near_mode;           /* hysteresis of the whole-body / head rule */
    int mv_x, mv_y;          /* continuous: the axis is being moved (the start / stop hysteresis) */
    int cmd_px, cmd_py;      /* continuous: last commanded velocity, motor steps/s, signed (pan, tilt) */
    int vact;                /* a velocity command is on (a TL_STOP is due when nothing is to be moved) */
    int64_t cmd_ms;          /* when the last velocity command was issued */
    int64_t search_until;    /* continuous exit search: moving in the exit direction until then */
    int64_t lastcap_ms;      /* capture time of the last fresh detection */
    int box_h, box_w;        /* last box size (identity cue) */
    int nstart;              /* continuous: number of starts (motion episodes), for the sims */
    int out_n;               /* a sudden large jump of the box vs the followed track is held as an outlier until a second consistent fresh frame confirms it */
    struct tl_box out_box;
    int out_down;            /* the held jump is a near, downward one with the bottom cut: the tilt is frozen until 2 further frames agree (out_topw) */
    long out_topw;
    int out_dn;              /* consecutive held near downward jumps */
    int dw_n;                /* world-vertical position of the box TOP (down positive) of the last fresh boxes: the DOWN exit needs a real downward trend */
    long dw[4];
    float dws2[4];           /* per dw sample: the variance (steps^2) of the box top at capture: the jitter and the camera rate x the residual capture-time lag */
    int64_t head_ms;         /* when rule 6 was last used (the head hold window: a no-head frame with a cut box does not switch the vertical reference back to the box top) */
    int hcw_valid;           /* the world vertical position of the last associated head centre (head gate when there is no person box) */
    long hcw;
    int tilt_unclip_set, tilt_unclip; /* the tilt at the last frame whose top was uncut and between 5 % and 35 % of the height: the origin of the close-range up budget */
    int amb_tilt_set, amb_tilt0; /* the tilt where the up-trend latch of an ambiguous (rule 5) box started: the origin of its up budget */
    int exit_tilt0;          /* the tilt at the start of the current loss (the exit-up search budget starts there) */
    int last_near;           /* the last fresh box was a near one (tall, or the distance below NEAR_MM): the tilt-back after its loss starts earlier */
    int pre_valid, pre_pan, pre_tilt; /* the pose before the last large move (> 1/3 frame): where a person lost after an overshoot must be */
    int64_t last_move_ms;    /* when the last (large or not) tracking motion was commanded */
    int sweep_n;             /* steps mode: sweep-back nudges in this loss (at most 2) */
    int sweeping, sweep_sx, sweep_sy; /* continuous: sweeping back toward the pre-move pose, the directions (motor sign) it is allowed to move */
    int64_t fresh_loss_ms;   /* first fresh (non-stale) frame without a person of the current loss, 0 = none: the exit search / sweep only start LOSS_FRESH_MS after it */
    int wprev;               /* ptopw / pcenw: world top / centre of the last accepted box */
    long ptopw, pcenw;
    int ep_active;           /* continuous: a motion episode is on (its start pose is the pre pose) */
    int last_rule, last_nl;  /* the rule of the last accepted box; it was a near one (bottom cut and >= 40 % tall, a low crop, or closer than 1200 mm): the close-loss machine runs when it is lost */
    int nl_phase, nl_ev;     /* the close-loss machine (NL_* in track_logic.c) and its loss evidence (E_*) */
    int nl_n, nl_w0ok;       /* search moves started; the widen phase is initialised */
    int64_t nl_t0, nl_until, nl_w0; /* the first fresh no-box frame; the end of the running search move (negative: the stop time of the last one); the start of the widen phase */
    int vs_bsgn;             /* VS_BACK: the side of the loss tilt (+1 above / -1 below, 0 = not started) it started from */
    int vs_st, vs_want_up, vs_want_dn, vs_hu, vs_hd; /* the close-scan stage (VS_*), which sides it covers, its two limits as heights (up * tilt) */
    int64_t vs_t0, vs_tdw;   /* the start of the scan; the start of the running dwell */
    int64_t down_ms;         /* when the last tilt command of the follow went DOWN (0 = the last one went up / none) */
    int nl_down;             /* the target was last sinking (a person sitting / crouching): the close scan skips its UP side */
    int nl_pan0, nl_tilt0;   /* the pose at the loss: the search budget (400 steps) is counted from it */
    int le_on, le_moved;     /* the loss episode of a near target: it starts with the close-loss machine and ends when the target is held for LE_CONFIRM_MS or the head goes home; the machine MOVES the head (directed search or VSCAN) once per episode */
    int64_t ds_last_ms;      /* the start of the last directed close-loss search (0 = never): under close_scan_cooldown_s like the scan */
    int64_t le_hold0, le_last, vs_last_ms; /* the start of the current continuous hold of the target, the last accepted box of the episode, the start of the last VSCAN (0 = never) */
    struct tl_rev rev[2];    /* the reversal throttle: pan, tilt */
    int ff_set, ff_tilt0;    /* a frame-filling top-clipped box: set, and the tilt where it became frame-filling (the climb is capped from it) */
    int64_t ff_last;         /* the last frame-filling box */
    int ff_y2ref;            /* the largest box bottom of the close sighting (a person who fills the frame: the reference of a rise) */
    int ff_y2h[3], ff_y2n;   /* the bottoms of the last 3 consecutive accepted top-clipped boxes (a flicker never fills it with low ones) */
    int64_t ff_y2t;          /* time of the last of them */
    int64_t ff_low0, ff_low_last; /* the start / the last of a run of boxes shorter than 77 % (the sighting ends after 2 s of them) */
    int kf_force;            /* the box is the same close person as the last one (close_same): the lateral filter takes it, no hold as an outlier */
    int last_ff;             /* the last accepted person box was frame-filling: a short detector dropout is bridged (no loss, nothing moves) */
    int vs_ev, vs_ev_tilt;   /* a VSCAN event for the caller to log (VSEV_*) and the tilt it happened at; the caller clears vs_ev */
} tl_t;

enum { VSEV_NONE = 0, VSEV_UP, VSEV_DOWN, VSEV_FOUND, VSEV_NOTHING }; /* tl_t.vs_ev: a VSCAN started going up / down (vs_ev_tilt = the loss tilt), or ended with a box seen / without */

/* The lost-target chain text: "none", or steps joined by ',': hold:N (1..3600 s), backsweep, exit, local:W:D:N (width 40..1500 steps, divisor 2..20, 1..600 s), route:<name>[:N] (N 1..3600 s, default 60), home, stay (last only), up to 8.
 * All or nothing: 0 and the chain stored, -1 and *c untouched. The default text "backsweep,exit,hold:30,home" selects the legacy return policy (lost_n = -1). */
#define TL_LOST_DEFAULT "backsweep,exit,hold:30,home"
int tl_lost_parse(const char *s, struct tl_cfg *c);
void tl_lost_format(const struct tl_cfg *c, char *out, size_t cap);
void tl_cfg_defaults(struct tl_cfg *c);
/* The measured geometry of the Rotatable 2K (docs/porting/lsc-rotatable-2k/ptz-findings.md, 2026-11-18: about 2 motor steps per px of the 320 px
 * detector frame, both axes; the nominal 1000 steps per frame width = 3.1 steps/px assumed in tl_cfg_defaults was 1.5x too high). ONE constant, x10 so it stays
 * integer; the gains (steps per frame width / height) and steps_rad (steps per radian, scaled with the gain: same HFOV) follow from it. */
#define TL_2K_STEPS_PER_PX_X10 20
#define TL_2K_FRAME_W 320
#define TL_2K_FRAME_H 192
#define TL_2K_GAIN_PAN 640  /* = TL_2K_FRAME_W * TL_2K_STEPS_PER_PX_X10 / 10 (literal: the settings table stringifies it; checked in track_logic.c) */
#define TL_2K_GAIN_TILT 384 /* = TL_2K_FRAME_H * TL_2K_STEPS_PER_PX_X10 / 10 */
void tl_cfg_geometry_2k(struct tl_cfg *c); /* call after tl_cfg_defaults / tl_cfg_legacy / tl_cfg_dev on the 2K; the API / CLI gains override it afterwards */
/* The dev policy of before the targeting work and the continuous mode (central band, upper-third rule, steps): the "before" of the simulations and of the old unit cases. */
void tl_cfg_dev(struct tl_cfg *c);
/* The slow policy of firmware 0.1.0 (12 % band, 1.5 s rate limit, fixed 300 step nudges, no lead, home after 10 s): the "before" of the simulation. */
void tl_cfg_legacy(struct tl_cfg *c);
/* 0 when valid, -1 otherwise. */
int tl_cfg_validate(const struct tl_cfg *c);
/* The caller's TL_HOME was refused by the motor layer (a manual hold, a drift window, busy): the policy asks for it again after 1.5 s, at most 5 times per loss. */
void tl_home_refused(tl_t *t, int64_t now_ms);
/* -1 on an invalid cfg (state zeroed, tl_update then does nothing), 0 otherwise. */
int tl_init(tl_t *t, const struct tl_cfg *c);
/* One tick. n boxes (invalid/empty boxes are ignored; head boxes (cls 1) are used by the continuous policy only (rule 6), the steps policy drops them; more than TL_MAX_BOXES persons / TL_MAX_HEADS heads are truncated). Fills *out. */
void tl_update(tl_t *t, int64_t now_ms, const struct tl_box *boxes, int n, int fw, int fh, const struct tl_ptz *ptz,
               struct tl_out *out);
#endif
