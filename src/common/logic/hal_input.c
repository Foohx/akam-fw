/* The portable part of hal_input: the descriptor lookup and the debouncer. No I/O, no clock of its own. */
#include <string.h>

#include "hal_input.h"

const hal_input_t *hal_input_find(const hal_input_desc_t *d, const char *name)
{
    for (int i = 0; d && i < d->count; i++)
        if (!strcmp(d->inputs[i].name, name))
            return &d->inputs[i];
    return 0;
}

void hal_deb_init(hal_deb_t *d) { memset(d, 0, sizeof *d); }

int hal_deb_update(hal_deb_t *d, const hal_input_t *in, int64_t now_ms, int raw)
{
    if (raw != 0 && raw != 1)
        return d->pressed;
    if (!d->init) {
        d->init = 1;
        d->raw = raw;
        d->raw_since_ms = now_ms;
        d->pressed = in->active_low ? !raw : raw;
        return d->pressed;
    }
    if (raw != d->raw) {
        d->raw = raw;
        d->raw_since_ms = now_ms;
    } else if (now_ms - d->raw_since_ms >= in->debounce_ms) {
        d->pressed = in->active_low ? !raw : raw;
    }
    return d->pressed;
}
