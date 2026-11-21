/* akam2k_led: PoC / control tool for the lights of the Rotatable 2K, through the portable hal_light module.
 *
 *   akam2k_led list
 *   akam2k_led [--force] [--secs N] <name> <on|off|blink-fast|blink-slow>
 *
 * on / off set the light and exit (the pin keeps its level). blink-* stays in the foreground for N seconds (default 15, 1..60)
 * and then turns the light off; SIGINT / SIGTERM / SIGALRM do the same. The running app owns the IR LED (day/night), so "ir"
 * is refused unless --force is given. "blue" is the status LED (the app owns it while it runs). The IR-cut pins are not reachable: they are not emitters.
 * Exit codes: 0 ok, 1 hardware error, 2 usage / refused. Plain libc, no vendor lib. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hal.h"
#include "light_logic.h"

#define DEFAULT_SECS 15
#define NAP_NS 50000000L
static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int usage(void)
{
    fprintf(stderr, "usage: akam2k_led list\n       akam2k_led [--force] [--secs N] <name> <on|off|blink-fast|blink-slow>\n");
    return 2;
}

int main(int argc, char **argv)
{
    light_t l;
    int force = 0, secs = DEFAULT_SECS, mode, a = 1;
    const char *name;
    while (a < argc && strncmp(argv[a], "--", 2) == 0) {
        if (strcmp(argv[a], "--force") == 0)
            force = 1;
        else if (strcmp(argv[a], "--secs") == 0 && a + 1 < argc)
            secs = atoi(argv[++a]);
        else
            return usage();
        a++;
    }
    if (light_init(&l, &a2k_light_desc, &hal_light_sysfs_ops) != 0)
        return 1;
    if (argc - a == 1 && strcmp(argv[a], "list") == 0) {
        for (int i = 0; i < a2k_light_desc.count; i++)
            printf("%s\n", a2k_light_desc.emitters[i].name);
        return 0;
    }
    if (argc - a != 2 || secs < 1 || secs > 60 || light_parse_mode(argv[a + 1], &mode) != 0)
        return usage();
    name = argv[a];
    if (light_find(&l, name) < 0) {
        fprintf(stderr, "akam2k_led: unknown light '%s' (try: akam2k_led list)\n", name);
        return 2;
    }
    if (strcmp(name, "ir") == 0 && !force) {
        fprintf(stderr, "akam2k_led: 'ir' belongs to the running app (day/night); pass --force to override\n");
        return 2;
    }
    if (light_set(&l, name, mode, now_ms()) != 0) {
        fprintf(stderr, "akam2k_led: cannot drive '%s'\n", name);
        return 1;
    }
    if (mode == LIGHT_BLINK_FAST || mode == LIGHT_BLINK_SLOW) {
        long end = now_ms() + (long)secs * 1000L;
        struct timespec nap = { 0, NAP_NS };
        int rc = 0;
        signal(SIGINT, on_sig);
        signal(SIGTERM, on_sig);
        signal(SIGALRM, on_sig);
        alarm((unsigned)secs + 2u);
        while (!g_stop && now_ms() < end) {
            if (light_tick(&l, now_ms()) != 0) {
                rc = 1;
                break;
            }
            nanosleep(&nap, NULL);
        }
        if (light_set(&l, name, LIGHT_OFF, now_ms()) != 0)
            rc = 1;
        return rc;
    }
    return 0;
}
