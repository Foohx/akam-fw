/* Harness of the integration test of the signed control API of the 1080p (tests/ak1080_api_http_test.sh): the REAL binding of
 * firmware/lsc-indoor-1080p-ak3918/api_bind.c, the REAL registry (ak1080_settings.c), the REAL shared API (src/api: router, signatures, rate limits,
 * key store, audit, events, HTTP server) and the REAL LED engine, with FAKE hardware modules (ak1080_fakes.h), listening on loopback.
 *   ak1080_api_server DIR key new|list|revoke ...     the on-camera key tool (ak1080_api_key_main) against DIR/api_keys
 *   ak1080_api_server DIR [--fast] [--no-clock]       the server; prints "PORT n" (0 = no key = closed), then reads commands on stdin:
 *       port?          -> PORT n
 *       night 0|1      -> the day/night event of the night thread
 *       vals?          -> the values the fake modules hold (what the setters really received)
 *       led?           -> the content of the LED brightness file
 *       flash N        -> set the flash limiter tokens
 *       quit */
#include "ak1080_fakes.h"

#include <signal.h>
#include <time.h>

#include "api_bind.h"
#include "api_events.h"
#include "api_httpd.h"

static int g_noclock;
static int64_t wall_s(void) { return g_noclock ? 1000 : (int64_t)time(NULL); }

static int cmd_key(int argc, char **argv)
{
    a2k_api_env_t env;
    char keys[260];
    ak1080_api_env(&env);
    snprintf(keys, sizeof keys, "%s/api_keys", g_root);
    env.keys_path = keys;
    return a2k_api_key_cli(&env, argc, argv);
}

int main(int argc, char **argv)
{
    a2k_api_env_t env;
    char keys[260], audit[260], line[128], p[260];
    int i, fast = 0;
    if (argc < 2)
        return 2;
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    if (argc >= 3 && !strcmp(argv[2], "key"))
        return cmd_key(argc - 3, argv + 3);
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--no-clock")) g_noclock = 1;
        else if (!strcmp(argv[i], "--fast")) fast = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    leds_setup();
    snprintf(p, sizeof p, "%s/led/brightness", g_root);
    {
        FILE *f = fopen(p, "w");
        if (f) { fputs("1\n", f); fclose(f); }
    }
    snprintf(keys, sizeof keys, "%s/api_keys", g_root);
    snprintf(audit, sizeof audit, "%s/audit", g_root);
    snprintf(p, sizeof p, "%s/onvif", g_root);
    onvif_cfg_path = strdup(p);
    snprintf(p, sizeof p, "%s/mqtt", g_root);
    mqtt_cfg_path = strdup(p);
    snprintf(p, sizeof p, "%s/orient", g_root);
    image_orient_path = strdup(p);
    image_orient_hw = fake_orient_hw;
    ak1080_api_env(&env);
    env.keys_path = keys;
    env.audit_path = audit;
    env.log = NULL; /* the server log would only add noise to the harness output */
    if (g_noclock)
        env.wall_s = wall_s;
    if (fast) {
        env.rate_milli = 50000;
        env.burst = 60;
    }
    if (ak1080_api_start(0, 1, &env) != 0)
        return 1;
    for (i = 0; i < 50 && a2k_httpd_port() == 0 && a2k_api_keys_count() > 0; i++)
        usleep(100000);
    printf("PORT %d\n", a2k_httpd_port());
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "quit", 4))
            break;
        if (!strncmp(line, "port?", 5)) {
            printf("PORT %d\n", a2k_httpd_port());
        } else if (!strncmp(line, "night ", 6)) {
            int n = atoi(line + 6);
            if (g_night_cb) g_night_cb(n, n);
            printf("OK night\n");
        } else if (!strncmp(line, "flash ", 6)) {
            ak1080_api_test_set_flash(atof(line + 6));
            printf("OK flash\n");
        } else if (!strncmp(line, "vals?", 5)) {
            printf("VALS brightness=%d contrast=%d saturation=%d sharpness=%d denoise=%d led=%s ir=%s night=%s kbps=%d fps=%d gop=%d minqp=%d maxqp=%d\n",
                   g_vals[0], g_vals[1], g_vals[2], g_vals[3], g_vals[4], g_led_status, g_ir, g_night_mode, g_video[0], g_video[1], g_video[2],
                   g_video[3], g_video[4]);
        } else if (!strncmp(line, "led?", 4)) {
            char b[16] = "";
            FILE *f;
            snprintf(p, sizeof p, "%s/led/brightness", g_root);
            f = fopen(p, "r");
            if (f) { if (fgets(b, sizeof b, f)) { b[strcspn(b, "\n")] = 0; } fclose(f); }
            printf("LED %s\n", b);
        } else {
            printf("ERR\n");
        }
    }
    ak1080_api_stop();
    return 0;
}
