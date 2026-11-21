/* The real motor side of the ONVIF PTZ service (onvif_services.h): the PTZ controller and the patrol presets. Kept apart so that the host tests of the service
 * link a fake instead. No new driver request: everything goes through a2k_ptz_vel_manual / a2k_ptz_command / a2k_patrol_goto_preset, i.e. the safe motion layer. */
#include "onvif_services.h"

static int real_home(void) { return a2k_ptz_command(A2K_PTZ_HOME); }

static const a2k_onvif_ptz_ops_t g_real = {
    a2k_ptz_vel_manual, a2k_ptz_manual_stop, a2k_ptz_manual_gate, real_home, a2k_patrol_goto_preset, a2k_patrol_preset_at, a2k_ptz_info,
};

const a2k_onvif_ptz_ops_t *a2k_onvif_real_ptz_ops(void) { return &g_real; }
