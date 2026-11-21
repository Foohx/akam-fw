/* Host unit test of the Rotatable 2K own app's detection state (detstate.c): the hysteresis state machine (ON after
 * K consecutive positive inferences, OFF after M ms without one, class 1 as extra info only), the observation
 * collector, the state-file format and its atomic writer, and the model path rule. No vendor library involved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../firmware/lsc-rotatable-2k-ak3918av130/detstate.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

static struct a2k_det_obs pos(int score, int x1)
{
    struct a2k_det_obs o;
    struct a2k_det_box b = { A2K_DET_CLASS_PERSON, score, x1, 10, x1 + 100, 150 };
    a2k_det_obs_reset(&o);
    a2k_det_obs_add(&o, &b);
    return o;
}

static struct a2k_det_obs neg(void)
{
    struct a2k_det_obs o;
    a2k_det_obs_reset(&o);
    return o;
}

static char *slurp(const char *path)
{
    static char buf[1024];
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(void)
{
    struct a2k_det_cfg cfg = { 2, 3000 };
    a2k_det_t d;
    struct a2k_det_event ev;
    struct a2k_det_obs o, p, n;
    struct a2k_det_box b;
    int64_t t = 1000;
    int i, got;

    /* ---- close persons need a lower score (large box), small boxes do not */
    CHECK(a2k_det_person_score_ok(650, 700, 600, 2, 188, 192), "a close person (98 % tall) at score 650 passes with close_thr 600");
    CHECK(!a2k_det_person_score_ok(650, 700, 600, 60, 120, 192), "a small box (31 % tall) at score 650 does not pass");
    CHECK(!a2k_det_person_score_ok(550, 700, 600, 2, 188, 192), "below close_thr nothing passes");
    CHECK(a2k_det_person_score_ok(710, 700, 600, 60, 120, 192) && !a2k_det_person_score_ok(650, 700, 0, 2, 188, 192), "the normal threshold is unchanged; close_thr 0 is off");

    /* ---- observation collector */
    a2k_det_obs_reset(&o);
    b = (struct a2k_det_box){ A2K_DET_CLASS_PERSON, 800, 1, 2, 3, 4 };
    a2k_det_obs_add(&o, &b);
    b = (struct a2k_det_box){ A2K_DET_CLASS_PERSON, 930, 5, 6, 7, 8 };
    a2k_det_obs_add(&o, &b);
    b = (struct a2k_det_box){ A2K_DET_CLASS_HEAD, 880, 9, 9, 9, 9 };
    a2k_det_obs_add(&o, &b);
    b = (struct a2k_det_box){ A2K_DET_CLASS_HEAD, 900, 9, 9, 9, 9 };
    a2k_det_obs_add(&o, &b);
    b = (struct a2k_det_box){ 7, 999, 0, 0, 1, 1 };
    a2k_det_obs_add(&o, &b);
    CHECK(o.person == 1 && o.best.score == 930 && o.best.x1 == 5, "collector keeps the best class 0 box");
    CHECK(o.heads == 2 && o.head_score == 900, "collector counts class 1 and keeps its best score as extra info");
    a2k_det_obs_reset(&o);
    b = (struct a2k_det_box){ A2K_DET_CLASS_HEAD, 900, 1, 1, 2, 2 };
    a2k_det_obs_add(&o, &b);
    CHECK(o.person == 0 && o.heads == 1, "a head alone is not a person");

    /* ---- hysteresis */
    a2k_det_init(&d, &cfg);
    n = neg();
    p = pos(900, 10);
    CHECK(a2k_det_update(&d, t, &n, &ev) == 0 && !d.present, "start: absent, no event on an empty scene");
    t += 200;
    CHECK(a2k_det_update(&d, t, &p, &ev) == 0 && !d.present, "one positive is not enough (K=2)");
    t += 200;
    CHECK(a2k_det_update(&d, t, &n, &ev) == 0 && d.streak == 0, "a negative breaks the streak");
    t += 200;
    CHECK(a2k_det_update(&d, t, &p, &ev) == 0 && !d.present, "positive after a break: streak restarts at 1");
    t += 200;
    p = pos(950, 40);
    got = a2k_det_update(&d, t, &p, &ev);
    CHECK(got == 1 && ev.type == A2K_DET_EV_ON && d.present, "second consecutive positive: ON");
    CHECK(ev.box.score == 950 && ev.box.x1 == 40 && ev.t_ms == t, "ON carries the best box of the streak and its time");
    t += 200;
    p = pos(920, 60);
    CHECK(a2k_det_update(&d, t, &p, &ev) == 0, "more positives while present: no new event");
    for (i = 0; i < 14; i++) { /* 14 x 200 ms = 2.8 s without a person: still present */
        t += 200;
        got = a2k_det_update(&d, t, &n, &ev);
        if (got) break;
    }
    CHECK(i == 14 && d.present, "2.8 s without a person: still present (M=3 s)");
    t += 200;
    got = a2k_det_update(&d, t, &n, &ev);
    CHECK(got == 1 && ev.type == A2K_DET_EV_OFF && !d.present, "3.0 s without a person: OFF");
    CHECK(ev.absent_ms == 3000 && ev.present_ms > 3000, "OFF reports the absence (3.0 s) and the presence duration");
    CHECK(ev.box.x1 == 60 && ev.box.score == 920, "OFF carries the last box seen");
    t += 200;
    CHECK(a2k_det_update(&d, t, &n, &ev) == 0, "no repeated OFF");
    CHECK(d.last_event == A2K_DET_EV_OFF && d.inferences > 10, "last event and inference count kept");

    /* a flicker (one negative between positives) never turns it OFF */
    a2k_det_init(&d, &cfg);
    t = 0;
    for (i = 0; i < 3; i++, t += 200)
        a2k_det_update(&d, t, &p, &ev);
    CHECK(d.present, "present after three positives");
    for (i = 0; i < 20; i++, t += 200) {
        struct a2k_det_obs in = (i % 2) ? p : n;
        if (a2k_det_update(&d, t, &in, &ev))
            break;
    }
    CHECK(i == 20 && d.present, "alternating positive/negative never reaches the OFF timeout");

    /* K=1, M=0 edge: immediate both ways */
    cfg.on_count = 1;
    cfg.off_ms = 0;
    a2k_det_init(&d, &cfg);
    CHECK(a2k_det_update(&d, 0, &p, &ev) == 1 && ev.type == A2K_DET_EV_ON, "K=1: ON at once");
    CHECK(a2k_det_update(&d, 10, &n, &ev) == 1 && ev.type == A2K_DET_EV_OFF, "M=0: OFF at the first empty inference");
    cfg.on_count = 0;
    cfg.off_ms = -5;
    a2k_det_init(&d, &cfg);
    CHECK(d.cfg.on_count == 1 && d.cfg.off_ms == 0, "init clamps K to at least 1 and M to at least 0");

    /* ---- state file */
    {
        struct a2k_det_snapshot s;
        char tmpl[] = "/tmp/rot2k_detect_unit.XXXXXX";
        char path[128], tmp[140], *txt;
        char big[64];
        struct stat st;

        cfg.on_count = 2;
        cfg.off_ms = 3000;
        a2k_det_init(&d, &cfg);
        t = 5000;
        a2k_det_update(&d, t, &p, &ev);
        t += 200;
        a2k_det_update(&d, t, &p, &ev);
        d.last_event_epoch = 1700000000;
        memset(&s, 0, sizeof s);
        s.status = "run";
        s.enabled = 1;
        s.hz = 5;
        s.threshold = 700;
        s.seq = 7;
        s.now_epoch = 1700000001;
        s.uptime_ms = 123456;
        s.det = &d;

        CHECK(mkdtemp(tmpl) != NULL, "temp dir");
        snprintf(path, sizeof path, "%s/akam2k_detect.state", tmpl);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        CHECK(a2k_det_state_write(path, &s) == 0, "state file written");
        CHECK(stat(tmp, &st) != 0, "no temp file left behind (renamed)");
        txt = slurp(path);
        CHECK(txt && strstr(txt, "version=1\n") && strstr(txt, "status=run\n"), "file has version and status");
        CHECK(txt && strstr(txt, "person=1\n") && strstr(txt, "score=920\n") && strstr(txt, "class=0\n"),
              "person=1 with score and class");
        CHECK(txt && strstr(txt, "box=60,10,160,150\n"), "box is x1,y1,x2,y2");
        CHECK(txt && strstr(txt, "last_event=on\n") && strstr(txt, "last_event_time=1700000000\n"), "last event on, with time");
        CHECK(txt && strstr(txt, "hz=5\n") && strstr(txt, "threshold=700\n") && strstr(txt, "seq=7\n"), "config echo and sequence");
        CHECK(txt && strstr(txt, "heads=0\n") && strstr(txt, "head_score=0\n"), "head extras present");

        /* an update replaces the file whole */
        a2k_det_update(&d, t + 4000, &n, &ev);
        s.seq = 8;
        s.status = "stopped";
        CHECK(a2k_det_state_write(path, &s) == 0, "second write");
        txt = slurp(path);
        CHECK(txt && strstr(txt, "person=0\n") && strstr(txt, "score=0\n") && strstr(txt, "box=0,0,0,0\n"), "absent: person=0, zeroed box");
        CHECK(txt && strstr(txt, "last_event=off\n") && strstr(txt, "status=stopped\n") && strstr(txt, "seq=8\n"), "last event off, status stopped");
        CHECK(txt && txt[strlen(txt) - 1] == '\n' && !strstr(txt, "seq=7"), "no remnant of the previous content");

        CHECK(a2k_det_state_format(big, sizeof big, &s) == -1, "a too small buffer is refused, not truncated");
        snprintf(path, sizeof path, "%s/missing_dir/x.state", tmpl);
        CHECK(a2k_det_state_write(path, &s) == -1, "an unwritable path fails cleanly");
        {
            char longp[300];
            memset(longp, 'a', sizeof longp - 1);
            longp[sizeof longp - 1] = 0;
            CHECK(a2k_det_state_write(longp, &s) == -1, "an over-long path is refused");
        }
        snprintf(path, sizeof path, "%s/akam2k_detect.state", tmpl);
        unlink(path);
        rmdir(tmpl);
    }
    CHECK(strcmp(A2K_DET_STATE_FILE, "/tmp/akam2k_detect.state") == 0, "the state file lives in /tmp");

    /* ---- model path rule */
    CHECK(a2k_model_path_ok("/appfs/resource/model/model_0x0A00000D_V1.00.01.bin"), "stock model path accepted");
    CHECK(!a2k_model_path_ok("/appfs/resource/model/../../etc/passwd"), "a .. path is refused");
    CHECK(!a2k_model_path_ok("/tmp/model.bin") && !a2k_model_path_ok("/appfs/resource/model/") && !a2k_model_path_ok(NULL),
          "outside the model directory, the bare directory and NULL are refused");

    /* ---- frame age reference: all-time minimum vs the 60 s window (experiment E3) */
    {
        struct a2k_agewin all, win;
        long long a_all = 0, a_win = 0, t;
        a2k_agewin_init(&all, 0);
        a2k_agewin_init(&win, 1);
        for (t = 0; t < 300000; t += 200) { /* a steady 40 ms queueing delay on a clock that drifts 1 ms per 2 s, one early outlier at the start */
            long long raw = 100000 + t / 2000 + (t == 0 ? -50 : 40);
            a_all = a2k_agewin_age(&all, 1000000 + t, raw);
            a_win = a2k_agewin_age(&win, 1000000 + t, raw);
        }
        CHECK(a_all > 100, "all-time minimum: a drifting clock and an early outlier make the age grow (kept for ever)");
        CHECK(a_win >= 0 && a_win <= 40, "60 s window: the age stays the real queueing delay, whatever the drift");
        a2k_agewin_init(&win, 1);
        CHECK(a2k_agewin_age(&win, 5000, 777) == 0, "the first sample is age 0");
        CHECK(a2k_agewin_age(&win, 5200, 800) == 23 && a2k_agewin_age(&win, 5400, 760) == 0, "a smaller raw age lowers the reference at once");
        CHECK(a2k_agewin_age(&win, 5400 + 59000, 900) == 140 && a2k_agewin_age(&win, 5400 + 61000, 900) == 0, "a minimum leaves the window after 60 s");
    }

    printf("rot2k detect unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
