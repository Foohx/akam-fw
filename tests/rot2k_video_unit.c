/* Host unit test of the video settings (akam2k_vidcfg.c rules + akam2k_video.c live copy): defaults unchanged, ranges, strict parsing, the runtime file
 * (only the keys that differ from the base, atomic, invalid = ignored), set / pending / applied / failed re-open, boot fallback. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_store.h"
#include "akam2k_video.h"

static int fails;
void a2k_log(const char *fmt, ...) { (void)fmt; }
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static int notes;
static void note(void) { notes++; }

static void test_rules(void)
{
    a2k_vid_t m, s, t;
    int v, lo, hi;
    char k[40], txt[16];
    printf("== defaults, ranges, parsing\n");
    a2k_vid_defaults(0, 2000, 30, &m);
    a2k_vid_defaults(1, 512, 30, &s);
    CHECK(m.v[A2K_VP_KBPS] == 2000 && m.v[A2K_VP_MAXKBPS] == 0 && a2k_vid_eff_max_kbps(&m) == 2500 && m.v[A2K_VP_FPS] == 15 && m.v[A2K_VP_MINQP] == 32 &&
              m.v[A2K_VP_MAXQP] == 51 && m.v[A2K_VP_GOP] == 30 && m.v[A2K_VP_RC] == A2K_RC_VBR,
          "main defaults: 2000 kbit/s, peak 1.25x = 2500, 15 fps, VBR, QP 32..51, GOP 30 (as the encoder ran before)");
    CHECK(s.v[A2K_VP_KBPS] == 512 && a2k_vid_eff_max_kbps(&s) == 640 && s.v[A2K_VP_MAXQP] == 50 && s.v[A2K_VP_MINQP] == 32 && s.v[A2K_VP_FPS] == 15 && s.v[A2K_VP_RC] == A2K_RC_VBR,
          "sub defaults: 512 kbit/s, peak 640, QP 32..50");
    CHECK(a2k_vid_valid(0, &m) && a2k_vid_valid(1, &s), "the defaults are valid");
    CHECK(A2K_RC_CBR == 0 && A2K_RC_VBR == 1 && A2K_RC_AVBR == 4, "rc values are the vendor rc_mode numbers");
    CHECK(a2k_vid_range(0, A2K_VP_KBPS, &lo, &hi) == 0 && lo == 64 && hi == 8000 && a2k_vid_range(1, A2K_VP_KBPS, &lo, &hi) == 0 && lo == 32 && hi == 1000 &&
              a2k_vid_range(2, 0, &lo, &hi) == -1 && a2k_vid_range(0, A2K_VP_N, &lo, &hi) == -1,
          "ranges per stream; bad index refused");
    CHECK(a2k_vid_value_ok(0, A2K_VP_KBPS, 64) && !a2k_vid_value_ok(0, A2K_VP_KBPS, 63) && a2k_vid_value_ok(0, A2K_VP_KBPS, 8000) && !a2k_vid_value_ok(0, A2K_VP_KBPS, 8001) &&
              a2k_vid_value_ok(1, A2K_VP_KBPS, 32) && !a2k_vid_value_ok(1, A2K_VP_KBPS, 31) && !a2k_vid_value_ok(1, A2K_VP_KBPS, 1001),
          "bit rate edges");
    CHECK(a2k_vid_value_ok(0, A2K_VP_MAXKBPS, 0) && !a2k_vid_value_ok(0, A2K_VP_MAXKBPS, 63) && a2k_vid_value_ok(0, A2K_VP_MAXKBPS, 12000) && !a2k_vid_value_ok(0, A2K_VP_MAXKBPS, 12001) &&
              a2k_vid_value_ok(1, A2K_VP_MAXKBPS, 1500) && !a2k_vid_value_ok(1, A2K_VP_MAXKBPS, 1501) && !a2k_vid_value_ok(0, A2K_VP_KBPS, 0),
          "peak: 0 = automatic or a rate; the target is never 0");
    CHECK(a2k_vid_value_ok(0, A2K_VP_FPS, 1) && a2k_vid_value_ok(0, A2K_VP_FPS, 15) && !a2k_vid_value_ok(0, A2K_VP_FPS, 0) && !a2k_vid_value_ok(0, A2K_VP_FPS, 16) &&
              a2k_vid_value_ok(0, A2K_VP_MINQP, 10) && !a2k_vid_value_ok(0, A2K_VP_MINQP, 9) && a2k_vid_value_ok(0, A2K_VP_MAXQP, 51) && !a2k_vid_value_ok(0, A2K_VP_MAXQP, 52) &&
              a2k_vid_value_ok(0, A2K_VP_GOP, 1) && a2k_vid_value_ok(0, A2K_VP_GOP, 300) && !a2k_vid_value_ok(0, A2K_VP_GOP, 0) && !a2k_vid_value_ok(0, A2K_VP_GOP, 301),
          "fps, QP and GOP edges");
    CHECK(a2k_vid_value_ok(0, A2K_VP_RC, 0) && a2k_vid_value_ok(0, A2K_VP_RC, 1) && a2k_vid_value_ok(0, A2K_VP_RC, 4) && !a2k_vid_value_ok(0, A2K_VP_RC, 2) && !a2k_vid_value_ok(0, A2K_VP_RC, 3) &&
              !a2k_vid_value_ok(0, A2K_VP_RC, 5),
          "rc: cbr, vbr, avbr only (constant QP and low latency are not offered)");
    t = m;
    t.v[A2K_VP_MINQP] = 40;
    t.v[A2K_VP_MAXQP] = 39;
    CHECK(!a2k_vid_valid(0, &t), "min QP above max QP is invalid");
    t = m;
    t.v[A2K_VP_MAXKBPS] = 1999;
    CHECK(!a2k_vid_valid(0, &t), "a peak below the target is invalid");
    t.v[A2K_VP_MAXKBPS] = 2000;
    CHECK(a2k_vid_valid(0, &t) && a2k_vid_eff_max_kbps(&t) == 2000, "peak = target is fine (CBR-like)");
    t = m;
    t.v[A2K_VP_KBPS] = 8000;
    CHECK(a2k_vid_eff_max_kbps(&t) == 10000, "automatic peak follows the target");
    CHECK(a2k_vid_parse(A2K_VP_KBPS, "3000", &v) == 0 && v == 3000 && a2k_vid_parse(A2K_VP_KBPS, "00064", &v) == 0 && v == 64 && a2k_vid_parse(A2K_VP_KBPS, "", &v) == -1 &&
              a2k_vid_parse(A2K_VP_KBPS, "-1", &v) == -1 && a2k_vid_parse(A2K_VP_KBPS, "+1", &v) == -1 && a2k_vid_parse(A2K_VP_KBPS, " 1", &v) == -1 &&
              a2k_vid_parse(A2K_VP_KBPS, "1 ", &v) == -1 && a2k_vid_parse(A2K_VP_KBPS, "1e3", &v) == -1 && a2k_vid_parse(A2K_VP_KBPS, "123456", &v) == -1 &&
              a2k_vid_parse(A2K_VP_KBPS, "vbr", &v) == -1 && a2k_vid_parse(A2K_VP_KBPS, NULL, &v) == -1,
          "numbers: digits only, at most 5");
    CHECK(a2k_vid_parse(A2K_VP_RC, "vbr", &v) == 0 && v == A2K_RC_VBR && a2k_vid_parse(A2K_VP_RC, "cbr", &v) == 0 && v == A2K_RC_CBR && a2k_vid_parse(A2K_VP_RC, "avbr", &v) == 0 &&
              v == A2K_RC_AVBR && a2k_vid_parse(A2K_VP_RC, "VBR", &v) == -1 && a2k_vid_parse(A2K_VP_RC, "1", &v) == -1 && a2k_vid_parse(A2K_VP_RC, "vbr ", &v) == -1 &&
              a2k_vid_parse(A2K_VP_RC, "", &v) == -1,
          "rc: exactly one of three words");
    a2k_vid_key(1, A2K_VP_MAXKBPS, k, sizeof k);
    CHECK(!strcmp(k, "video1_max_kbps"), "key names");
    CHECK(a2k_vid_text(A2K_VP_RC, A2K_RC_AVBR, txt, sizeof txt) == 0 && !strcmp(txt, "avbr") && a2k_vid_text(A2K_VP_KBPS, 2000, txt, sizeof txt) == 0 && !strcmp(txt, "2000") &&
              a2k_vid_text(A2K_VP_KBPS, 2000, txt, 3) == -1,
          "value text");
}

static void test_file(void)
{
    a2k_vid_t base[2], out[2], cur[2];
    char buf[400];
    int n;
    printf("== runtime file form\n");
    a2k_vid_defaults(0, 2000, 30, &base[0]);
    a2k_vid_defaults(1, 512, 30, &base[1]);
    cur[0] = base[0];
    cur[1] = base[1];
    n = a2k_vid_format_file(base, cur, buf, sizeof buf);
    CHECK(n == 0 && buf[0] == '\0', "no difference: an empty file");
    cur[0].v[A2K_VP_KBPS] = 3000;
    cur[1].v[A2K_VP_RC] = A2K_RC_AVBR;
    cur[1].v[A2K_VP_FPS] = 10;
    n = a2k_vid_format_file(base, cur, buf, sizeof buf);
    CHECK(n > 0 && !strcmp(buf, "video0_kbps=3000\nvideo1_fps=10\nvideo1_rc=avbr\n"), "only the differing keys, in stream / parameter order");
    CHECK(a2k_vid_parse_file(buf, (size_t)n, base, out) == 0 && out[0].v[A2K_VP_KBPS] == 3000 && out[1].v[A2K_VP_RC] == A2K_RC_AVBR && out[1].v[A2K_VP_FPS] == 10 &&
              out[0].v[A2K_VP_FPS] == 15 && out[1].v[A2K_VP_KBPS] == 512,
          "round trip over the base");
    CHECK(a2k_vid_format_file(base, cur, buf, 10) == -1, "a too small buffer is refused");
    CHECK(a2k_vid_parse_file("", 0, base, out) == 0 && out[0].v[A2K_VP_KBPS] == 2000, "an empty file is the base");
#define BAD(t) (a2k_vid_parse_file(t, strlen(t), base, out) == -1)
    CHECK(BAD("video0_kbps=63\n") && BAD("video0_kbps=3000\nvideo0_kbps=3001\n") && BAD("video2_kbps=100\n") && BAD("video0_kbps\n") && BAD("video0_kbps=\n") &&
              BAD("video0_rc=VBR\n") && BAD("video0_rc=1\n") && BAD("video0_min_qp=40\nvideo0_max_qp=39\n") && BAD("video0_max_kbps=100\nvideo0_kbps=200\n") && BAD("junk\n") &&
              BAD("video0_kbps=3000\n\n") && BAD("video0_fps=16\n") && BAD(" video0_fps=10\n") && BAD("video0_fps =10\n") && BAD("video0_kbps=3000\r\n"),
          "invalid lines, duplicates, unknown keys, blank lines, spaces and inconsistent sets make the whole file invalid");
    {
        char big[A2K_STORE_MAX + 2];
        memset(big, 'a', sizeof big);
        CHECK(a2k_vid_parse_file(big, sizeof big, base, out) == -1, "a file over the store limit is refused");
    }
    CHECK(a2k_vid_parse_file("video0_kbps=3000", 16, base, out) == 0 && out[0].v[A2K_VP_KBPS] == 3000, "a last line without LF is accepted");
#undef BAD
}

static void test_live(const char *dir)
{
    char p[300], buf[A2K_STORE_MAX + 1], t[16];
    a2k_vid_t base[2], g, want;
    printf("== live copy: set, pending, applied, failed, persistence\n");
    snprintf(p, sizeof p, "%s/video", dir);
    a2k_vid_defaults(0, 2000, 30, &base[0]);
    a2k_vid_defaults(1, 512, 30, &base[1]);
    CHECK(a2k_vid_init(base, p) == 0, "no runtime file: the base is used");
    a2k_vid_set_notify(note);
    a2k_vid_get(0, &g);
    CHECK(g.v[A2K_VP_KBPS] == 2000 && !a2k_vid_pending(0, &want) && !a2k_vid_pending(1, &want), "nothing pending at start");
    CHECK(a2k_vid_set(0, A2K_VP_KBPS, "3000") == 0, "a bit rate change is accepted");
    CHECK(a2k_vid_pending(0, &want) && want.v[A2K_VP_KBPS] == 3000 && !a2k_vid_pending(1, &want), "it is pending for that stream only");
    CHECK(a2k_vid_get_text(0, A2K_VP_KBPS, t, sizeof t) == 0 && !strcmp(t, "3000"), "the entity text shows the new value at once");
    CHECK(a2k_store_read(p, buf, sizeof buf) > 0 && !strcmp(buf, "video0_kbps=3000\n"), "stored atomically, the differing key only");
    CHECK(a2k_vid_set(0, A2K_VP_FPS, "10") == 0 && a2k_vid_pending(0, &want) && want.v[A2K_VP_KBPS] == 3000 && want.v[A2K_VP_FPS] == 10, "a burst of changes is one pending re-open");
    a2k_vid_applied(0, 1, &want);
    CHECK(!a2k_vid_pending(0, &want) && notes == 0, "applied: nothing pending, nobody told (the command path publishes the value back itself)");
    CHECK(a2k_vid_set(0, A2K_VP_KBPS, "63") == -1 && a2k_vid_set(0, A2K_VP_KBPS, "x") == -1 && a2k_vid_set(0, A2K_VP_RC, "2") == -1 && a2k_vid_set(5, 0, "100") == -1 &&
              a2k_vid_set(0, 9, "100") == -1 && !a2k_vid_pending(0, &want),
          "out of range, not a number, a wrong rc word, a bad stream or parameter: refused, nothing changes");
    CHECK(a2k_vid_set(0, A2K_VP_MINQP, "52") == -1 && a2k_vid_set(0, A2K_VP_MINQP, "40") == 0 && a2k_vid_set(0, A2K_VP_MAXQP, "39") == -1 && a2k_vid_set(0, A2K_VP_MAXQP, "40") == 0,
          "an inconsistent set (min QP above max QP) is refused whole");
    (void)a2k_vid_pending(0, &want);
    a2k_vid_applied(0, 1, &want);
    CHECK(a2k_vid_set(0, A2K_VP_MAXKBPS, "2999") == -1 && a2k_vid_set(0, A2K_VP_MAXKBPS, "3000") == 0 && a2k_vid_set(0, A2K_VP_MAXKBPS, "0") == 0, "peak below the target refused; 0 = automatic again");
    (void)a2k_vid_pending(0, &want);
    a2k_vid_applied(0, 1, &want);
    CHECK(a2k_vid_set(0, A2K_VP_FPS, "10") == 0 && !a2k_vid_pending(0, &want), "setting the value already in force changes nothing");
    /* a change made while the encoder is being re-opened is not marked as applied: it stays pending */
    CHECK(a2k_vid_set(0, A2K_VP_GOP, "20") == 0 && a2k_vid_pending(0, &want) && a2k_vid_set(0, A2K_VP_GOP, "25") == 0, "a change during a re-open");
    a2k_vid_applied(0, 1, &want);
    CHECK(a2k_vid_pending(0, &want) && want.v[A2K_VP_GOP] == 25, "...is still pending after the first one was applied");
    a2k_vid_applied(0, 1, &want);
    CHECK(a2k_vid_set(0, A2K_VP_GOP, "30") == 0 && a2k_vid_pending(0, &want), "gop back to the base");
    a2k_vid_applied(0, 1, &want);
    /* a re-open the encoder refuses: the previous values come back, stored, and the MQTT module is told to publish them */
    CHECK(a2k_vid_set(1, A2K_VP_RC, "cbr") == 0 && a2k_vid_pending(1, &want) && want.v[A2K_VP_RC] == A2K_RC_CBR, "sub stream to CBR");
    a2k_vid_applied(1, 0, &want);
    a2k_vid_get(1, &g);
    CHECK(g.v[A2K_VP_RC] == A2K_RC_VBR && !a2k_vid_pending(1, &want) && notes == 1, "refused by the encoder: back to VBR, nothing pending, the real value is announced");
    CHECK(a2k_store_read(p, buf, sizeof buf) > 0 && !strstr(buf, "video1_rc") && strstr(buf, "video0_kbps=3000\n") && strstr(buf, "video0_fps=10\n"), "...and the stored file has no video1_rc any more");
    /* restart: the runtime file over the base */
    CHECK(a2k_vid_init(base, p) == 1, "a restart reads the runtime file");
    a2k_vid_get(0, &g);
    CHECK(g.v[A2K_VP_KBPS] == 3000 && g.v[A2K_VP_FPS] == 10 && g.v[A2K_VP_MINQP] == 40 && g.v[A2K_VP_MAXQP] == 40 && g.v[A2K_VP_MAXKBPS] == 0, "values restored");
    CHECK(!a2k_vid_pending(0, &want), "what boots is what runs: nothing pending");
    /* a value set back to the base value leaves the file */
    CHECK(a2k_vid_set(0, A2K_VP_KBPS, "2000") == 0 && a2k_vid_set(0, A2K_VP_FPS, "15") == 0 && a2k_vid_set(0, A2K_VP_MINQP, "32") == 0 && a2k_vid_set(0, A2K_VP_MAXQP, "51") == 0, "back to the base");
    CHECK(a2k_store_read(p, buf, sizeof buf) == 0, "the file is empty again");
    /* boot failure of the encoder with runtime values */
    CHECK(a2k_vid_set(0, A2K_VP_KBPS, "4000") == 0, "change");
    (void)a2k_vid_pending(0, &want);
    a2k_vid_applied(0, 1, &want);
    CHECK(a2k_vid_reset_runtime() == 1 && a2k_vid_get_text(0, A2K_VP_KBPS, t, sizeof t) == 0 && !strcmp(t, "2000") && !a2k_vid_pending(0, &want) && a2k_vid_reset_runtime() == 0,
          "reset_runtime: back to the base, once");
    CHECK(a2k_store_read(p, buf, sizeof buf) == 0, "and the file is empty");
    /* an invalid runtime file is ignored */
    { FILE *f = fopen(p, "w"); fputs("garbage\n", f); fclose(f); }
    CHECK(a2k_vid_init(base, p) == 0, "an invalid runtime file is ignored");
    a2k_vid_get(0, &g);
    CHECK(g.v[A2K_VP_KBPS] == 2000, "the base stays");
    CHECK(a2k_vid_init(base, NULL) == 0 && a2k_vid_set(0, A2K_VP_GOP, "60") == 0, "without a store path: RAM only");
    CHECK(a2k_vid_get_text(0, A2K_VP_GOP, t, sizeof t) == 0 && !strcmp(t, "60") && a2k_vid_get_text(2, 0, t, sizeof t) == -1, "text of a value; bad stream refused");
}

int main(void)
{
    char dir[] = "/tmp/rot2k_video_XXXXXX";
    char cmd[300];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    test_rules();
    test_file();
    test_live(dir);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0) { /* best effort */ }
    printf("rot2k video unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
