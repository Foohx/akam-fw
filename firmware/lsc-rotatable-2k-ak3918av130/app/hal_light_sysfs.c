/* The generic sysfs light driver (hal_light.h). GPIO emitters go through the pin helper (akam2k_pin: whitelist, export,
 * direction out, value kept open; polarity applied here); LED-class emitters write <led_dir>/brightness, after writing
 * "none" to <led_dir>/trigger once. The only paths built here are those two file names under the descriptor's led_dir, and
 * files are opened for writing only, never created. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "akam2k_pin.h"
#include "hal_light.h"

#define MAX_TRACKED 8

static const hal_light_emitter_t *g_pin_open[MAX_TRACKED];  /* emitters whose pin this process already claimed */
static const hal_light_emitter_t *g_trig_done[MAX_TRACKED]; /* LED-class emitters whose trigger is already "none" */

static int seen(const hal_light_emitter_t **tab, const hal_light_emitter_t *e)
{
    for (int i = 0; i < MAX_TRACKED; i++)
        if (tab[i] == e)
            return 1;
    return 0;
}

static int remember(const hal_light_emitter_t **tab, const hal_light_emitter_t *e)
{
    for (int i = 0; i < MAX_TRACKED; i++)
        if (!tab[i]) {
            tab[i] = e;
            return 0;
        }
    return -1;
}

/* Writes `s` to an EXISTING file. */
static int put(const char *path, const char *s)
{
    size_t n = strlen(s);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t w;
    if (fd < 0)
        return -1;
    w = write(fd, s, n);
    if (close(fd) != 0 || w != (ssize_t)n)
        return -1;
    return 0;
}

static int set_line(const hal_light_emitter_t *e, int level)
{
    if (seen(g_pin_open, e))
        return a2k_pin_set(e->gpio, level);
    if (a2k_pin_open(e->gpio, level) != 0)
        return -1;
    return remember(g_pin_open, e);
}

static int set_ledclass(const hal_light_emitter_t *e, int level)
{
    char p[200];
    if (!e->led_dir)
        return -1;
    if (!seen(g_trig_done, e)) {
        snprintf(p, sizeof p, "%s/trigger", e->led_dir);
        if (put(p, "none") != 0 || remember(g_trig_done, e) != 0)
            return -1;
    }
    snprintf(p, sizeof p, "%s/brightness", e->led_dir);
    return put(p, level ? "1" : "0");
}

static int sysfs_set(const hal_light_emitter_t *e, int on)
{
    int level;
    if (!e)
        return -1;
    level = (on != 0) == (e->active_high != 0);
    return e->kind == LIGHT_GPIO ? set_line(e, level) : e->kind == LIGHT_LEDCLASS ? set_ledclass(e, level) : -1;
}

const hal_light_ops_t hal_light_sysfs_ops = { sysfs_set };
