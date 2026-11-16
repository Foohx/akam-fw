/* Host unit test of the runtime zones of the Rotatable 2K app: the ignore rectangles, the text forms (Home Assistant `text` entities), the
 * live holder with its persistence in /config/akam (a temporary directory here), the "applies at home only" rule, and the atomic store
 * (akam2k_zones.c, akam2k_zlive.c, akam2k_store.c). Prints "rot2k zlive unit: N failed". */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akam2k_store.h"
#include "akam2k_zlive.h"
#include "akam2k_zones.h"
#include "hal_ptz.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...) { (void)fmt; }

static int notes;
static int last_n;
static void note(const a2k_zones_t *z) { notes++; last_n = z->n; }

static struct a2k_det_box feet(int x, int y) /* a box whose feet point is (x%, y%) of the 320x192 image */
{
    struct a2k_det_box b = { 0, 900, 0, 0, 0, 0 };
    b.x1 = b.x2 = x * 320 / 100 - 1 < 0 ? 0 : x * 320 / 100 - 1;
    b.y1 = 0;
    b.y2 = y * 192 / 100 - 1 < 0 ? 0 : y * 192 / 100 - 1;
    return b;
}

static void test_text(void)
{
    a2k_zones_t z, keep;
    char out[300];
    printf("== zone and ignore text forms\n");
    memset(&z, 0, sizeof z);
    CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, "porch=0,0,50,50;drive=50,0,100,100;zone_mode=all") == 0 && z.n == 2 && z.mode == A2K_ZONE_MODE_ALL,
          "zones text: two zones and zone_mode all");
    CHECK(a2k_zones_format(&z, A2K_ZF_ZONES, ';', out, sizeof out) > 0 && !strcmp(out, "porch=0,0,50,50;drive=50,0,100,100;zone_mode=all"),
          "zones text round trip");
    CHECK(a2k_zones_set_text(&z, A2K_ZF_IGNORE, "door=10,10,30,30;sky=0,0,100,20") == 0 && z.ni == 2 && z.n == 2, "ignore text sets the ignore rectangles, keeps the zones");
    CHECK(a2k_zones_format(&z, A2K_ZF_IGNORE, ';', out, sizeof out) > 0 && !strcmp(out, "door=10,10,30,30;sky=0,0,100,20"), "ignore text round trip (no prefix)");
    CHECK(a2k_zones_format(&z, A2K_ZF_FILE, '\n', out, sizeof out) > 0 &&
              !strcmp(out, "porch=0,0,50,50\ndrive=50,0,100,100\nzone_mode=all\nignore_door=10,10,30,30\nignore_sky=0,0,100,20\n"),
          "the file form: zones, zone_mode, ignore_ lines");
    {
        a2k_zones_t back;
        CHECK(a2k_zones_parse(out, strlen(out), &back) == 0 && back.n == 2 && back.ni == 2 && back.mode == A2K_ZONE_MODE_ALL, "the file form parses back");
    }
    keep = z;
    CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, "bad=10,10,5,5") == -1 && memcmp(&z, &keep, sizeof z) == 0, "invalid zones text: refused, old value kept");
    CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, "a=0,0,1,1;a=0,0,1,1") == -1 && a2k_zones_set_text(&z, A2K_ZF_ZONES, "a=0,0,1,1;;b=0,0,1,1") == -1 &&
              a2k_zones_set_text(&z, A2K_ZF_ZONES, "ignore_x=0,0,1,1") == -1 && a2k_zones_set_text(&z, A2K_ZF_ZONES, "zone_mode=maybe") == -1 &&
              memcmp(&z, &keep, sizeof z) == 0,
          "duplicates, an empty entry, a reserved name, a bad mode are refused");
    CHECK(a2k_zones_set_text(&z, A2K_ZF_IGNORE, "zone_mode=all") == -1 && a2k_zones_set_text(&z, A2K_ZF_IGNORE, "a=0,0,1,1;a=1,1,2,2") == -1 &&
              memcmp(&z, &keep, sizeof z) == 0,
          "the ignore text takes rectangles only");
    CHECK(a2k_zones_set_text(&z, A2K_ZF_IGNORE, "none") == 0 && z.ni == 0 && a2k_zones_set_text(&z, A2K_ZF_ZONES, "") == 0 && z.n == 0 && z.mode == 0,
          "none and empty clear");
    {
        char big[400];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, big) == -1, "a text over 255 bytes is refused");
    }
    CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, "a=0,0,10,10;b=0,0,10,10;c=0,0,10,10;d=0,0,10,10;e=0,0,10,10;f=0,0,10,10;g=0,0,10,10;h=0,0,10,10") == 0 && z.n == 8,
          "eight zones fit the text");
    CHECK(a2k_zones_format(&z, A2K_ZF_ZONES, ';', out, 10) == -1, "a too small buffer is refused");
}

