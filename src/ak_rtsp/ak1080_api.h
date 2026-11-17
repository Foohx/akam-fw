#pragma once
/* The 1080p binding of the signed control API on port 8092 (src/common/api/api_core.h, api_httpd.h; the protocol, keys, scopes, audit and
 * rate limits are those of the Rotatable 2K, see docs/porting/lsc-1080p/control-api.md). Convergence step P8.
 *
 * - The port opens only while an API key exists in /etc/config/akam/api_keys (mode 0600): a camera without a key listens on nothing. The key
 *   tool is `ak_rtsp --api-key new|list|revoke`, run from SSH before any hardware is touched; the running HTTP thread notices the file within
 *   a second, so creating the first key opens the port and revoking the last one closes it, without a restart.
 * - ONE HTTP thread (64 KiB stack, 1 s tick: one stat of the key file), connection buffers malloc'd at accept and freed at close, no worker
 *   thread: a PATCH or an action runs inline on the HTTP thread (the setters are the ones of the 8091 port and return in milliseconds).
 * - Flash: a PATCH or an action that writes settings spends one token of a flash limiter (burst 6, then one per 20 s; 429 flash_rate when it
 *   is empty), on top of the request rate limit of the API. The audit file keeps the last 20 WRITES (persisted at most twice at once, then one
 *   write per 30 s, only after a write request). Nothing else is ever written periodically. */
#include <stddef.h>

#include "api_core.h"

#define AK1080_API_KEYS "/etc/config/akam/api_keys"
#define AK1080_API_AUDIT "/etc/config/akam/audit"

/* Fills *env with the 1080p binding (real paths, clocks and random source). Tests start from it and override their seams. */
void ak1080_api_env(a2k_api_env_t *env);

/* Initialises the registry (idempotent), hooks the day/night event and starts the HTTP thread on `port` (0 = ephemeral, for tests;
 * loopback_only for tests). 0, or -1 (logged: the camera goes on without the API). */
int ak1080_api_start(int port, int loopback_only, const a2k_api_env_t *env);
void ak1080_api_stop(void);

/* `ak_rtsp --api-key new|list|revoke ...` (argv after the option). Returns the process exit status. */
int ak1080_api_key_main(int argc, char **argv);

/* Test hooks: the flash limiter state. */
void ak1080_api_test_set_flash(double tokens);
