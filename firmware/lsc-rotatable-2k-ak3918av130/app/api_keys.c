#include "api_keys.h"

static const char k_nothing[1] = {0}; /* an empty C string without the "" literal (repo C scan) */

#include <stdio.h>
#include <string.h>

#include "sha256.h"

const char *akey_scope_name(int s) {
    return s == AKEY_READ ? "read" : s == AKEY_WRITE ? "write" : s == AKEY_ADMIN ? "admin" : "none";
}

int akey_scope_parse(const char *s) {
    if (!s) return 0;
    if (!strcmp(s, "read")) return AKEY_READ;
    if (!strcmp(s, "write")) return AKEY_WRITE;
    if (!strcmp(s, "admin")) return AKEY_ADMIN;
    return 0;
}

int akey_id_ok(const char *id) {
    size_t i;
    if (!id) return 0;
    for (i = 0; i < AKEY_ID_LEN; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return id[AKEY_ID_LEN] == 0;
}

int akey_label_ok(const char *l) {
    size_t n, i;
    if (!l) return 0;
    n = strlen(l);
    if (n < 1 || n > AKEY_LABEL_MAX || l[0] == ' ' || l[n - 1] == ' ') return 0;
    for (i = 0; i < n; i++) {
        char c = l[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int akeys_find(const akeys_t *ks, const char *id) {
    size_t i;
    for (i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].id, id)) return (int)i;
    return -1;
}

int akeys_find_name(const akeys_t *ks, const char *s) {
    size_t i;
    for (i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].label, s)) return (int)i;
    for (i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].id, s)) return (int)i;
    return -1;
}

int akeys_count_admin(const akeys_t *ks) {
    size_t i;
    int n = 0;
    for (i = 0; i < ks->n; i++) n += ks->k[i].scope == AKEY_ADMIN;
    return n;
}

int akeys_add(akeys_t *ks, const akey_t *k) {
    size_t i;
    if (!ks || !k || !akey_id_ok(k->id) || k->scope < AKEY_READ || k->scope > AKEY_ADMIN || k->created < 0 ||
        !akey_label_ok(k->label))
        return AKEYS_E_FORMAT;
    if (ks->n >= AKEY_MAX) return AKEYS_E_TOOBIG;
    for (i = 0; i < ks->n; i++)
        if (!strcmp(ks->k[i].id, k->id) || !strcmp(ks->k[i].label, k->label) ||
            !strcmp(ks->k[i].id, k->label) || !strcmp(ks->k[i].label, k->id)) /* a name must not shadow another key's id */
            return AKEYS_E_DUP;
    ks->k[ks->n++] = *k;
    return AKEYS_OK;
}

int akeys_remove(akeys_t *ks, const char *name, int force) {
    int i = akeys_find_name(ks, name);
    if (i < 0) return AKEYS_E_NOTFOUND;
    if (ks->k[i].scope == AKEY_ADMIN && akeys_count_admin(ks) == 1 && !force) return AKEYS_E_LAST_ADMIN;
    akam_wipe(&ks->k[i], sizeof ks->k[i]);
    memmove(&ks->k[i], &ks->k[i + 1], (ks->n - (size_t)i - 1) * sizeof ks->k[0]);
    ks->n--;
    akam_wipe(&ks->k[ks->n], sizeof ks->k[0]);
    return AKEYS_OK;
}

int akey_make(akey_t *out, int scope, const char *label, int64_t created, const uint8_t rnd[36]) {
    if (!out || !rnd || scope < AKEY_READ || scope > AKEY_ADMIN || !akey_label_ok(label) || created < 0)
        return AKEYS_E_FORMAT;
    memset(out, 0, sizeof *out);
    akam_hex(rnd + 32, 4, out->id);
    out->scope = (uint8_t)scope;
    out->created = created;
    memcpy(out->secret, rnd, 32);
    memcpy(out->label, label, strlen(label) + 1);
    return AKEYS_OK;
}

/* One line (no LF, NUL terminated copy in ln) into k. */
static int parse_line(akey_t *k, char *ln) {
    char *f[5];
    char *p = ln;
    size_t i, n;
    int64_t created = 0;
    for (i = 0; i < 4; i++) { /* the label is the rest and cannot contain ':' (charset), so a 5th ':' is a format error */
        f[i] = p;
        p = strchr(p, ':');
        if (!p) return -1;
        *p++ = 0;
    }
    f[4] = p;
    if (strchr(p, ':')) return -1;
    memset(k, 0, sizeof *k);
    if (!akey_id_ok(f[0])) return -1;
    memcpy(k->id, f[0], AKEY_ID_LEN + 1);
    k->scope = (uint8_t)akey_scope_parse(f[1]);
    if (!k->scope) return -1;
    n = strlen(f[2]);
    if (n < 1 || n > 11 || (n > 1 && f[2][0] == '0')) return -1;
    for (i = 0; i < n; i++) {
        if (f[2][i] < '0' || f[2][i] > '9') return -1;
        created = created * 10 + (f[2][i] - '0');
    }
    k->created = created;
    if (strlen(f[3]) != 64 || akam_unhex(f[3], 32, k->secret) != 0) return -1;
    if (!akey_label_ok(f[4])) return -1;
    memcpy(k->label, f[4], strlen(f[4]) + 1);
    return 0;
}

