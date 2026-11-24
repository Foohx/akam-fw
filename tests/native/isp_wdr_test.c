/* Host test of isp.wdr of the 1080p (isp_wdr.c): the composer (strength blend of the tone curves, refusals) and the sender (nothing sent at the
 * stock strength, one GET + one SET otherwise, the live block as the baseline, the replay after a reload, a failed SET keeps the old strength).
 * The ISP is a fake isp_tunnel() that holds the live WDR block. No camera. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ak_ioctls.h"
#include "isp_nr.h"
#include "isp_wdr.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define ENT 996
static unsigned r16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint8_t *ent(uint8_t *b, int e) { return b + 4 + e * ENT; }
static unsigned curve(const uint8_t *b, int e, int c, int i) { return r16(b + 4 + e * ENT + 0x10 + c * 130 + 2 * i); }

/* ---------------------------------------------------------------- the fake ISP */
static uint8_t g_live[ISP_WDR_BLOCK_LEN];
static int g_gets, g_sets, g_other, g_fail_set, g_fail_get, g_drop_set;
/* the Y_GAMMA block of the fake: its own live copy and counters, so the WDR counts above stay exact */
static uint8_t g_glive[ISP_YGAMMA_BLOCK_LEN];
static int g_ggets, g_gsets, g_gfail_set, g_gfail_get, g_gdrop_set;

int isp_tunnel(uint32_t cmd, void *payload)
{
    if (cmd == isp_wdr_get_cmd()) {
        g_gets++;
        if (g_fail_get) { errno = EIO; return -1; }
        memcpy(payload, g_live, ISP_WDR_BLOCK_LEN);
        return 0;
    }
    if (cmd == isp_wdr_set_cmd()) {
        g_sets++;
        if (g_fail_set) { errno = EIO; return -1; }
        if (!g_drop_set) memcpy(g_live, payload, ISP_WDR_BLOCK_LEN); /* g_drop_set: accepted but not stored (the kernel ignoring it) */
        return 0;
    }
    if (cmd == isp_ygamma_get_cmd()) {
        g_ggets++;
        if (g_gfail_get) { errno = EIO; return -1; }
        memcpy(payload, g_glive, ISP_YGAMMA_BLOCK_LEN);
        return 0;
    }
    if (cmd == isp_ygamma_set_cmd()) {
        g_gsets++;
        if (g_gfail_set) { errno = EIO; return -1; }
        if (!g_gdrop_set) memcpy(g_glive, payload, ISP_YGAMMA_BLOCK_LEN);
        return 0;
    }
    g_other++;
    return 0;
}

/* The day Y_GAMMA of the camera as the conf has it: the identity ramp 8 i with a toe lift (gone from i = 32, input 256),
 * 22 trailing bytes of fields; the night curve is the plain ramp. */
static unsigned gid(int i) { return 8 * i > 1023 ? 1023u : (unsigned)(8 * i); }
static void gbuild(uint8_t *b, int night)
{
    memset(b, 0, ISP_YGAMMA_BLOCK_LEN);
    for (int i = 0; i < ISP_YGAMMA_POINTS; i++) {
        unsigned v = gid(i);
        if (!night && i < 32) v += (unsigned)((32 - i) * (i < 16 ? i : 16) * 2 / 5); /* lifts the toe, 0 stays 0 */
        if (v > 1023) v = 1023;
        w16(b + 2 * i, v);
    }
    for (int k = 0; k < 22; k++) b[258 + k] = (uint8_t)(0xa0 + k);
}
static unsigned gpt(const uint8_t *b, int i) { return r16(b + 2 * i); }

/* A vendor-like block: mode 1, entries 1..4 and 10 switched on with a shadow lift (curve 0 and 1), entries 5..9 switched on with the identity,
 * the others off (and, like the night subfile, entry 2 off with a lifted curve that must never be touched). */
