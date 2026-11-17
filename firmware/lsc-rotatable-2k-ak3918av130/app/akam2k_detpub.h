/* Publication of the person boxes of the detection thread to outside services (HTTP event `detections`, the state key detect.boxes,
 * the MQTT topic BASE/detections): the pure part. Decides WHEN a list is published (rate limit, change detection, one empty list when
 * the last person leaves) and formats the JSON. No socket, no file, no vendor call: host unit-tested (tests/rot2k_detpub_unit.c). RAM
 * only. The fan-out to the sinks (a2k_api_event, a2k_mqtt_detections) is done by akam2k_detect.c with the text returned here.
 *
 * JSON (one object): {"ts_ms":<wall clock ms>,"w":320,"h":192,"pan":P,"tilt":T,"boxes":[{"x":..,"y":..,"w":..,"h":..,"score":..,"id":..,
 * "target":bool,"ignored":bool}...]}. x y w h are per-mille of the frame (0..1000); pan / tilt (motor steps) only when the position is known;
 * score 0..1000; id: a number kept while the person stays in view; target: the person the tracker follows; ignored: the person stands in
 * an ignore zone (published, not dropped). */
#ifndef AKAM2K_DETPUB_H
#define AKAM2K_DETPUB_H
#include <stddef.h>
#include <stdint.h>

#define A2K_DP_MAX 8        /* boxes per list */
#define A2K_DP_JSON 832     /* buffer for a list text incl. NUL (= STREG_VAL_MAX); a full list is under 800 (= A2K_EVT_DDATA, ids stay under 1000) */
#define A2K_DP_HZ_DEFAULT 2
#define A2K_DP_HZ_MAX 10
#define A2K_DP_MIN_MS 200   /* never more than 5 lists per second, whatever detect.publish_hz says */
#define A2K_DP_KEEPALIVE_MS 1000 /* an unchanged list is repeated this often while a person is present */
#define A2K_DP_MOVE_PM 15   /* a box edge / size must move by more than this (per-mille) to count as a change */

/* One person box in the detector frame: pixel indexes, x2 / y2 inclusive, as struct a2k_det_box. */
struct a2k_dp_in {
    int x1, y1, x2, y2, score;
    int ignored; /* inside an ignore zone */
    int target;  /* followed by the tracker */
};

typedef struct {
    int pm[A2K_DP_MAX][4]; /* the last published list: x y w h per-mille */
    int id[A2K_DP_MAX], ign[A2K_DP_MAX], tgt[A2K_DP_MAX];
    int n;                 /* boxes in the last published list */
    int64_t last_ms;       /* time of the last publication, -1 = none since the start / the last reset */
    int next_id;
} a2k_dp_t;

void a2k_dp_init(a2k_dp_t *p);
/* One inference at monotonic now_ms (ts_ms: wall clock ms for the text). hz: detect.publish_hz (0 = off). blocked: privacy mode or parked
 * (nothing is published and the state is forgotten). b[n]: every person box of the inference. pan_known: pan / tilt valid.
 * Returns the length of the JSON written to json (cap >= A2K_DP_JSON) when a list is due, 0 when nothing is. */
int a2k_dp_update(a2k_dp_t *p, int64_t now_ms, int64_t ts_ms, int hz, int blocked, const struct a2k_dp_in *b, int n, int pan_known, int pan,
                  int tilt, char *json, size_t cap);

/* Settings and the latest list (any thread). */
int a2k_dp_hz(void);
void a2k_dp_set_hz(int hz);                /* clamped to 0..A2K_DP_HZ_MAX */
void a2k_dp_set_latest(const char *json);  /* the state key detect.boxes */
void a2k_dp_clear_latest(void);            /* an empty list (off, privacy, parked, detection ended) */
int a2k_dp_latest(char *out, size_t cap);  /* text length, -1 when it does not fit */

#endif
