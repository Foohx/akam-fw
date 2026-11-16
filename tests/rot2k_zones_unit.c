/* Host unit test of the detection zones of the Rotatable 2K app: the strict parser of akam/zones (accepted and refused
 * inputs, fuzzed), the file reader, the feet point membership rule, the zone aware observation (zone_mode any / all) and
 * the per-zone hysteresis (akam2k_zones.c + akam2k_detstate.c, pure code). Build with -fsanitize=address,undefined: a crash
 * or an out of bounds read is the failure. Prints "rot2k zones unit: N failed". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "akam2k_detstate.h"
#include "akam2k_zones.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int parse(const char *t, a2k_zones_t *z) { return a2k_zones_parse(t, strlen(t), z); }

static void test_parser(void)
{
    a2k_zones_t z;
    size_t i;
    static const char *const bad[] = {
        "porch=0,0,50,50\nporch=10,10,20,20\n",   /* duplicate name */
        "porch=0,0,50,50\nporch=0,0,50,50\n",
        "a=0,0,10,10\nb=0,0,10,10\nc=0,0,10,10\nd=0,0,10,10\ne=0,0,10,10\nf=0,0,10,10\ng=0,0,10,10\nh=0,0,10,10\ni=0,0,10,10\n", /* nine zones */
        "ignore_a=0,0,10,10\nignore_a=1,1,5,5\n", "ignore_zone_mode=0,0,10,10\n", "ignore_=0,0,10,10\n", "ignore_a=5,5,5,5\n",
        "ignore_a=0,0,1,1\nignore_b=0,0,1,1\nignore_c=0,0,1,1\nignore_d=0,0,1,1\nignore_e=0,0,1,1\nignore_f=0,0,1,1\nignore_g=0,0,1,1\nignore_h=0,0,1,1\nignore_i=0,0,1,1\n", /* nine ignore */
        "ignore_x=0,0,1,1\nz=0,0,1,1\nz=0,0,1,1\n",
        "a=0,0,10,10\n\nb=0,0,10,10\n",           /* blank line */
        "\n",                                     /* a blank line alone */
        "porch=0,0,50\n", "porch=0,0,50,50,60\n", "porch=0,0,50,\n", "porch=,0,50,50\n", "porch=0,0,,50\n",
        "porch=0,0,50,50 \n", "porch= 0,0,50,50\n", "porch=0, 0,50,50\n", "porch =0,0,50,50\n",
        "porch=0;0;50;50\n", "porch=-1,0,50,50\n", "porch=+1,0,50,50\n", "porch=0,0,101,50\n", "porch=0,0,50,101\n",
        "porch=0,0,1000,50\n", "porch=0,0,0050,50\n", "porch=1.5,0,50,50\n", "porch=0x1,0,50,50\n",
        "porch=50,0,50,50\n", "porch=60,0,50,50\n", "porch=0,50,50,50\n", "porch=0,60,50,50\n", /* empty or inverted */
        "Porch=0,0,50,50\n", "1porch=0,0,50,50\n", "_porch=0,0,50,50\n", "por-ch=0,0,50,50\n", "porch.x=0,0,50,50\n",
        "=0,0,50,50\n", "abcdefghijklmnopq=0,0,50,50\n",    /* 17 characters */
        "score=0,0,50,50\n", "zone_mode=0,0,50,50\n",     /* reserved names */
        "zone_mode=ANY\n", "zone_mode=\n", "zone_mode=anyx\n", "zone_mode=any\nzone_mode=all\n", "zone_mode=1\n",
        "porch\n", "porch=\n", "# comment\n", "porch=0,0,50,50 # c\n", "porch=0,0,50,50;\n",
        "enabled=1\n", "porch=0,0,50,50\r\r\n", "porch=0,0,50,50\n\n",
    };
    printf("== zones file parser\n");
    CHECK(parse("", &z) == 0 && z.n == 0 && z.mode == A2K_ZONE_MODE_ANY, "an empty file is valid: no zone, whole image");
    CHECK(parse("porch=0,0,50,50\n", &z) == 0 && z.n == 1 && !strcmp(z.z[0].name, "porch") && z.z[0].x1 == 0 && z.z[0].y1 == 0 &&
          z.z[0].x2 == 50 && z.z[0].y2 == 50 && z.mode == A2K_ZONE_MODE_ANY, "one zone, default mode any");
    CHECK(parse("porch=0,0,50,50", &z) == 0 && z.n == 1, "the last line needs no LF");
    CHECK(parse("porch=0,0,50,50\r\ndrive=50,0,100,100\r\n", &z) == 0 && z.n == 2 && !strcmp(z.z[1].name, "drive") && z.z[1].x2 == 100,
          "CRLF is tolerated");
    CHECK(parse("a=0,0,1,1\nb2=0,0,100,100\nc_3=99,99,100,100\nzone_mode=all\nd=1,2,3,4\n", &z) == 0 && z.n == 4 &&
          z.mode == A2K_ZONE_MODE_ALL && !strcmp(z.z[2].name, "c_3") && z.z[2].x1 == 99, "four zones, zone_mode all, any line order");
    CHECK(parse("a=0,0,1,1\nb=0,0,1,1\nc=0,0,1,1\nd=0,0,1,1\ne=0,0,1,1\nf=0,0,1,1\ng=0,0,1,1\nh=0,0,1,1\n", &z) == 0 && z.n == 8 && A2K_ZONE_MAX == 8,
          "eight zones are accepted (the maximum)");
    CHECK(parse("ignore_door=10,10,30,30\nporch=0,0,50,50\nignore_sky=0,0,100,20\n", &z) == 0 && z.n == 1 && z.ni == 2 && !strcmp(z.ig[0].name, "door") &&
          z.ig[1].y2 == 20, "ignore rectangles: prefix stripped, any order");
    CHECK(parse("zone_mode=any\nporch=10,20,30,40\n", &z) == 0 && z.mode == A2K_ZONE_MODE_ANY && z.n == 1, "zone_mode first");
    CHECK(parse("abcdefghijklmnop=0,0,50,50\n", &z) == 0 && strlen(z.z[0].name) == 16, "a 16 character name is the longest");
    CHECK(parse("zone_mode=all\n", &z) == 0 && z.n == 0 && z.mode == A2K_ZONE_MODE_ALL, "zone_mode alone: valid, still the whole image");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        a2k_zones_t keep;
        memset(&keep, 0x7e, sizeof keep);
        z = keep;
        if (parse(bad[i], &z) == 0 || memcmp(&z, &keep, sizeof z) != 0) {
            printf("  FAIL - refused (and the output untouched): %s", bad[i]);
            fails++;
        }
    }
    printf("  ok   - %zu malformed files are refused and leave the output untouched\n", sizeof bad / sizeof bad[0]);
    CHECK(a2k_zones_parse("a=0,0,1,1\n\0b=0,0,1,1\n", 22, &z) == -1, "a NUL byte is refused");
    CHECK(a2k_zones_parse("a=0,0,1,1\n", A2K_ZONES_FILE_MAX + 1, &z) == -1 && a2k_zones_parse(NULL, 0, &z) == -1 &&
          a2k_zones_parse("", 0, NULL) == -1, "over the size cap, NULL text, NULL out: refused");
    {
        char big[A2K_ZONES_FILE_MAX];
        memset(big, '\n', sizeof big);
        CHECK(a2k_zones_parse(big, sizeof big, &z) == -1, "a file of blank lines at the cap is refused");
    }
}

