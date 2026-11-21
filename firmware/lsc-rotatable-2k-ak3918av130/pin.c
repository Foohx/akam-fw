/* Sysfs pins of the day/night and light hardware (four outputs) and the button (one input) (see pin.h). Paths are built from the fixed root and the pin number
 * only after the number passed the whitelist, so no other pin and no other file can be reached. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pin.h"

#define SYSFS_ROOT "/sys/class/gpio"

static const char *g_root = SYSFS_ROOT;
static struct {
    int pin;
    int fd; /* value file, -1 when not open */
} g_pins[4] = { { A2K_PIN_IRCUT_P, -1 }, { A2K_PIN_IRCUT_M, -1 }, { A2K_PIN_IRLED, -1 }, { A2K_PIN_BLUE, -1 } };

static int g_in_fd = -1; /* value file of the input pin (read-only) */

void a2k_pin_test_root(const char *root) { g_root = root ? root : SYSFS_ROOT; }

int a2k_pin_allowed(int pin)
{
    return pin == A2K_PIN_IRCUT_P || pin == A2K_PIN_IRCUT_M || pin == A2K_PIN_IRLED || pin == A2K_PIN_BLUE;
}

static int slot(int pin)
{
    for (int i = 0; i < 4; i++)
        if (g_pins[i].pin == pin)
            return i;
    return -1;
}

/* Writes `s` to an EXISTING file (never creates one). */
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

int a2k_pin_open(int pin, int level)
{
    char p[160], num[8];
    struct stat st;
    int i = a2k_pin_allowed(pin) ? slot(pin) : -1;
    if (i < 0)
        return -1;
    if (g_pins[i].fd >= 0) {
        close(g_pins[i].fd);
        g_pins[i].fd = -1;
    }
    snprintf(p, sizeof p, "%s/gpio%d", g_root, pin);
    if (stat(p, &st) != 0) { /* not exported yet */
        snprintf(p, sizeof p, "%s/export", g_root);
        snprintf(num, sizeof num, "%d", pin);
        if (put(p, num) != 0 && errno != EBUSY)
            return -1;
    }
    snprintf(p, sizeof p, "%s/gpio%d/direction", g_root, pin);
    if (put(p, "out") != 0)
        return -1;
    snprintf(p, sizeof p, "%s/gpio%d/value", g_root, pin);
    g_pins[i].fd = open(p, O_WRONLY | O_CLOEXEC);
    if (g_pins[i].fd < 0)
        return -1;
    if (a2k_pin_set(pin, level) != 0) {
        close(g_pins[i].fd);
        g_pins[i].fd = -1;
        return -1;
    }
    return 0;
}

int a2k_pin_set(int pin, int level)
{
    int i = a2k_pin_allowed(pin) ? slot(pin) : -1;
    if (i < 0 || g_pins[i].fd < 0)
        return -1;
    return pwrite(g_pins[i].fd, level ? "1" : "0", 1, 0) == 1 ? 0 : -1;
}

void a2k_pin_emergency_low(void)
{
    if (g_pins[0].fd >= 0 && pwrite(g_pins[0].fd, "0", 1, 0) < 0) {
    }
    if (g_pins[1].fd >= 0 && pwrite(g_pins[1].fd, "0", 1, 0) < 0) {
    }
}

void a2k_pin_close_all(void)
{
    for (int i = 0; i < 4; i++)
        if (g_pins[i].fd >= 0) {
            close(g_pins[i].fd);
            g_pins[i].fd = -1;
        }
    if (g_in_fd >= 0) {
        close(g_in_fd);
        g_in_fd = -1;
    }
}

int a2k_pin_input_allowed(int pin) { return pin == A2K_PIN_BUTTON; }

int a2k_pin_open_in(int pin)
{
    char p[160], num[8];
    struct stat st;
    if (!a2k_pin_input_allowed(pin))
        return -1;
    if (g_in_fd >= 0) {
        close(g_in_fd);
        g_in_fd = -1;
    }
    snprintf(p, sizeof p, "%s/gpio%d", g_root, pin);
    if (stat(p, &st) != 0) { /* not exported yet */
        snprintf(p, sizeof p, "%s/export", g_root);
        snprintf(num, sizeof num, "%d", pin);
        if (put(p, num) != 0 && errno != EBUSY)
            return -1;
    }
    snprintf(p, sizeof p, "%s/gpio%d/direction", g_root, pin);
    if (put(p, "in") != 0)
        return -1;
    snprintf(p, sizeof p, "%s/gpio%d/value", g_root, pin);
    g_in_fd = open(p, O_RDONLY | O_CLOEXEC);
    return g_in_fd < 0 ? -1 : 0;
}

int a2k_pin_read(int pin)
{
    char c;
    if (!a2k_pin_input_allowed(pin) || g_in_fd < 0 || pread(g_in_fd, &c, 1, 0) != 1)
        return -1;
    return c == '1' ? 1 : c == '0' ? 0 : -1;
}
