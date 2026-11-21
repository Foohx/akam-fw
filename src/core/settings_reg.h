/* Settings registry core (specs/control-api.md section 2): one table row per user-visible setting, strict text parsers
 * per type, bounds/options validation, apply modes, persistence flag, secret flag, per-model availability, get/set/check
 * hooks, all-or-nothing PATCH, "key=value" persistence text, and bounded JSON for values, one setting and the schema.
 * Pure: no I/O, no allocation, no global state (the table and the hooks belong to the caller). Values are text in canonical
 * form (bool 0/1, decimal int without sign '+' or leading zeros, enum word, ASCII string, pattern). Target home:
 * src/core. Deviation from the spec sketch: `check` also receives the registry, so a cross-field rule can read the
 * values of the same PATCH through streg_peek(). */
#ifndef AKAM_SETTINGS_REG_H
#define AKAM_SETTINGS_REG_H
#include <stddef.h>
#include <stdint.h>

#define STREG_KEY_MAX 48
#define STREG_VAL_MAX 832   /* longest value text incl. NUL; ST_STRING hi <= 831 (detect.boxes is the long one) */
#define STREG_MAX_DEFS 192
#define STREG_FILE_MAX 4096 /* /config/akam/settings */
#define STREG_ENUM_MAX 16

typedef enum { ST_BOOL, ST_INT, ST_ENUM, ST_STRING, ST_PATTERN } st_type_t;
typedef enum { AP_LIVE, AP_RESTART_COMPONENT, AP_REBOOT } st_apply_t;

#define ST_ACC_READ 1u
#define ST_ACC_WRITE 2u /* absent = read-only state value */
#define ST_EXPOSE_HTTP 1u
#define ST_EXPOSE_MQTT 2u

typedef enum { ST_SRC_HTTP, ST_SRC_MQTT, ST_SRC_CONF, ST_SRC_LOCAL } st_src_t;

enum {
    ST_OK = 0,
    ST_E_UNKNOWN = -1,   /* unknown key, or absent on this model/capability set, or hidden from this channel */
    ST_E_FORMAT = -2,    /* not the canonical text of the type */
    ST_E_RANGE = -3,     /* out of bounds, too long, or not one of the options */
    ST_E_READONLY = -4,
    ST_E_SECRET = -5,    /* secret: never read; never written over HTTP */
    ST_E_SCOPE = -6,
    ST_E_CHECK = -7,     /* cross-field rule refused it */
    ST_E_SETTER = -8,    /* the set hook failed */
    ST_E_DUP = -9,       /* key twice in one PATCH */
    ST_E_NOTAPPLIED = -10, /* valid, but the PATCH was refused as a whole */
    ST_E_TABLE = -11,    /* bad table (streg_init) */
    ST_E_TOOBIG = -12,   /* file or output buffer too big */
    ST_E_GETTER = -13
};

struct streg;
typedef struct {
    const char *key;          /* [a-z0-9_.]+, <= 48 bytes, unique */
    st_type_t type;
    long lo, hi;              /* ST_INT bounds; ST_STRING min and max length; ST_PATTERN: bounds of each number */
    const char *const *opts;  /* ST_ENUM: NULL-terminated options; ST_PATTERN: opts[0] = grammar id "led" | "pan_tilt" | "zone4" */
    const char *def;          /* default, canonical text (secrets: may be NULL) */
    st_apply_t apply;
    uint8_t persist;          /* 1 = saved in /config/akam/settings, 0 = RAM only. Never 1 for a secret */
    uint8_t access;           /* ST_ACC_READ | ST_ACC_WRITE */
    uint8_t expose;           /* ST_EXPOSE_HTTP | ST_EXPOSE_MQTT (component metadata lives in the MQTT layer) */
    uint8_t secret;           /* write-only, never in any reply, refused over HTTP, never persisted here; no get hook */
    uint8_t min_scope;        /* AKEY_* level needed (0 = write for writable keys, read otherwise) */
    uint32_t model_mask;      /* models that have it (0 = all) */
    uint32_t cap_req;         /* capability bits required from the HAL descriptor (0 = none) */
    int (*get)(const char *key, char *out, size_t cap);                           /* length, or < 0. NULL: reads the default */
    int (*set)(const char *key, const char *val, st_apply_t *effective);          /* 0 or error; required when writable */
    int (*check)(const struct streg *reg, const char *key, const char *val);      /* optional cross-field rule, 0 = ok */
} st_def_t;

