/* Host test of the switched-off stand-ins of the Rotatable 2K (the *_off.c files of firmware/<model>, linked instead of a feature with AKAM_FEAT_<X>=0): the parsers of the
 * absent features ACCEPT and ignore their akam.conf lines (main.c turns a refusal into usage() and exits: a left-over line would stop the app), and
 * pp_name_ok stays the real name rule (the settings use it for a button mode name: mode.apply). Built twice by feature_switch_test.sh: with the stand-ins
 * (OFF) and with the real ptz_patrol.c (REAL, only the name rule is compared). Prints "rot2k off unit: N failed" and one result line per name. */
#include <stdio.h>
#include <string.h>

#include "ptz_patrol.h"
#ifdef OFF
#include "ptz.h"
#include "scene.h"
#include "track.h"
#include "scene_check.h"
#include "drift_match.h"
#endif

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

int main(void)
{
    static const char *const names[] = { "night", "a", "route_1", "", "Night", "1abc", "a-b", "a b", "this_name_is_far_too_long_to_be_a_patrol_or_mode_name_x", "_x", NULL };
    int i;
#ifdef OFF
    int pan = 7, tilt = 7;
    struct tl_cfg c;
    a2k_tareas_t ta;
    memset(&c, 0x5a, sizeof c);
    CHECK(a2k_ptz_parse_cal("off") >= 0 && a2k_ptz_parse_cal("auto") >= 0, "PTZ=0: ptz_cal=... is accepted");
    CHECK(a2k_ptz_parse_park("1275,1300", &pan, &tilt) == 0, "PTZ=0: ptz_park=... is accepted");
    CHECK(a2k_ptz_parse_drift_correct("on") >= 0 && a2k_ptz_parse_boot_calibrate("1") >= 0 && a2k_ptz_parse_tilt_rehome("auto") >= 0, "PTZ=0: the drift / boot / rehome options are accepted");
    CHECK(sc_parse_min("300") >= 0, "SCENE=0: scene_min=... is accepted");
    CHECK(a2k_tareas_parse("track_area_door=500:1500:500:1500\n", 34, &ta) == 0, "TRACK=0: track_area_... is accepted");
    CHECK(tl_lost_parse("search", &c) == 0 && ((unsigned char *)&c)[0] == 0x5a, "TRACK=0: the lost-chain option is accepted and leaves the other tracking options alone");
    CHECK(a2k_ptz_scene_pending() == 0 && dm_static_ok(NULL) == 0, "PTZ=0: the boolean stand-ins answer false, not -1 (true)");
    printf("variant OFF\n");
#else
    printf("variant REAL\n");
#endif
    for (i = 0; names[i]; i++)
        printf("name %d %d\n", i, pp_name_ok(names[i]));
    CHECK(pp_name_ok("night") == 1 && pp_name_ok("Night") == 0 && pp_name_ok("") == 0, "pp_name_ok is the real rule (a button mode name passes with the patrol off)");
    printf("rot2k off unit: %d failed\n", fails);
    return fails != 0;
}