static unsigned rs = 0x1234567u;
static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static void test_fuzz(void)
{
    static const char *const seeds[] = { "porch=0,0,50,50\n", "a=0,0,10,10\nb=10,10,20,20\nzone_mode=all\n",
        "drive=50,40,100,100\r\nporch=0,40,50,100\r\n", "zone_mode=any\n" };
    static const char alpha[] = "abcz09_=,\n\r -+.#xX\0\xff";
    int acc = 0, ref = 0, i;
    printf("== parser fuzz (ASan/UBSan: a crash is the failure)\n");
    for (i = 0; i < 300000; i++) {
        char b[A2K_ZONES_FILE_MAX + 8];
        size_t n;
        a2k_zones_t z;
        if (rnd() % 3 == 0) { /* pure noise, sometimes over the cap */
            n = rnd() % (A2K_ZONES_FILE_MAX + 8);
            for (size_t k = 0; k < n; k++)
                b[k] = (char)rnd();
        } else { /* a valid seed with a few mutations */
            const char *sd = seeds[rnd() % 4];
            n = strlen(sd);
            memcpy(b, sd, n);
            for (int m = (int)(rnd() % 4); m > 0 && n; m--) {
                size_t at = rnd() % n;
                switch (rnd() % 4) {
                case 0: b[at] = alpha[rnd() % (sizeof alpha - 1)]; break;
                case 1: if (n + 1 < sizeof b) { memmove(b + at + 1, b + at, n - at); b[at] = alpha[rnd() % (sizeof alpha - 1)]; n++; } break;
                case 2: memmove(b + at, b + at + 1, n - at - 1); n--; break;
                default: n = at; break;
                }
            }
        }
        if (a2k_zones_parse(b, n, &z) == 0) {
            acc++;
            /* every accepted result is well formed */
            if (z.n < 0 || z.n > A2K_ZONE_MAX || (z.mode != 0 && z.mode != 1)) { printf("  FAIL - bad accepted result\n"); fails++; }
            for (int k = 0; k < z.n; k++)
                if (z.z[k].x1 < 0 || z.z[k].x2 > 100 || z.z[k].y1 < 0 || z.z[k].y2 > 100 || z.z[k].x1 >= z.z[k].x2 ||
                    z.z[k].y1 >= z.z[k].y2 || !z.z[k].name[0] || strlen(z.z[k].name) > A2K_ZONE_NAME_MAX) {
                    printf("  FAIL - accepted a zone out of range\n");
                    fails++;
                }
        } else {
            ref++;
        }
    }
    CHECK(acc > 100 && ref > 100, "fuzz ran with both accepted and refused inputs");
    printf("         (%d accepted, %d refused)\n", acc, ref);
}

