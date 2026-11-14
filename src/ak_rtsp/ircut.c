#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "ircut.h"

/* GPIO values from _ht_hw_settings.ini — ircut_type = 1 (IRCUT_LINE_TYPE_A_B)
 *
 * Physical wiring: dual-coil solenoid.  GPIO_A drives one coil, GPIO_B_EN
 * drives the other.  To move the filter, both pins are asserted in opposite
 * polarity, then GPIO_A is released to the idle level after a 100ms pulse.
 *
 * Matches the stock firmware: SetIrcutState / ircut_load_hw_config.
 */
#define IRCUT_GPIO_A        65
#define IRCUT_GPIO_B_EN     66
#define IR_LED_GPIO         70

#define IRCUT_A_DAY         0   /* ircut_a_day_level */
#define IRCUT_B_DAY         1   /* ircut_b_en_day_level */
#define IRCUT_A_NIGHT       1   /* ircut_a_night_level */
#define IRCUT_B_NIGHT       0   /* ircut_b_en_night_level */

static void gpio_write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[ircut] open %s: %s\n", path, strerror(errno));
        return;
    }
    if (write(fd, val, strlen(val)) < 0)
        fprintf(stderr, "[ircut] write %s: %s\n", path, strerror(errno));
    close(fd);
}

static void gpio_export(int gpio)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    if (access(path, F_OK) == 0) return;  /* already exported */
    char num[8];
    snprintf(num, sizeof(num), "%d", gpio);
    gpio_write_file("/sys/class/gpio/export", num);
    usleep(10000);  /* wait for kernel to create sysfs entries */
}

/* Set direction=output and initial level atomically via "low"/"high". */
static void gpio_direction_out(int gpio, int initial)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    gpio_write_file(path, initial ? "high" : "low");
}

static void gpio_set(int gpio, int value)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    gpio_write_file(path, value ? "1" : "0");
}

/* Drive the solenoid to night or day position.
 *
 * Mirrors SetIrcutState for ircut_type=1 (IRCUT_LINE_TYPE_A_B):
 *   Step 1 — assert both coils in opposite polarity to start movement
 *   Step 2 — release GPIO_A to its idle level after 100ms
 *   The filter takes ~100ms to travel; the second 100ms is settling time.
 */
static void ircut_set_state(int night)
{
    if (night) {
        gpio_set(IRCUT_GPIO_A,    IRCUT_A_NIGHT);  /* 1 */
        gpio_set(IRCUT_GPIO_B_EN, IRCUT_B_NIGHT);  /* 0 */
        usleep(100000);
        gpio_set(IRCUT_GPIO_A,    IRCUT_B_NIGHT);  /* release A to 0 (idle night) */
        usleep(100000);
    } else {
        gpio_set(IRCUT_GPIO_A,    IRCUT_A_DAY);    /* 0 */
        gpio_set(IRCUT_GPIO_B_EN, IRCUT_B_DAY);    /* 1 */
        usleep(100000);
        gpio_set(IRCUT_GPIO_A,    IRCUT_B_DAY);    /* release A to 1 (idle day) */
        usleep(100000);
    }
}

/* Initialize IR cut filter and IR LED GPIOs, then fire the reset pulse.
 *
 * Same sequence as the stock firmware:
 *   ht_night_mode_init
 *     → ircut_gpio_export_and_reset   (export GPIO A/B, reset pulse)
 *       → ircut_reset_pulse_once      (night→day once per boot)
 *   → export GPIO 70 and set it as output (IR LED)
 *   → SetDayNightMode(0)                            (already in day state: no-op)
 *
 * The reset pulse is what causes the characteristic two solenoid clicks at
 * boot — one for night, one for day — ensuring a known filter position.
 * The flag file prevents a second reset if ak_rtsp is restarted without
 * a full reboot.
 */