static unsigned ident(int i) { return i * 16 > 1023 ? 1023 : (unsigned)(i * 16); }
static void build(uint8_t *b, int night)
{
    memset(b, 0, ISP_WDR_BLOCK_LEN);
    w16(b, 1);
    w16(b + 2, 0);
    for (int e = 0; e < 17; e++) {
        uint8_t *p = ent(b, e);
        for (int c = 0; c < 6; c++)
            for (int i = 0; i < 65; i++) {
                unsigned v = ident(i);
                if (e >= 1 && e <= 4 && c < 2) { /* the lift: strongest at the bottom, gone at 0 and 64 */
                    v += (unsigned)(i * (64 - i) * (c ? 1 : 3) / 16);
                    if (v > 1023) v = 1023;
                }
                w16(p + 0x10 + c * 130 + 2 * i, v);
            }
        if (e == 10) for (int i = 1; i < 64; i++) w16(p + 0x10 + 2 * i, ident(i) + 5);
        int on = !night && ((e >= 1 && e <= 10));
        w16(p + 4, e >= 1 && e <= 10 ? 1 : 0);
        w16(p + 0x3dc, on ? 1 : 0);
        w16(p + 0x3de, 32); w16(p + 0x3e0, 64); w16(p + 0x3e2, 192);
        for (int k = 0; k < 5; k++) w16(p + 6 + 2 * k, (unsigned)(32 << (k < 4 ? k : 3)));
        w16(p + 0x31c, 4); w16(p + 0x31e, 12); w16(p + 0x320, 64); /* the tail fields the composer never touches */
    }
}

static void reset_counts(void) { g_gets = g_sets = g_other = g_ggets = g_gsets = 0; }