static void write_file(const char *path, const char *text, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fwrite(text, 1, n, f);
    fclose(f);
}

static void test_load(const char *dir)
{
    char p[300], q[300], big[A2K_ZONES_FILE_MAX + 100];
    a2k_zones_t z;
    printf("== zones file reader\n");
    snprintf(p, sizeof p, "%s/zones", dir);
    CHECK(a2k_zones_load(p, &z) == 1 && z.n == 0, "absent file: 1, whole image");
    write_file(p, "porch=0,40,50,100\ndrive=50,40,100,100\nzone_mode=all\n", 52);
    CHECK(a2k_zones_load(p, &z) == 0 && z.n == 2 && z.mode == A2K_ZONE_MODE_ALL, "valid file loaded");
    write_file(p, "porch=0,40,50\n", 14);
    z.n = 3;
    CHECK(a2k_zones_load(p, &z) == -1 && z.n == 0, "invalid file: -1 and the output is zeroed (whole image)");
    write_file(p, "", 0);
    CHECK(a2k_zones_load(p, &z) == 0 && z.n == 0, "empty file: valid, no zone");
    memset(big, 'a', sizeof big);
    write_file(p, big, sizeof big);
    CHECK(a2k_zones_load(p, &z) == -1, "a file over the cap is refused");
    snprintf(q, sizeof q, "%s/fifo", dir);
    if (mkfifo(q, 0600) == 0)
        CHECK(a2k_zones_load(q, &z) == -1, "a FIFO is refused without being opened (no block)");
    CHECK(a2k_zones_load(dir, &z) == -1, "a directory is refused");
    snprintf(q, sizeof q, "%s/link", dir);
    write_file(p, "porch=0,0,50,50\n", 16);
    if (symlink(p, q) == 0)
        CHECK(a2k_zones_load(q, &z) == -1, "a symbolic link is refused");
}

static struct a2k_det_box box(int x1, int y1, int x2, int y2, int score)
{
    struct a2k_det_box b = { A2K_DET_CLASS_PERSON, score, x1, y1, x2, y2 };
    return b;
}