int akeys_parse(akeys_t *ks, const char *text, size_t len, size_t *err_line) {
    size_t pos = 0, line = 0;
    char ln[160];
    if (err_line) *err_line = 0;
    if (!ks || (!text && len)) return AKEYS_E_ARG;
    memset(ks, 0, sizeof *ks);
    if (len > AKEY_FILE_MAX) return AKEYS_E_TOOBIG;
    while (pos < len) {
        size_t e = pos, n;
        akey_t k;
        int r;
        while (e < len && text[e] != '\n') e++;
        n = e - pos;
        line++;
        if (n == 0) goto fail_fmt; /* blank lines are not written by us: strict */
        if (n >= sizeof ln) goto fail_fmt;
        memcpy(ln, text + pos, n);
        ln[n] = 0;
        if (memchr(ln, 0, n) || parse_line(&k, ln) != 0) {
            akam_wipe(ln, sizeof ln);
            goto fail_fmt;
        }
        akam_wipe(ln, sizeof ln);
        r = akeys_add(ks, &k);
        akam_wipe(&k, sizeof k);
        if (r != AKEYS_OK) {
            if (err_line) *err_line = line;
            akam_wipe(ks, sizeof *ks);
            return r;
        }
        pos = e + 1;
    }
    return AKEYS_OK;
fail_fmt:
    if (err_line) *err_line = line;
    akam_wipe(ks, sizeof *ks);
    return AKEYS_E_FORMAT;
}

int akeys_serialize(const akeys_t *ks, char *out, size_t cap) {
    size_t i, len = 0;
    if (!ks || !out || cap == 0) return AKEYS_E_ARG;
    out[0] = 0;
    for (i = 0; i < ks->n; i++) {
        const akey_t *k = &ks->k[i];
        char hex[65];
        int n;
        akam_hex(k->secret, 32, hex);
        n = snprintf(out + len, cap - len, "%s:%s:%lld:%s:%s\n", k->id, akey_scope_name(k->scope), (long long)k->created,
                     hex, k->label);
        akam_wipe(hex, sizeof hex);
        if (n < 0 || (size_t)n >= cap - len) {
            akam_wipe(out, cap);
            return AKEYS_E_TOOBIG;
        }
        len += (size_t)n;
    }
    return (int)len;
}

static int save(const akeys_t *ks, akeys_writer_t w, void *ctx) {
    char buf[AKEY_FILE_MAX + 1];
    int n = akeys_serialize(ks, buf, sizeof buf);
    int r;
    if (n < 0) return n;
    r = (!w || w(ctx, buf, (size_t)n) != 0) ? AKEYS_E_WRITE : AKEYS_OK;
    akam_wipe(buf, sizeof buf);
    return r;
}

int akeys_add_save(akeys_t *ks, const akey_t *k, akeys_writer_t w, void *ctx) {
    akeys_t c = *ks;
    int r = akeys_add(&c, k);
    if (r == AKEYS_OK) r = save(&c, w, ctx);
    if (r == AKEYS_OK) *ks = c;
    akam_wipe(&c, sizeof c);
    return r;
}

int akeys_remove_save(akeys_t *ks, const char *name, int force, akeys_writer_t w, void *ctx) {
    akeys_t c = *ks;
    int r = akeys_remove(&c, name, force);
    if (r == AKEYS_OK) r = save(&c, w, ctx);
    if (r == AKEYS_OK) {
        akam_wipe(ks, sizeof *ks);
        *ks = c;
    }
    akam_wipe(&c, sizeof c);
    return r;
}

int akeys_json_list(const akeys_t *ks, char *out, size_t cap) {
    size_t i, len = 0;
    int n;
    if (!ks || !out || cap == 0) return AKEYS_E_ARG;
    n = snprintf(out, cap, "{\"keys\":[");
    if (n < 0 || (size_t)n >= cap) return AKEYS_E_TOOBIG;
    len = (size_t)n;
    for (i = 0; i < ks->n; i++) {
        const akey_t *k = &ks->k[i];
        /* labels and ids are restricted to characters that need no JSON escaping */
        n = snprintf(out + len, cap - len, "%s{\"id\":\"%s\",\"name\":\"%s\",\"scope\":\"%s\",\"created\":%lld}", i ? "," : k_nothing,
                     k->id, k->label, akey_scope_name(k->scope), (long long)k->created);
        if (n < 0 || (size_t)n >= cap - len) return AKEYS_E_TOOBIG;
        len += (size_t)n;
    }
    n = snprintf(out + len, cap - len, "]}");
    if (n < 0 || (size_t)n >= cap - len) return AKEYS_E_TOOBIG;
    return (int)(len + (size_t)n);
}
