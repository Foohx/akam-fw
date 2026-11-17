/* What the model binding gives the generic control API (api_core.c): the registry, the executor of settings writes and actions, the
 * list of actions, and the extra routes of the model. The job and action types are here (they were in akam2k_settings.h) so that
 * the 2K and the 1080p share them. Everything runs on the ONE HTTP thread of api_httpd.c; a backend may run the job inline
 * (the 1080p) or hand it to its own worker (the 2K) and wait at most its own bound. */
#ifndef AKAM_API_BACKEND_H
#define AKAM_API_BACKEND_H
#include <stddef.h>
#include <stdint.h>

#include "json_min.h"
#include "settings_reg.h"

/* ---- jobs ---- */
enum { A2K_JOB_PATCH, A2K_JOB_ACTION, A2K_JOB_APPLY /* internal: a2k_settings_apply of the 2K */ };
typedef struct {
    int kind;            /* A2K_JOB_* */
    int scope;           /* AKEY_* level of the caller */
    st_src_t src;
    jflat_t in;          /* the parsed body (PATCH: setting pairs; ACTION: the arguments) */
    const char *name;    /* ACTION: the action name (a pointer into the action table) */
    /* results */
    int http;            /* status to send */
    st_result_t res[JFLAT_MAX];
    const char *err;     /* ACTION: machine error code, NULL when ok */
    char detail[64];
} a2k_job_t;

/* Actions: name, minimum scope, the argument list as text for the schema. */
typedef struct {
    const char *name;
    int min_scope;
    const char *args;
} a2k_action_info_t;

typedef struct {
    const streg_t *(*reg)(void);
    /* Runs the job and waits at most the backend's own bound. 0 = done (results in *j), -1 = timeout (the job still runs: the caller
     * answers 504 and must not touch *j until idle() is 1), -2 = a job is already running (503). */
    int (*exec)(a2k_job_t *j);
    int (*idle)(void);
    size_t (*action_count)(void);
    const a2k_action_info_t *(*action)(size_t i);
    int (*action_find)(const char *name); /* index or -1 */
    void (*keys_changed)(int n);          /* the number of API keys changed (for the api.keys row); may be NULL */
} a2k_api_backend_t;

/* An extra family of routes of a model (the 2K: PTZ presets and patrol routes, the button and its modes). `match` says whether the
 * path (starting with '/', below /akam/v1) is this family's; `handle` answers a matched request in out (a JSON text) and returns the
 * HTTP status. GET is a read, POST and DELETE need the write scope (and POST a Content-Length). The audit line names sub + 1. */
typedef struct {
    int (*match)(const char *sub);
    int (*handle)(const char *method, const char *sub, const void *body, size_t body_len, char *out, size_t cap, unsigned *retry_s);
} a2k_api_ext_t;

#endif
