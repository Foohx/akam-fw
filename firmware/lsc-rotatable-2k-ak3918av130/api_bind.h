/* The 2K binding of the generic control API (src/api/api_core.h): the model name and version, the app log, the /config store, the
 * settings worker as the backend, and the two extra route families (PTZ presets and patrol routes, the programmable button and its
 * modes). Before the API went to src/api this was the body of akam2k_api.c. */
#ifndef AKAM2K_APIBIND_H
#define AKAM2K_APIBIND_H

#include "api_core.h"

#define A2K_API_KEYS "/config/akam/api_keys"
#define A2K_API_AUDIT "/config/akam/audit"

/* Fills *env with the 2K binding (paths and version of the real camera, the real clocks). Tests start from it and override their seams. */
void a2k_api_env_2k(a2k_api_env_t *env);

#endif