int ircut_init(void)
{
    /* Export ircut solenoid GPIOs, set direction + initial day-mode level. */
    gpio_export(IRCUT_GPIO_A);
    gpio_direction_out(IRCUT_GPIO_A, IRCUT_A_DAY);    /* low */

    gpio_export(IRCUT_GPIO_B_EN);
    gpio_direction_out(IRCUT_GPIO_B_EN, IRCUT_B_DAY); /* high */

    /* Export IR LED GPIO, start off. */
    gpio_export(IR_LED_GPIO);
    gpio_direction_out(IR_LED_GPIO, 0);

    usleep(100000);  /* 100ms settling before any pulse */

    /* Reset pulse: home solenoid to a known position (once per boot). */
    if (access("/tmp/ircut_has_reset_flag", F_OK) != 0) {
        printf("[ak_rtsp] ircut: reset pulse — night\n");
        ircut_set_state(1);
        usleep(50000);
        printf("[ak_rtsp] ircut: reset pulse — day\n");
        ircut_set_state(0);
        FILE *f = fopen("/tmp/ircut_has_reset_flag", "w");
        if (f) fclose(f);
    }

    printf("[ak_rtsp] ircut: init done — day mode, IR LED off\n");
    return 0;
}

/* IR LED policy (night.ir_led, see leds.h). g_ir_lock orders the night
 * thread's ircut_switch() against the control thread's setting change, so the
 * GPIO never ends up lit while the policy says off. */
static pthread_mutex_t g_ir_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ir_allowed = 1;  /* night.ir_led=auto */
static int g_ir_night   = 0;  /* last requested day/night position */
static int g_ir_probe   = 0;  /* night.c is probing the ambient light: the IR LEDs are dark for a moment */

void ircut_set_ir_led_allowed(int allowed)
{
    pthread_mutex_lock(&g_ir_lock);
    g_ir_allowed = allowed ? 1 : 0;
    /* Only touch the GPIO once it exists (ircut_init() exports GPIO 70 and
     * drives it low; a previous run may have left it exported). "off" always
     * drives it low, so an IR LED left lit by an earlier ak_rtsp that stopped
     * during a night is darkened at start-up, before ircut_init() runs. "auto"
     * lights it only in night mode. */
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", IR_LED_GPIO);
    if ((!g_ir_allowed || g_ir_night) && access(path, F_OK) == 0)
        gpio_set(IR_LED_GPIO, g_ir_allowed && g_ir_night && !g_ir_probe);
    pthread_mutex_unlock(&g_ir_lock);
}

/* Runtime day/night switch — filter position + IR LED together, matching
 * stock anyka_ipc's coordinated switch (night_mode_def=0 = NIGHT_MODE_IRLED
 * in _ht_hw_settings.ini: IR LED is part of night mode on this hardware).
 * With night.ir_led=off the filter still moves but the IR LED stays off. */
void ircut_switch(int night)
{
    ircut_set_state(night);
    pthread_mutex_lock(&g_ir_lock);
    g_ir_night = night ? 1 : 0;
    g_ir_probe = 0;  /* a switch ends any probe: the LED follows the new position */
    int led = night && g_ir_allowed;
    gpio_set(IR_LED_GPIO, led);
    pthread_mutex_unlock(&g_ir_lock);
    printf("[ak_rtsp] ircut: switched to %s mode (IR LED %s)\n",
           night ? "NIGHT" : "DAY", led ? "on" : "off");
}

/* 1 when the IR LEDs light in night mode (night.ir_led=auto). */
int ircut_ir_led_allowed(void)
{
    pthread_mutex_lock(&g_ir_lock);
    int a = g_ir_allowed;
    pthread_mutex_unlock(&g_ir_lock);
    return a;
}

/* night.c's ambient light probe (see night_logic.h): off=1 darkens the IR LEDs while the filter stays in the night
 * position, off=0 gives them back (they light only in night mode with night.ir_led=auto, as ircut_switch()
 * decides). ircut_switch() clears the probe, so a switch during a probe needs no restore. */
void ircut_ir_led_probe(int off)
{
    pthread_mutex_lock(&g_ir_lock);
    g_ir_probe = off ? 1 : 0;
    if (g_ir_night) {
        gpio_set(IR_LED_GPIO, g_ir_allowed && !g_ir_probe);
        printf("[ak_rtsp] ircut: IR LED probe %s (IR LED %s)\n", off ? "start" : "end",
               (g_ir_allowed && !g_ir_probe) ? "on" : "off");
    }
    pthread_mutex_unlock(&g_ir_lock);
}
