/* The generic sysfs input driver (hal_input.h): the line goes through the pin helper (akam2k_pin: whitelist of input lines, export,
 * direction in, value file opened read-only). Nothing is written to the line, no path is built here. */
#include "akam2k_pin.h"
#include "hal_input.h"

static int drv_open(const hal_input_t *in) { return a2k_pin_open_in(in->gpio); }
static int drv_read(const hal_input_t *in) { return a2k_pin_read(in->gpio); }

const hal_input_ops_t hal_input_sysfs_ops = { drv_open, drv_read };