typedef struct streg {
    const st_def_t *defs;
    size_t n;
    uint32_t model, caps; /* the running model bit and its HAL capability set */
    /* values of the PATCH being validated (for streg_peek); NULL outside a patch */
    const char *const *pend_k;
    const char *const *pend_v;
    size_t pend_n;
} streg_t;

/* Validates the table (keys, uniqueness, bounds, defaults parse, hooks, secret rules). 0, or ST_E_TABLE with *bad = row. */
int streg_init(streg_t *r, const st_def_t *defs, size_t n, uint32_t model_bit, uint32_t caps, size_t *bad);
/* The definition, or NULL when unknown or absent on this model (no entry, no schema line, no MQTT entity). */
const st_def_t *streg_find(const streg_t *r, const char *key);

/* The strict per-type parser, shared by HTTP, MQTT and conf. 0, ST_E_FORMAT or ST_E_RANGE. */
int streg_validate(const st_def_t *d, const char *val);
/* Current value text: length, or ST_E_*. Secrets: ST_E_SECRET always. From HTTP a key without ST_EXPOSE_HTTP is unknown. */
int streg_get(const streg_t *r, st_src_t src, const char *key, char *out, size_t cap);
/* The value a cross-field check should see: the pending PATCH value of the key, else the current one. */
int streg_peek(const streg_t *r, const char *key, char *out, size_t cap);
/* Everything but the setter and the cross-field check: key, channel, access, secret, scope, value. */
int streg_check_one(const streg_t *r, st_src_t src, int scope, const char *key, const char *val);

typedef struct {
    int ok;
    int err;          /* ST_E_* when !ok */
    st_apply_t apply; /* when ok */
} st_result_t;

/* PATCH as a whole: every key is checked, then every cross-field check, before any setter runs; one failure refuses the
 * whole batch (the others report ST_E_NOTAPPLIED). Setters then run in REGISTRY order. A setter failure leaves the earlier
 * setters applied (per-key results). Returns the HTTP status: 200 all ok, 207 mixed, 422 none. res has n entries. */
int streg_patch(streg_t *r, st_src_t src, int scope, size_t n, const char *const *keys, const char *const *vals,
                st_result_t *res);
/* Single key convenience (same path). */
int streg_set(streg_t *r, st_src_t src, int scope, const char *key, const char *val, st_result_t *res);

const char *streg_err_name(int err);        /* "unknown_key","format","range","readonly","secret","scope",... */
const char *streg_apply_name(st_apply_t a); /* "applied","restart_component","pending_device_restart" */

/* Persistence text: "key=value\n" for the persistent, writable, non-secret, available keys that differ from the default
 * (registry order). Length, or ST_E_TOOBIG (> 4096). */
int streg_save_text(const streg_t *r, char *out, size_t cap);
/* Loads it: validated as a whole first (any bad value, a duplicate, a failed cross-field check, > 4096 bytes: nothing is
 * applied, returns the error and *err_line). Keys unknown here or not persistent are ignored (*ignored). Then the setters
 * run in registry order (src CONF); *failed counts the setters that failed. Any out pointer may be NULL. */
int streg_load_text(streg_t *r, const char *text, size_t len, size_t *err_line, size_t *ignored, size_t *failed);

/* JSON, bounded, into out. All return the length or ST_E_TOOBIG. Over HTTP (the only channel here) secrets and keys
 * without ST_EXPOSE_HTTP never appear. */
/* {"k":"v",...}: writable keys, or with state_only the read-only state keys; optional key prefix. */
int streg_json_values(const streg_t *r, const char *prefix, int state_only, char *out, size_t cap);
/* One setting with metadata and value. ST_E_UNKNOWN when absent or hidden. */
int streg_json_setting(const streg_t *r, const char *key, char *out, size_t cap);
/* {"api":1,"settings":[{key,type,lo,hi,opts|grammar,default,apply,persist,access,scope}...]} (the action list is phase 2). */
int streg_json_schema(const streg_t *r, char *out, size_t cap);
/* PATCH reply body {"results":{...}} (spec 4.2). vals are the requested values (echoed when ok). */
int streg_json_patch_reply(const streg_t *r, size_t n, const char *const *keys, const char *const *vals,
                           const st_result_t *res, char *out, size_t cap);

#endif