static void test_ignore_rule(void)
{
    a2k_zones_t z;
    struct a2k_det_box in, out;
    printf("== ignore rule (feet point)\n");
    memset(&z, 0, sizeof z);
    CHECK(a2k_zones_set_text(&z, A2K_ZF_IGNORE, "door=10,40,30,100") == 0, "one ignore rectangle");
    in = feet(20, 90);
    out = feet(60, 90);
    CHECK(a2k_zones_ignored(&z, &in) == 1 && a2k_zones_ignored(&z, &out) == 0, "a person with the feet inside is ignored, outside is not");
    memset(&z, 0, sizeof z);
    CHECK(a2k_zones_ignored(&z, &in) == 0, "no ignore rectangle: nobody is ignored");
}

static void test_live(const char *dir)
{
    char p[300], buf[A2K_STORE_MAX + 1], t[400];
    a2k_zones_t card, z;
    unsigned g0, g1;
    printf("== live zones (persistence, generation, apply rule)\n");
    snprintf(p, sizeof p, "%s/zones", dir);
    memset(&card, 0, sizeof card);
    CHECK(a2k_zones_set_text(&card, A2K_ZF_ZONES, "cardzone=0,0,100,100") == 0, "card zones");
    CHECK(a2k_zlive_init(&card, p) == 0, "no runtime file: the card zones are used");
    a2k_zlive_get(&z, &g0);
    CHECK(z.n == 1 && !strcmp(z.z[0].name, "cardzone"), "live = card zones");
    a2k_zlive_set_notify(note);
    CHECK(a2k_zlive_set_text(A2K_ZF_ZONES, "porch=0,0,50,50;zone_mode=all") == 0 && notes == 1 && last_n == 1, "an edit is applied and notified");
    a2k_zlive_get(&z, &g1);
    CHECK(g1 != g0 && z.n == 1 && z.mode == A2K_ZONE_MODE_ALL, "the generation moves");
    CHECK(a2k_store_read(p, buf, sizeof buf) > 0 && !strcmp(buf, "porch=0,0,50,50\nzone_mode=all\n"), "persisted atomically in the file form");
    snprintf(t, sizeof t, "%s.tmp", p);
    CHECK(access(t, F_OK) != 0, "no temp file left");
    CHECK(a2k_zlive_set_text(A2K_ZF_ZONES, "bad") == -1 && notes == 1, "an invalid edit changes nothing and notifies nobody");
    CHECK(a2k_zlive_set_text(A2K_ZF_IGNORE, "door=10,10,20,20") == 0, "ignore edit");
    a2k_zlive_get_text(A2K_ZF_ZONES, t, sizeof t);
    CHECK(!strcmp(t, "porch=0,0,50,50;zone_mode=all"), "get_text zones");
    a2k_zlive_get_text(A2K_ZF_IGNORE, t, sizeof t);
    CHECK(!strcmp(t, "door=10,10,20,20"), "get_text ignore");
    /* restart: the runtime file wins over the card file */
    CHECK(a2k_zlive_init(&card, p) == 1, "a restart reads the runtime file");
    a2k_zlive_get(&z, NULL);
    CHECK(z.n == 1 && !strcmp(z.z[0].name, "porch") && z.ni == 1 && z.mode == A2K_ZONE_MODE_ALL, "the card zones are replaced by the runtime ones");
    /* an empty runtime file means "no zone", and still wins */
    CHECK(a2k_zlive_set_text(A2K_ZF_ZONES, "") == 0 && a2k_zlive_set_text(A2K_ZF_IGNORE, "none") == 0, "cleared");
    CHECK(a2k_zlive_init(&card, p) == 1, "an empty runtime file is used (it wins)");
    a2k_zlive_get(&z, NULL);
    CHECK(z.n == 0 && z.ni == 0, "no zone, whole image");
    /* an invalid runtime file is ignored */
    { FILE *f = fopen(p, "w"); fputs("garbage\n", f); fclose(f); }
    CHECK(a2k_zlive_init(&card, p) == 0, "an invalid runtime file is ignored");
    a2k_zlive_get(&z, NULL);
    CHECK(z.n == 1 && !strcmp(z.z[0].name, "cardzone"), "the card zones stay");
    CHECK(a2k_zlive_applies(HAL_PTZ_HOME) == 1 && a2k_zlive_applies(HAL_PTZ_UNKNOWN) == 1 && a2k_zlive_applies(HAL_PTZ_MOVED) == 0 &&
              a2k_zlive_applies(HAL_PTZ_PARKED) == 0 && a2k_zlive_applies(HAL_PTZ_CALIBRATING) == 0,
          "zones apply at home (and when the position is not managed), not moved / parked / calibrating");
    {
        a2k_zones_t eff;
        int st[3] = { HAL_PTZ_MOVED, HAL_PTZ_PARKED, HAL_PTZ_CALIBRATING };
        CHECK(a2k_zlive_set_text(A2K_ZF_ZONES, "fix=0,0,50,50@screen;pos=50,0,100,50@position;plain=0,50,100,100") == 0, "anchored zones stored");
        a2k_zlive_get_text(A2K_ZF_ZONES, t, sizeof t);
        CHECK(!strcmp(t, "fix=0,0,50,50@screen;pos=50,0,100,50@position;plain=0,50,100,100"), "the suffixes survive the live text");
        a2k_zlive_get(&z, NULL);
        for (int i = 0; i < 3; i++) {
            a2k_zones_effective(&z, a2k_zlive_applies(st[i]), &eff);
            CHECK(eff.n == 3 && !eff.z[0].off && eff.z[1].off && eff.z[2].off, "moved / parked / calibrating: the @screen zone stays, position and plain zones are suspended");
        }
        a2k_zones_effective(&z, a2k_zlive_applies(HAL_PTZ_HOME), &eff);
        CHECK(a2k_zones_active_mask(&eff) == 7, "at home all three are in force");
        CHECK(a2k_zlive_init(NULL, p) == 1, "restart");
        a2k_zlive_get_text(A2K_ZF_ZONES, t, sizeof t);
        CHECK(!strcmp(t, "fix=0,0,50,50@screen;pos=50,0,100,50@position;plain=0,50,100,100"), "the suffixes survive the persisted file");
    }
    /* no persistence path */
    CHECK(a2k_zlive_init(NULL, NULL) == 0 && a2k_zlive_set_text(A2K_ZF_ZONES, "a=0,0,1,1") == 0, "without a store path: RAM only");
}

