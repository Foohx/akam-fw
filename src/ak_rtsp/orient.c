#include "orient.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "ak_ioctls.h"
#include "isp.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_flip, g_mirror, g_ready;
static int g_sent_flip, g_sent_mirror; /* what the ISP was last told (0, 0 after its initialisation) */

/* Stock anyka_ipc: Ak_ISP_Set_Flip_Mirror() sends two 32-bit words, flip then mirror, each 0 or 1. */
static int send_locked(void)
{
    int32_t fm[2] = { g_flip, g_mirror };
    int rc = isp_tunnel(ISP_INNER_FLIP_MIRROR, fm);
    if (rc == 0) {
        g_sent_flip = g_flip;
        g_sent_mirror = g_mirror;
    }
    printf("[orient] flip=%d mirror=%d: %s\n", g_flip, g_mirror, rc == 0 ? "applied" : "refused by the ISP");
    return rc == 0 ? 0 : -1;
}

int orient_set(int flip, int mirror)
{
    pthread_mutex_lock(&g_lock);
    g_flip = flip ? 1 : 0;
    g_mirror = mirror ? 1 : 0;
    int rc = 0;
    if (g_ready && (g_flip != g_sent_flip || g_mirror != g_sent_mirror)) rc = send_locked();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

void orient_ready(void)
{
    pthread_mutex_lock(&g_lock);
    g_ready = 1;
    if (g_flip || g_mirror) (void)send_locked();
    pthread_mutex_unlock(&g_lock);
}

void orient_after_mode_switch(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_ready && (g_flip || g_mirror)) (void)send_locked();
    pthread_mutex_unlock(&g_lock);
}
