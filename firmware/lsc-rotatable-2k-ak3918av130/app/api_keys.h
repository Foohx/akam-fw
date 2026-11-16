/* API key store format (/config/akam/api_keys, specs/control-api.md sections 5.1 and 8). Pure functions: parse, serialise,
 * add, remove (last-admin guard), and an atomic save through an INJECTED writer (the camera passes a2k_store_write, tests a
 * fake). No file I/O, no allocation. One line per key, LF:
 *     key_id:scope:created:hex(secret, 64 chars):label
 * key_id = 8 lower-case hex, scope = read|write|admin, created = unix seconds (decimal, 0 when the clock was unset),
 * label = 1..24 of [A-Za-z0-9 ._-], not starting or ending with a space, unique. Up to 8 keys. */
#ifndef AKAM_API_KEYS_H
#define AKAM_API_KEYS_H
#include <stddef.h>
#include <stdint.h>

#define AKEY_MAX 8
#define AKEY_ID_LEN 8
#define AKEY_LABEL_MAX 24
#define AKEY_FILE_MAX 2048

/* Ordered scope levels: a key with level L may call anything needing level <= L. */
enum { AKEY_SCOPE_NONE = 0, AKEY_READ = 1, AKEY_WRITE = 2, AKEY_ADMIN = 3 };

enum {
    AKEYS_OK = 0,
    AKEYS_E_FORMAT = -1,    /* malformed line or field */
    AKEYS_E_TOOBIG = -2,    /* file or key count over the limit, or output buffer too small */
    AKEYS_E_DUP = -3,       /* duplicate key id or label */
    AKEYS_E_LAST_ADMIN = -4,/* removing the only admin key */
    AKEYS_E_NOTFOUND = -5,
    AKEYS_E_WRITE = -6,     /* the injected writer failed; nothing changed */
    AKEYS_E_ARG = -7
};

typedef struct {
    char id[AKEY_ID_LEN + 1];
    uint8_t scope; /* AKEY_READ..AKEY_ADMIN */
    int64_t created;
    uint8_t secret[32];
    char label[AKEY_LABEL_MAX + 1];
} akey_t;

typedef struct {
    akey_t k[AKEY_MAX];
    size_t n;
} akeys_t;

const char *akey_scope_name(int scope);        /* "read", "write", "admin", or "none" */
int akey_scope_parse(const char *s);           /* level, or 0 when unknown */
int akey_label_ok(const char *label);          /* 1 when valid */
int akey_id_ok(const char *id);                /* 1 when 8 lower-case hex */

/* Parses the whole file or fails as a whole (the caller keeps the file, closes the API, reports keys_invalid). *keys is
 * zeroed first. On failure *err_line (if non NULL) is the 1-based line. An empty file is valid (no key). */
int akeys_parse(akeys_t *keys, const char *text, size_t len, size_t *err_line);
/* Canonical text into out; returns its length, or a negative AKEYS_E_*. */
int akeys_serialize(const akeys_t *keys, char *out, size_t cap);
/* Index of the key with this id, or -1. */
int akeys_find(const akeys_t *keys, const char *id);
/* Index of the key whose label or id equals s, or -1. */
int akeys_find_name(const akeys_t *keys, const char *s);
int akeys_count_admin(const akeys_t *keys);

/* Builds a new key from injected randomness: rnd = 32 secret bytes then 4 id bytes. The id is the 4 bytes in lower hex
 * (the caller retries with fresh bytes on AKEYS_E_DUP from akeys_add). */
int akey_make(akey_t *out, int scope, const char *label, int64_t created, const uint8_t rnd[36]);
/* In-memory edits (validate, no I/O). */
int akeys_add(akeys_t *keys, const akey_t *k);
int akeys_remove(akeys_t *keys, const char *id_or_label, int force);

/* Atomic variants: edit a COPY, serialise, call writer(ctx, text, len); only when it returns 0 is *keys updated. */
typedef int (*akeys_writer_t)(void *ctx, const char *text, size_t len);
int akeys_add_save(akeys_t *keys, const akey_t *k, akeys_writer_t w, void *ctx);
int akeys_remove_save(akeys_t *keys, const char *id_or_label, int force, akeys_writer_t w, void *ctx);

/* GET /keys body: {"keys":[{"id","name","scope","created"}]}; never a secret. Returns length or negative. */
int akeys_json_list(const akeys_t *keys, char *out, size_t cap);

#endif
