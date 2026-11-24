#include "image_orient.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ak_ioctls.h"
#include "cfg_file.h"
#include "isp.h"
#include "orient_rows.h"

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
const char *image_orient_path = ORIENT_FILE;
static int g_flip, g_mirror;

static int isp_flip_mirror(int flip, int mirror)
{
    struct isp_flip_mirror p;
    int rc;
    if (!isp_pipeline_ready()) return -1;
    p.flip = flip ? 1 : 0;
    p.mirror = mirror ? 1 : 0;
    rc = isp_tunnel(ISP_INNER_FLIP_MIRROR, &p); /* the caller holds the ISP configuration lock */
    printf("[orient] isp flip_mirror(%d, %d) = %d\n", p.flip, p.mirror, rc);
    return rc < 0 ? -1 : 0;
}

int (*image_orient_hw)(int flip, int mirror) = isp_flip_mirror;

void image_orient_get(int *flip, int *mirror)
{
    pthread_mutex_lock(&g_mu);
    if (flip) *flip = g_flip;
    if (mirror) *mirror = g_mirror;
    pthread_mutex_unlock(&g_mu);
}

static int save(int flip, int mirror)
{
    char buf[ORIENT_FILE_MAX];
    int n = orient_file_format(flip, mirror, buf, sizeof buf);
    if (n < 0) {
        errno = ENOSPC;
        return -1;
    }
    if (n == 0) return (unlink(image_orient_path) < 0 && errno != ENOENT) ? -1 : 0;
    return cfg_write_atomic(image_orient_path, buf, (size_t)n);
}

int image_orient_set_row(const char *key, int v, int persist)
{
    int nf, nm, rc = 0;
    /* Lock order everywhere: the ISP configuration lock (a day / night switch holds it), then g_mu. */
    isp_cfg_lock();
    pthread_mutex_lock(&g_mu);
    if (orient_row_next(key, v, g_flip, g_mirror, &nf, &nm) != 0) {
        rc = -1;
    } else if (nf == g_flip && nm == g_mirror) {
        rc = 0;
    } else if (image_orient_hw(nf, nm) != 0) {
        rc = -1;
    } else {
        g_flip = nf;
        g_mirror = nm;
        if (persist && save(nf, nm) != 0) rc = 1;
    }
    pthread_mutex_unlock(&g_mu);
    isp_cfg_unlock();
    return rc;
}

int image_orient_apply_saved(void)
{
    char buf[ORIENT_FILE_MAX + 2];
    size_t n;
    int f = 0, m = 0, rc = 0;
    FILE *fp = fopen(image_orient_path, "r");
    if (!fp) return 0;
    n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    if (orient_file_parse(buf, n, &f, &m) != 0) {
        printf("[orient] %s is not valid: picture not flipped\n", image_orient_path);
        return 0;
    }
    if (!f && !m) return 0;
    isp_cfg_lock();
    pthread_mutex_lock(&g_mu);
    if (image_orient_hw(f, m) == 0) {
        g_flip = f;
        g_mirror = m;
    } else {
        printf("[orient] the ISP refused the saved orientation (flip %d, mirror %d): picture not flipped\n", f, m);
        rc = -1;
    }
    pthread_mutex_unlock(&g_mu);
    isp_cfg_unlock();
    return rc;
}

void image_orient_replay_locked(void)
{
    int f, m;
    image_orient_get(&f, &m); /* g_mu is taken and released here: the ISP lock is already held by the caller */
    if (!f && !m) return;     /* nothing was ever turned: nothing is sent, the default behaviour is untouched */
    if (image_orient_hw(f, m) != 0) printf("[orient] replay after the mode switch refused by the ISP (flip %d, mirror %d)\n", f, m);
}