static void test_membership(void)
{
    a2k_zones_t z;
    struct a2k_det_box b;
    printf("== zone membership: the feet point (bottom centre of the box)\n");
    /* porch: left half, lower 60 % ; 320x192 frame: x 0..160, y 76.8..192 */
    parse("porch=0,40,50,100\ndrive=50,40,100,100\n", &z);
    b = box(40, 20, 100, 150, 800); /* feet (70, 151): left half, y 79 % */
    CHECK(a2k_zones_hit(&z, &b) == 1, "feet in the left zone");
    b = box(200, 20, 280, 150, 800);
    CHECK(a2k_zones_hit(&z, &b) == 2, "feet in the right zone");
    b = box(140, 20, 200, 150, 800); /* centre x = 170.5: right of the 50 % line (160) */
    CHECK(a2k_zones_hit(&z, &b) == 2, "the centre of the box decides, not its width");
    /* a box whose head is high but whose feet are above the zones' top edge (y < 40 %) counts in neither */
    b = box(40, 0, 100, 60, 800); /* y2 = 60 -> 61/192 = 31.8 % */
    CHECK(a2k_zones_hit(&z, &b) == 0, "feet above the zones: no zone, although the body overlaps them");
    /* the head or an arm leaning into the other zone does not flip it */
    b = box(100, 20, 220, 150, 800); /* wide box overlapping both zones, feet x = 160.5 -> 50.2 % */
    CHECK(a2k_zones_hit(&z, &b) == 2, "a box spanning both zones belongs to the one its feet are in");
    /* edges are included; the zones touch at x = 50 % */
    b = box(150, 20, 169, 150, 800); /* (150 + 169 + 1) / 2 = 160 -> exactly 50 % */
    CHECK(a2k_zones_hit(&z, &b) == 3, "exactly on the shared edge: both zones");
    /* a person cut by the bottom edge: y2 is clamped to 191, the feet point is the image bottom = 100 % */
    b = box(40, 20, 100, 191, 800);
    CHECK(a2k_zones_hit(&z, &b) == 1, "a box clamped at the bottom edge still lands in a zone that reaches 100 %");
    parse("top=0,0,100,50\n", &z);
    b = box(40, 20, 100, 191, 800);
    CHECK(a2k_zones_hit(&z, &b) == 0, "...and not in a zone that stops at 50 %");
    b = box(0, 0, 319, 95, 800); /* feet y = 96/192 = 50 % exactly */
    CHECK(a2k_zones_hit(&z, &b) == 1, "bottom edge of the zone is included");
    b = box(0, 0, 319, 96, 800);
    CHECK(a2k_zones_hit(&z, &b) == 0, "one pixel below it: out");
    memset(&z, 0, sizeof z);
    b = box(0, 0, 319, 191, 800);
    CHECK(a2k_zones_hit(&z, &b) == 0, "no zone configured: mask 0 (the caller uses the whole image path)");
}

static void feed(struct a2k_det_obs *o, const a2k_zones_t *z, const struct a2k_det_box *boxes, int n)
{
    a2k_det_obs_reset(o);
    for (int i = 0; i < n; i++) {
        if (boxes[i].cls == A2K_DET_CLASS_PERSON) {
            unsigned hit = a2k_zones_hit(z, &boxes[i]);
            if (hit)
                a2k_det_obs_add_z(o, &boxes[i], hit);
        } else {
            a2k_det_obs_add(o, &boxes[i]);
        }
    }
    a2k_det_obs_finish(o, z->n, z->mode == A2K_ZONE_MODE_ALL);
}

