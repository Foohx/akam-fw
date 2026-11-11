/* Host test of src/ak_rtsp/klog.c (filter core, fake clock) and klog_run.c (real file in, unix datagram
 * socket out). Synthetic fixture text only. Build/run: tests/klog_test.sh */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "klog.h"

static int fails;
#define CHECK(cond, ...) do { if (cond) { printf("  ok   - "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL - "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { int prio[256]; char text[256][300]; int n; } cap_t;

static void cap_emit(void *u, int prio, const char *text)
{
    cap_t *c = u;
    if (c->n >= 256) return;
    c->prio[c->n] = prio;
    snprintf(c->text[c->n], sizeof(c->text[0]), "%s", text);
    c->n++;
}

static void core_tests(void)
{
    cap_t c;
    klog_t k;

    printf("core: pass-through and priorities\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    klog_feed(&k, "<6>[    1.000000] usb 1-1: new high-speed USB device", 0);
    klog_feed(&k, "<3>[1234]:[venc_encode_tasklet_process:4467] error! AKV_Encoder_Process failed.ret =128", 0);
    klog_feed(&k, "<4>plain warning without a counter", 0);
    klog_feed(&k, "no prefix at all", 0);
    klog_feed(&k, "<11>facility bits are ignored, level 3 kept", 0);
    CHECK(c.n == 5, "five distinct messages forwarded (%d)", c.n);
    CHECK(c.prio[0] == 6 && !strcmp(c.text[0], "[    1.000000] usb 1-1: new high-speed USB device"),
          "info line forwarded verbatim");
    CHECK(c.prio[1] == 3 && strstr(c.text[1], "ret =128") && c.text[1][0] == '[', "err stays err, text untouched");
    CHECK(c.prio[2] == 4, "warning stays warning");
    CHECK(c.prio[3] == 6 && !strcmp(c.text[3], "no prefix at all"), "no prefix defaults to info");
    CHECK(c.prio[4] == 3, "level is the low 3 bits (<11> -> 3)");

    printf("core: LED trace is dropped\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    for (int i = 0; i < 200; i++) {
        char l[120];
        snprintf(l, sizeof(l), "<6>[%d]gpio_led_set value=%d, level=%d", 100 + i, i & 1, i & 1);
        klog_feed(&k, l, i);
    }
    CHECK(c.n == 0 && k.dropped == 200, "200 gpio_led_set lines, nothing forwarded");
    klog_feed(&k, "<6>gpio_led_other something else", 0);
    CHECK(c.n == 1, "a different gpio message still passes");

    printf("core: collapse repeats, summary once per window\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    for (int i = 0; i < 600; i++) { /* 10 lines a second for 60 s, a different [N] counter each time */
        char l[160];
        snprintf(l, sizeof(l), "<3>[%d]:[venc_encode_tasklet_process:4467] error! AKV_Encoder_Process failed.ret =128",
                 5000 + i);
        klog_feed(&k, l, i / 10);
    }
    CHECK(c.n == 1, "only the first occurrence is forwarded inside the window (%d)", c.n);
    CHECK(klog_next_due(&k, 59) == 1, "a summary is due at the end of the window");
    klog_tick(&k, 59);
    CHECK(c.n == 1, "no summary before the window ends");
    klog_tick(&k, 60);
    CHECK(c.n == 2 && c.prio[1] == 3 && strstr(c.text[1], "repeated 599 times in the last 60s") &&
          strstr(c.text[1], "ret =128"), "summary with count, window and the message: %s", c.text[1]);
    CHECK(klog_next_due(&k, 60) == -1, "nothing pending right after the summary");
    for (int i = 0; i < 100; i++) klog_feed(&k, "<3>[9999]:[venc_encode_tasklet_process:4467] error! AKV_Encoder_Process failed.ret =129", 60 + i / 10);
    klog_tick(&k, 120);
    CHECK(c.n == 3 && strstr(c.text[2], "repeated 100 times"), "a still-running burst gets one line per window: %s", c.text[2]);
    klog_tick(&k, 180);
    CHECK(c.n == 3, "no summary for a window without repeats");
    klog_feed(&k, "<3>[1]:[venc_encode_tasklet_process:4467] error! AKV_Encoder_Process failed.ret =128", 400);
    CHECK(c.n == 4 && c.text[3][0] == '[', "after a quiet period the message is forwarded again at once");

    printf("core: pid-style numbers fold into one message\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    klog_feed(&k, "<6>sh (101): drop_caches: 3", 0);
    klog_feed(&k, "<6>sh (102): drop_caches: 3", 30);
    klog_feed(&k, "<6>sh (103): drop_caches: 3", 61);
    CHECK(c.n == 3 && strstr(c.text[1], "repeated 1 times") && !strcmp(c.text[2], "sh (103): drop_caches: 3"),
          "feed after the window emits the summary then the new line");

    printf("core: different messages are independent\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    klog_feed(&k, "<3>disk error", 0);
    klog_feed(&k, "<3>disk error", 1);
    klog_feed(&k, "<3>wifi error", 2);
    klog_feed(&k, "<3>disk error", 3);
    CHECK(c.n == 2 && !strcmp(c.text[1], "wifi error"), "a second message is forwarded while the first is collapsed");
    klog_flush(&k);
    CHECK(c.n == 3 && strstr(c.text[2], "repeated 2 times") && strstr(c.text[2], "disk error"), "flush writes pending summaries");

    printf("core: full table recycles without losing the summary\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    klog_feed(&k, "<3>first message", 0);
    klog_feed(&k, "<3>first message", 1);
    for (int i = 0; i < KLOG_MAX_KEYS; i++) {
        char l[40];
        snprintf(l, sizeof(l), "<6>m%c unique", 'a' + i);
        klog_feed(&k, l, 2 + i);
    }
    int seen = 0;
    for (int i = 0; i < c.n; i++) if (strstr(c.text[i], "repeated 1 times") && strstr(c.text[i], "first message")) seen = 1;
    CHECK(seen, "the evicted entry's summary was emitted");

    printf("core: long lines are safe\n");
    memset(&c, 0, sizeof(c));
    klog_init(&k, cap_emit, &c, 60);
    char big[1500];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    big[0] = '<'; big[1] = '4'; big[2] = '>';
    klog_feed(&k, big, 0);
    klog_feed(&k, big, 1);
    CHECK(c.n == 1 && strlen(c.text[0]) < 300, "oversized line is cut, repeats collapse");
}

/* ---- klog_run: file in, datagram socket out -------------------------------------------------------- */

static int recv_all(int s, char out[][1100], int max)
{
    int n = 0;
    while (n < max) {
        ssize_t r = recv(s, out[n], 1099, MSG_DONTWAIT);
        if (r < 0) break;
        out[n][r] = '\0';
        n++;
    }
    return n;
}

static void run_tests(const char *dir)
{
    char kmsg[256], sock[256];
    snprintf(kmsg, sizeof(kmsg), "%s/kmsg", dir);
    snprintf(sock, sizeof(sock), "%s/log", dir);

    printf("run: end to end through a unix datagram socket\n");
    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(sock) >= sizeof(sa.sun_path)) { printf("  FAIL - work dir path too long\n"); exit(1); }
    memcpy(sa.sun_path, sock, strlen(sock) + 1);
    CHECK(bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0, "fake syslogd socket bound");

    FILE *f = fopen(kmsg, "w");
    fputs("<6>[ 10.1] boot message\n", f);
    for (int i = 0; i < 30; i++) fprintf(f, "<6>[%d]gpio_led_set value=%d, level=%d\n", 20 + i, i & 1, i & 1);
    for (int i = 0; i < 50; i++) fprintf(f, "<3>[%d]:[venc_encode_tasklet_process:4467] error! failed.ret =128\n", 40 + i);
    fputs("<2>[99] real emergency\n", f);
    fputs("<4>last line without newline", f);
    fclose(f);

    CHECK(klog_run(kmsg, sock, 60) == 0, "run returns 0 at end of input");
    char got[16][1100];
    int n = recv_all(s, got, 16);
    CHECK(n == 5, "boot, first venc error, emergency, last line, one summary at EOF (%d)", n);
    CHECK(n > 0 && !strcmp(got[0], "<6>kernel: [ 10.1] boot message"), "info line as kern.info: %s", n > 0 ? got[0] : "");
    CHECK(n > 1 && got[1][0] == '<' && got[1][1] == '3' && strstr(got[1], "kernel: [40]:[venc"), "err keeps priority 3");
    CHECK(n > 2 && !strcmp(got[2], "<2>kernel: [99] real emergency"), "emergency kept");
    CHECK(n > 3 && !strcmp(got[3], "<4>kernel: last line without newline"), "unterminated last line forwarded");
    CHECK(n > 4 && !strncmp(got[4], "<3>kernel: message repeated 49 times", 36), "summary at EOF: %s", n > 4 ? got[4] : "");
    int led = 0;
    for (int i = 0; i < n; i++) if (strstr(got[i], "gpio_led_set")) led++;
    CHECK(led == 0, "no LED trace forwarded");

    printf("run: syslogd absent is survived\n");
    close(s);
    unlink(sock);
    CHECK(klog_run(kmsg, sock, 60) == 0, "no socket: still returns 0, nothing crashes");

    printf("run: unreadable kernel log reports failure (caller falls back to klogd)\n");
    CHECK(klog_run("/nonexistent/kmsg", sock, 60) == 1, "returns 1");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: klog_test WORKDIR\n"); return 2; }
    core_tests();
    run_tests(argv[1]);
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("ALL OK\n");
    return 0;
}