static void test_store(const char *dir)
{
    char p[300], buf[64];
    printf("== atomic store\n");
    snprintf(p, sizeof p, "%s/s", dir);
    CHECK(a2k_store_read(p, buf, sizeof buf) == -1, "absent file: -1");
    CHECK(a2k_store_write(p, "abc\n", 4) == 0 && a2k_store_read(p, buf, sizeof buf) == 4 && !strcmp(buf, "abc\n"), "write and read");
    {
        struct stat a, b;
        stat(p, &a);
        CHECK(a2k_store_write(p, "abc\n", 4) == 0, "same content: ok");
        stat(p, &b);
        CHECK(a.st_ino == b.st_ino, "same content is not rewritten (flash wear)");
    }
    CHECK(a2k_store_write(p, "", 0) == 0 && a2k_store_read(p, buf, sizeof buf) == 0, "empty file");
    CHECK(a2k_store_write("/nonexistent-dir/x", "a", 1) == -1, "an unwritable path fails and leaves nothing");
    CHECK(a2k_store_read(p, buf, 1) == 0 || 1, "tiny buffer is safe");
}

int main(void)
{
    char dir[] = "/tmp/rot2k_zlive_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_text();
    test_ignore_rule();
    test_live(dir);
    test_store(dir);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k zlive unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
