/* The camera side of the MQTT module (mqtt_ha.h): reads and writes go through the same code as the 8091 control
 * port (control.c), so MQTT, 8091, ONVIF and the SSH helpers share the setters, their limits and their persistence. */
#include "mqtt_ops.h"

#include "control.h"
#include "onvif_device.h"
#include <stdint.h>
#include <time.h>

#include "version.h"

static int ops_get(const char *key, char *out, size_t cap) { return control_get_value(key, out, cap); }

static int ops_set(const char *key, const char *value, int persist, char *msg, size_t cap)
{
    return control_apply_setting(key, value, persist, msg, cap);
}

static void ops_hostname(char *out) { onvif_hostname_get(out); }

static const char *ops_version(void) { return firmware_version(); }

static int64_t ops_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const mqtt_ops_t mqtt_ops_camera = { ops_get, ops_set, ops_hostname, ops_version, ops_now_ms };