int main(void)
{
    uint8_t base[ISP_WDR_BLOCK_LEN], out[ISP_WDR_BLOCK_LEN];
    char dump[512];
    build(base, 0);
    uint8_t gbase[ISP_YGAMMA_BLOCK_LEN], gout[ISP_YGAMMA_BLOCK_LEN];
    gbuild(gbase, 0);
    memcpy(g_glive, gbase, sizeof(gbase));
    isp_wdr_dump(dump, sizeof(dump));
    CHECK(!strcmp(dump, "[isp.wdr] no event yet"));

    /* ---- the composer */
    CHECK(isp_wdr_check(base, ISP_WDR_BLOCK_LEN) == 0);
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, ISP_WDR_STOCK, out) == 0 && !memcmp(out, base, sizeof(base))); /* the stock strength is a plain copy */
    CHECK(ISP_WDR_STOCK == 40 && ISP_WDR_MAX == 50);
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, -1, out) == -1 && isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, ISP_WDR_MAX + 1, out) == -1);

    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, 0, out) == 0);
    for (int e = 0; e < 17; e++)
        for (int c = 0; c < 6; c++)
            for (int i = 0; i < 65; i++) {
                if (e >= 1 && e <= 10) CHECK(curve(out, e, c, i) == ident(i)); /* strength 0: every switched-on entry is the identity, the lift is gone */
                else CHECK(curve(out, e, c, i) == curve(base, e, c, i));
            }
    /* only the curves of the switched-on entries differ: every other byte of the block is untouched (mode, entry 0, switches, thresholds, tail) */
    for (size_t k = 0; k < ISP_WDR_BLOCK_LEN; k++) {
        if (k < 4) { CHECK(out[k] == base[k]); continue; }
        size_t o = (k - 4) % ENT, e = (k - 4) / ENT;
        int in_curve = o >= 0x10 && o < 0x10 + 6 * 130;
        if (!in_curve || e == 0 || e > 10) CHECK(out[k] == base[k]);
    }
    /* the night-like block (switch off everywhere): every strength composes to the same bytes */
    {
        uint8_t nb[ISP_WDR_BLOCK_LEN], no[ISP_WDR_BLOCK_LEN];
        build(nb, 1);
        CHECK(isp_wdr_compose(nb, ISP_WDR_BLOCK_LEN, 0, no) == 0 && !memcmp(no, nb, sizeof(nb)));
        CHECK(isp_wdr_compose(nb, ISP_WDR_BLOCK_LEN, 50, no) == 0 && !memcmp(no, nb, sizeof(nb)));
    }
    /* half strength: halfway between the identity and the stock curve (rounded); above stock: further, clamped, monotone */
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, 20, out) == 0);
    for (int i = 0; i < 65; i++) {
        int id = (int)ident(i), c = (int)curve(base, 1, 0, i), h = (int)curve(out, 1, 0, i);
        CHECK(h >= id && h <= c && abs(h - (id + c) / 2) <= 1);
    }
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, ISP_WDR_MAX, out) == 0);
    for (int e = 1; e <= 10; e++)
        for (int c = 0; c < 6; c++)
            for (int i = 0; i < 65; i++) {
                CHECK(curve(out, e, c, i) <= 1023);
                CHECK(i == 0 || curve(out, e, c, i) >= curve(out, e, c, i - 1));
                CHECK(curve(out, e, c, i) >= curve(base, e, c, i));
            }
    CHECK(curve(out, 1, 0, 8) > curve(base, 1, 0, 8) && curve(out, 1, 0, 0) == 0 && curve(out, 1, 0, 64) == 1023);
    /* monotone in the strength */
    {
        uint8_t a[ISP_WDR_BLOCK_LEN], b[ISP_WDR_BLOCK_LEN];
        for (int s = 0; s < ISP_WDR_MAX; s++) {
            CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, s, a) == 0 && isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, s + 1, b) == 0);
            for (int i = 0; i < 65; i++) CHECK(curve(b, 1, 0, i) >= curve(a, 1, 0, i));
        }
    }

    /* ---- the refusals: the block read back is not as decoded, nothing is composed */
    CHECK(isp_wdr_check(base, ISP_WDR_BLOCK_LEN - 4) == ISP_NR_REFUSED && isp_wdr_compose(base, ISP_WDR_BLOCK_LEN - 4, 0, out) == ISP_NR_REFUSED);
    CHECK(isp_wdr_check(base, ISP_WDR_BLOCK_LEN + 4) == ISP_NR_REFUSED && isp_wdr_check(NULL, ISP_WDR_BLOCK_LEN) == ISP_NR_REFUSED);
    {
        uint8_t bad[ISP_WDR_BLOCK_LEN];
        memcpy(bad, base, sizeof(bad)); w16(bad, 0);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);                                       /* manual mode word */
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 3) + 0x10 + 2 * 7, 1024);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);                                       /* a curve value above 10 bits */
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 3) + 0x3dc, 2);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);                                       /* the switch is not 0 / 1 */
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 3) + 4, 2);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 3) + 0x3e2, 1024);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 2) + 0x10 + 130 * 3, 5);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);                                       /* a tone curve that does not start at black */
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 12) + 0x10 + 2, 5000);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == ISP_NR_REFUSED);                                       /* any entry counts, also a switched-off one */
        memcpy(bad, base, sizeof(bad)); w16(ent(bad, 12) + 0x10 + 130 * 3, 5);
        CHECK(isp_wdr_check(bad, sizeof(bad)) == 0);                                                    /* an unused entry may hold anything <= 1023 */
    }

    /* ---- the user scale of the 2K: 0..90 linear onto strength 0..40, 91..100 onto 41..50 */
    CHECK(ISP_WDR_USER_STOCK == 90 && ISP_WDR_USER_MAX == 100);
    CHECK(isp_wdr_strength(0) == 0 && isp_wdr_strength(90) == ISP_WDR_STOCK && isp_wdr_strength(45) == 20 && isp_wdr_strength(91) == 41 && isp_wdr_strength(100) == 50);
    CHECK(isp_wdr_strength(-1) == -1 && isp_wdr_strength(101) == -1);
    for (int w = 1; w <= 100; w++) CHECK(isp_wdr_strength(w) >= isp_wdr_strength(w - 1));

    /* ---- the sender */
    memcpy(g_live, base, sizeof(base));
    reset_counts();
    CHECK(isp_wdr_get() == ISP_WDR_USER_STOCK);
    CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_gets == 0 && g_sets == 0 && g_other == 0);               /* the default sends and reads nothing */
    CHECK(isp_wdr_set(-1) == -1 && isp_wdr_set(ISP_WDR_USER_MAX + 1) == -1 && g_gets == 0 && g_sets == 0);   /* out of range: nothing sent */
    CHECK(isp_wdr_get() == ISP_WDR_USER_STOCK);

    CHECK(isp_wdr_set(10) == 0 && g_gets == 2 && g_sets == 1 && g_other == 0 && isp_wdr_get() == 10); /* the baseline GET and the read-back GET */
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, isp_wdr_strength(10), out) == 0 && !memcmp(out, g_live, sizeof(out)));  /* the live block is the composed one */
    CHECK(isp_wdr_set(10) == 0 && g_gets == 3 && g_sets == 2);                                          /* the same value again: composed from the cached baseline, no new GET */
    CHECK(isp_wdr_set(30) == 0 && g_gets == 4 && g_sets == 3);
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, isp_wdr_strength(30), out) == 0 && !memcmp(out, g_live, sizeof(out)));   /* from the pristine baseline, not from the strength 10 block */
    CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_sets == 4 && !memcmp(g_live, base, sizeof(base)));       /* back to stock: the original bytes, once */
    CHECK(isp_wdr_get() == ISP_WDR_USER_STOCK);
    CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_sets == 4);                                              /* and nothing more at the default */

    /* the replay after a day / night reload */
    CHECK(isp_wdr_set(5) == 0);
    memcpy(g_live, base, sizeof(base));          /* the reload put the camera's own block back */
    reset_counts();
    isp_wdr_reset();
    isp_wdr_reapply();
    CHECK(g_gets == 2 && g_sets == 1 && g_other == 0);
    CHECK(isp_wdr_compose(base, ISP_WDR_BLOCK_LEN, isp_wdr_strength(5), out) == 0 && !memcmp(out, g_live, sizeof(out)));
    /* the night subfile has the switch off everywhere: the replay finds nothing to change and sends nothing */
    {
        uint8_t nb[ISP_WDR_BLOCK_LEN];
        build(nb, 1);
        memcpy(g_live, nb, sizeof(nb));
        reset_counts();
        isp_wdr_reset();
        isp_wdr_reapply();
        CHECK(g_gets == 1 && g_sets == 0 && !memcmp(g_live, nb, sizeof(nb)));
        memcpy(g_live, base, sizeof(base));      /* back to the day block: the strength comes back */
        reset_counts();
        isp_wdr_reset();
        isp_wdr_reapply();
        CHECK(g_sets == 1 && isp_wdr_get() == 5);
    }
    /* a reload with the stock strength: nothing at all */
    CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
    memcpy(g_live, base, sizeof(base));
    reset_counts();
    isp_wdr_reset();
    isp_wdr_reapply();
    CHECK(g_gets == 1 && g_sets == 0 && g_other == 0);                  /* a read for the [isp.wdr] line, never a write */
    CHECK(!memcmp(g_live, base, sizeof(base)));

    /* a live block that is not as decoded: refused, nothing sent, the old strength stays */
    {
        uint8_t bad[ISP_WDR_BLOCK_LEN];
        memcpy(bad, base, sizeof(bad)); w16(bad, 0);
        memcpy(g_live, bad, sizeof(bad));
        isp_wdr_reset();
        reset_counts();
        CHECK(isp_wdr_set(10) == ISP_NR_REFUSED && g_gets == 1 && g_sets == 0 && isp_wdr_get() == ISP_WDR_USER_STOCK);
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_gets == 1);
    }
    /* the ISP refuses the SET, or cannot be read: an error, the old strength stays, and the next try works */
    memcpy(g_live, base, sizeof(base));
    isp_wdr_reset();
    g_fail_get = 1;
    CHECK(isp_wdr_set(10) == -1 && isp_wdr_get() == ISP_WDR_USER_STOCK);
    g_fail_get = 0;
    g_fail_set = 1;
    CHECK(isp_wdr_set(10) == -1 && isp_wdr_get() == ISP_WDR_USER_STOCK && !memcmp(g_live, base, sizeof(base)));
    g_fail_set = 0;
    CHECK(isp_wdr_set(10) == 0 && isp_wdr_get() == 10);
    g_fail_set = 1;
    CHECK(isp_wdr_set(20) == -1 && isp_wdr_get() == 10);
    g_fail_set = 0;

    /* ---- the diagnostic line (log and WDRDUMP): get / set results, mode word, masks, checksums, changed bytes, read-back */
    {
        memcpy(g_live, base, sizeof(base));
        isp_wdr_set(ISP_WDR_USER_STOCK);
        isp_wdr_reset();
        CHECK(isp_wdr_set(0) == 0);
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, "[isp.wdr] ev=set user=0 s=0 get=ok mode=1 "));
        CHECK(strstr(dump, " sw=0x007fe prog=0x007fe lift=0x0041e "));                    /* entries 1..10 on; 1..4 and 10 lift the curves */
        CHECK(strstr(dump, " set=ok readback=match"));
        CHECK(!strstr(dump, " changed=0 "));
        /* the checksums differ before and after a rescale, and are equal at a no-op strength */
        const char *cb = strstr(dump, "csum_before="), *ca = strstr(dump, "csum_after=");
        CHECK(cb && ca && strncmp(cb + 12, ca + 11, 8) != 0);
        /* a second set from the cached baseline: no GET for the baseline, the line says so */
        CHECK(isp_wdr_set(45) == 0);
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, "user=45 s=20 get=none mode=1 "));
        /* the ISP accepts the SET but does not store it: the read-back says so */
        g_drop_set = 1;
        CHECK(isp_wdr_set(60) == 0);
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, " set=ok readback=DIFFERS"));
        g_drop_set = 0;
        /* the SET fails with an errno: the line carries it (EIO = 5); a GET that fails carries its errno and no mask */
        g_fail_set = 1;
        CHECK(isp_wdr_set(70) == -1);
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, " set=errno5 readback=none"));
        g_fail_set = 0;
        isp_wdr_reset();
        g_fail_get = 1;
        CHECK(isp_wdr_set(70) == -1);
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, " get=errno5 set=none readback=none note=get-failed") && !strstr(dump, "mode="));
        g_fail_get = 0;
        /* a load at the stock strength is read-only and still logged: the camera's own block, switches and lift */
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
        memcpy(g_live, base, sizeof(base));
        isp_wdr_reset();
        isp_wdr_reapply();
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, "ev=load user=90 s=40 get=ok mode=1 sw=0x007fe prog=0x007fe lift=0x0041e "));
        CHECK(strstr(dump, "changed=0 set=none readback=none note=stock-nothing-sent"));
        /* the night block: no switch, no lift */
        uint8_t nb[ISP_WDR_BLOCK_LEN];
        build(nb, 1);
        memcpy(g_live, nb, sizeof(nb));
        isp_wdr_reset();
        isp_wdr_reapply();
        isp_wdr_dump(dump, sizeof(dump));
        CHECK(strstr(dump, " sw=0x00000 prog=0x007fe lift=0x00000 "));
    }

    /* ================================================================ the Y_GAMMA toe of the same slider */
    {
        uint8_t nb[ISP_YGAMMA_BLOCK_LEN], bad[ISP_YGAMMA_BLOCK_LEN];
        CHECK(isp_ygamma_get_cmd() == 0x80044976 && isp_ygamma_set_cmd() == 0x40044975 && ISP_YGAMMA_BLOCK_LEN == 302);
        CHECK(isp_ygamma_check(gbase, ISP_YGAMMA_BLOCK_LEN) == 0);
        gbuild(nb, 1);
        CHECK(isp_ygamma_check(nb, ISP_YGAMMA_BLOCK_LEN) == 0);
        CHECK(gpt(gbase, 1) > gid(1) && gpt(gbase, 12) > gid(12) && gpt(gbase, 0) == 0 && gpt(gbase, 40) == gid(40));   /* the fixture lifts the toe */

        /* composer: at 90 and above a plain copy (nothing more lifted), at 0 the identity toe, in between linear, never above the stock curve */
        for (int u = 90; u <= 100; u++) CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, u, gout) == 0 && !memcmp(gout, gbase, sizeof(gbase)));
        CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, 0, gout) == 0);
        for (int i = 0; i < ISP_YGAMMA_POINTS; i++) CHECK(gpt(gout, i) == gid(i));                       /* the toe at 0 is the identity */
        CHECK(!memcmp(gout + 258, gbase + 258, 22));                                                      /* the 22 bytes of fields are untouched */
        CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, 45, gout) == 0);
        for (int i = 0; i < ISP_YGAMMA_POINTS; i++) {
            int id = (int)gid(i), c = (int)gpt(gbase, i), h = (int)gpt(gout, i);
            CHECK(h >= id && h <= c && abs(h - (id + c) / 2) <= 1);                                       /* halfway at 45 */
            CHECK(i == 0 || gpt(gout, i) >= gpt(gout, i - 1));
        }
        {
            uint8_t a[ISP_YGAMMA_BLOCK_LEN], b[ISP_YGAMMA_BLOCK_LEN];
            for (int u = 0; u < 90; u++) {
                CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, u, a) == 0 && isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, u + 1, b) == 0);
                for (int i = 0; i < ISP_YGAMMA_POINTS; i++) CHECK(gpt(b, i) >= gpt(a, i) && gpt(b, i) <= gpt(gbase, i));
            }
        }
        CHECK(isp_ygamma_compose(nb, ISP_YGAMMA_BLOCK_LEN, 0, gout) == 0 && !memcmp(gout, nb, sizeof(nb)));  /* the night ramp is linear: nothing to change */
        CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, 101, gout) == -1 && isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, -1, gout) == -1);
        /* refusals: wrong length, NULL, a point above 10 bits, a curve that goes down */
        CHECK(isp_ygamma_check(gbase, ISP_YGAMMA_BLOCK_LEN - 2) == ISP_NR_REFUSED && isp_ygamma_check(NULL, ISP_YGAMMA_BLOCK_LEN) == ISP_NR_REFUSED);
        memcpy(bad, gbase, sizeof(bad)); w16(bad + 2 * 50, 1025);
        CHECK(isp_ygamma_check(bad, sizeof(bad)) == ISP_NR_REFUSED && isp_ygamma_compose(bad, sizeof(bad), 0, gout) == ISP_NR_REFUSED);
        memcpy(bad, gbase, sizeof(bad)); w16(bad + 2 * 60, 3);
        CHECK(isp_ygamma_check(bad, sizeof(bad)) == ISP_NR_REFUSED);

        /* sender (through isp.wdr): the default sends and reads nothing; the slider at 0 sends the identity toe once; no cumulative effect */
        char d2[768];
        memcpy(g_live, base, sizeof(base));
        memcpy(g_glive, gbase, sizeof(gbase));
        isp_wdr_reset();
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
        reset_counts();
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_ggets == 0 && g_gsets == 0 && !memcmp(g_glive, gbase, sizeof(gbase)));
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " gamma=none gset=none greadback=none gnote=stock-nothing-to-send"));
        CHECK(isp_wdr_set(95) == 0 && g_ggets == 0 && g_gsets == 0 && !memcmp(g_glive, gbase, sizeof(gbase)));  /* above 90: no more gamma lift */
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
        reset_counts();
        CHECK(isp_wdr_set(0) == 0 && g_ggets == 2 && g_gsets == 1);                                          /* baseline GET + read-back GET, one SET */
        for (int i = 0; i < ISP_YGAMMA_POINTS; i++) CHECK(gpt(g_glive, i) == gid(i));                       /* the toe at 0 equals the identity */
        CHECK(!memcmp(g_glive + 258, gbase + 258, 22));
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " gamma=ok glift=") && strstr(d2, " gset=ok greadback=match") && !strstr(d2, " gchanged=0 "));
        {
            const char *gb = strstr(d2, "gcsum_before="), *ga = strstr(d2, "gcsum_after=");
            CHECK(gb && ga && strncmp(gb + 13, ga + 12, 8) != 0);
        }
        CHECK(isp_wdr_set(45) == 0 && g_gsets == 2 && g_ggets == 3);                                         /* from the cached baseline: no new baseline GET */
        CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, 45, gout) == 0 && !memcmp(gout, g_glive, sizeof(gout)));  /* not cumulative: composed from the pristine block */
        CHECK(isp_wdr_set(45) == 0 && !memcmp(gout, g_glive, sizeof(gout)));                                 /* repeating the value changes nothing more */
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && !memcmp(g_glive, gbase, sizeof(gbase)));               /* back to stock: the original bytes */
        {
            int n = g_gsets;
            CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && g_gsets == n);                                     /* and nothing more at the default */
        }
        /* going above 90 from a lowered toe puts the stock one back, no extra lift */
        CHECK(isp_wdr_set(20) == 0 && isp_wdr_set(100) == 0 && !memcmp(g_glive, gbase, sizeof(gbase)));
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);

        /* replay after a day / night reload: the reload puts the conf curve back, the toe comes back from a fresh baseline */
        CHECK(isp_wdr_set(30) == 0);
        memcpy(g_live, base, sizeof(base));
        memcpy(g_glive, gbase, sizeof(gbase));
        isp_wdr_reset();
        reset_counts();
        isp_wdr_reapply();
        CHECK(g_ggets == 2 && g_gsets == 1);
        CHECK(isp_ygamma_compose(gbase, ISP_YGAMMA_BLOCK_LEN, 30, gout) == 0 && !memcmp(gout, g_glive, sizeof(gout)));
        /* the night curve (linear) after the reload: nothing to change, nothing sent, and the day one is replayed afterwards */
        memcpy(g_live, base, sizeof(base));
        memcpy(g_glive, nb, sizeof(nb));
        isp_wdr_reset();
        reset_counts();
        isp_wdr_reapply();
        CHECK(g_ggets == 1 && g_gsets == 0 && !memcmp(g_glive, nb, sizeof(nb)));
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " gnote=composed-equals-live-nothing-to-send"));
        memcpy(g_glive, gbase, sizeof(gbase));
        isp_wdr_reset();
        isp_wdr_reapply();
        CHECK(g_gsets == 1 && isp_wdr_get() == 30);
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);

        /* a load at the stock strength: one read-only GET for the line, never a write */
        memcpy(g_glive, gbase, sizeof(gbase));
        isp_wdr_reset();
        reset_counts();
        isp_wdr_reapply();
        CHECK(g_ggets == 1 && g_gsets == 0);
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, "ev=load user=90 s=40 ") && strstr(d2, " gamma=ok glift=") && strstr(d2, " gchanged=0 gset=none greadback=none gnote=stock-nothing-sent"));

        /* refusal: a live curve that is not as decoded is not touched; the WDR part still applies, the call succeeds (and the line says so) */
        memcpy(g_live, base, sizeof(base));
        memcpy(g_glive, gbase, sizeof(gbase));
        w16(g_glive + 2 * 60, 3);
        memcpy(bad, g_glive, sizeof(bad));
        isp_wdr_reset();
        reset_counts();
        CHECK(isp_wdr_set(10) == 0 && g_gsets == 0 && !memcmp(g_glive, bad, sizeof(bad)) && g_sets == 1);
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " set=ok readback=match gamma=ok glift=") && strstr(d2, " gset=none greadback=none gnote=live-block-not-as-decoded"));
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
        /* both refused: the call fails and the old strength stays */
        w16(g_live, 0);
        isp_wdr_reset();
        CHECK(isp_wdr_set(10) == ISP_NR_REFUSED && isp_wdr_get() == ISP_WDR_USER_STOCK);
        /* the WDR block refused but the toe applies: the strength is in force */
        memcpy(g_glive, gbase, sizeof(gbase));
        isp_wdr_reset();
        CHECK(isp_wdr_set(10) == 0 && isp_wdr_get() == 10 && gpt(g_glive, 1) < gpt(gbase, 1));
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0 && !memcmp(g_glive, gbase, sizeof(gbase)));
        /* gamma GET / SET failures and a dropped SET: reported on the line, the WDR part is unaffected */
        memcpy(g_live, base, sizeof(base));
        isp_wdr_reset();
        g_gfail_get = 1;
        CHECK(isp_wdr_set(10) == 0);
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " set=ok readback=match gamma=errno5 gset=none greadback=none gnote=get-failed"));
        g_gfail_get = 0;
        g_gfail_set = 1;
        CHECK(isp_wdr_set(11) == 0);
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " gset=errno5 greadback=none"));
        g_gfail_set = 0;
        g_gdrop_set = 1;
        CHECK(isp_wdr_set(12) == 0);
        isp_wdr_dump(d2, sizeof(d2));
        CHECK(strstr(d2, " gset=ok greadback=DIFFERS"));
        g_gdrop_set = 0;
        memcpy(g_glive, gbase, sizeof(gbase));
        CHECK(isp_wdr_set(ISP_WDR_USER_STOCK) == 0);
    }

    printf("isp_wdr_test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
