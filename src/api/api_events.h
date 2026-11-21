/* Event ring of the local control API (specs/control-api.md 4.2, GET /events): the push channel for Home Assistant when there
 * is no MQTT broker. Producers (button, detection, day/night, PTZ, settings worker) call a2k_api_event* from ANY thread: a
 * short mutex-protected copy into a ring of A2K_EVT_RING entries and one byte written to the wake pipe of the HTTP thread; they
 * never block on the network and never allocate. The consumer reads with a cursor: events with seq > since. No other module
 * dependency, so a unit test that links a producer only needs this one file. */
#ifndef AKAM_API_EVENTS_H
#define AKAM_API_EVENTS_H
#include <stddef.h>
#include <stdint.h>

#define A2K_EVT_RING 32
#define A2K_EVT_TYPE 12
#define A2K_EVT_DATA 144
/* "detections" events (detpub.h) have a ring of their own: a few larger entries, so they never evict the other types. */
#define A2K_EVT_DETECTIONS "detections"
#define A2K_EVT_DRING 4
#define A2K_EVT_DDATA 800
/* "patrol" events (PTZ patrol, specs/ptz-patrol.md) also have a ring of their own, so a patrol can never evict a button / person / night event. */
#define A2K_EVT_PATROL "patrol"
#define A2K_EVT_PRING 8

/* type: [a-z_]+ (button, person, night, ptz, setting); data: a JSON object text, or NULL for {}. Longer text is dropped (the limit is A2K_EVT_DDATA for "detections", A2K_EVT_DATA otherwise). */
void a2k_api_event(const char *type, const char *data);
void a2k_api_event_button(void);               /* a short press with the default bindings (the button itself posts {"gesture","actions","result"} through a2k_api_event) */
void a2k_api_event_person(int present, int score); /* only a change of presence makes an event */

/* The write end of a non-blocking pipe the HTTP thread polls (-1 = none). Set before the producers run. */
void a2k_apievt_set_wake(int fd);
/* The newest sequence number (0 before any event). */
uint32_t a2k_apievt_seq(void);
/* {"seq":N,"missed":bool,"events":[{"seq","up_s","type","data"}...]} for the events after `since` (oldest first). missed is true when
 * events older than the ring were lost. Returns the length, or -1 when cap is too small. *count = events included. */
int a2k_apievt_json(uint32_t since, char *out, size_t cap, int *count);
/* The last presence reported by a2k_api_event_person (0 before any). */
int a2k_apievt_person(void);
/* Test hook: forget everything. */
void a2k_apievt_reset(void);

#endif