static void test_obs_and_hysteresis(void)
{
    a2k_zones_t z;
    struct a2k_det_obs o;
    struct a2k_det_box left = box(40, 20, 100, 150, 700), right = box(200, 20, 280, 150, 900), out = box(40, 0, 100, 60, 990);
    struct a2k_det_box head = { A2K_DET_CLASS_HEAD, 600, 60, 20, 80, 40 };
    struct a2k_det_box two[2];
    struct a2k_det_cfg cfg = { 2, 3000 };
    a2k_det_t g;
    a2k_zdet_t zd;
    struct a2k_det_event ev, zev[A2K_ZONE_MAX];
    int64_t t = 1000;
    unsigned ch;

    printf("== zone aware observation (zone_mode any / all)\n");
    parse("porch=0,40,50,100\ndrive=50,40,100,100\n", &z);
    feed(&o, &z, &out, 1);
    CHECK(!o.person && o.zmask == 0, "a person outside every zone: nobody");
    feed(&o, &z, &left, 1);
    CHECK(o.person && o.zmask == 1 && o.best.score == 700 && o.zbest[0].score == 700, "left only: zone 0, global person");
    two[0] = left; two[1] = right;
    feed(&o, &z, two, 2);
    CHECK(o.person && o.zmask == 3 && o.best.score == 900 && o.zbest[0].score == 700 && o.zbest[1].score == 900,
          "both zones: best score overall, per-zone best boxes");
    two[0] = out; two[1] = left;
    feed(&o, &z, two, 2);
    CHECK(o.person && o.best.score == 700, "the strongest box outside the zones does not win");
    two[0] = head; two[1] = out;
    feed(&o, &z, two, 2);
    CHECK(!o.person && o.heads == 1 && o.head_score == 600, "heads are extra information only, they never open a zone");
    parse("porch=0,40,50,100\ndrive=50,40,100,100\nzone_mode=all\n", &z);
    feed(&o, &z, &left, 1);
    CHECK(!o.person && o.zmask == 1, "zone_mode all: one zone occupied is not enough (zone state still tracked)");
    two[0] = left; two[1] = right;
    feed(&o, &z, two, 2);
    CHECK(o.person && o.zmask == 3, "zone_mode all: every zone occupied");

    printf("== per-zone hysteresis (same K and M as the global one)\n");
    parse("porch=0,40,50,100\ndrive=50,40,100,100\n", &z);
    a2k_det_init(&g, &cfg);
    a2k_zdet_init(&zd, z.n, &cfg);
    ev.type = 0;
    /* inference 1: person in the porch only */
    feed(&o, &z, &left, 1);
    CHECK(!a2k_det_update(&g, t, &o, &ev) && a2k_zdet_update(&zd, t, &o, zev) == 0, "first positive inference: no event (K = 2)");
    t += 200;
    CHECK(a2k_det_update(&g, t, &o, &ev) == 1 && ev.type == A2K_DET_EV_ON, "second: global ON");
    ch = a2k_zdet_update(&zd, t, &o, zev);
    CHECK(ch == 1 && zev[0].type == A2K_DET_EV_ON && a2k_zdet_mask(&zd) == 1, "zone 0 ON, zone 1 untouched");
    /* the person walks to the drive */
    t += 200;
    feed(&o, &z, &right, 1);
    a2k_det_update(&g, t, &o, &ev);
    CHECK(a2k_zdet_update(&zd, t, &o, zev) == 0 && a2k_zdet_mask(&zd) == 1, "first inference in the drive: zone 1 not ON yet (K = 2), the porch holds (M)");
    t += 200;
    a2k_det_update(&g, t, &o, &ev);
    ch = a2k_zdet_update(&zd, t, &o, zev);
    CHECK(ch == 2 && zev[1].type == A2K_DET_EV_ON && a2k_zdet_mask(&zd) == 3 && g.present, "zone 1 ON while the porch is still inside its 3 s hold");
    t += 2500;
    a2k_det_update(&g, t, &o, &ev);
    CHECK(a2k_zdet_update(&zd, t, &o, zev) == 0 && a2k_zdet_mask(&zd) == 3, "2.9 s after the last porch inference: still ON");
    t += 200;
    a2k_det_update(&g, t, &o, &ev);
    ch = a2k_zdet_update(&zd, t, &o, zev);
    CHECK(ch == 1 && zev[0].type == A2K_DET_EV_OFF && a2k_zdet_mask(&zd) == 2, "past 3 s: the porch goes OFF, the drive stays");
    CHECK(zev[0].present_ms > 0, "the OFF event carries the presence time");
    CHECK(g.present, "global still ON (the drive)");
    /* the person leaves the frame */
    a2k_det_obs_reset(&o);
    t += 3100;
    CHECK(a2k_det_update(&g, t, &o, &ev) == 1 && ev.type == A2K_DET_EV_OFF, "global OFF");
    ch = a2k_zdet_update(&zd, t, &o, zev);
    CHECK(ch == 2 && zev[1].type == A2K_DET_EV_OFF && a2k_zdet_mask(&zd) == 0, "drive OFF");
    /* zones beyond n are never touched */
    a2k_zdet_init(&zd, 0, &cfg);
    CHECK(a2k_zdet_update(&zd, t, &o, zev) == 0 && a2k_zdet_mask(&zd) == 0, "no zones: nothing to update");
    a2k_zdet_init(&zd, 9, &cfg);
    CHECK(zd.n == A2K_ZONE_MAX, "more zones than the maximum are clamped");

    printf("== state file lines\n");
    {
        struct a2k_det_snapshot s;
        char buf[900];
        a2k_det_t d;
        a2k_det_init(&d, &cfg);
        memset(&s, 0, sizeof s);
        s.status = "run";
        s.det = &d;
        CHECK(a2k_det_state_format(buf, sizeof buf, &s) > 0 && !strstr(buf, "zones="), "no zone: the file is exactly the old format");
        s.zn = 2;
        s.zname[0] = "porch";
        s.zname[1] = "drive";
        s.zmask = 2;
        s.zall = 1;
        CHECK(a2k_det_state_format(buf, sizeof buf, &s) > 0 && strstr(buf, "zones=2\nzone_mode=all\nzone_porch=0\nzone_drive=1\n"),
              "zones, zone_mode and one zone_<name>=0|1 line each");
        CHECK(a2k_det_state_format(buf, 200, &s) == -1, "a too small buffer is refused with zones too");
    }
}

static void test_anchors(void)
{
    a2k_zones_t z, e;
    struct a2k_det_box b;
    struct a2k_det_obs o;
    char buf[300];
    printf("== anchors: @screen / @position\n");
    CHECK(parse("a=0,0,50,100@screen\nb=50,0,100,100@position\nc=0,0,10,10\nignore_d=0,0,10,10@screen\nignore_e=20,20,30,30@position\n", &z) == 0 &&
              z.n == 3 && z.z[0].anchor == A2K_ZA_SCREEN && z.z[1].anchor == A2K_ZA_POSITION && z.z[2].anchor == A2K_ZA_NONE &&
              z.ni == 2 && z.ig[0].anchor == A2K_ZA_SCREEN && z.ig[1].anchor == A2K_ZA_POSITION,
          "anchors parse on zones and ignore rectangles; no suffix = none");
    CHECK(parse("a=0,0,50,100@\n", &z) == -1 && parse("a=0,0,50,100@Screen\n", &z) == -1 && parse("a=0,0,50,100@screen@screen\n", &z) == -1 &&
              parse("a=0,0,50,100@screenx\n", &z) == -1 && parse("a=0,0,50,100 @screen\n", &z) == -1 && parse("a=@screen\n", &z) == -1 &&
              parse("a=0,0,50@screen\n", &z) == -1 && parse("a=50,0,0,100@screen\n", &z) == -1 && parse("ignore_a=0,0,10,10@home\n", &z) == -1,
          "a bad or repeated suffix, or a bad rectangle with a suffix, makes the file invalid");
    parse("a=0,0,50,100@screen\nb=50,0,100,100@position\nc=0,0,10,10\nzone_mode=all\nignore_d=0,0,10,10@screen\n", &z);
    CHECK(a2k_zones_format(&z, A2K_ZF_FILE, '\n', buf, sizeof buf) > 0 &&
              !strcmp(buf, "a=0,0,50,100@screen\nb=50,0,100,100@position\nc=0,0,10,10\nzone_mode=all\nignore_d=0,0,10,10@screen\n"),
          "the file form round-trips the suffixes");
    CHECK(a2k_zones_format(&z, A2K_ZF_ZONES, ';', buf, sizeof buf) > 0 && !strcmp(buf, "a=0,0,50,100@screen;b=50,0,100,100@position;c=0,0,10,10;zone_mode=all") &&
              a2k_zones_format(&z, A2K_ZF_IGNORE, ';', buf, sizeof buf) > 0 && !strcmp(buf, "d=0,0,10,10@screen"),
          "the one-line text forms (Home Assistant) carry the suffixes");
    CHECK(a2k_zones_set_text(&z, A2K_ZF_ZONES, "x=0,0,10,10@screen;y=10,10,20,20@position") == 0 && z.n == 2 && z.z[0].anchor == A2K_ZA_SCREEN &&
              z.z[1].anchor == A2K_ZA_POSITION && z.ni == 1 && a2k_zones_set_text(&z, A2K_ZF_IGNORE, "q=1,1,2,2@screen") == 0 &&
              z.ig[0].anchor == A2K_ZA_SCREEN && a2k_zones_set_text(&z, A2K_ZF_ZONES, "x=0,0,10,10@nowhere") == -1,
          "the text edits take the suffixes");

    parse("a=0,0,50,100@screen\nb=50,0,100,100@position\nc=0,0,10,10\nignore_d=0,0,100,10@screen\nignore_e=0,90,100,100@position\n", &z);
    a2k_zones_effective(&z, 1, &e);
    CHECK(e.n == 3 && !e.z[0].off && !e.z[1].off && !e.z[2].off && a2k_zones_active_mask(&e) == 7 && !e.ig[0].off && !e.ig[1].off,
          "at home everything is in force");
    a2k_zones_effective(&z, 0, &e);
    CHECK(e.n == 3 && !e.z[0].off && e.z[1].off && e.z[2].off && a2k_zones_active_mask(&e) == 1 && !e.ig[0].off && e.ig[1].off,
          "moved: only the @screen zone and ignore rectangle stay, the indexes do not shift");
    b = box(40, 20, 60, 150, 800); /* feet (50.5, 151): x 31.6 %, y 79 % : in a, outside ignore d (y 0..10) and e (y 90..100) */
    CHECK(a2k_zones_hit(&e, &b) == 1, "an anchored zone is active while the camera is moved");
    b = box(200, 20, 220, 150, 800); /* feet x 65.9 %: zone b, suspended */
    CHECK(a2k_zones_hit(&e, &b) == 0, "a position zone is suspended while moved");
    a2k_zones_effective(&z, 1, &e);
    CHECK(a2k_zones_hit(&e, &b) == 2, "...and in force again at home");
    b = box(10, 150, 20, 190, 800); /* feet y 99.5 %, x 4.7 %: in ignore e (position) and zone a / c? c is 0..10 x, y 0..10: no */
    CHECK(a2k_zones_ignored(&e, &b) == 1, "a position ignore rectangle ignores at home");
    a2k_zones_effective(&z, 0, &e);
    CHECK(a2k_zones_ignored(&e, &b) == 0, "...and stops while moved");
    b = box(10, 0, 20, 10, 800); /* feet y 5.7 %: ignore d (screen) */
    CHECK(a2k_zones_ignored(&e, &b) == 1, "a @screen ignore rectangle ignores while moved");

    parse("b=50,0,100,100@position\nc=0,0,10,10\nignore_e=0,90,100,100\n", &z);
    a2k_zones_effective(&z, 0, &e);
    CHECK(e.n == 0 && e.mode == A2K_ZONE_MODE_ANY && a2k_zones_active_mask(&e) == 0 && e.ni == 1 && e.ig[0].off,
          "moved with only position zones: no zone left, the whole image counts (as without anchors)");
    /* zone_mode all with a suspended zone: only the zones in force are required */
    parse("a=0,0,50,100@screen\nb=50,0,100,100\nzone_mode=all\n", &z);
    a2k_zones_effective(&z, 0, &e);
    a2k_det_obs_reset(&o);
    b = box(40, 20, 60, 150, 800);
    a2k_det_obs_add_z(&o, &b, a2k_zones_hit(&e, &b));
    a2k_det_obs_finish_mask(&o, a2k_zones_active_mask(&e), e.mode == A2K_ZONE_MODE_ALL);
    CHECK(o.person == 1, "zone_mode all while moved: the one zone in force is enough");
    a2k_zones_effective(&z, 1, &e);
    a2k_det_obs_reset(&o);
    a2k_det_obs_add_z(&o, &b, a2k_zones_hit(&e, &b));
    a2k_det_obs_finish_mask(&o, a2k_zones_active_mask(&e), e.mode == A2K_ZONE_MODE_ALL);
    CHECK(o.person == 0, "zone_mode all at home: both zones are required");
}

int main(void)
{
    char dir[] = "/tmp/rot2k_zones_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_parser();
    test_fuzz();
    test_load(dir);
    test_membership();
    test_anchors();
    test_obs_and_hysteresis();
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k zones unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
